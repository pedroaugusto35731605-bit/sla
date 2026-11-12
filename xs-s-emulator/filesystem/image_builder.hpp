// =============================================================================
//  SeriesS-Emu — image_builder.hpp
//  Gera uma imagem XDVDFS de demonstração (sem conteúdo protegido):
//    MicrosoftGame.config, Assets/StoreLogo.png, game.xbin (homebrew),
//    data/level0.pak (stream XLZ) e textures/*.xtex (mips comprimidos).
//  Serve para testar o parser, o SSD/DirectStorage e o streaming de texturas.
// =============================================================================
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xss::fs {

struct DemoImageOptions {
    std::string   title     = "Series S Tech Demo";
    std::string   title_id  = "5E5E0001";
    std::string   publisher = "SeriesS-Emu Project";
    std::uint32_t textures  = 24;       // texturas RGBA8
    std::uint32_t texture_size = 2048;  // lado do mip 0
    std::uint64_t level_bytes = 64ull * 1024 * 1024;  // dados de nível (geometria/áudio sintéticos)
    std::vector<std::uint8_t> executable;  // opcional: vira game.xbin
};

struct DemoImageResult {
    std::uint64_t image_bytes = 0, raw_asset_bytes = 0, disk_asset_bytes = 0;
    double        build_seconds = 0;
};

DemoImageResult build_demo_image(const std::filesystem::path& out, const DemoImageOptions& opt);

}  // namespace xss::fs
