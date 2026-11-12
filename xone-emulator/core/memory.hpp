// =============================================================================
//  XOne-Emu — memory.hpp
//  RAM unificada de 8 GB + paginação x86-64 de 4 níveis (PML4 → PDPT → PD → PT).
//
//  - A RAM física do convidado é uma única reserva contígua de memória virtual
//    do host (mmap/VirtualAlloc). As páginas só ocupam RAM real do host quando
//    são tocadas (commit sob demanda), então reservar 8 GB é barato.
//  - As page tables vivem DENTRO da RAM do convidado, no formato real x86-64,
//    então código convidado que leia CR3/PTEs vê estruturas legítimas.
//  - Uma TLB em software evita a page walk em todo acesso.
// =============================================================================
#pragma once

#include "hardware_spec.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>

namespace xone {

using GuestVA = std::uint64_t;  // endereço virtual do convidado (Xbox)
using GuestPA = std::uint64_t;  // endereço físico do convidado (offset na RAM de 8 GB)

// Bits de PTE x86-64 (Intel SDM vol. 3A, 4.5)
namespace pte {
    inline constexpr std::uint64_t kPresent  = 1ull << 0;
    inline constexpr std::uint64_t kWritable = 1ull << 1;
    inline constexpr std::uint64_t kUser     = 1ull << 2;
    inline constexpr std::uint64_t kAccessed = 1ull << 5;
    inline constexpr std::uint64_t kDirty    = 1ull << 6;
    inline constexpr std::uint64_t kNoExec   = 1ull << 63;
    inline constexpr std::uint64_t kAddrMask = 0x000F'FFFF'FFFF'F000ull;
}

enum class Access : std::uint8_t { Read, Write, Execute };

class PageFault : public std::runtime_error {
public:
    PageFault(GuestVA va, Access access, const std::string& why);
    GuestVA address() const noexcept { return va_; }
    Access  access()  const noexcept { return access_; }
private:
    GuestVA va_;
    Access  access_;
};

class MemoryManager {
public:
    explicit MemoryManager(std::uint64_t ram_size = spec::kMemory.system_ram);
    ~MemoryManager();
    MemoryManager(const MemoryManager&) = delete;
    MemoryManager& operator=(const MemoryManager&) = delete;

    // ---------------- memória física
    std::uint64_t ram_size() const noexcept { return size_; }
    std::uint8_t* host_pointer(GuestPA pa, std::uint64_t len = 1);
    void          read_physical (GuestPA pa, void* dst, std::uint64_t len);
    void          write_physical(GuestPA pa, const void* src, std::uint64_t len);

    // Alocador de frames físicos (bump + freelist simples)
    GuestPA allocate_frames(std::uint64_t bytes);

    // ---------------- paginação
    GuestPA cr3() const noexcept { return cr3_; }
    void    map(GuestVA va, GuestPA pa, std::uint64_t size, std::uint64_t flags);
    GuestPA map_anonymous(GuestVA va, std::uint64_t size, std::uint64_t flags);
    void    unmap(GuestVA va, std::uint64_t size);
    GuestPA translate(GuestVA va, Access access);  // page walk + TLB
    void    flush_tlb();

    // ---------------- acesso virtual (o que a CPU usa)
    void read_virtual (GuestVA va, void* dst, std::uint64_t len, Access a = Access::Read);
    void write_virtual(GuestVA va, const void* src, std::uint64_t len);

    template <typename T> T read(GuestVA va, Access a = Access::Read) {
        T v{}; read_virtual(va, &v, sizeof(T), a); return v;
    }
    template <typename T> void write(GuestVA va, T v) { write_virtual(va, &v, sizeof(T)); }

    // Estatísticas
    struct Stats {
        std::uint64_t tlb_hits = 0, tlb_misses = 0, page_walks = 0;
        std::uint64_t mapped_pages = 0, page_table_pages = 0, frames_used_bytes = 0;
    };
    Stats stats() const;

private:
    GuestPA alloc_page_table();
    std::uint64_t* entry_ptr(GuestPA table, unsigned index);
    std::uint64_t* walk(GuestVA va, bool create, std::uint64_t flags);

    std::uint8_t* base_ = nullptr;
    std::uint64_t size_ = 0;
    GuestPA       cr3_  = 0;
    GuestPA       pt_next_   = spec::layout::kPageTableBase;
    GuestPA       frame_next_ = spec::layout::kGeneralBase;

    struct TlbEntry { GuestVA vpn = ~0ull; GuestPA pfn = 0; std::uint64_t flags = 0; };
    static constexpr std::size_t kTlbSize = 1024;  // direct-mapped
    std::array<TlbEntry, kTlbSize> tlb_{};

    mutable std::mutex mu_;
    Stats stats_{};
};

}  // namespace xone
