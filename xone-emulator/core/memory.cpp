// =============================================================================
//  XOne-Emu — memory.cpp
// =============================================================================
#include "memory.hpp"

#include <cstdio>
#include <format>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

namespace xone {

namespace {
constexpr std::uint64_t kPage = spec::kMemory.page_size;

const char* access_name(Access a) {
    switch (a) {
        case Access::Read:    return "read";
        case Access::Write:   return "write";
        case Access::Execute: return "execute";
    }
    return "?";
}

// Índices de cada nível da page walk (9 bits cada) — x86-64 4-level paging.
constexpr unsigned idx(GuestVA va, unsigned level) {  // level 3=PML4 ... 0=PT
    return static_cast<unsigned>((va >> (12 + 9 * level)) & 0x1FF);
}

bool is_canonical(GuestVA va) {
    const std::uint64_t top = va >> 47;
    return top == 0 || top == 0x1FFFF;
}
}  // namespace

PageFault::PageFault(GuestVA va, Access access, const std::string& why)
    : std::runtime_error(std::format("#PF {} at 0x{:016X}: {}", access_name(access), va, why)),
      va_(va), access_(access) {}

// ----------------------------------------------------------------------------
MemoryManager::MemoryManager(std::uint64_t ram_size) : size_(ram_size) {
    if (size_ < spec::layout::kGeneralBase + 16 * spec::MiB)
        throw std::invalid_argument("RAM do convidado pequena demais para o layout físico");

#if defined(_WIN32)
    // Reserva + commit sob demanda: o Windows só usa RAM real em páginas tocadas.
    base_ = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base_) throw std::runtime_error("VirtualAlloc falhou ao reservar a RAM do convidado");
#else
    void* p = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) throw std::runtime_error("mmap falhou ao reservar a RAM do convidado");
    base_ = static_cast<std::uint8_t*>(p);
#endif

    cr3_ = alloc_page_table();  // PML4 raiz
    std::printf("[XONE-MEM] %llu MiB de RAM unificada reservados em %p (CR3=0x%llX)\n",
                static_cast<unsigned long long>(size_ / spec::MiB), static_cast<void*>(base_),
                static_cast<unsigned long long>(cr3_));
}

MemoryManager::~MemoryManager() {
    if (!base_) return;
#if defined(_WIN32)
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, size_);
#endif
}

// ---------------------------------------------------------------- físico
std::uint8_t* MemoryManager::host_pointer(GuestPA pa, std::uint64_t len) {
    if (pa >= size_ || len > size_ - pa)
        throw std::out_of_range(std::format("acesso físico fora da RAM: 0x{:X}+{}", pa, len));
    return base_ + pa;
}

void MemoryManager::read_physical(GuestPA pa, void* dst, std::uint64_t len) {
    std::memcpy(dst, host_pointer(pa, len), len);
}

void MemoryManager::write_physical(GuestPA pa, const void* src, std::uint64_t len) {
    std::memcpy(host_pointer(pa, len), src, len);
}

GuestPA MemoryManager::allocate_frames(std::uint64_t bytes) {
    std::lock_guard lk(mu_);
    bytes = (bytes + kPage - 1) & ~(kPage - 1);
    if (frame_next_ + bytes > size_) throw std::bad_alloc();
    const GuestPA pa = frame_next_;
    frame_next_ += bytes;
    stats_.frames_used_bytes += bytes;
    return pa;
}

GuestPA MemoryManager::alloc_page_table() {
    // chamado com mu_ já travado (ou no construtor)
    if (pt_next_ + kPage > spec::layout::kPageTableBase + spec::layout::kPageTableSize)
        throw std::runtime_error("pool de page tables esgotado");
    const GuestPA pa = pt_next_;
    pt_next_ += kPage;
    std::memset(base_ + pa, 0, kPage);
    ++stats_.page_table_pages;
    return pa;
}

std::uint64_t* MemoryManager::entry_ptr(GuestPA table, unsigned index) {
    return reinterpret_cast<std::uint64_t*>(base_ + table) + index;
}

// Percorre PML4→PDPT→PD→PT e devolve o ponteiro para a PTE final.
std::uint64_t* MemoryManager::walk(GuestVA va, bool create, std::uint64_t flags) {
    GuestPA table = cr3_;
    for (unsigned level = 3; level > 0; --level) {
        std::uint64_t* e = entry_ptr(table, idx(va, level));
        if (!(*e & pte::kPresent)) {
            if (!create) return nullptr;
            const GuestPA next = alloc_page_table();
            // Níveis intermediários são permissivos; a PTE final restringe.
            *e = next | pte::kPresent | pte::kWritable | pte::kUser;
        }
        (void)flags;
        table = *e & pte::kAddrMask;
    }
    return entry_ptr(table, idx(va, 0));
}

// ---------------------------------------------------------------- mapeamento
void MemoryManager::map(GuestVA va, GuestPA pa, std::uint64_t size, std::uint64_t flags) {
    if ((va | pa) & (kPage - 1)) throw std::invalid_argument("map: endereços devem ser alinhados a 4 KiB");
    std::lock_guard lk(mu_);
    for (std::uint64_t off = 0; off < size; off += kPage) {
        if (!is_canonical(va + off)) throw std::invalid_argument("map: VA não canônico");
        std::uint64_t* e = walk(va + off, true, flags);
        if (!(*e & pte::kPresent)) ++stats_.mapped_pages;
        *e = ((pa + off) & pte::kAddrMask) | flags | pte::kPresent;
        tlb_[((va + off) >> 12) % kTlbSize].vpn = ~0ull;  // invlpg
    }
}

GuestPA MemoryManager::map_anonymous(GuestVA va, std::uint64_t size, std::uint64_t flags) {
    size = (size + kPage - 1) & ~(kPage - 1);
    const GuestPA pa = allocate_frames(size);
    std::memset(base_ + pa, 0, size);
    map(va, pa, size, flags);
    return pa;
}

void MemoryManager::unmap(GuestVA va, std::uint64_t size) {
    std::lock_guard lk(mu_);
    for (std::uint64_t off = 0; off < size; off += kPage) {
        if (std::uint64_t* e = walk(va + off, false, 0); e && (*e & pte::kPresent)) {
            *e = 0;
            --stats_.mapped_pages;
        }
        tlb_[((va + off) >> 12) % kTlbSize].vpn = ~0ull;
    }
}

void MemoryManager::flush_tlb() {
    std::lock_guard lk(mu_);
    for (auto& t : tlb_) t.vpn = ~0ull;
}

GuestPA MemoryManager::translate(GuestVA va, Access access) {
    const GuestVA vpn = va >> 12;
    std::lock_guard lk(mu_);

    TlbEntry& t = tlb_[vpn % kTlbSize];
    std::uint64_t flags;
    GuestPA pfn;
    if (t.vpn == vpn) {
        ++stats_.tlb_hits;
        flags = t.flags;
        pfn = t.pfn;
    } else {
        ++stats_.tlb_misses;
        ++stats_.page_walks;
        if (!is_canonical(va)) throw PageFault(va, access, "endereço não canônico");
        std::uint64_t* e = walk(va, false, 0);
        if (!e || !(*e & pte::kPresent)) throw PageFault(va, access, "página não presente");
        *e |= pte::kAccessed;
        flags = *e;
        pfn = *e & pte::kAddrMask;
        t = {vpn, pfn, flags};
    }

    if (access == Access::Write && !(flags & pte::kWritable))
        throw PageFault(va, access, "página somente leitura");
    if (access == Access::Execute && (flags & pte::kNoExec))
        throw PageFault(va, access, "página NX");
    return pfn | (va & (kPage - 1));
}

// ---------------------------------------------------------------- virtual
void MemoryManager::read_virtual(GuestVA va, void* dst, std::uint64_t len, Access a) {
    auto* out = static_cast<std::uint8_t*>(dst);
    while (len) {  // divide em fronteiras de página
        const std::uint64_t chunk = std::min(len, kPage - (va & (kPage - 1)));
        std::memcpy(out, base_ + translate(va, a), chunk);
        va += chunk; out += chunk; len -= chunk;
    }
}

void MemoryManager::write_virtual(GuestVA va, const void* src, std::uint64_t len) {
    auto* in = static_cast<const std::uint8_t*>(src);
    while (len) {
        const std::uint64_t chunk = std::min(len, kPage - (va & (kPage - 1)));
        std::memcpy(base_ + translate(va, Access::Write), in, chunk);
        va += chunk; in += chunk; len -= chunk;
    }
}

MemoryManager::Stats MemoryManager::stats() const {
    std::lock_guard lk(mu_);
    return stats_;
}

}  // namespace xone
