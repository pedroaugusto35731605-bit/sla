// =============================================================================
//  SeriesS-Emu — testes do núcleo (sem framework externo)
// =============================================================================
#include "bios_uefi.hpp"
#include "codec.hpp"
#include "cpu.hpp"
#include "gpu.hpp"
#include "log.hpp"
#include "memory_manager.hpp"
#include "ssd_io.hpp"
#include "../filesystem/firmware.hpp"
#include "../filesystem/image_builder.hpp"
#include "../filesystem/iso_parser.hpp"
#include "../filesystem/texture_streamer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <functional>
#include <random>
#include <string>

using namespace xss;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                                                   \
    do {                                                                                              \
        if (cond) ++g_passed;                                                                         \
        else { ++g_failed; std::printf("  FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); }          \
    } while (0)

static void test(const char* name, const std::function<void()>& fn) {
    std::printf("[TEST] %s\n", name);
    try { fn(); } catch (const std::exception& e) { ++g_failed; std::printf("  EXCEÇÃO: %s\n", e.what()); }
}

int main() {
    set_log_level(LogLevel::Warn);
    constexpr std::uint64_t GiB = spec::GiB, MiB = spec::MiB;

    test("memória: divisão 7 + 3 GB e limite rígido do User Space", [&] {
        MemoryManager mm;
        CHECK(mm.budget(Partition::User) == 7 * GiB);
        CHECK(mm.budget(Partition::System) == 3 * GiB);
        std::vector<AllocationId> ids;
        bool oom = false;
        try { for (;;) ids.push_back(mm.allocate(Partition::User, 1 * GiB, "bloco")); }
        catch (const OutOfMemory& e) { oom = true; CHECK(e.partition() == Partition::User); }
        CHECK(oom);
        CHECK(ids.size() == 7);
        CHECK(mm.used(Partition::User) == 7 * GiB);
        CHECK(mm.stats().oom_events == 1);
        mm.free(ids.back());
        CHECK(mm.available(Partition::User) == 1 * GiB);
        ids.back() = mm.allocate(Partition::User, 1 * GiB, "de volta");   // cabe de novo
        CHECK(mm.pressure() == PressureLevel::Critical);
    });

    test("memória: page tables x86-64 com páginas de 2 MB e de 4 KB", [&] {
        MemoryManager mm;
        const auto big = mm.allocate(Partition::User, 8 * MiB, "big");
        mm.map_allocation(0x10'0000'0000ull, big, pte::kWritable | pte::kUser);
        const auto small = mm.allocate(Partition::User, 12 * 1024, "small");
        mm.map_allocation(0x20'0000'1000ull, small, pte::kWritable | pte::kUser);
        const auto s = mm.stats();
        CHECK(s.large_mappings == 4);
        CHECK(s.small_mappings == 3);
        const GuestPA pa = mm.info(big)->pa;
        CHECK(mm.translate(0x10'0000'0000ull + 3 * MiB + 0x123, Access::Read) == pa + 3 * MiB + 0x123);
        mm.write<std::uint64_t>(0x10'001F'FFFCull, 0xA1B2C3D4E5F60718ull);   // cruza a fronteira de 2 MB
        CHECK(mm.read<std::uint64_t>(0x10'001F'FFFCull) == 0xA1B2C3D4E5F60718ull);
        bool pf = false;
        try { mm.read<std::uint8_t>(0x10'0000'0000ull, Access::Execute); } catch (const PageFault&) { pf = true; }
        CHECK(!pf);   // sem NX: executável
        mm.unmap_allocation(0x10'0000'0000ull, big);
        try { mm.read<std::uint8_t>(0x10'0000'0000ull); } catch (const PageFault&) { pf = true; }
        CHECK(pf);
        CHECK(mm.stats().page_table_pages >= 4);
        CHECK(mm.used(Partition::System) > 0);    // page tables são cobradas do sistema
    });

    test("memória: compressão forçada de dados frios e descompressão no acesso", [&] {
        MemoryManager mm;
        const auto cold = mm.allocate(Partition::User, 64 * MiB, "frio", alloc_flags::kCompressible);
        std::uint8_t* p = mm.host_pointer(mm.acquire(cold), 64 * MiB);
        for (std::uint64_t i = 0; i < 64 * MiB; ++i) p[i] = static_cast<std::uint8_t>((i / 4096) * 31 + (i % 64));
        const std::uint64_t before = mm.used(Partition::User);
        // Enche o User Space até faltar 32 MiB e pede 64 MiB: precisa comprimir o bloco frio.
        const auto fill = mm.allocate(Partition::User, 7 * GiB - before - 32 * MiB, "jogo", alloc_flags::kPinned);
        const auto extra = mm.allocate(Partition::User, 48 * MiB, "extra");
        CHECK(mm.info(cold)->compressed);
        CHECK(mm.stats().compressions == 1);
        // Libera espaço e acessa o bloco frio: volta íntegro.
        mm.free(extra);
        mm.free(fill);
        std::vector<std::uint8_t> q(64 * MiB);
        mm.read_allocation(cold, 0, q.data(), q.size());     // pode estar espalhada após descomprimir
        CHECK(!mm.info(cold)->compressed);
        bool same = true;
        for (std::uint64_t i = 0; i < 64 * MiB; i += 997)
            if (q[i] != static_cast<std::uint8_t>((i / 4096) * 31 + (i % 64))) { same = false; break; }
        CHECK(same);
        CHECK(mm.stats().decompressions == 1);
    });

    test("memória: acquire/lock nunca devolvem um PA que a pressão comprime em seguida", [&] {
        MemoryManager mm;
        const auto cold = mm.allocate(Partition::User, 128 * MiB, "frio", alloc_flags::kCompressible);
        // Leva o User Space à faixa Critical: a política comprime o bloco frio (ainda zerado).
        const auto fill = mm.allocate(Partition::User, 7 * GiB - 128 * MiB - 64 * MiB, "jogo", alloc_flags::kPinned);
        CHECK(mm.info(cold)->compressed);
        // acquire descomprime e volta a Critical: o bloco recém-adquirido não pode ser recomprimido.
        const GuestPA pa = mm.lock(cold);
        CHECK(!mm.info(cold)->compressed);
        CHECK(mm.info(cold)->pa == pa);
        CHECK(mm.info(cold)->extents.size() > 1);          // espaço fragmentado → scatter-gather
        for (auto s : mm.host_spans(cold)) std::memset(s.data(), 0x5A, s.size());
        mm.compress_cold(1 * GiB);                         // travado: ignorado
        CHECK(!mm.info(cold)->compressed);
        mm.unlock(cold);
        mm.compress_cold(1 * GiB);                         // destravado: comprime
        CHECK(mm.info(cold)->compressed);
        mm.free(fill);
        std::uint8_t first = 0, last = 0;
        mm.read_allocation(cold, 0, &first, 1);
        mm.read_allocation(cold, 128 * MiB - 1, &last, 1);
        CHECK(first == 0x5A && last == 0x5A);
    });

    test("memória: fragmentação não impede alocação (scatter-gather + mapeamento contínuo)", [&] {
        MemoryManager mm;
        const auto a = mm.allocate(Partition::User, 4 * MiB, "a");
        const auto b = mm.allocate(Partition::User, 4 * MiB, "b");
        const auto c2 = mm.allocate(Partition::User, 4 * MiB, "c");
        const auto rest = mm.allocate(Partition::User, mm.available(Partition::User), "resto");
        mm.free(a);
        mm.free(c2);                                   // dois buracos de 4 MB separados por "b"
        const auto big = mm.allocate(Partition::User, 8 * MiB, "8 MB em buracos");
        CHECK(mm.info(big)->extents.size() == 2);
        CHECK(mm.stats().scattered_allocations == 1);
        const GuestVA va = 0x30'0000'0000ull;
        mm.map_allocation(va, big, pte::kWritable | pte::kUser);
        CHECK(mm.stats().large_mappings == 4);         // 2 + 2 páginas de 2 MB
        mm.write<std::uint64_t>(va + 4 * MiB - 4, 0x1122334455667788ull);   // atravessa as duas extensões
        std::uint64_t back = 0;
        mm.read_allocation(big, 4 * MiB - 4, &back, 8);
        CHECK(back == 0x1122334455667788ull);
        (void)b; (void)rest;
    });

    test("memória: handlers de reclaim são chamados antes do limite", [&] {
        MemoryManager mm;
        std::vector<AllocationId> cache;
        for (int i = 0; i < 6; ++i) cache.push_back(mm.allocate(Partition::User, 1 * GiB, "cache"));
        int calls = 0;
        mm.add_reclaim_handler([&](std::uint64_t, PressureLevel) {
            ++calls;
            if (cache.empty()) return std::uint64_t{0};
            mm.free(cache.back());
            cache.pop_back();
            return 1 * GiB;
        });
        const auto a = mm.allocate(Partition::User, 1 * GiB, "a");   // cabe (7º GB)
        const auto b = mm.allocate(Partition::User, 1 * GiB, "b");   // precisa de reclaim
        CHECK(calls == 1);
        CHECK(mm.stats().oom_events == 0);
        (void)a; (void)b;
    });

    test("codec XLZ: ida e volta (texto, aleatório, vazio, paralelo)", [&] {
        std::mt19937 rng(7);
        for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{13}, std::size_t{70000}, std::size_t{5} << 20}) {
            std::vector<std::uint8_t> d(n);
            for (std::size_t i = 0; i < n; ++i) d[i] = (i % 1000 < 700) ? static_cast<std::uint8_t>(i / 9 % 17) : static_cast<std::uint8_t>(rng());
            const auto c = codec::compress(d, codec::kDefaultBlock, 4);
            std::vector<std::uint8_t> o(n);
            CHECK(codec::decompress(c, o, 4));
            CHECK(o == d);
        }
        std::vector<std::uint8_t> d(1 << 20, 0x41);
        auto c = codec::compress(d);
        CHECK(c.size() < d.size() / 50);
        c[c.size() / 2] ^= 0xFF;              // corrompido: falha sem estourar o buffer
        std::vector<std::uint8_t> o(d.size());
        CHECK(!codec::decompress(c, o) || o != d);
    });

    test("SSD: modelo de tempo — NVMe 2,4 GB/s, descompressão e comparação com HDD", [&] {
        const auto tmp = std::filesystem::temp_directory_path() / "xss_ssd_test.bin";
        // Compressão realista (~2–3×, como assets de jogo): padrões + 1/3 de bytes aleatórios
        std::vector<std::uint8_t> raw(32 * MiB);
        std::mt19937 rng(3);
        for (std::size_t i = 0; i < raw.size(); ++i)
            raw[i] = (i % 96 < 64) ? static_cast<std::uint8_t>(i / 16 % 251) : static_cast<std::uint8_t>(rng());
        const auto packed = codec::compress(raw);
        { std::ofstream(tmp, std::ios::binary).write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size())); }

        SsdDevice ssd;
        const int f = ssd.open(tmp);
        std::vector<std::uint8_t> out(raw.size());
        std::vector<IoRequest> b(1);
        b[0].file = f; b[0].offset = 0; b[0].size = packed.size();
        b[0].compression = IoCompression::Xlz; b[0].uncompressed_size = raw.size();
        b[0].dst = out.data(); b[0].dst_capacity = out.size();
        IoResult res;
        b[0].on_complete = [&](const IoResult& r) { res = r; };
        const auto rep = ssd.submit(b);
        CHECK(res.ok);
        CHECK(out == raw);
        CHECK(rep.raw_gbs() <= 2.4 + 1e-6);                 // nunca acima do barramento
        CHECK(rep.effective_gbs() > rep.raw_gbs());         // descompressão multiplica a banda
        CHECK(rep.effective_gbs() <= 8.0 + 1e-6);           // teto do descompressor
        std::printf("  razão %.2f× | NVMe %.2f ms | ", rep.ratio(), rep.sim_us / 1e3);
        const auto hdd = SsdDevice::simulate(spec::kHdd, {res}, b);
        const auto sata = SsdDevice::simulate(spec::kSataSsd, {res}, b);
        std::printf("SATA %.2f ms | HDD %.2f ms\n", sata.sim_us / 1e3, hdd.sim_us / 1e3);
        CHECK(rep.ratio() > 1.5 && rep.ratio() < 4.0);
        CHECK(sata.sim_us > rep.sim_us * 2);
        CHECK(hdd.sim_us > rep.sim_us * 10);

        // Leitura pequena: dominada pela latência de ~30 µs
        std::uint8_t tiny[512];
        const auto r2 = ssd.read(f, 0, tiny, sizeof(tiny));
        CHECK(r2.ok);
        CHECK(r2.complete_us - r2.issue_us >= 30.0 && r2.complete_us - r2.issue_us < 40.0);
        std::filesystem::remove(tmp);
    });

    test("imagem de demo + parser + streaming de texturas sob orçamento", [&] {
        const auto iso = std::filesystem::temp_directory_path() / "xss_demo_test.iso";
        fs::DemoImageOptions o;
        o.textures = 6;
        o.texture_size = 512;
        o.level_bytes = 4 * MiB;
        fs::build_demo_image(iso, o);
        const auto m = fs::IsoParser::parse(iso);
        CHECK(m.format == fs::ImageFormat::Xdvdfs);
        CHECK(m.title_id == "5E5E0001");
        CHECK(m.target_height == 1440);
        CHECK(!m.thumbnail.empty());
        CHECK(m.compressed_files == 7);       // data/level0.pak + 6 texturas
        CHECK(m.raw_bytes > m.disk_bytes);
        for (const char* t : {"4TFLOPS", "Zen 2", "1440p", "DirectStorage"})
            CHECK(std::find(m.tags.begin(), m.tags.end(), t) != m.tags.end());

        MemoryManager mm;
        SsdDevice ssd;
        fs::TextureStreamer st(mm, ssd, {.pool_budget = 2 * MiB, .max_loads_per_frame = 64});
        CHECK(st.register_image(iso, m) == 6);
        st.request(0, 0);                      // 512² RGBA = 1 MiB
        auto r = st.update();
        CHECK(r.loads == 1);
        CHECK(r.raw_bytes == 1 * MiB);
        st.request(0, 0);
        r = st.update();
        CHECK(r.hits == 1 && r.loads == 0);
        st.request(1, 0); st.request(2, 0);    // pool de 2 MiB: força despejo da textura 0
        r = st.update();
        CHECK(r.loads == 2);
        CHECK(r.evictions >= 1);
        CHECK(st.pool_used() <= 2 * MiB);
        // Conteúdo chegou correto na RAM do convidado (mip 0 da textura 1 tem o padrão de blocos)
        const auto& t1 = st.texture(1);
        CHECK(t1.resident[0] != 0);
        std::filesystem::remove(iso);
    });

    test("firmware OSU1: validação, versão, instalação e recusa de pacotes de jogo", [&] {
        namespace fsys = std::filesystem;
        const auto base = fsys::temp_directory_path() / "xss_fw_test";
        fsys::remove_all(base);
        // Cabeçalho XVD sintético: magic em 0x200, drive size, tipo de conteúdo e versão.
        auto fake_xvd = [](const fsys::path& p, std::uint32_t type, std::uint16_t major, std::uint16_t build) {
            fsys::create_directories(p.parent_path());
            std::vector<std::uint8_t> h(0x1000, 0);
            std::memcpy(h.data() + 0x200, "msft-xvd", 8);
            const std::uint64_t drive = 64 * MiB;
            std::memcpy(h.data() + 0x218, &drive, 8);
            std::memcpy(h.data() + 0x284, &type, 4);
            const std::uint16_t ver[4] = {0, 0, build, major};   // a.b.c.d gravado ao contrário
            std::memcpy(h.data() + 0x3BC, ver, 8);
            std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
        };
        const auto osu = base / "OSU1" / "$SystemUpdate";
        fake_xvd(osu / "system.xvd", 2, 10, 25398);     // SystemOS
        fake_xvd(osu / "host.xvd", 7, 10, 25398);       // HostOS
        fake_xvd(osu / "systemtools.xvd", 19, 10, 25398);
        std::ofstream(osu / "notas.txt") << "arquivo auxiliar";

        const auto info = fs::inspect_system_update(base / "OSU1");   // aceita a pasta pai
        CHECK(info.valid);
        CHECK(info.root.filename() == "$SystemUpdate");
        CHECK(info.version == "10.25398.0.0");
        CHECK(info.system_packages == 3);
        CHECK(info.packages.size() == 4);

        const auto root = base / "firmware";
        const auto inst = fs::install_system_update(base / "OSU1", root);
        CHECK(inst.valid);
        CHECK(fsys::exists(root / "10.25398.0.0" / "$SystemUpdate" / "system.xvd"));
        const auto cur = fs::installed_firmware(root);
        CHECK(cur && cur->version == "10.25398.0.0");

        // Pasta só com pacote de jogo (tipo Title) é recusada
        fake_xvd(base / "jogo" / "game.xvc", 1, 1, 0);
        const auto bad = fs::inspect_system_update(base / "jogo");
        CHECK(!bad.valid);
        CHECK(bad.error.find("não são do sistema") != std::string::npos);
        CHECK(!fs::inspect_system_update(base / "nao_existe").valid);
        fsys::remove_all(base);
    });

    test("boot da ROM de diagnóstico: o título estoura o orçamento e o kernel nega", [&] {
        for (auto mode : {CpuCluster::Mode::Interpreter, CpuCluster::Mode::Jit}) {
            MemoryManager mem;
            GpuDevice gpu(mem);
            XboxOsKernel k(mem, gpu);
            CpuCluster cl(mem, k);
            SsdDevice ssd;
            UefiFirmware fw(mem, cl, gpu, ssd);
            CHECK(fw.power_on_self_test());
            fw.boot(UefiFirmware::diagnostic_rom());
            CpuCluster::RunOptions o; o.trace = false; o.mode = mode;
            const auto rep = cl.main_loop(o);
            CHECK(rep.fault.empty());
            CHECK(k.terminated());
            CHECK(rep.exit_code == 0);
            CHECK(gpu.stats().draws == 2);
            CHECK(gpu.stats().dispatches == 1);
            CHECK(k.failed_allocations() == 1);
            CHECK(mem.stats().oom_events == 1);
            CHECK(cl.core(0).state().gpr[R12] == 6);   // 6 × 1 GiB couberam (o resto: RTs, imagem, pilha, heap)
        }
    });

    std::printf("\n%d verificações OK, %d falhas\n", g_passed, g_failed);
    return g_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
