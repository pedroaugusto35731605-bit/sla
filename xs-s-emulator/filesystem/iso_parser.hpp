// =============================================================================
//  SeriesS-Emu — iso_parser.hpp
//  Leitura de imagens de jogo (Gen9):
//    • XDVDFS / GDF ("XISO") — árvore binária de entradas, setores de 2048 bytes;
//      usado pelas imagens de teste/homebrew do SeriesS-Emu.
//    • XVD / XVC ("msft-xvd") — contêiner de pacote do Xbox Series/One. O
//      conteúdo é criptografado com chaves do console; só o cabeçalho é lido.
//  Arquivos internos comprimidos com XLZ (core/codec.hpp) são detectados e
//  marcados para carga via DirectStorage (descompressão "em hardware").
//  Metadados: MicrosoftGame.config (GDK) / AppxManifest.xml / default.xbe.
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace xss::fs {

enum class ImageFormat { Unknown, Xdvdfs, Xvd };
const char* format_name(ImageFormat f);

struct FileEntry {
    std::string   path;       // caminho completo com '/'
    std::uint64_t offset = 0; // offset absoluto em bytes na imagem
    std::uint64_t size   = 0;
    bool          is_dir = false;
    bool          compressed = false;   // stream XLZ ou textura com mips comprimidos
    bool          texture = false;      // .xtex (mips XLZ individuais; carregado pelo TextureStreamer)
    std::uint64_t raw_size = 0;         // tamanho descomprimido (== size se não comprimido)
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
    std::uint32_t target_width  = 2560;  // perfil Series S: 1440p nativo
    std::uint32_t target_height = 1440;
    std::uint32_t target_fps    = 60;
    std::vector<std::string> tags;       // ex.: "4TFLOPS", "Zen 2", "1440p"
    std::uint64_t compressed_files = 0, disk_bytes = 0, raw_bytes = 0;
    std::vector<FileEntry> files;
    std::vector<std::string> warnings;
};

// Leitor direto do arquivo (metadados). A carga de assets passa pelo SsdDevice.
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

}  // namespace xss::fs
