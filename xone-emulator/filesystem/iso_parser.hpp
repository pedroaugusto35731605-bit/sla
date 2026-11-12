// =============================================================================
//  XOne-Emu — iso_parser.hpp
//  Leitura de imagens de jogo:
//    • XDVDFS / GDF ("XISO")  — sistema de arquivos dos discos Xbox/Xbox 360,
//      também usado por imagens homebrew/de teste do XOne-Emu. Árvore binária de
//      entradas de diretório, setores de 2048 bytes. Testamos os offsets de
//      partição conhecidos (XGD1/XGD2/XGD3 e imagem "redump-trimmed").
//    • XVD / XVC              — contêiner de pacote do Xbox One ("msft-xvd").
//      O conteúdo é criptografado com chaves do console; extraímos somente os
//      metadados do cabeçalho (IDs, tipo de conteúdo, tamanho, versão).
//
//  Metadados de título são extraídos de:
//    MicrosoftGame.config / AppxManifest.xml (Xbox One/GDK), default.xbe (Xbox).
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace xone::fs {

enum class ImageFormat { Unknown, Xdvdfs, Xvd };
const char* format_name(ImageFormat f);

struct FileEntry {
    std::string   path;       // caminho completo com '/'
    std::uint64_t offset = 0; // offset absoluto em bytes na imagem
    std::uint64_t size   = 0;
    bool          is_dir = false;
};

struct GameMetadata {
    ImageFormat   format = ImageFormat::Unknown;
    std::string   title;
    std::string   title_id;          // 8 dígitos hex
    std::string   publisher;
    std::string   version;
    std::string   content_type;      // XVD
    std::string   content_id;        // XVD VDUID / GUID
    std::string   product_id;        // XVD
    std::string   metadata_source;   // arquivo de onde os metadados vieram
    std::string   thumbnail_name;    // caminho interno da imagem de capa
    std::vector<std::uint8_t> thumbnail;
    std::uint64_t image_size = 0;
    std::uint64_t partition_offset = 0;
    std::uint32_t target_width  = 1920;  // perfil Xbox One padrão
    std::uint32_t target_height = 1080;
    std::uint32_t target_fps    = 60;
    std::vector<FileEntry> files;
    std::vector<std::string> warnings;
};

// Leitor com latência de NVMe simulada (contabilizada, não dormida).
class NvmeReader {
public:
    explicit NvmeReader(const std::filesystem::path& p);
    bool          ok() const noexcept { return static_cast<bool>(f_); }
    std::uint64_t size() const noexcept { return size_; }
    bool          read(std::uint64_t offset, void* dst, std::uint64_t len);
    std::vector<std::uint8_t> read_vec(std::uint64_t offset, std::uint64_t len);
    std::uint64_t simulated_ns() const noexcept { return simulated_ns_; }
    std::uint64_t commands() const noexcept { return commands_; }
private:
    std::ifstream f_;
    std::uint64_t size_ = 0, simulated_ns_ = 0, commands_ = 0;
};

class IsoParser {
public:
    // Lança std::runtime_error se o arquivo não puder ser aberto.
    static GameMetadata parse(const std::filesystem::path& image);

    // Extrai um arquivo interno (XDVDFS) para o disco.
    static bool extract(const std::filesystem::path& image, const FileEntry& e,
                        const std::filesystem::path& out);

    static std::string to_json(const GameMetadata& m, bool include_files = false);
};

}  // namespace xone::fs
