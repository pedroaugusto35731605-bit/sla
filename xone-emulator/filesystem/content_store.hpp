// =============================================================================
//  XOne-Emu — content_store.hpp
//  Biblioteca de jogos persistida em JSON — o MESMO arquivo lido/escrito pela UI
//  em Python, para que importações feitas pela CLI apareçam na interface e
//  vice-versa.
//
//  Local: $XONE_HOME/library.json  (padrão: ~/.xone-emu/library.json)
//  Capas extraídas: $XONE_HOME/thumbnails/<id>.png
// =============================================================================
#pragma once

#include "iso_parser.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace xone::fs {

struct LibraryEntry {
    std::string   id;              // title_id ou hash do caminho
    std::string   title;
    std::string   title_id;
    std::string   publisher;
    std::string   path;            // imagem no disco
    std::string   format;
    std::string   thumbnail;       // caminho do PNG extraído (pode ser vazio)
    std::string   source;          // "import" | "download"
    std::string   added_at;        // ISO-8601 UTC
    std::uint64_t size_bytes   = 0;
    std::uint32_t target_width  = 1920;
    std::uint32_t target_height = 1080;
    std::uint32_t target_fps    = 60;
    // Chaves que o núcleo não conhece (ex.: adicionadas pela UI), preservadas como JSON cru.
    std::vector<std::pair<std::string, std::string>> extra;
};

class ContentStore {
public:
    explicit ContentStore(std::filesystem::path root = default_root());
    static std::filesystem::path default_root();

    const std::filesystem::path& root() const noexcept { return root_; }
    std::filesystem::path library_file() const { return root_ / "library.json"; }

    void load();
    void save() const;

    // Analisa a imagem com IsoParser, extrai a capa e adiciona/atualiza a entrada.
    LibraryEntry import_image(const std::filesystem::path& image, const std::string& source = "import");
    bool remove(const std::string& id);

    const std::vector<LibraryEntry>& entries() const noexcept { return entries_; }
    std::optional<LibraryEntry> find(const std::string& id) const;

    static std::string to_json(const LibraryEntry& e);

private:
    std::filesystem::path root_;
    std::vector<LibraryEntry> entries_;
};

}  // namespace xone::fs
