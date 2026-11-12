// =============================================================================
//  XOne-Emu — content_store.cpp
// =============================================================================
#include "content_store.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace xone::fs {

namespace {

std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            case '\r': o += "\\r"; break;
            default:
                if (c < 0x20) o += std::format("\\u{:04x}", c); else o += static_cast<char>(c);
        }
    }
    return o;
}

// ----------------------------------------------------------------------------
//  Leitor JSON mínimo (suficiente para library.json)
// ----------------------------------------------------------------------------
class JsonReader {
public:
    explicit JsonReader(const std::string& s) : s_(s) {}

    void ws() { while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_; }
    bool peek(char c) { ws(); return i_ < s_.size() && s_[i_] == c; }
    void expect(char c) {
        ws();
        if (i_ >= s_.size() || s_[i_] != c)
            throw std::runtime_error(std::format("library.json: esperado '{}' na posição {}", c, i_));
        ++i_;
    }

    std::string string() {
        expect('"');
        std::string o;
        while (i_ < s_.size() && s_[i_] != '"') {
            char c = s_[i_++];
            if (c != '\\') { o += c; continue; }
            if (i_ >= s_.size()) break;
            c = s_[i_++];
            switch (c) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    unsigned cp = static_cast<unsigned>(std::stoul(s_.substr(i_, 4), nullptr, 16));
                    i_ += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && s_.compare(i_, 2, "\\u") == 0) {  // par substituto
                        const unsigned lo = static_cast<unsigned>(std::stoul(s_.substr(i_ + 2, 4), nullptr, 16));
                        i_ += 6;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    append_utf8(o, cp);
                    break;
                }
                default: o += c;
            }
        }
        expect('"');
        return o;
    }

    // Devolve o texto cru do próximo valor e avança.
    std::string raw_value() {
        ws();
        const std::size_t start = i_;
        skip();
        return s_.substr(start, i_ - start);
    }

    void skip() {
        ws();
        if (i_ >= s_.size()) throw std::runtime_error("library.json: fim inesperado");
        const char c = s_[i_];
        if (c == '"') { string(); return; }
        if (c == '{' || c == '[') {
            const char close = c == '{' ? '}' : ']';
            ++i_;
            if (peek(close)) { ++i_; return; }
            for (;;) {
                if (c == '{') { string(); expect(':'); }
                skip();
                if (peek(',')) { ++i_; continue; }
                expect(close);
                return;
            }
        }
        while (i_ < s_.size() && std::string_view(",}] \t\r\n").find(s_[i_]) == std::string_view::npos) ++i_;
    }

    void object(const std::function<void(const std::string&)>& on_key) {
        expect('{');
        if (peek('}')) { ++i_; return; }
        for (;;) {
            const std::string k = string();
            expect(':');
            on_key(k);
            if (peek(',')) { ++i_; continue; }
            expect('}');
            return;
        }
    }

    void array(const std::function<void()>& on_item) {
        expect('[');
        if (peek(']')) { ++i_; return; }
        for (;;) {
            on_item();
            if (peek(',')) { ++i_; continue; }
            expect(']');
            return;
        }
    }

private:
    static void append_utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    const std::string& s_;
    std::size_t i_ = 0;
};

std::string now_iso8601() {
    const auto t = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%Y-%m-%dT%H:%M:%SZ}", t);
}

std::string path_hash_id(const std::string& s) {
    std::uint64_t h = 1469598103934665603ull;  // FNV-1a
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return std::format("local-{:012x}", h & 0xFFFF'FFFF'FFFFull);
}

}  // namespace

// ============================================================================
ContentStore::ContentStore(std::filesystem::path root) : root_(std::move(root)) {
    std::filesystem::create_directories(root_ / "thumbnails");
}

std::filesystem::path ContentStore::default_root() {
    if (const char* x = std::getenv("XONE_HOME"); x && *x) return x;
#if defined(_WIN32)
    if (const char* a = std::getenv("APPDATA")) return std::filesystem::path(a) / "XOne-Emu";
#endif
    if (const char* h = std::getenv("HOME")) return std::filesystem::path(h) / ".xone-emu";
    return ".xone-emu";
}

void ContentStore::load() {
    entries_.clear();
    std::ifstream f(library_file());
    if (!f) return;
    std::stringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return;

    JsonReader r(text);
    r.object([&](const std::string& key) {
        if (key != "games") { r.skip(); return; }
        r.array([&] {
            LibraryEntry e;
            r.object([&](const std::string& k) {
                auto num = [&] { return static_cast<std::uint64_t>(std::stoull(r.raw_value())); };
                if      (k == "id")            e.id = r.string();
                else if (k == "title")         e.title = r.string();
                else if (k == "title_id")      e.title_id = r.string();
                else if (k == "publisher")     e.publisher = r.string();
                else if (k == "path")          e.path = r.string();
                else if (k == "format")        e.format = r.string();
                else if (k == "thumbnail")     e.thumbnail = r.string();
                else if (k == "source")        e.source = r.string();
                else if (k == "added_at")      e.added_at = r.string();
                else if (k == "size_bytes")    e.size_bytes = num();
                else if (k == "target_width")  e.target_width = static_cast<std::uint32_t>(num());
                else if (k == "target_height") e.target_height = static_cast<std::uint32_t>(num());
                else if (k == "target_fps")    e.target_fps = static_cast<std::uint32_t>(num());
                else e.extra.emplace_back(k, r.raw_value());
            });
            entries_.push_back(std::move(e));
        });
    });
}

std::string ContentStore::to_json(const LibraryEntry& e) {
    std::string j = std::format(
        "{{\"id\": \"{}\", \"title\": \"{}\", \"title_id\": \"{}\", \"publisher\": \"{}\", \"path\": \"{}\", "
        "\"format\": \"{}\", \"thumbnail\": \"{}\", \"source\": \"{}\", \"added_at\": \"{}\", "
        "\"size_bytes\": {}, \"target_width\": {}, \"target_height\": {}, \"target_fps\": {}",
        esc(e.id), esc(e.title), esc(e.title_id), esc(e.publisher), esc(e.path), esc(e.format),
        esc(e.thumbnail), esc(e.source), esc(e.added_at), e.size_bytes, e.target_width,
        e.target_height, e.target_fps);
    for (const auto& [k, raw] : e.extra) j += std::format(", \"{}\": {}", esc(k), raw);
    return j + "}";
}

void ContentStore::save() const {
    const auto tmp = library_file().string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) throw std::runtime_error("não foi possível gravar " + tmp);
        f << "{\n  \"version\": 1,\n  \"games\": [";
        for (std::size_t i = 0; i < entries_.size(); ++i)
            f << (i ? ",\n    " : "\n    ") << to_json(entries_[i]);
        f << (entries_.empty() ? "]\n}\n" : "\n  ]\n}\n");
    }
    std::filesystem::rename(tmp, library_file());  // gravação atômica
}

LibraryEntry ContentStore::import_image(const std::filesystem::path& image, const std::string& source) {
    const GameMetadata m = IsoParser::parse(image);
    const auto abs = std::filesystem::absolute(image).string();

    LibraryEntry e;
    e.id            = m.title_id.empty() ? path_hash_id(abs) : m.title_id;
    e.title         = m.title;
    e.title_id      = m.title_id;
    e.publisher     = m.publisher;
    e.path          = abs;
    e.format        = format_name(m.format);
    e.source        = source;
    e.added_at      = now_iso8601();
    e.size_bytes    = m.image_size;
    e.target_width  = m.target_width;
    e.target_height = m.target_height;
    e.target_fps    = m.target_fps;

    if (!m.thumbnail.empty()) {
        const auto ext = std::filesystem::path(m.thumbnail_name).extension().string();
        const auto out = root_ / "thumbnails" / (e.id + (ext.empty() ? ".png" : ext));
        std::ofstream o(out, std::ios::binary);
        o.write(reinterpret_cast<const char*>(m.thumbnail.data()), static_cast<std::streamsize>(m.thumbnail.size()));
        e.thumbnail = out.string();
    }

    for (auto& old : entries_) {
        if (old.id == e.id) {
            e.extra = old.extra;  // preserva dados da UI
            old = e;
            return e;
        }
    }
    entries_.push_back(e);
    return e;
}

bool ContentStore::remove(const std::string& id) {
    const auto n = std::erase_if(entries_, [&](const LibraryEntry& e) { return e.id == id; });
    return n > 0;
}

std::optional<LibraryEntry> ContentStore::find(const std::string& id) const {
    for (const auto& e : entries_) if (e.id == id) return e;
    return std::nullopt;
}

}  // namespace xone::fs
