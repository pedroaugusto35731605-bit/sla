// =============================================================================
//  SeriesS-Emu — ssd_io.hpp
//  Simulador do SSD NVMe + bloco de descompressão em hardware ("Velocity
//  Architecture" / DirectStorage).
//
//  Cada requisição passa por um pipeline de 3 estágios com relógio simulado:
//
//     emissão ──► comando NVMe (latência ~30 µs, até QD 64 sobrepostos)
//             ──► transferência no barramento (2,4 GB/s compartilhados)
//             ──► descompressão em hardware (até 8 GB/s de saída, pipelined)
//
//  Os bytes são lidos de verdade do arquivo de imagem e descomprimidos de
//  verdade (codec XLZ) na memória de destino; o relógio simulado diz quanto
//  tempo aquilo levaria no console. O mesmo lote pode ser re-simulado com os
//  perfis HDD / SSD SATA para comparar (sem HW de descompressão, a CPU
//  descomprime: usamos o tempo real medido no host, em 1 thread).
// =============================================================================
#pragma once

#include "hardware_spec.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace xss {

class MemoryManager;
using AllocationId = std::uint64_t;

enum class IoCompression : std::uint8_t { None, Xlz };

struct IoResult {
    bool          ok = false;
    std::string   error;
    std::uint64_t bytes_read = 0;        // lidos do "SSD"
    std::uint64_t bytes_out  = 0;        // entregues ao destino (descomprimidos)
    double        issue_us = 0, transfer_done_us = 0, complete_us = 0;  // relógio simulado
    double        host_read_us = 0, host_decompress_us = 0;              // tempo real no host
};

struct IoRequest {
    int           file = -1;                     // handle de SsdDevice::open
    std::uint64_t offset = 0;
    std::uint64_t size = 0;                      // bytes no disco
    IoCompression compression = IoCompression::None;
    std::uint64_t uncompressed_size = 0;         // obrigatório se comprimido
    std::uint8_t* dst = nullptr;                 // destino contíguo (host ou RAM do convidado)
    std::uint64_t dst_capacity = 0;
    std::vector<std::span<std::uint8_t>> scatter; // destino espalhado (alocação scatter-gather)
    std::string   name;
    std::function<void(const IoResult&)> on_complete;

    // Destino na RAM unificada do convidado (como o DirectStorage escreve direto na
    // memória da GPU). A alocação fica travada (lock) até a requisição concluir.
    IoRequest& to_guest(MemoryManager& mem, AllocationId id);
    MemoryManager* guest_mem = nullptr;
    AllocationId   guest_alloc = 0;
};

struct BatchReport {
    std::string   profile;
    std::size_t   requests = 0, failed = 0;
    std::uint64_t bytes_read = 0, bytes_out = 0;
    double        sim_us = 0;                    // do primeiro issue à última conclusão
    double        first_data_us = 0;            // tempo até a 1ª requisição concluir
    double        host_wall_us = 0, host_decompress_us = 0;
    double raw_gbs() const { return sim_us > 0 ? static_cast<double>(bytes_read) / (sim_us * 1e3) : 0; }
    double effective_gbs() const { return sim_us > 0 ? static_cast<double>(bytes_out) / (sim_us * 1e3) : 0; }
    double ratio() const { return bytes_read ? static_cast<double>(bytes_out) / static_cast<double>(bytes_read) : 1.0; }
    std::string summary() const;
};

class SsdDevice {
public:
    explicit SsdDevice(spec::StorageProfile profile = spec::kSeriesS);
    ~SsdDevice();

    // realtime = true: a submissão espera o tempo de parede igualar o simulado
    // (útil para a UI ver o ritmo real de um SSD); false = o mais rápido possível.
    void set_realtime(bool on) noexcept { realtime_ = on; }
    const spec::StorageProfile& profile() const noexcept { return profile_; }

    int  open(const std::filesystem::path& p);
    void close(int handle);
    std::uint64_t file_size(int handle) const;

    // Leitura síncrona simples (passa pelo mesmo modelo de tempo).
    IoResult read(int file, std::uint64_t offset, void* dst, std::uint64_t size);

    // Processa um lote como uma fila DirectStorage: enfileira tudo, emite em QD
    // e conclui na ordem do pipeline. Chama on_complete de cada requisição.
    BatchReport submit(std::vector<IoRequest>& batch);

    // Re-simula as conclusões de um lote já executado com outro perfil (só tempo).
    static BatchReport simulate(const spec::StorageProfile& p, const std::vector<IoResult>& executed,
                                const std::vector<IoRequest>& reqs);

    double clock_us() const noexcept { return clock_us_; }

    struct Stats {
        std::uint64_t commands = 0, bytes_read = 0, bytes_out = 0, decompressed_requests = 0, errors = 0;
        double busy_us = 0;
    };
    Stats stats() const;

private:
    struct OpenFile { std::FILE* f = nullptr; std::uint64_t size = 0; std::string path; std::mutex mu; };
    bool host_read(int file, std::uint64_t offset, void* dst, std::uint64_t size);

    spec::StorageProfile profile_;
    bool realtime_ = false;
    std::vector<std::unique_ptr<OpenFile>> files_;
    mutable std::mutex mu_;
    double clock_us_ = 0;          // relógio simulado do dispositivo
    Stats  stats_{};
};

}  // namespace xss
