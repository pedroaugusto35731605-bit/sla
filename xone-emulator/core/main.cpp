// =============================================================================
//  XOne-Emu — main.cpp (CLI do núcleo; também é o backend chamado pela UI)
//
//  xone-emu boot   [--jit] [--quiet] [--max N] [--image arquivo.xbin] [--vk-validation]
//  xone-emu info   <imagem> [--files]        → metadados em JSON (usado pela UI)
//  xone-emu import <imagem> [--source S]     → adiciona à biblioteca compartilhada
//  xone-emu library                          → lista a biblioteca em JSON
//  xone-emu extract <imagem> <caminho-interno> <saída>
//  xone-emu spec                             → especificações de hardware emuladas
// =============================================================================
#include "bios.hpp"
#include "cpu.hpp"
#include "gpu.hpp"
#include "hardware_spec.hpp"
#include "jit_engine.hpp"
#include "memory.hpp"
#include "../filesystem/content_store.hpp"
#include "../filesystem/iso_parser.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

using namespace xone;

namespace {

void usage() {
    std::puts(
        "XOne-Emu 0.1 — emulador experimental de Xbox One\n"
        "uso:\n"
        "  xone-emu boot [--jit] [--quiet] [--max N] [--image arquivo.xbin] [--vk-validation]\n"
        "  xone-emu info <imagem> [--files]\n"
        "  xone-emu import <imagem> [--source import|download]\n"
        "  xone-emu library\n"
        "  xone-emu extract <imagem> <caminho-interno> <saida>\n"
        "  xone-emu spec");
}

bool has_flag(const std::vector<std::string_view>& a, std::string_view f) {
    for (auto x : a) if (x == f) return true;
    return false;
}
std::string_view flag_value(const std::vector<std::string_view>& a, std::string_view f, std::string_view def = {}) {
    for (std::size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == f) return a[i + 1];
    return def;
}

int cmd_spec() {
    using namespace spec;
    std::printf("CPU     : %s, %u núcleos/%u threads @ %.2f GHz (L2 %llu MiB x %u módulos)\n",
                std::string(kCpu.name).c_str(), kCpu.cores, kCpu.threads, kCpu.clock_hz / 1e9,
                static_cast<unsigned long long>(kCpu.l2_per_module / MiB), kCpu.modules);
    std::printf("GPU     : %s, %u CUs, %.2f TFLOPS (perfil alvo)\n", std::string(kGpu.name).c_str(),
                kGpu.compute_units, kGpu.tflops_fp32);
    std::printf("RAM     : %llu GiB %s + %llu MiB ESRAM, %.1f GB/s\n",
                static_cast<unsigned long long>(kMemory.system_ram / GiB), std::string(kMemory.type).c_str(),
                static_cast<unsigned long long>(kMemory.esram / MiB), kMemory.bandwidth_gbs);
    std::printf("Storage : %s, latência %llu µs\n", std::string(kStorage.name).c_str(),
                static_cast<unsigned long long>(kStorage.read_latency_ns / 1000));
    std::printf("Ref.    : Xbox One de varejo = Jaguar @ %.2f GHz, %u CUs, %.2f TFLOPS\n",
                kRetail.cpu_clock_hz / 1e9, kRetail.gpu_cus, kRetail.tflops_fp32);
    return 0;
}

int cmd_boot(const std::vector<std::string_view>& args) {
    MemoryManager mem;
    KernelHle     kernel(mem);
    CpuCluster    cpu(mem, kernel);
    GpuDevice     gpu(mem);
    Bios          bios(mem, cpu, gpu);

    kernel.set_gpu_submit([&](GuestVA va, std::uint32_t dwords) { gpu.submit_command_buffer(va, dwords); });

    if (!bios.power_on_self_test()) return 2;
    bios.build_memory_map();
    bios.initialize_devices(has_flag(args, "--vk-validation"));

    const auto image_path = flag_value(args, "--image");
    bios.boot(image_path.empty() ? Bios::builtin_test_rom()
                                 : Bios::load_flat_binary(std::string(image_path)));

    CpuCluster::RunOptions opt;
    opt.mode  = has_flag(args, "--jit") ? CpuCluster::Mode::Jit : CpuCluster::Mode::Interpreter;
    opt.trace = !has_flag(args, "--quiet");
    if (auto m = flag_value(args, "--max"); !m.empty()) opt.max_instructions = std::stoull(std::string(m));

    std::printf("[XONE-CORE] main_loop iniciado (%s)\n", opt.mode == CpuCluster::Mode::Jit ? "JIT" : "interpretador");
    const auto rep = cpu.main_loop(opt);

    const auto ms = mem.stats();
    const auto gs = gpu.stats();
    std::printf("\n========== XOne-Emu: sessão encerrada ==========\n");
    std::printf("Instruções       : %llu  (%llu ciclos estimados = %.3f µs emulados, %.3f ms no host)\n",
                static_cast<unsigned long long>(rep.instructions), static_cast<unsigned long long>(rep.cycles),
                rep.emulated_seconds * 1e6, rep.host_seconds * 1e3);
    std::printf("Memória          : %llu páginas mapeadas, %llu page tables, TLB %llu hits / %llu misses\n",
                static_cast<unsigned long long>(ms.mapped_pages), static_cast<unsigned long long>(ms.page_table_pages),
                static_cast<unsigned long long>(ms.tlb_hits), static_cast<unsigned long long>(ms.tlb_misses));
    std::printf("GPU (%s)  : %llu pacotes PM4, %llu draws, %llu vértices, %llu frames\n", gpu.backend_name().c_str(),
                static_cast<unsigned long long>(gs.packets), static_cast<unsigned long long>(gs.draws),
                static_cast<unsigned long long>(gs.vertices), static_cast<unsigned long long>(gs.frames));
    if (opt.mode == CpuCluster::Mode::Jit) {
        const auto js = cpu.jit().stats();
        std::printf("JIT              : %llu blocos traduzidos, %llu cache hits\n",
                    static_cast<unsigned long long>(js.blocks_translated), static_cast<unsigned long long>(js.cache_hits));
    }
    if (rep.budget_exhausted) std::printf("Aviso            : limite de instruções atingido\n");
    if (!rep.fault.empty()) {
        std::printf("FALHA            : %s\n", rep.fault.c_str());
        return 3;
    }
    std::printf("Código de saída  : %lld\n", static_cast<long long>(rep.exit_code));
    return static_cast<int>(rep.exit_code);
}

int cmd_info(const std::vector<std::string_view>& args) {
    if (args.empty()) { usage(); return 1; }
    const auto m = fs::IsoParser::parse(std::string(args[0]));
    std::puts(fs::IsoParser::to_json(m, has_flag(args, "--files")).c_str());
    return m.format == fs::ImageFormat::Unknown ? 4 : 0;
}

int cmd_import(const std::vector<std::string_view>& args) {
    if (args.empty()) { usage(); return 1; }
    fs::ContentStore store;
    store.load();
    const auto e = store.import_image(std::string(args[0]), std::string(flag_value(args, "--source", "import")));
    store.save();
    std::puts(fs::ContentStore::to_json(e).c_str());
    return 0;
}

int cmd_library() {
    fs::ContentStore store;
    store.load();
    std::printf("[");
    for (std::size_t i = 0; i < store.entries().size(); ++i)
        std::printf("%s%s", i ? ",\n " : "", fs::ContentStore::to_json(store.entries()[i]).c_str());
    std::printf("]\n");
    return 0;
}

int cmd_extract(const std::vector<std::string_view>& args) {
    if (args.size() < 3) { usage(); return 1; }
    const auto m = fs::IsoParser::parse(std::string(args[0]));
    for (const auto& f : m.files) {
        if (f.path == args[1]) {
            if (fs::IsoParser::extract(std::string(args[0]), f, std::string(args[2]))) {
                std::printf("extraído: %s (%llu bytes)\n", f.path.c_str(), static_cast<unsigned long long>(f.size));
                return 0;
            }
            std::fprintf(stderr, "falha ao extrair %s\n", f.path.c_str());
            return 5;
        }
    }
    std::fprintf(stderr, "arquivo interno não encontrado: %.*s\n", static_cast<int>(args[1].size()), args[1].data());
    return 5;
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
    const std::vector<std::string_view> args(argv + 2, argv + argc);
    try {
        if (cmd == "boot")    return cmd_boot(args);
        if (cmd == "info")    return cmd_info(args);
        if (cmd == "import")  return cmd_import(args);
        if (cmd == "library") return cmd_library();
        if (cmd == "extract") return cmd_extract(args);
        if (cmd == "spec")    return cmd_spec();
        usage();
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[XONE] erro fatal: %s\n", e.what());
        return 10;
    }
}
