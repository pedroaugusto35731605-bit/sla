// =============================================================================
//  SeriesS-Emu — firmware.cpp
// =============================================================================
#include "firmware.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <set>

namespace xss::fs {

namespace fsys = std::filesystem;

namespace {

constexpr const char* kUpdateDir = "$SystemUpdate";

// Tipos de conteúdo XVD que pertencem ao sistema operacional (não a jogos/apps).
const std::set<std::string>& system_types() {
    static const std::set<std::string> t = {
        "SystemOS", "EraOS", "HostOS", "Updater", "OfflineUpdater", "SystemTools", "SystemAux",
        "AcousticModel", "SystemCodecsVolume", "SystemDataVolume", "EraTools", "ResetData", "Template"};
    return t;
}

bool has_xvd_files(const fsys::path& dir) {
    std::error_code ec;
    for (const auto& e : fsys::directory_iterator(dir, ec))
        if (e.is_regular_file() && read_xvd_header(e.path())) return true;
    return false;
}

// Encontra a pasta com os pacotes: a própria, "<p>/$SystemUpdate" ou "<p>/<sub>/$SystemUpdate".
std::optional<fsys::path> locate(const fsys::path& p) {
    std::error_code ec;
    if (!fsys::is_directory(p, ec)) return std::nullopt;
    if (fsys::is_directory(p / kUpdateDir, ec)) return p / kUpdateDir;
    if (p.filename() == kUpdateDir || has_xvd_files(p)) return p;
    for (const auto& e : fsys::directory_iterator(p, ec))
        if (e.is_directory() && fsys::is_directory(e.path() / kUpdateDir, ec)) return e.path() / kUpdateDir;
    return std::nullopt;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) o += std::format("\\u{:04x}", c);
        else o += static_cast<char>(c);
    }
    return o;
}

std::string safe_dir_name(const std::string& v) {
    std::string s;
    for (char c : v) s += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') ? c : '_';
    return s.empty() ? "desconhecida" : s;
}

}  // namespace

fsys::path default_firmware_root() {
    if (const char* x = std::getenv("XSS_HOME"); x && *x) return fsys::path(x) / "firmware";
#if defined(_WIN32)
    if (const char* a = std::getenv("APPDATA")) return fsys::path(a) / "SeriesS-Emu" / "firmware";
#endif
    if (const char* h = std::getenv("HOME")) return fsys::path(h) / ".seriess-emu" / "firmware";
    return ".seriess-emu/firmware";
}

FirmwareInfo inspect_system_update(const fsys::path& source) {
    FirmwareInfo f;
    std::error_code ec;
    if (!fsys::exists(source, ec)) { f.error = "caminho não encontrado: " + source.string(); return f; }
    if (fsys::is_regular_file(source, ec)) {
        f.error = source.extension() == ".zip"
                      ? "extraia o .zip primeiro (a interface faz isso automaticamente) e aponte para a pasta"
                      : "aponte para a pasta $SystemUpdate (ou a pasta extraída do OSU1 que a contém)";
        return f;
    }
    const auto root = locate(source);
    if (!root) { f.error = "pasta $SystemUpdate não encontrada"; return f; }
    f.root = *root;

    for (const auto& e : fsys::recursive_directory_iterator(*root, ec)) {
        if (!e.is_regular_file()) continue;
        FirmwarePackage p;
        p.file = fsys::relative(e.path(), *root, ec).generic_string();
        p.size = e.file_size(ec);
        p.xvd = read_xvd_header(e.path());
        f.total_bytes += p.size;
        if (p.xvd && system_types().contains(p.xvd->content_type)) ++f.system_packages;
        f.packages.push_back(std::move(p));
    }
    std::sort(f.packages.begin(), f.packages.end(), [](auto& a, auto& b) { return a.file < b.file; });

    const auto xvds = std::count_if(f.packages.begin(), f.packages.end(), [](auto& p) { return p.xvd.has_value(); });
    if (xvds == 0) { f.error = "nenhum pacote XVD encontrado na pasta"; return f; }
    if (f.system_packages == 0) {
        f.error = "os XVDs encontrados não são do sistema (parecem jogos/apps); use \"Importar ISO\" para jogos";
        return f;
    }

    // Versão: o pacote do Game OS (SystemOS) manda; senão, o do Host OS; senão, o primeiro.
    for (const char* t : {"SystemOS", "HostOS"}) {
        for (const auto& p : f.packages)
            if (p.xvd && p.xvd->content_type == t) { f.version = p.xvd->version; break; }
        if (!f.version.empty()) break;
    }
    if (f.version.empty())
        for (const auto& p : f.packages) if (p.xvd) { f.version = p.xvd->version; break; }

    bool has_system_os = false;
    for (const auto& p : f.packages) if (p.xvd && p.xvd->content_type == "SystemOS") has_system_os = true;
    if (!has_system_os) f.warnings.push_back("nenhum pacote SystemOS (Game OS) encontrado; a atualização pode estar incompleta");
    for (const auto& p : f.packages)
        if (p.xvd && p.xvd->drive_size == 0)
            f.warnings.push_back(p.file + ": cabeçalho XVD com tamanho de drive zero (arquivo truncado?)");
    f.warnings.push_back("pacotes criptografados e assinados pelo console: inventariados, não executados");
    f.valid = true;
    return f;
}

FirmwareInfo install_system_update(const fsys::path& source, const fsys::path& firmware_root, bool move,
                                   const CopyProgress& progress) {
    FirmwareInfo f = inspect_system_update(source);
    if (!f.valid) return f;

    const fsys::path dest = firmware_root / safe_dir_name(f.version) / kUpdateDir;
    std::error_code ec;
    if (fsys::equivalent(f.root, dest, ec)) {
        // Já está no lugar (reinstalação): só marca como atual.
    } else {
        if (fsys::exists(dest, ec)) fsys::remove_all(dest, ec);
        fsys::create_directories(dest.parent_path(), ec);
        bool moved = false;
        if (move) {
            fsys::rename(f.root, dest, ec);           // mesmo volume: instantâneo
            moved = !ec;
        }
        if (!moved) {
            std::uint64_t done = 0;
            for (const auto& p : f.packages) {
                const auto to = dest / p.file;
                fsys::create_directories(to.parent_path(), ec);
                if (progress) progress(done, f.total_bytes, p.file);
                if (!fsys::copy_file(f.root / p.file, to, fsys::copy_options::overwrite_existing, ec)) {
                    f.valid = false;
                    f.error = std::format("falha ao copiar {}: {}", p.file, ec.message());
                    return f;
                }
                done += p.size;
            }
            if (progress) progress(done, f.total_bytes, "");
            if (move) fsys::remove_all(f.root, ec);
        }
        f.root = dest;
    }
    std::ofstream(firmware_root / "current") << safe_dir_name(f.version) << "\n";
    return f;
}

std::optional<FirmwareInfo> installed_firmware(const fsys::path& firmware_root) {
    std::ifstream in(firmware_root / "current");
    std::string name;
    if (!in || !std::getline(in, name) || name.empty()) return std::nullopt;
    auto f = inspect_system_update(firmware_root / name);
    if (!f.valid) return std::nullopt;
    return f;
}

std::string to_json(const FirmwareInfo& f) {
    std::string j = std::format("{{\"valid\":{},\"error\":\"{}\",\"root\":\"{}\",\"version\":\"{}\","
                                "\"total_bytes\":{},\"system_packages\":{},\"packages\":[",
                                f.valid, json_escape(f.error), json_escape(f.root.generic_string()),
                                json_escape(f.version), f.total_bytes, f.system_packages);
    for (std::size_t i = 0; i < f.packages.size(); ++i) {
        const auto& p = f.packages[i];
        j += std::format("{}{{\"file\":\"{}\",\"size\":{}", i ? "," : "", json_escape(p.file), p.size);
        if (p.xvd)
            j += std::format(",\"content_type\":\"{}\",\"version\":\"{}\",\"vduid\":\"{}\",\"drive_size\":{}",
                             p.xvd->content_type, p.xvd->version, p.xvd->vduid, p.xvd->drive_size);
        j += "}";
    }
    j += "],\"warnings\":[";
    for (std::size_t i = 0; i < f.warnings.size(); ++i)
        j += std::format("{}\"{}\"", i ? "," : "", json_escape(f.warnings[i]));
    return j + "]}";
}

}  // namespace xss::fs
