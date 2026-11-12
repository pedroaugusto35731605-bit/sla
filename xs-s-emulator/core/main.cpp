// =============================================================================
//  SeriesS-Emu — main.cpp (CLI do núcleo; também é o backend chamado pela UI)
// =============================================================================
#include "bios_uefi.hpp"
#include "codec.hpp"
#include "cpu.hpp"
#include "gpu.hpp"
#include "hardware_spec.hpp"
#include "jit_engine.hpp"
#include "log.hpp"
#include "memory_manager.hpp"
#include "ssd_io.hpp"
#include "../filesystem/firmware.hpp"
#include "../filesystem/image_builder.hpp"
#include "../filesystem/iso_parser.hpp"
#include "../filesystem/texture_streamer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace xss;

namespace {

void usage() {
    std::puts(
        "SeriesS-Emu 0.1 — emulador experimental de Xbox Series S\n"
        "uso:\n"
        "  xss-emu spec                                   especificações emuladas\n"
        "  xss-emu boot [--jit] [--quiet] [--max N] [--image app.xbin] [--firmware pasta] [--vk-validation]\n"
        "  xss-emu mkimage <saida.iso> [--textures N] [--title NOME] [--title-id ID]\n"
        "  xss-emu info <imagem> [--files] [--thumbnail capa.png]  metadados + tags em JSON\n"
        "  xss-emu bench <imagem>                         carga DirectStorage e comparação HDD/SATA/NVMe\n"
        "  xss-emu stream <imagem> [--frames N] [--heap GiB]  streaming de texturas sob o limite de 7 GB\n"
        "  xss-emu memtest [--chunk MiB]                  estoura o orçamento de 7 GB de propósito\n"
        "  xss-emu fwinfo [pasta]                         inspeciona um OSU1 extraído (sem argumento: o instalado)\n"
        "  xss-emu fwinstall <pasta> [--move]             instala o firmware do OSU1 ($SystemUpdate)\n"
        "\nvariável SERIESS_LOG=quiet|debug ajusta o log");
}

using Args = std::vector<std::string_view>;
bool has(const Args& a, std::string_view f) { return std::find(a.begin(), a.end(), f) != a.end(); }
std::string_view opt(const Args& a, std::string_view f, std::string_view def = {}) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == f) return a[i + 1];
    return def;
}
std::uint64_t opt_u64(const Args& a, std::string_view f, std::uint64_t def) {
    const auto v = opt(a, f);
    return v.empty() ? def : std::stoull(std::string(v));
}

// ----------------------------------------------------------------------------
int cmd_spec() {
    using namespace spec;
    std::printf("CPU      : %s — %u núcleos / %u threads @ %.1f GHz (%.1f GHz sem SMT)\n",
                std::string(kCpu.name).c_str(), kCpu.cores, kCpu.threads, kCpu.clock_hz / 1e9, kCpu.clock_no_smt_hz / 1e9);
    std::printf("GPU      : %s — %u CUs @ %.3f GHz = %.2f TFLOPS (valor pedido de clock: %.3f GHz)\n",
                std::string(kGpu.name).c_str(), kGpu.compute_units, kGpu.clock_hz / 1e9, kComputedTflops,
                kGpu.requested_clock_hz / 1e9);
    std::printf("RAM      : %s %s — User Space %s (limite rígido) + System Reserved %s\n",
                human_bytes(kMemory.total).c_str(), std::string(kMemory.type).c_str(),
                human_bytes(kMemory.user_space).c_str(), human_bytes(kMemory.system_reserved).c_str());
    std::printf("SSD      : %s — %llu GB, %.1f GB/s bruto, até %.1f GB/s descomprimido, %llu µs/comando\n",
                std::string(kSsd.name).c_str(), static_cast<unsigned long long>(kSsd.capacity / GB),
                kSsd.raw_bytes_s / 1e9, kSsd.decompressed_max_bytes_s / 1e9,
                static_cast<unsigned long long>(kSsd.command_latency_ns / 1000));
    std::printf("Vídeo    : %ux%u nativo → %ux%u (upscaling) @ %u FPS\n", kDisplay.render_width, kDisplay.render_height,
                kDisplay.output_width, kDisplay.output_height, kDisplay.target_fps);
    return 0;
}

// ----------------------------------------------------------------------------
int cmd_boot(const Args& a) {
    MemoryManager mem;
    GpuDevice     gpu(mem);
    XboxOsKernel  kernel(mem, gpu);
    CpuCluster    cpu(mem, kernel);
    SsdDevice     ssd;
    UefiFirmware  fw(mem, cpu, gpu, ssd);

    // Firmware oficial (OSU1): --firmware <pasta> ou o instalado em $XSS_HOME/firmware
    std::optional<fs::FirmwareInfo> sysfw;
    if (const auto p = opt(a, "--firmware"); !p.empty()) {
        auto f = fs::inspect_system_update(std::string(p));
        if (!f.valid) log_warn("UEFI", "firmware ignorado: {}", f.error);
        else sysfw = std::move(f);
    } else {
        sysfw = fs::installed_firmware(fs::default_firmware_root());
    }
    if (sysfw) {
        SystemSoftware s{sysfw->version, sysfw->root.string(), {}, sysfw->total_bytes};
        for (const auto& p : sysfw->packages) if (p.xvd) s.packages.push_back(p.xvd->content_type);
        fw.set_system_software(std::move(s));
    }

    if (!fw.power_on_self_test()) return 2;
    fw.initialize_devices(has(a, "--vk-validation"));
    const auto image = opt(a, "--image");
    fw.boot(image.empty() ? UefiFirmware::diagnostic_rom() : UefiFirmware::load_flat_binary(std::string(image)));

    CpuCluster::RunOptions ro;
    ro.mode = has(a, "--jit") ? CpuCluster::Mode::Jit : CpuCluster::Mode::Interpreter;
    ro.trace = !has(a, "--quiet");
    ro.max_instructions = opt_u64(a, "--max", 10'000'000);
    log_info("CPU", "main_loop iniciado ({})", ro.mode == CpuCluster::Mode::Jit ? "JIT" : "interpretador");
    const auto rep = cpu.main_loop(ro);

    const auto gs = gpu.stats();
    std::printf("\n=========== SeriesS-Emu: sessão encerrada ===========\n");
    std::printf("CPU   : %llu instruções, %llu ciclos (%.3f µs emulados @ %.1f GHz), %.2f ms no host\n",
                static_cast<unsigned long long>(rep.instructions), static_cast<unsigned long long>(rep.cycles),
                rep.emulated_seconds * 1e6, spec::kCpu.clock_hz / 1e9, rep.host_seconds * 1e3);
    std::printf("GPU   : %s | %llu pacotes PM4, %llu draws, %llu dispatches, %llu frames\n", gpu.backend_name().c_str(),
                static_cast<unsigned long long>(gs.packets), static_cast<unsigned long long>(gs.draws),
                static_cast<unsigned long long>(gs.dispatches), static_cast<unsigned long long>(gs.frames));
    std::printf("Kernel: %llu alocações negadas por limite de memória\n",
                static_cast<unsigned long long>(kernel.failed_allocations()));
    std::printf("%s", mem.report().c_str());
    if (rep.budget_exhausted) std::printf("Aviso : limite de instruções atingido\n");
    if (!rep.fault.empty()) { std::printf("FALHA : %s\n", rep.fault.c_str()); return 3; }
    std::printf("Saída : %lld\n", static_cast<long long>(rep.exit_code));
    return static_cast<int>(rep.exit_code);
}

// ----------------------------------------------------------------------------
int cmd_mkimage(const Args& a) {
    if (a.empty()) { usage(); return 1; }
    fs::DemoImageOptions o;
    o.textures = static_cast<std::uint32_t>(opt_u64(a, "--textures", o.textures));
    if (auto t = opt(a, "--title"); !t.empty()) o.title = t;
    if (auto t = opt(a, "--title-id"); !t.empty()) o.title_id = t;
    o.executable = UefiFirmware::diagnostic_rom().bytes;
    log_info("BUILD", "gerando \"{}\" com {} texturas {}² + nível de {}...", o.title, o.textures, o.texture_size,
             human_bytes(o.level_bytes));
    const auto r = fs::build_demo_image(std::string(a[0]), o);
    log_info("BUILD", "{} criado: {} no disco | assets {} → {} comprimidos ({:.2f}×) em {:.1f} s",
             std::string(a[0]), human_bytes(r.image_bytes), human_bytes(r.raw_asset_bytes),
             human_bytes(r.disk_asset_bytes),
             static_cast<double>(r.raw_asset_bytes) / static_cast<double>(r.disk_asset_bytes), r.build_seconds);
    return 0;
}

int cmd_info(const Args& a) {
    if (a.empty()) { usage(); return 1; }
    const auto m = fs::IsoParser::parse(std::string(a[0]));
    if (const auto thumb = opt(a, "--thumbnail"); !thumb.empty() && !m.thumbnail.empty()) {
        std::ofstream o(std::string(thumb), std::ios::binary);    // capa extraída da imagem (para a UI)
        o.write(reinterpret_cast<const char*>(m.thumbnail.data()), static_cast<std::streamsize>(m.thumbnail.size()));
    }
    std::puts(fs::IsoParser::to_json(m, has(a, "--files")).c_str());
    return m.format == fs::ImageFormat::Unknown ? 4 : 0;
}

// ----------------------------------------------------------------------------
//  bench: carrega todos os assets da imagem para a RAM do convidado num lote
// ----------------------------------------------------------------------------
int cmd_bench(const Args& a) {
    if (a.empty()) { usage(); return 1; }
    const std::string path(a[0]);
    const auto meta = fs::IsoParser::parse(path);
    if (meta.format != fs::ImageFormat::Xdvdfs) { log_error("BENCH", "imagem XDVDFS necessária"); return 4; }

    MemoryManager mem;
    SsdDevice ssd;
    const int file = ssd.open(path);

    // Monta as requisições: arquivos XLZ inteiros + cada mip das texturas.
    std::vector<IoRequest> batch;
    std::vector<AllocationId> allocs;
    auto add = [&](const std::string& name, std::uint64_t off, std::uint64_t disk, std::uint64_t raw, bool comp) {
        const AllocationId id = mem.allocate(Partition::User, raw, "bench: " + name, alloc_flags::kCompressible);
        allocs.push_back(id);
        IoRequest q;
        q.file = file; q.offset = off; q.size = disk; q.name = name;
        q.compression = comp ? IoCompression::Xlz : IoCompression::None;
        q.uncompressed_size = raw;
        q.to_guest(mem, id);
        batch.push_back(std::move(q));
    };
    for (const auto& f : meta.files)
        if (!f.is_dir && f.compressed && !f.texture) add(f.path, f.offset, f.size, f.raw_size, true);
    fs::TextureStreamer probe(mem, ssd);
    probe.register_image(path, meta);
    for (std::size_t t = 0; t < probe.texture_count(); ++t)
        for (std::size_t m = 0; m < probe.texture(t).mips.size(); ++m) {
            const auto& lv = probe.texture(t).mips[m];
            add(std::format("{} mip{}", probe.texture(t).name, m), lv.offset, lv.disk_size, lv.raw_size, lv.compressed);
        }

    log_info("BENCH", "{} requisições DirectStorage, {} no disco → {} na RAM", batch.size(),
             human_bytes(meta.disk_bytes), human_bytes(meta.raw_bytes));
    std::vector<IoResult> results(batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i) batch[i].on_complete = [&results, i](const IoResult& r) { results[i] = r; };
    const BatchReport rep = ssd.submit(batch);

    std::printf("\nCarga completa da imagem (mesmo lote, perfis diferentes):\n");
    std::printf("  %s\n", rep.summary().c_str());
    for (const auto* p : {&spec::kSataSsd, &spec::kHdd})
        std::printf("  %s\n", SsdDevice::simulate(*p, results, batch).summary().c_str());
    std::printf("\nHost (trabalho real): leitura+descompressão em %.1f ms; descompressão XLZ a %.2f GB/s por núcleo\n",
                rep.host_wall_us / 1e3,
                rep.host_decompress_us > 0 ? static_cast<double>(rep.bytes_out) / (rep.host_decompress_us * 1e3) : 0.0);
    std::printf("%s", mem.report().c_str());
    for (auto id : allocs) mem.free(id);
    return rep.failed ? 5 : 0;
}

// ----------------------------------------------------------------------------
//  stream: um "jogo" disputa os 7 GB com o streamer de texturas, em fases:
//    1–16  exploração          — texturas chegam em resolução total (pressão Normal)
//    17    zona nova           — o jogo aloca +1,5 GiB de geometria: reclaim despeja
//                                texturas frias, dados frios são comprimidos, bias sobe
//    30–33 arena               — todas as texturas pedidas ao mesmo tempo
//    40    pico                — o jogo tenta mais 512 MiB: limite do Series S
// ----------------------------------------------------------------------------
int cmd_stream(const Args& a) {
    if (a.empty()) { usage(); return 1; }
    const std::string path(a[0]);
    const auto meta = fs::IsoParser::parse(path);
    const std::uint64_t frames = opt_u64(a, "--frames", 48);
    const double heap_gib = std::stod(std::string(opt(a, "--heap", "5.2")));

    MemoryManager mem;
    SsdDevice ssd;
    fs::TextureStreamer streamer(mem, ssd, {.pool_budget = 2 * spec::GiB, .max_loads_per_frame = 96});
    if (streamer.register_image(path, meta) == 0) { log_error("STREAM", "a imagem não tem texturas .xtex"); return 4; }

    // Working set do jogo: heap fixo (não tocado → não gasta RAM do host) + dados frios compressíveis.
    const auto heap = mem.allocate(Partition::User, static_cast<std::uint64_t>(heap_gib * static_cast<double>(spec::GiB)),
                                   "jogo: heap/áudio/scripts", alloc_flags::kPinned);
    const std::uint64_t cold_size = 192 * spec::MiB;
    const auto cold = mem.allocate(Partition::User, cold_size, "jogo: dados frios (cache de IA/save)",
                                   alloc_flags::kCompressible);
    {
        mem.lock(cold);                          // travado enquanto o host escreve
        std::uint64_t i = 0;
        for (auto span : mem.host_spans(cold))
            for (std::uint64_t k = 0; k < span.size(); k += 4096, i += 4096)
                std::memset(span.data() + k, static_cast<int>(i / 4096 % 7), 4096);
        mem.unlock(cold);
    }
    log_info("STREAM", "jogo reservou {}; {} livres para texturas (conjunto completo em mip 0: {})",
             human_bytes(mem.used(Partition::User)), human_bytes(mem.available(Partition::User)),
             human_bytes(streamer.full_set_bytes()));

    auto event = [](const std::string& s) { std::printf("  ── %s\n", s.c_str()); };
    std::printf("\nframe | pedidos hits cargas adiadas despejos | lido → descomprimido   | I/O sim  | pool       | bias | pressão\n");
    const std::size_t n = streamer.texture_count();
    for (std::uint64_t f = 1; f <= frames; ++f) {
        if (f == 17) {
            event("zona nova: o jogo aloca +1,5 GiB de geometria");
            try {
                mem.allocate(Partition::User, 1536 * spec::MiB, "jogo: geometria da zona 2", alloc_flags::kPinned);
                event(std::format("concedido — User Space em {}", human_bytes(mem.used(Partition::User))));
            } catch (const OutOfMemory&) { event("negado"); }
        }
        if (f == 40) {
            event("pico: o jogo tenta alocar mais 512 MiB (efeitos de partículas)");
            try {
                mem.allocate(Partition::User, 512 * spec::MiB, "jogo: partículas", alloc_flags::kPinned);
                event("concedido");
            } catch (const OutOfMemory&) {
                event("negado (STATUS_NO_MEMORY) — o jogo precisa reduzir a qualidade dos efeitos");
            }
        }
        // Câmera: 8 texturas visíveis, as mais próximas em mip 0. Na arena, todas em mip 0.
        const bool arena = f >= 30 && f <= 33;
        for (std::size_t k = 0; k < (arena ? n : 8); ++k) {
            const std::size_t tex = arena ? k : (f + k) % n;
            streamer.request(tex, arena ? 0 : static_cast<std::uint32_t>(k / 3));
        }
        const auto r = streamer.update();
        std::printf("%5llu | %7u %4u %6u %7u %8u | %10s → %-11s | %6.2f ms | %-10s | +%u   | %s%s\n",
                    static_cast<unsigned long long>(r.frame), r.requested, r.hits, r.loads, r.deferred, r.evictions,
                    human_bytes(r.disk_bytes).c_str(), human_bytes(r.raw_bytes).c_str(), r.io_ms,
                    human_bytes(r.pool_used).c_str(), r.mip_bias, to_string(r.pressure), arena ? "  ← arena" : "");
    }
    const auto s = streamer.stats();
    std::printf("\nStreaming: %llu cargas, %llu despejos, %llu adiadas | %s lidos do SSD → %s na RAM em %.1f ms simulados\n",
                static_cast<unsigned long long>(s.loads), static_cast<unsigned long long>(s.evictions),
                static_cast<unsigned long long>(s.deferred), human_bytes(s.disk_bytes).c_str(),
                human_bytes(s.raw_bytes).c_str(), s.io_ms);
    std::printf("%s", mem.report().c_str());
    (void)heap;
    return 0;
}

// ----------------------------------------------------------------------------
int cmd_fwinfo(const Args& a) {
    std::optional<fs::FirmwareInfo> f;
    if (a.empty()) {
        f = fs::installed_firmware(fs::default_firmware_root());
        if (!f) { std::puts("{\"valid\":false,\"error\":\"nenhum firmware instalado\"}"); return 6; }
    } else {
        f = fs::inspect_system_update(std::string(a[0]));
    }
    std::puts(fs::to_json(*f).c_str());
    return f->valid ? 0 : 6;
}

int cmd_fwinstall(const Args& a) {
    if (a.empty()) { usage(); return 1; }
    const auto root = fs::default_firmware_root();
    std::filesystem::create_directories(root);
    const auto f = fs::install_system_update(std::string(a[0]), root, has(a, "--move"),
        [](std::uint64_t done, std::uint64_t total, const std::string& file) {
            if (!file.empty()) log_info("FW", "copiando {} ({} de {})", file, human_bytes(done), human_bytes(total));
        });
    if (!f.valid) { log_error("FW", "instalação recusada: {}", f.error); std::puts(fs::to_json(f).c_str()); return 6; }
    for (const auto& w : f.warnings) log_warn("FW", "{}", w);
    log_info("FW", "firmware {} instalado em {} ({} pacotes do sistema, {})", f.version, f.root.string(),
             f.system_packages, human_bytes(f.total_bytes));
    std::puts(fs::to_json(f).c_str());
    return 0;
}

int cmd_memtest(const Args& a) {
    MemoryManager mem;
    const std::uint64_t chunk = opt_u64(a, "--chunk", 512) * spec::MiB;
    std::uint64_t n = 0;
    try {
        for (;; ++n) mem.allocate(Partition::User, chunk, std::format("memtest #{}", n));
    } catch (const OutOfMemory& e) {
        log_info("MEMTEST", "{} blocos de {} concedidos antes do limite ({})", n, human_bytes(chunk), e.what());
    }
    try {
        mem.allocate(Partition::System, 4 * spec::GiB, "título tentando invadir a área do SO");
    } catch (const OutOfMemory& e) {
        log_info("MEMTEST", "System Reserved protegido: {}", e.what());
    }
    std::printf("%s", mem.report().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // A CRT do Windows não tem buffer de linha (_IOLBF vira _IOFBF) e exige tamanho >= 2:
    // com 0 ela aborta o programa antes de imprimir qualquer coisa (0xC0000417).
    // Sem buffer, a UI recebe cada linha do log na hora.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
    if (argc < 2) { usage(); return 1; }
    const std::string_view cmd = argv[1];
    const Args args(argv + 2, argv + argc);
    try {
        if (cmd == "spec")    return cmd_spec();
        if (cmd == "boot")    return cmd_boot(args);
        if (cmd == "mkimage") return cmd_mkimage(args);
        if (cmd == "info")    return cmd_info(args);
        if (cmd == "bench")   return cmd_bench(args);
        if (cmd == "stream")  return cmd_stream(args);
        if (cmd == "memtest") return cmd_memtest(args);
        if (cmd == "fwinfo")  return cmd_fwinfo(args);
        if (cmd == "fwinstall") return cmd_fwinstall(args);
        usage();
        return 1;
    } catch (const std::exception& e) {
        log_error("XSS", "erro fatal: {}", e.what());
        return 10;
    }
}
