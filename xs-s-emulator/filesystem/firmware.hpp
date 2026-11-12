// =============================================================================
//  SeriesS-Emu — firmware.hpp
//  Instalação do firmware oficial do console a partir da Atualização Offline do
//  Sistema (OSU1/OSU2/OSU3), que a Microsoft distribui no site de suporte do Xbox.
//
//  O pacote extraído contém a pasta "$SystemUpdate" com contêineres XVD do
//  sistema operacional (tipos SystemOS, HostOS, SystemTools, SystemAux...).
//
//  O que este módulo faz: localiza a pasta, lê o cabeçalho de cada XVD, valida
//  que é uma atualização de SISTEMA (e não um jogo), descobre a versão e copia
//  tudo para $XSS_HOME/firmware/<versão>/, marcando-a como a versão atual.
//
//  O que ele NÃO faz: os XVDs do sistema são criptografados e assinados com
//  chaves que só existem no hardware do console. O SeriesS-Emu não as possui
//  nem tenta contorná-las; o firmware instalado é inventariado e registrado no
//  boot, mas a execução continua pelo kernel em HLE (bios_uefi.cpp).
// =============================================================================
#pragma once

#include "iso_parser.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace xss::fs {

struct FirmwarePackage {
    std::string   file;            // caminho relativo dentro de $SystemUpdate
    std::uint64_t size = 0;
    std::optional<XvdHeader> xvd;  // vazio se não for XVD (ex.: arquivos de controle)
};

struct FirmwareInfo {
    bool          valid = false;
    std::string   error;
    std::filesystem::path root;    // pasta que contém os pacotes ($SystemUpdate)
    std::string   version;         // lida do cabeçalho do pacote SystemOS (ou HostOS)
    std::vector<FirmwarePackage> packages;
    std::uint64_t total_bytes = 0;
    std::uint32_t system_packages = 0;
    std::vector<std::string> warnings;
};

using CopyProgress = std::function<void(std::uint64_t done, std::uint64_t total, const std::string& file)>;

std::filesystem::path default_firmware_root();     // $XSS_HOME/firmware

// Inspeciona sem alterar nada. Aceita a pasta "$SystemUpdate" ou uma pasta que a contenha.
FirmwareInfo inspect_system_update(const std::filesystem::path& source);

// Valida e instala em <firmware_root>/<versão>/. move=true renomeia em vez de copiar
// (usado quando a UI já extraiu o zip dentro de firmware_root).
FirmwareInfo install_system_update(const std::filesystem::path& source, const std::filesystem::path& firmware_root,
                                   bool move = false, const CopyProgress& progress = {});

// Firmware marcado como atual em firmware_root (arquivo "current").
std::optional<FirmwareInfo> installed_firmware(const std::filesystem::path& firmware_root);

std::string to_json(const FirmwareInfo& f);

}  // namespace xss::fs
