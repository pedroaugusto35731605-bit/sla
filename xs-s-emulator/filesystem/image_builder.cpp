// =============================================================================
//  SeriesS-Emu — image_builder.cpp
// =============================================================================
#include "image_builder.hpp"

#include "../core/codec.hpp"
#include "texture_streamer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <stdexcept>

namespace xss::fs {

namespace {

constexpr std::uint32_t kSector = 2048;

template <typename T> void put(std::vector<std::uint8_t>& v, T x) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&x);
    v.insert(v.end(), p, p + sizeof(T));
}
template <typename T> void put_be(std::vector<std::uint8_t>& v, T x) {
    for (int i = sizeof(T) - 1; i >= 0; --i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

// ---------------------------------------------------------------- PNG (deflate "stored")
std::uint32_t crc32(const std::uint8_t* d, std::size_t n, std::uint32_t c = 0) {
    static std::uint32_t table[256] = {};
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t r = i;
            for (int k = 0; k < 8; ++k) r = (r & 1) ? 0xEDB88320u ^ (r >> 1) : r >> 1;
            table[i] = r;
        }
        init = true;
    }
    c = ~c;
    for (std::size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

std::vector<std::uint8_t> make_png(std::uint32_t w, std::uint32_t h, std::uint32_t seed) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(h) * (w * 3 + 1));
    const double hue = (seed % 360) / 360.0;
    for (std::uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (std::uint32_t x = 0; x < w; ++x) {
            const double t = static_cast<double>(y) / h;
            const double band = std::exp(-std::pow((static_cast<double>(x) / w - t * 0.6 - 0.2) * 6.0, 2.0));
            const double base = 0.08 + 0.10 * (1.0 - t);
            const double g = std::min(1.0, base + 0.55 * band);
            raw.push_back(static_cast<std::uint8_t>(255 * std::min(1.0, base + 0.15 * band * hue)));
            raw.push_back(static_cast<std::uint8_t>(255 * g));
            raw.push_back(static_cast<std::uint8_t>(255 * std::min(1.0, base + 0.25 * band)));
        }
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::size_t off = 0; off < raw.size(); off += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        z.push_back(off + n == raw.size() ? 1 : 0);
        put<std::uint16_t>(z, static_cast<std::uint16_t>(n));
        put<std::uint16_t>(z, static_cast<std::uint16_t>(~n));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(off), raw.begin() + static_cast<std::ptrdiff_t>(off + n));
    }
    for (auto c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    put_be<std::uint32_t>(z, (b << 16) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* tag, const std::vector<std::uint8_t>& data) {
        put_be<std::uint32_t>(png, static_cast<std::uint32_t>(data.size()));
        const std::size_t start = png.size();
        png.insert(png.end(), tag, tag + 4);
        png.insert(png.end(), data.begin(), data.end());
        put_be<std::uint32_t>(png, crc32(png.data() + start, png.size() - start));
    };
    std::vector<std::uint8_t> ihdr;
    put_be<std::uint32_t>(ihdr, w);
    put_be<std::uint32_t>(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

// ---------------------------------------------------------------- conteúdo procedural
// Texturas: blocos 8x8 de cor sólida (como dados BC-ish de jogos) + ruído esparso.
// Comprime ~2–4× com LZ, parecido com assets reais.
std::vector<std::uint8_t> make_texture_mip0(std::uint32_t size, std::uint32_t seed) {
    std::vector<std::uint8_t> px(static_cast<std::size_t>(size) * size * 4);
    std::mt19937 rng(seed);
    const std::uint32_t blocks = size / 8;
    std::vector<std::uint32_t> palette(64);
    for (auto& c : palette) c = rng() | 0xFF000000u;
    for (std::uint32_t by = 0; by < blocks; ++by)
        for (std::uint32_t bx = 0; bx < blocks; ++bx) {
            const std::uint32_t c = palette[((bx * 7 + by * 13) ^ (bx * by / 16) ^ seed) & 63];
            for (std::uint32_t y = 0; y < 8; ++y)
                for (std::uint32_t x = 0; x < 8; ++x)
                    std::memcpy(&px[((static_cast<std::size_t>(by) * 8 + y) * size + bx * 8 + x) * 4], &c, 4);
        }
    for (std::size_t i = 0; i < px.size() / 4; i += 13 + (rng() & 31)) px[i * 4] = static_cast<std::uint8_t>(rng());
    return px;
}

std::vector<std::uint8_t> downsample(const std::vector<std::uint8_t>& src, std::uint32_t size) {
    const std::uint32_t h = size / 2;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(h) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < h; ++x)
            for (int c = 0; c < 4; ++c) {
                auto at = [&](std::uint32_t xx, std::uint32_t yy) { return src[(static_cast<std::size_t>(yy) * size + xx) * 4 + c]; };
                out[(static_cast<std::size_t>(y) * h + x) * 4 + c] = static_cast<std::uint8_t>(
                    (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1)) / 4);
            }
    return out;
}

std::vector<std::uint8_t> make_xtex(std::uint32_t size, std::uint32_t seed, std::uint64_t& raw_total) {
    std::vector<std::vector<std::uint8_t>> mips;
    std::vector<std::uint8_t> cur = make_texture_mip0(size, seed);
    std::uint32_t s = size;
    while (true) {
        raw_total += cur.size();
        mips.push_back(codec::compress(cur));
        if (s <= 64) break;          // cauda de mips pequenos agrupada no último nível
        cur = downsample(cur, s);
        s /= 2;
    }
    std::vector<std::uint8_t> f;
    put<std::uint32_t>(f, kXtexMagic);
    put<std::uint16_t>(f, 1);
    put<std::uint16_t>(f, 1);
    put<std::uint32_t>(f, size);
    put<std::uint32_t>(f, size);
    put<std::uint32_t>(f, static_cast<std::uint32_t>(mips.size()));
    put<std::uint32_t>(f, 0);
    std::uint64_t off = kXtexHeader + mips.size() * kXtexMipEntry;
    std::uint32_t ms = size;
    for (auto& m : mips) {
        put<std::uint64_t>(f, off);
        put<std::uint64_t>(f, m.size());
        put<std::uint64_t>(f, static_cast<std::uint64_t>(ms) * ms * 4);
        put<std::uint32_t>(f, 1);
        put<std::uint32_t>(f, 0);
        off += m.size();
        ms /= 2;
    }
    for (auto& m : mips) f.insert(f.end(), m.begin(), m.end());
    return f;
}

// Dados de nível: registros estruturados (vértices quantizados) — bem compressíveis.
std::vector<std::uint8_t> make_level(std::uint64_t bytes) {
    std::vector<std::uint8_t> d(bytes);
    std::mt19937 rng(42);
    for (std::uint64_t i = 0; i + 16 <= bytes; i += 16) {
        const std::uint32_t v[4] = {static_cast<std::uint32_t>(i / 16 % 4096), static_cast<std::uint32_t>(i / 65536),
                                    0x3F800000u, (rng() & 7) == 0 ? static_cast<std::uint32_t>(rng()) : 0u};
        std::memcpy(&d[i], v, 16);
    }
    return d;
}

// ---------------------------------------------------------------- escritor XDVDFS
struct Node {
    std::string name;
    bool dir = false;
    std::vector<std::uint8_t> data;
    std::vector<std::unique_ptr<Node>> children;
    std::uint32_t sector = 0, size = 0;
};

Node& child_dir(Node& parent, const std::string& name) {
    for (auto& c : parent.children) if (c->dir && c->name == name) return *c;
    parent.children.push_back(std::make_unique<Node>());
    parent.children.back()->name = name;
    parent.children.back()->dir = true;
    return *parent.children.back();
}

void add_file(Node& root, const std::string& path, std::vector<std::uint8_t> data) {
    Node* cur = &root;
    std::size_t p = 0;
    while (true) {
        const auto slash = path.find('/', p);
        if (slash == std::string::npos) break;
        cur = &child_dir(*cur, path.substr(p, slash - p));
        p = slash + 1;
    }
    auto n = std::make_unique<Node>();
    n->name = path.substr(p);
    n->data = std::move(data);
    cur->children.push_back(std::move(n));
}

std::uint32_t sectors(std::uint64_t bytes) { return static_cast<std::uint32_t>((bytes + kSector - 1) / kSector); }

// Tabela de diretório: lista ligada pela subárvore direita (válida como árvore binária).
std::vector<std::uint8_t> dir_table(const Node& d) {
    std::vector<std::vector<std::uint8_t>> blobs;
    for (const auto& c : d.children) {
        std::vector<std::uint8_t> b;
        put<std::uint16_t>(b, 0);
        put<std::uint16_t>(b, 0);
        put<std::uint32_t>(b, c->sector);
        put<std::uint32_t>(b, c->size);
        b.push_back(c->dir ? 0x10 : 0x20);
        b.push_back(static_cast<std::uint8_t>(c->name.size()));
        b.insert(b.end(), c->name.begin(), c->name.end());
        while (b.size() % 4) b.push_back(0xFF);
        blobs.push_back(std::move(b));
    }
    std::vector<std::uint8_t> out;
    std::size_t pos = 0;
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        pos += blobs[i].size();
        if (i + 1 < blobs.size()) {
            const auto right = static_cast<std::uint16_t>(pos / 4);
            std::memcpy(blobs[i].data() + 2, &right, 2);
        }
        out.insert(out.end(), blobs[i].begin(), blobs[i].end());
    }
    if (out.empty()) out.resize(4, 0xFF);
    while (out.size() % kSector) out.push_back(0xFF);
    return out;
}

}  // namespace

DemoImageResult build_demo_image(const std::filesystem::path& out, const DemoImageOptions& opt) {
    const auto t0 = std::chrono::steady_clock::now();
    DemoImageResult res;
    Node root;
    root.dir = true;

    const std::string config = std::format(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Game configVersion=\"1\" xmlns:xss=\"urn:seriess-emu\">\n"
        "  <Identity Name=\"SeriesSEmu.TechDemo\" Publisher=\"CN=Homebrew\" Version=\"1.0.0.0\"/>\n"
        "  <TitleId>{}</TitleId>\n"
        "  <ShellVisuals DefaultDisplayName=\"{}\" PublisherDisplayName=\"{}\"\n"
        "                StoreLogo=\"Assets\\StoreLogo.png\"/>\n"
        "  <xss:Performance xss:TargetResolution=\"2560x1440\" xss:TargetFps=\"60\"\n"
        "                   xss:Tags=\"Optimized for Series X|S;Sampler Feedback Streaming\"/>\n"
        "</Game>\n",
        opt.title_id, opt.title, opt.publisher);
    add_file(root, "MicrosoftGame.config", {config.begin(), config.end()});
    add_file(root, "Assets/StoreLogo.png", make_png(300, 400, std::hash<std::string>{}(opt.title) & 0xFFFF));
    if (!opt.executable.empty()) add_file(root, "game.xbin", opt.executable);

    // Nível: stream XLZ de arquivo inteiro
    {
        auto level = make_level(opt.level_bytes);
        res.raw_asset_bytes += level.size();
        auto packed = codec::compress(level);
        res.disk_asset_bytes += packed.size();
        add_file(root, "data/level0.pak", std::move(packed));
    }
    // Texturas: mips comprimidos individualmente (geradas em paralelo pelo codec)
    for (std::uint32_t i = 0; i < opt.textures; ++i) {
        std::uint64_t raw = 0;
        auto x = make_xtex(opt.texture_size, 1000 + i, raw);
        res.raw_asset_bytes += raw;
        res.disk_asset_bytes += x.size();
        add_file(root, std::format("textures/tex_{:03}.xtex", i), std::move(x));
    }

    // Layout: setor 32 = descritor de volume; diretórios e arquivos em seguida.
    std::uint32_t next = 33;
    std::vector<Node*> dirs;
    std::function<void(Node&)> place_dirs = [&](Node& d) {
        dirs.push_back(&d);
        d.size = static_cast<std::uint32_t>(dir_table(d).size());   // tamanho independe dos setores
        d.sector = next;
        next += sectors(d.size);
        for (auto& c : d.children) if (c->dir) place_dirs(*c);
    };
    place_dirs(root);
    std::function<void(Node&)> place_files = [&](Node& d) {
        for (auto& c : d.children) {
            if (c->dir) { place_files(*c); continue; }
            c->size = static_cast<std::uint32_t>(c->data.size());
            c->sector = next;
            next += std::max(1u, sectors(c->size));
        }
    };
    place_files(root);

    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("não foi possível criar " + out.string());
    auto write_at = [&](std::uint32_t sector, const std::vector<std::uint8_t>& d) {
        f.seekp(static_cast<std::streamoff>(sector) * kSector);
        f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
    };
    std::vector<std::uint8_t> vd(kSector, 0);
    std::memcpy(vd.data(), "MICROSOFT*XBOX*MEDIA", 20);
    std::memcpy(vd.data() + 20, &root.sector, 4);
    std::memcpy(vd.data() + 24, &root.size, 4);
    std::memcpy(vd.data() + 0x7EC, "MICROSOFT*XBOX*MEDIA", 20);
    write_at(32, vd);
    for (Node* d : dirs) write_at(d->sector, dir_table(*d));
    std::function<void(Node&)> write_files = [&](Node& d) {
        for (auto& c : d.children) c->dir ? write_files(*c) : write_at(c->sector, c->data);
    };
    write_files(root);
    // completa o último setor
    f.seekp(0, std::ios::end);
    const auto end = static_cast<std::uint64_t>(f.tellp());
    if (end % kSector) { std::vector<char> pad(kSector - end % kSector, 0); f.write(pad.data(), static_cast<std::streamsize>(pad.size())); }
    f.close();

    res.image_bytes = std::filesystem::file_size(out);
    res.build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

}  // namespace xss::fs
