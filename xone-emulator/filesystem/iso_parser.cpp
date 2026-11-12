// =============================================================================
//  XOne-Emu — iso_parser.cpp
// =============================================================================
#include "iso_parser.hpp"
#include "../core/hardware_spec.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <functional>
#include <regex>
#include <set>
#include <stdexcept>

namespace xone::fs {

namespace {

constexpr std::uint32_t kSector = 2048;
constexpr char kXdvdfsMagic[] = "MICROSOFT*XBOX*MEDIA";  // 20 bytes
constexpr std::size_t kXdvdfsMagicLen = 20;
constexpr std::uint64_t kVolumeDescSector = 32;

// Offsets de partição de jogo conhecidos (bytes)
constexpr std::uint64_t kPartitionOffsets[] = {
    0x0000'0000ull,   // XISO "trimmed" / homebrew
    0x1830'0000ull,   // XGD1 (Xbox original, redump)
    0x0FD9'0000ull,   // XGD2 (Xbox 360)
    0x0208'0000ull,   // XGD3 (Xbox 360)
};

template <typename T> T le(const std::uint8_t* p) {
    T v{}; std::memcpy(&v, p, sizeof(T)); return v;  // hosts suportados são little-endian
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string guid_str(const std::uint8_t* g) {
    // GUID em layout Microsoft (Data1..3 little-endian)
    return std::format("{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
                       le<std::uint32_t>(g), le<std::uint16_t>(g + 4), le<std::uint16_t>(g + 6),
                       g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) o += std::format("\\u{:04x}", c);
                else o += static_cast<char>(c);
        }
    }
    return o;
}

// Remove BOM UTF-8 e converte UTF-16LE (com BOM) para ASCII/UTF-8 básico.
std::string decode_text(const std::vector<std::uint8_t>& b) {
    if (b.size() >= 2 && b[0] == 0xFF && b[1] == 0xFE) {
        std::string s;
        for (std::size_t i = 2; i + 1 < b.size(); i += 2) {
            const std::uint16_t ch = static_cast<std::uint16_t>(b[i] | (b[i + 1] << 8));
            s += ch < 0x80 ? static_cast<char>(ch) : '?';
        }
        return s;
    }
    std::size_t start = (b.size() >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) ? 3 : 0;
    return std::string(b.begin() + static_cast<std::ptrdiff_t>(start), b.end());
}

std::optional<std::string> regex_first(const std::string& text, const std::vector<std::string>& patterns) {
    for (const auto& p : patterns) {
        std::smatch m;
        if (std::regex_search(text, m, std::regex(p, std::regex::icase)) && m.size() > 1 && m[1].length() > 0)
            return m[1].str();
    }
    return std::nullopt;
}

const char* xvd_content_type(std::uint32_t t) {
    static const char* names[] = {"Data", "Title", "SystemOS", "EraOS", "Scratch", "ResetData",
                                  "Application", "HostOS", "X360STFS", "X360FATX", "X360GDFX",
                                  "Updater", "OfflineUpdater", "Template", "MteHost", "MteApp",
                                  "MteTitle", "MteEraOS", "EraTools", "SystemTools", "SystemAux",
                                  "AcousticModel", "SystemCodecsVolume", "QasltPackage", "AppDlc",
                                  "TitleDlc", "UniversalDlc", "SystemDataVolume", "TestVolume"};
    return t < std::size(names) ? names[t] : "Unknown";
}

// ----------------------------------------------------------------------------
//  XDVDFS
// ----------------------------------------------------------------------------
class Xdvdfs {
public:
    Xdvdfs(NvmeReader& r, std::uint64_t part) : r_(r), part_(part) {}

    bool probe(std::uint32_t& root_sector, std::uint32_t& root_size) {
        std::uint8_t vd[kSector];
        if (!r_.read(part_ + kVolumeDescSector * kSector, vd, kSector)) return false;
        if (std::memcmp(vd, kXdvdfsMagic, kXdvdfsMagicLen) != 0) return false;
        if (std::memcmp(vd + 0x7EC, kXdvdfsMagic, kXdvdfsMagicLen) != 0) return false;  // magic final
        root_sector = le<std::uint32_t>(vd + 20);
        root_size   = le<std::uint32_t>(vd + 24);
        return root_size > 0;
    }

    // Percorre a árvore binária de entradas (left/right em unidades de 4 bytes).
    void walk_dir(std::uint32_t sector, std::uint32_t size, const std::string& prefix,
                  std::vector<FileEntry>& out, int depth = 0) {
        if (depth > 32 || size == 0 || size > 64 * 1024 * 1024) return;
        if (!visited_.insert(sector).second) return;  // proteção contra ciclos
        const auto table = r_.read_vec(part_ + static_cast<std::uint64_t>(sector) * kSector, size);
        if (table.size() != size) return;

        std::function<void(std::uint32_t, int)> visit = [&](std::uint32_t off, int guard) {
            if (guard > 4096 || off + 14 > table.size()) return;
            const std::uint8_t* e = table.data() + off;
            const auto left  = le<std::uint16_t>(e);
            const auto right = le<std::uint16_t>(e + 2);
            if (left == 0xFFFF && right == 0xFFFF) return;  // padding de setor
            const auto start = le<std::uint32_t>(e + 4);
            const auto fsize = le<std::uint32_t>(e + 8);
            const auto attr  = e[12];
            const auto nlen  = e[13];
            if (off + 14 + nlen > table.size()) return;
            const std::string name(reinterpret_cast<const char*>(e + 14), nlen);

            if (left) visit(static_cast<std::uint32_t>(left) * 4, guard + 1);

            FileEntry fe;
            fe.path   = prefix + name;
            fe.offset = part_ + static_cast<std::uint64_t>(start) * kSector;
            fe.size   = fsize;
            fe.is_dir = attr & 0x10;
            out.push_back(fe);
            if (fe.is_dir) walk_dir(start, fsize, fe.path + "/", out, depth + 1);

            if (right) visit(static_cast<std::uint32_t>(right) * 4, guard + 1);
        };
        visit(0, 0);
    }

private:
    NvmeReader& r_;
    std::uint64_t part_;
    std::set<std::uint32_t> visited_;
};

// Xbox original: certificado do default.xbe tem Title ID e nome UTF-16.
void parse_xbe(const std::vector<std::uint8_t>& xbe, GameMetadata& m) {
    if (xbe.size() < 0x178 || std::memcmp(xbe.data(), "XBEH", 4) != 0) return;
    const auto base = le<std::uint32_t>(xbe.data() + 0x104);
    const auto cert = le<std::uint32_t>(xbe.data() + 0x118);
    if (cert < base) return;
    const std::uint64_t off = cert - base;
    if (off + 0x0C + 80 > xbe.size()) return;
    m.title_id = std::format("{:08X}", le<std::uint32_t>(xbe.data() + off + 0x08));
    std::string name;
    for (int i = 0; i < 40; ++i) {
        const auto ch = le<std::uint16_t>(xbe.data() + off + 0x0C + i * 2);
        if (!ch) break;
        name += ch < 0x80 ? static_cast<char>(ch) : '?';
    }
    if (!name.empty()) m.title = name;
    m.metadata_source = "default.xbe";
}

void parse_manifest(const std::string& text, const std::string& source, GameMetadata& m) {
    if (auto v = regex_first(text, {R"re(<TitleId>\s*([0-9A-Fa-f]{8})\s*</TitleId>)re",
                                    R"re(TitleId\s*=\s*"([0-9A-Fa-f]{8})")re"})) {
        m.title_id = *v;
        std::transform(m.title_id.begin(), m.title_id.end(), m.title_id.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    }
    if (auto v = regex_first(text, {R"re(DefaultDisplayName\s*=\s*"([^"]+)")re",
                                    R"re(<DisplayName>\s*([^<]+?)\s*</DisplayName>)re"}))
        m.title = *v;
    if (auto v = regex_first(text, {R"re(PublisherDisplayName\s*=\s*"([^"]+)")re",
                                    R"re(<PublisherDisplayName>\s*([^<]+?)\s*</PublisherDisplayName>)re"}))
        m.publisher = *v;
    if (auto v = regex_first(text, {R"re(<Identity[^>]*\sVersion\s*=\s*"([^"]+)")re"}))
        m.version = *v;
    if (auto v = regex_first(text, {R"re(StoreLogo\s*=\s*"([^"]+)")re", R"re(<Logo>\s*([^<]+?)\s*</Logo>)re",
                                    R"re(Square150x150Logo\s*=\s*"([^"]+)")re"}))
        m.thumbnail_name = *v;
    // Extensão XOne-Emu opcional para perfis de desempenho
    if (auto v = regex_first(text, {R"re(xone:TargetResolution\s*=\s*"(\d+x\d+)")re"})) {
        const auto x = v->find('x');
        m.target_width  = static_cast<std::uint32_t>(std::stoul(v->substr(0, x)));
        m.target_height = static_cast<std::uint32_t>(std::stoul(v->substr(x + 1)));
    }
    if (auto v = regex_first(text, {R"re(xone:TargetFps\s*=\s*"(\d+)")re"}))
        m.target_fps = static_cast<std::uint32_t>(std::stoul(*v));
    m.metadata_source = source;
}

const FileEntry* find_file(const std::vector<FileEntry>& files, const std::string& name_or_path) {
    const std::string want = lower(name_or_path);
    for (const auto& f : files) {
        if (f.is_dir) continue;
        const std::string p = lower(f.path);
        std::string base = p.substr(p.find_last_of('/') == std::string::npos ? 0 : p.find_last_of('/') + 1);
        std::string want_norm = want;
        std::replace(want_norm.begin(), want_norm.end(), '\\', '/');
        if (p == want_norm || base == want_norm) return &f;
    }
    return nullptr;
}

bool parse_xvd(NvmeReader& r, GameMetadata& m) {
    std::uint8_t h[0x400];
    if (r.size() < sizeof(h) || !r.read(0, h, sizeof(h))) return false;
    if (std::memcmp(h + 0x200, "msft-xvd", 8) != 0) return false;

    // Layout do cabeçalho conforme documentado pelo projeto xvdtool (best-effort).
    m.format       = ImageFormat::Xvd;
    m.content_id   = guid_str(h + 0x220);                     // VDUID
    m.content_type = xvd_content_type(le<std::uint32_t>(h + 0x284));
    m.product_id   = guid_str(h + 0x39C);
    const auto drive_size = le<std::uint64_t>(h + 0x218);
    const auto v = h + 0x3BC;
    m.version = std::format("{}.{}.{}.{}", le<std::uint16_t>(v + 6), le<std::uint16_t>(v + 4),
                            le<std::uint16_t>(v + 2), le<std::uint16_t>(v));
    m.metadata_source = "XVD header";
    m.warnings.push_back(std::format("Pacote XVD/XVC: conteúdo criptografado (drive {} MiB); "
                                     "apenas metadados de cabeçalho foram lidos.", drive_size / (1024 * 1024)));
    return true;
}

}  // namespace

// ============================================================================
const char* format_name(ImageFormat f) {
    switch (f) {
        case ImageFormat::Xdvdfs: return "XDVDFS/GDF";
        case ImageFormat::Xvd:    return "XVD/XVC";
        default:                  return "Unknown";
    }
}

NvmeReader::NvmeReader(const std::filesystem::path& p) : f_(p, std::ios::binary) {
    if (f_) {
        f_.seekg(0, std::ios::end);
        size_ = static_cast<std::uint64_t>(f_.tellg());
    }
}

bool NvmeReader::read(std::uint64_t offset, void* dst, std::uint64_t len) {
    if (!f_ || offset > size_ || len > size_ - offset) return false;
    ++commands_;
    simulated_ns_ += spec::kStorage.read_latency_ns +
                     len * 1'000'000'000ull / spec::kStorage.seq_read_bytes_s;
    f_.clear();
    f_.seekg(static_cast<std::streamoff>(offset));
    f_.read(static_cast<char*>(dst), static_cast<std::streamsize>(len));
    return static_cast<std::uint64_t>(f_.gcount()) == len;
}

std::vector<std::uint8_t> NvmeReader::read_vec(std::uint64_t offset, std::uint64_t len) {
    std::vector<std::uint8_t> v(len);
    if (!read(offset, v.data(), len)) v.clear();
    return v;
}

GameMetadata IsoParser::parse(const std::filesystem::path& image) {
    NvmeReader r(image);
    if (!r.ok()) throw std::runtime_error("não foi possível abrir " + image.string());

    GameMetadata m;
    m.image_size = r.size();
    m.title = image.stem().string();

    if (parse_xvd(r, m)) return m;

    for (const auto part : kPartitionOffsets) {
        Xdvdfs fs(r, part);
        std::uint32_t root_sector = 0, root_size = 0;
        if (!fs.probe(root_sector, root_size)) continue;

        m.format = ImageFormat::Xdvdfs;
        m.partition_offset = part;
        fs.walk_dir(root_sector, root_size, "", m.files);

        // Metadados: GDK (Xbox One/Series) > AppX > XBE (Xbox original)
        for (const char* cfg : {"MicrosoftGame.config", "AppxManifest.xml"}) {
            if (const FileEntry* f = find_file(m.files, cfg); f && f->size < 4 * 1024 * 1024) {
                parse_manifest(decode_text(r.read_vec(f->offset, f->size)), f->path, m);
                break;
            }
        }
        if (m.metadata_source.empty()) {
            if (const FileEntry* f = find_file(m.files, "default.xbe"))
                parse_xbe(r.read_vec(f->offset, std::min<std::uint64_t>(f->size, 64 * 1024)), m);
        }

        // Capa: o logo declarado no manifesto, ou o primeiro PNG/JPG com nome sugestivo
        const FileEntry* thumb = m.thumbnail_name.empty() ? nullptr : find_file(m.files, m.thumbnail_name);
        if (!thumb) {
            for (const auto& f : m.files) {
                const std::string p = lower(f.path);
                if (!f.is_dir && (p.ends_with(".png") || p.ends_with(".jpg")) &&
                    (p.find("logo") != std::string::npos || p.find("cover") != std::string::npos ||
                     p.find("boxart") != std::string::npos || p.find("thumb") != std::string::npos)) {
                    thumb = &f; break;
                }
            }
        }
        if (thumb && thumb->size < 16 * 1024 * 1024) {
            m.thumbnail_name = thumb->path;
            m.thumbnail = r.read_vec(thumb->offset, thumb->size);
        }
        if (m.metadata_source.empty())
            m.warnings.push_back("Nenhum MicrosoftGame.config/AppxManifest.xml/default.xbe encontrado.");
        return m;
    }

    m.warnings.push_back("Formato não reconhecido (sem assinatura XDVDFS nem msft-xvd).");
    return m;
}

bool IsoParser::extract(const std::filesystem::path& image, const FileEntry& e, const std::filesystem::path& out) {
    if (e.is_dir) return false;
    NvmeReader r(image);
    const auto data = r.read_vec(e.offset, e.size);
    if (data.size() != e.size) return false;
    std::ofstream o(out, std::ios::binary);
    o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(o);
}

std::string IsoParser::to_json(const GameMetadata& m, bool include_files) {
    std::string j = "{";
    auto kv = [&](const char* k, const std::string& v) { j += std::format("\"{}\":\"{}\",", k, json_escape(v)); };
    kv("format", format_name(m.format));
    kv("title", m.title);
    kv("title_id", m.title_id);
    kv("publisher", m.publisher);
    kv("version", m.version);
    kv("content_type", m.content_type);
    kv("content_id", m.content_id);
    kv("product_id", m.product_id);
    kv("metadata_source", m.metadata_source);
    kv("thumbnail_name", m.thumbnail_name);
    j += std::format("\"image_size\":{},\"partition_offset\":{},\"target_width\":{},\"target_height\":{},"
                     "\"target_fps\":{},\"file_count\":{},",
                     m.image_size, m.partition_offset, m.target_width, m.target_height, m.target_fps,
                     m.files.size());
    j += "\"warnings\":[";
    for (std::size_t i = 0; i < m.warnings.size(); ++i)
        j += (i ? ",\"" : "\"") + json_escape(m.warnings[i]) + "\"";
    j += "]";
    if (include_files) {
        j += ",\"files\":[";
        for (std::size_t i = 0; i < m.files.size(); ++i)
            j += std::format("{}{{\"path\":\"{}\",\"size\":{},\"dir\":{}}}", i ? "," : "",
                             json_escape(m.files[i].path), m.files[i].size, m.files[i].is_dir);
        j += "]";
    }
    return j + "}";
}

}  // namespace xone::fs
