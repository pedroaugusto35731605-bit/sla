// =============================================================================
//  SeriesS-Emu — memory_manager.hpp
//
//  10 GB de GDDR6 unificada, divididos em duas partições com orçamento rígido:
//
//    ┌──────────────────── 10 GB físicos (uma única reserva no host) ───────────────────┐
//    │ System Reserved 3 GB                    │ User Space 7 GB (título)               │
//    │ [0,2 GB) pool lento 56 GB/s │ [2,3 GB)  │ [3,10 GB) pool rápido 224 GB/s         │
//    └──────────────────────────────────────────────────────────────────────────────────┘
//
//  Política de orçamento do User Space (o diferencial do Series S):
//    < 80 %   Normal    — tudo liberado
//    ≥ 80 %   Elevated  — ouvintes (ex.: TextureStreamer) passam a fazer streaming agressivo
//    ≥ 95 %   Critical  — dados frios marcados como Compressible são comprimidos na RAM
//    > 100 %  a alocação tenta, em ordem: (1) handlers de reclaim (evicção de streaming),
//             (2) compressão forçada de dados frios; se ainda faltar memória:
//             "[WARN] Memory threshold exceeded on Series S backend." + OutOfMemory.
//
//  Eficiência:
//    • Alocações scatter-gather: tenta um bloco físico contíguo; se o espaço livre
//      estiver fragmentado, a alocação é montada com várias extensões (em granulos de
//      2 MB quando possível) — a page table as junta num intervalo virtual contínuo,
//      como num SO real. Fragmentação nunca causa falha se o orçamento permitir.
//    • A reserva de 10 GB é virtual (mmap NORESERVE / VirtualAlloc RESERVE): o host só
//      gasta RAM nas páginas tocadas, e frames liberados são devolvidos ao host
//      (MADV_DONTNEED / MEM_DECOMMIT), o que também garante páginas zeradas sem memset.
//    • Alocador de frames por partição com free-list ordenada e coalescência.
//    • Page tables x86-64 reais (4 níveis) com páginas grandes de 2 MB quando
//      alinhado — 512× menos PTEs para buffers grandes — e TLB em software.
// =============================================================================
#pragma once

#include "hardware_spec.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace xss {

using GuestVA = std::uint64_t;
using GuestPA = std::uint64_t;
using AllocationId = std::uint64_t;

namespace pte {
    inline constexpr std::uint64_t kPresent   = 1ull << 0;
    inline constexpr std::uint64_t kWritable  = 1ull << 1;
    inline constexpr std::uint64_t kUser      = 1ull << 2;
    inline constexpr std::uint64_t kAccessed  = 1ull << 5;
    inline constexpr std::uint64_t kDirty     = 1ull << 6;
    inline constexpr std::uint64_t kLargePage = 1ull << 7;   // PS no PDE → página de 2 MB
    inline constexpr std::uint64_t kNoExec    = 1ull << 63;
    inline constexpr std::uint64_t kAddrMask  = 0x000F'FFFF'FFFF'F000ull;
    inline constexpr std::uint64_t kLargeMask = 0x000F'FFFF'FFE0'0000ull;
}

enum class Access : std::uint8_t { Read, Write, Execute };
enum class Partition : std::uint8_t { System = 0, User = 1 };
enum class PressureLevel : std::uint8_t { Normal, Elevated, Critical };

const char* to_string(Partition p);
const char* to_string(PressureLevel p);

namespace alloc_flags {
    inline constexpr std::uint32_t kNone         = 0;
    inline constexpr std::uint32_t kCompressible = 1u << 0;  // pode ser comprimido quando frio
    inline constexpr std::uint32_t kPinned       = 1u << 1;  // nunca comprimido nem reclamado
}

class PageFault : public std::runtime_error {
public:
    PageFault(GuestVA va, Access a, const std::string& why);
    GuestVA address() const noexcept { return va_; }
private:
    GuestVA va_;
};

class OutOfMemory : public std::runtime_error {
public:
    OutOfMemory(Partition p, std::uint64_t requested, std::uint64_t available);
    Partition     partition() const noexcept { return part_; }
    std::uint64_t requested() const noexcept { return requested_; }
private:
    Partition     part_;
    std::uint64_t requested_;
};

struct Extent { GuestPA pa = 0; std::uint64_t size = 0; };
using ExtentList = std::vector<Extent>;

struct AllocationInfo {
    AllocationId  id = 0;
    Partition     partition = Partition::User;
    GuestPA       pa = 0;                 // início da 1ª extensão (0 enquanto comprimido)
    ExtentList    extents;                // frames residentes (1 extensão = contígua)
    std::uint64_t size = 0;               // tamanho lógico (arredondado a 4 KiB)
    std::string   tag;
    std::uint32_t flags = 0;
    bool          compressed = false;
    ExtentList    compressed_extents;     // frames do payload comprimido
    std::uint64_t compressed_size = 0;    // bytes do payload comprimido
    std::uint64_t compressed_frames = 0;  // bytes de frames ocupados pelo payload
    std::uint32_t mapped = 0;             // nº de mapeamentos virtuais ativos
    std::uint32_t locks = 0;              // nº de locks do host (lock/unlock) — impede compressão
    std::uint64_t last_use = 0;           // relógio LRU
    bool contiguous() const noexcept { return extents.size() == 1; }
};

// Alocador first-fit com coalescência sobre uma faixa física.
class FrameAllocator {
public:
    FrameAllocator(GuestPA base, std::uint64_t size);
    std::optional<GuestPA> allocate(std::uint64_t size, std::uint64_t align);
    // Monta `size` bytes com várias extensões (2 MB primeiro, depois 4 KB).
    bool          allocate_scattered(std::uint64_t size, ExtentList& out);
    void          release(GuestPA pa, std::uint64_t size);
    std::uint64_t free_bytes() const noexcept { return free_; }
    std::uint64_t largest_free_block() const;
    std::size_t   fragments() const noexcept { return blocks_.size(); }
private:
    bool          carve(GuestPA pa, std::uint64_t size);
    std::map<GuestPA, std::uint64_t> blocks_;   // início → tamanho
    std::uint64_t free_ = 0;
};

class MemoryManager {
public:
    using ReclaimHandler   = std::function<std::uint64_t(std::uint64_t bytes_needed, PressureLevel)>;
    using PressureListener = std::function<void(PressureLevel from, PressureLevel to)>;

    MemoryManager();
    ~MemoryManager();
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    // ---------------- memória física
    std::uint64_t total_size() const noexcept { return spec::kMemory.total; }
    std::uint8_t* host_pointer(GuestPA pa, std::uint64_t len);
    void          read_physical (GuestPA pa, void* dst, std::uint64_t len);
    void          write_physical(GuestPA pa, const void* src, std::uint64_t len);
    static double bandwidth_gbs(GuestPA pa);   // 56 GB/s no pool lento, 224 GB/s no rápido

    // ---------------- alocações com orçamento
    AllocationId  allocate(Partition p, std::uint64_t size, std::string tag,
                           std::uint32_t flags = alloc_flags::kNone);
    void          free(AllocationId id);
    // Garante residência (descomprime se preciso) e marca uso. O PA devolvido só é
    // estável enquanto a alocação não puder ser comprimida: para usar o ponteiro do
    // host por mais tempo (ex.: destino de I/O), use lock()/unlock().
    GuestPA       acquire(AllocationId id);
    GuestPA       lock(AllocationId id);       // acquire + impede compressão até unlock()
    void          unlock(AllocationId id);
    void          touch(AllocationId id);
    std::optional<AllocationInfo> info(AllocationId id) const;

    // Acesso do host a uma alocação (possivelmente espalhada). host_spans exige lock().
    std::vector<std::span<std::uint8_t>> host_spans(AllocationId id);
    void read_allocation (AllocationId id, std::uint64_t offset, void* dst, std::uint64_t len);
    void write_allocation(AllocationId id, std::uint64_t offset, const void* src, std::uint64_t len);

    std::uint64_t budget(Partition p) const noexcept;
    std::uint64_t used(Partition p) const;
    std::uint64_t available(Partition p) const;
    PressureLevel pressure() const;

    int  add_reclaim_handler(ReclaimHandler h);
    void remove_reclaim_handler(int handle);
    int  add_pressure_listener(PressureListener l);
    void remove_pressure_listener(int handle);

    // Comprime alocações Compressible frias (LRU) até liberar `target` bytes.
    std::uint64_t compress_cold(std::uint64_t target);

    // ---------------- paginação (x86-64, 4 níveis, páginas de 4 KB e 2 MB)
    GuestPA cr3() const noexcept { return cr3_; }
    void    map(GuestVA va, GuestPA pa, std::uint64_t size, std::uint64_t flags);
    GuestVA map_allocation(GuestVA va, AllocationId id, std::uint64_t flags);
    void    unmap(GuestVA va, std::uint64_t size);
    void    unmap_allocation(GuestVA va, AllocationId id);
    GuestPA translate(GuestVA va, Access a);
    void    flush_tlb();

    void read_virtual (GuestVA va, void* dst, std::uint64_t len, Access a = Access::Read);
    void write_virtual(GuestVA va, const void* src, std::uint64_t len);
    template <typename T> T read(GuestVA va, Access a = Access::Read) { T v{}; read_virtual(va, &v, sizeof(T), a); return v; }
    template <typename T> void write(GuestVA va, T v) { write_virtual(va, &v, sizeof(T)); }

    // ---------------- estatísticas
    struct PartitionStats {
        std::uint64_t budget = 0, used = 0, peak = 0, allocations = 0, free_bytes = 0, largest_free = 0;
        std::size_t   fragments = 0;
    };
    struct Stats {
        PartitionStats system, user;
        std::uint64_t page_table_pages = 0, large_mappings = 0, small_mappings = 0;
        std::uint64_t tlb_hits = 0, tlb_misses = 0;
        std::uint64_t compressions = 0, decompressions = 0, compression_saved = 0;
        std::uint64_t reclaim_runs = 0, reclaimed_bytes = 0, oom_events = 0, scattered_allocations = 0;
        std::uint64_t host_committed_estimate = 0;
    };
    Stats       stats() const;
    std::string report() const;

private:
    struct PartitionState {
        PartitionState(GuestPA b, std::uint64_t s, std::uint64_t budget)
            : base(b), size(s), budget(budget), frames(b, s) {}
        GuestPA        base;
        std::uint64_t  size;
        std::uint64_t  budget;
        std::uint64_t  used = 0;     // inclui payload comprimido e page tables (sistema)
        std::uint64_t  peak = 0;
        std::uint64_t  allocations = 0;
        FrameAllocator frames;
    };

    PartitionState&       part(Partition p) { return p == Partition::System ? system_ : user_; }
    const PartitionState& part(Partition p) const { return p == Partition::System ? system_ : user_; }

    // Obtém frames respeitando o orçamento; pode liberar o lock para rodar reclaim.
    ExtentList    obtain_frames(Partition p, std::uint64_t size, std::uint64_t align,
                                std::unique_lock<std::mutex>& lk, AllocationId protect);
    std::optional<ExtentList> try_take(Partition p, std::uint64_t size, std::uint64_t align);
    void          give_back(Partition p, const ExtentList& ex);
    void          gather (const ExtentList& ex, std::uint8_t* dst, std::uint64_t len) const;
    void          scatter(const ExtentList& ex, const std::uint8_t* src, std::uint64_t len);
    std::uint64_t compress_cold_locked(std::uint64_t target, AllocationId protect);
    void          decompress_locked(AllocationInfo& a, std::unique_lock<std::mutex>& lk);
    PressureLevel level_locked() const;
    void          publish_pressure(std::unique_lock<std::mutex>& lk, AllocationId protect = 0);

    void host_decommit(GuestPA pa, std::uint64_t size);
    void host_commit(GuestPA pa, std::uint64_t size);

    GuestPA        acquire_locked(AllocationId id, std::unique_lock<std::mutex>& lk, bool take_lock);
    GuestPA        alloc_page_table();
    std::uint64_t* entry(GuestPA table, unsigned idx);
    std::uint64_t* walk_to(GuestVA va, unsigned stop_level, bool create);
    void           invalidate_tlb(GuestVA va, std::uint64_t size);

    std::uint8_t*  base_ = nullptr;
    GuestPA        cr3_  = 0;

    mutable std::mutex mu_;
    PartitionState system_;
    PartitionState user_;
    std::unordered_map<AllocationId, AllocationInfo> allocs_;
    AllocationId   next_id_ = 1;
    std::uint64_t  clock_ = 0;
    PressureLevel  published_level_ = PressureLevel::Normal;

    std::map<int, ReclaimHandler>   reclaimers_;
    std::map<int, PressureListener> listeners_;
    int next_handle_ = 1;

    struct TlbEntry { GuestVA vpn = ~0ull; GuestPA pfn = 0; std::uint64_t flags = 0; };
    static constexpr std::size_t kTlbSize = 4096;
    std::array<TlbEntry, kTlbSize> tlb_{};

    Stats counters_{};
};

}  // namespace xss
