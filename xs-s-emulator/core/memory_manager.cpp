// =============================================================================
//  SeriesS-Emu — memory_manager.cpp
//  Divisão 7 GB (título) + 3 GB (sistema), orçamento rígido, pressão de
//  memória, compressão de dados frios e paginação x86-64 com páginas de 2 MB.
// =============================================================================
#include "memory_manager.hpp"

#include "codec.hpp"
#include "log.hpp"

#include <algorithm>
#include <cstring>
#include <format>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <sys/mman.h>
#endif

namespace xss {

namespace {
constexpr std::uint64_t kPage  = spec::kMemory.page_size;
constexpr std::uint64_t kLarge = spec::kMemory.large_page;

constexpr std::uint64_t round_up(std::uint64_t v, std::uint64_t a) { return (v + a - 1) & ~(a - 1); }
constexpr unsigned idx(GuestVA va, unsigned level) { return static_cast<unsigned>((va >> (12 + 9 * level)) & 0x1FF); }
bool canonical(GuestVA va) { const auto t = va >> 47; return t == 0 || t == 0x1FFFF; }

const char* access_name(Access a) {
    return a == Access::Read ? "leitura" : a == Access::Write ? "escrita" : "execução";
}
}  // namespace

const char* to_string(Partition p) { return p == Partition::System ? "System Reserved" : "User Space"; }
const char* to_string(PressureLevel p) {
    switch (p) {
        case PressureLevel::Normal:   return "Normal";
        case PressureLevel::Elevated: return "Elevated";
        case PressureLevel::Critical: return "Critical";
    }
    return "?";
}

PageFault::PageFault(GuestVA va, Access a, const std::string& why)
    : std::runtime_error(std::format("#PF ({}) em 0x{:016X}: {}", access_name(a), va, why)), va_(va) {}

OutOfMemory::OutOfMemory(Partition p, std::uint64_t requested, std::uint64_t available)
    : std::runtime_error(std::format("sem memória em {}: pedido {}, disponível {}", to_string(p),
                                     human_bytes(requested), human_bytes(available))),
      part_(p), requested_(requested) {}

// ============================================================================
//  FrameAllocator
// ============================================================================
FrameAllocator::FrameAllocator(GuestPA base, std::uint64_t size) : free_(size) { blocks_[base] = size; }

std::optional<GuestPA> FrameAllocator::allocate(std::uint64_t size, std::uint64_t align) {
    for (auto it = blocks_.begin(); it != blocks_.end(); ++it) {
        const GuestPA start = round_up(it->first, align);
        const std::uint64_t pad = start - it->first;
        if (pad > it->second || it->second - pad < size) continue;
        const GuestPA blk = it->first;
        const std::uint64_t blk_size = it->second;
        blocks_.erase(it);
        if (pad) blocks_[blk] = pad;                                   // sobra antes (alinhamento)
        if (const auto tail = blk_size - pad - size) blocks_[start + size] = tail;  // sobra depois
        free_ -= size;
        return start;
    }
    return std::nullopt;
}

void FrameAllocator::release(GuestPA pa, std::uint64_t size) {
    auto [it, inserted] = blocks_.emplace(pa, size);
    if (!inserted) throw std::logic_error("FrameAllocator: double free");
    free_ += size;
    if (auto next = std::next(it); next != blocks_.end() && it->first + it->second == next->first) {
        it->second += next->second;
        blocks_.erase(next);
    }
    if (it != blocks_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            blocks_.erase(it);
        }
    }
}

bool FrameAllocator::carve(GuestPA pa, std::uint64_t size) {
    auto it = blocks_.upper_bound(pa);
    if (it == blocks_.begin()) return false;
    --it;
    const GuestPA b = it->first;
    const std::uint64_t bs = it->second;
    if (pa < b || pa + size > b + bs) return false;
    blocks_.erase(it);
    if (pa > b) blocks_[b] = pa - b;
    if (pa + size < b + bs) blocks_[pa + size] = b + bs - (pa + size);
    free_ -= size;
    return true;
}

bool FrameAllocator::allocate_scattered(std::uint64_t size, ExtentList& out) {
    if (free_ < size) return false;
    std::uint64_t rem = size;
    auto by_size = [this] {
        std::vector<std::pair<GuestPA, std::uint64_t>> v(blocks_.begin(), blocks_.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        return v;
    };
    ExtentList plan;
    // 1) granulos de 2 MB alinhados (viram páginas grandes na page table), maiores blocos primeiro
    for (const auto& [start, len] : by_size()) {
        if (rem < kLarge) break;
        const GuestPA s = round_up(start, kLarge);
        if (s >= start + len) continue;
        const std::uint64_t usable = (start + len - s) & ~(kLarge - 1);
        const std::uint64_t take = std::min(usable, rem & ~(kLarge - 1));
        if (take) { plan.push_back({s, take}); rem -= take; }
    }
    for (const auto& e : plan) carve(e.pa, e.size);
    // 2) o resto em páginas de 4 KB
    ExtentList tail;
    for (const auto& [start, len] : by_size()) {
        if (rem == 0) break;
        const std::uint64_t take = std::min(len, rem);
        tail.push_back({start, take});
        rem -= take;
    }
    for (const auto& e : tail) carve(e.pa, e.size);
    out.insert(out.end(), plan.begin(), plan.end());
    out.insert(out.end(), tail.begin(), tail.end());
    return rem == 0;
}

std::uint64_t FrameAllocator::largest_free_block() const {
    std::uint64_t m = 0;
    for (const auto& [_, s] : blocks_) m = std::max(m, s);
    return m;
}

// ============================================================================
//  Construção / memória do host
// ============================================================================
MemoryManager::MemoryManager()
    : system_(spec::layout::kSystemBase, spec::layout::kSystemSize, spec::layout::kSystemSize),
      user_(spec::layout::kUserBase, spec::layout::kUserSize, spec::kMemory.user_space) {
    const std::uint64_t total = spec::kMemory.total;
#if defined(_WIN32)
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_NOACCESS));
    if (!base_) throw std::runtime_error("VirtualAlloc: não foi possível reservar 10 GB de espaço virtual");
#else
    void* p = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) throw std::runtime_error("mmap: não foi possível reservar 10 GB de espaço virtual");
    base_ = static_cast<std::uint8_t*>(p);
#endif
    // Os primeiros 2 MB do sistema guardam firmware/tabelas; PML4 raiz em seguida.
    auto fw = system_.frames.allocate(kLarge, kLarge);
    host_commit(*fw, kLarge);
    system_.used += kLarge;
    cr3_ = alloc_page_table();

    log_info("MEM", "10 GB GDDR6 reservados no host em {} — User Space {} (orçamento rígido) | System Reserved {}",
             static_cast<void*>(base_), human_bytes(user_.budget), human_bytes(system_.budget));
}

MemoryManager::~MemoryManager() {
    if (!base_) return;
#if defined(_WIN32)
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, spec::kMemory.total);
#endif
}

void MemoryManager::host_commit(GuestPA pa, std::uint64_t size) {
#if defined(_WIN32)
    if (!VirtualAlloc(base_ + pa, size, MEM_COMMIT, PAGE_READWRITE)) throw std::bad_alloc();
#else
    (void)pa; (void)size;   // Linux: commit implícito no primeiro toque
#endif
}

void MemoryManager::host_decommit(GuestPA pa, std::uint64_t size) {
    // Devolve a RAM ao host; a próxima leitura enxerga zeros (sem memset).
#if defined(_WIN32)
    VirtualFree(base_ + pa, size, MEM_DECOMMIT);
#else
    madvise(base_ + pa, size, MADV_DONTNEED);
#endif
}

std::uint8_t* MemoryManager::host_pointer(GuestPA pa, std::uint64_t len) {
    if (pa >= spec::kMemory.total || len > spec::kMemory.total - pa)
        throw std::out_of_range(std::format("acesso físico fora dos 10 GB: 0x{:X}+{}", pa, len));
    return base_ + pa;
}
void MemoryManager::read_physical(GuestPA pa, void* dst, std::uint64_t len) { std::memcpy(dst, host_pointer(pa, len), len); }
void MemoryManager::write_physical(GuestPA pa, const void* src, std::uint64_t len) { std::memcpy(host_pointer(pa, len), src, len); }

double MemoryManager::bandwidth_gbs(GuestPA pa) {
    return pa < spec::kMemory.slow_pool ? spec::kMemory.slow_bw_gbs : spec::kMemory.fast_bw_gbs;
}

// ============================================================================
//  Orçamento e pressão
// ============================================================================
std::uint64_t MemoryManager::budget(Partition p) const noexcept { return part(p).budget; }
std::uint64_t MemoryManager::used(Partition p) const { std::lock_guard lk(mu_); return part(p).used; }
std::uint64_t MemoryManager::available(Partition p) const {
    std::lock_guard lk(mu_);
    const auto& s = part(p);
    return s.budget > s.used ? s.budget - s.used : 0;
}

PressureLevel MemoryManager::level_locked() const {
    const double r = static_cast<double>(user_.used) / static_cast<double>(user_.budget);
    if (r >= spec::kMemory.critical_ratio) return PressureLevel::Critical;
    if (r >= spec::kMemory.elevated_ratio) return PressureLevel::Elevated;
    return PressureLevel::Normal;
}
PressureLevel MemoryManager::pressure() const { std::lock_guard lk(mu_); return level_locked(); }

void MemoryManager::publish_pressure(std::unique_lock<std::mutex>& lk, AllocationId protect) {
    PressureLevel now = level_locked();
    if (now == PressureLevel::Critical && published_level_ != PressureLevel::Critical) {
        // Política Critical: comprime dados frios proativamente até sair da faixa crítica.
        const auto limit = static_cast<std::uint64_t>(static_cast<double>(user_.budget) * spec::kMemory.critical_ratio);
        if (user_.used > limit && compress_cold_locked(user_.used - limit, protect) > 0) now = level_locked();
    }
    if (now == published_level_) return;
    const PressureLevel from = published_level_;
    published_level_ = now;
    const auto listeners = listeners_;          // cópia: ouvintes podem chamar de volta
    const auto used = user_.used;
    lk.unlock();
    log_info("MEM", "pressão de memória: {} → {} ({} de {} no User Space)", to_string(from), to_string(now),
             human_bytes(used), human_bytes(user_.budget));
    for (const auto& [_, l] : listeners) l(from, now);
    lk.lock();
}

int MemoryManager::add_reclaim_handler(ReclaimHandler h) {
    std::lock_guard lk(mu_); reclaimers_[next_handle_] = std::move(h); return next_handle_++;
}
void MemoryManager::remove_reclaim_handler(int h) { std::lock_guard lk(mu_); reclaimers_.erase(h); }
int MemoryManager::add_pressure_listener(PressureListener l) {
    std::lock_guard lk(mu_); listeners_[next_handle_] = std::move(l); return next_handle_++;
}
void MemoryManager::remove_pressure_listener(int h) { std::lock_guard lk(mu_); listeners_.erase(h); }

std::optional<ExtentList> MemoryManager::try_take(Partition p, std::uint64_t size, std::uint64_t align) {
    auto& s = part(p);
    if (s.used + size > s.budget) return std::nullopt;
    ExtentList ex;
    if (auto pa = s.frames.allocate(size, align)) {
        ex.push_back({*pa, size});                       // caminho rápido: contíguo
    } else if (!s.frames.allocate_scattered(size, ex)) { // fragmentado: scatter-gather
        return std::nullopt;
    } else {
        ++counters_.scattered_allocations;
        log_debug("MEM", "alocação de {} montada com {} extensões", human_bytes(size), ex.size());
    }
    s.used += size;
    s.peak = std::max(s.peak, s.used);
    for (const auto& e : ex) host_commit(e.pa, e.size);
    return ex;
}

void MemoryManager::give_back(Partition p, const ExtentList& ex) {
    auto& s = part(p);
    for (const auto& e : ex) {
        host_decommit(e.pa, e.size);
        s.frames.release(e.pa, e.size);
        s.used -= e.size;
    }
}

void MemoryManager::gather(const ExtentList& ex, std::uint8_t* dst, std::uint64_t len) const {
    for (const auto& e : ex) {
        if (!len) break;
        const auto n = std::min(len, e.size);
        std::memcpy(dst, base_ + e.pa, n);
        dst += n; len -= n;
    }
}

void MemoryManager::scatter(const ExtentList& ex, const std::uint8_t* src, std::uint64_t len) {
    for (const auto& e : ex) {
        if (!len) break;
        const auto n = std::min(len, e.size);
        std::memcpy(base_ + e.pa, src, n);
        src += n; len -= n;
    }
}

ExtentList MemoryManager::obtain_frames(Partition p, std::uint64_t size, std::uint64_t align,
                                     std::unique_lock<std::mutex>& lk, AllocationId protect) {
    if (auto pa = try_take(p, size, align)) return *pa;

    auto& s = part(p);
    if (p == Partition::User) {
        // 1) Streaming agressivo: os donos de recursos (ex.: TextureStreamer) liberam memória.
        const std::uint64_t need = s.used + size > s.budget ? s.used + size - s.budget : size;
        const auto handlers = reclaimers_;
        lk.unlock();
        std::uint64_t freed = 0;
        for (const auto& [_, h] : handlers) {
            freed += h(need, PressureLevel::Critical);
            if (freed >= need) break;
        }
        lk.lock();
        ++counters_.reclaim_runs;
        counters_.reclaimed_bytes += freed;
        if (freed) log_info("MEM", "reclaim: streaming liberou {} (necessário {})", human_bytes(freed), human_bytes(need));
        if (auto pa = try_take(p, size, align)) return *pa;

        // 2) Compressão forçada de dados frios.
        const std::uint64_t need2 = s.used + size > s.budget ? s.used + size - s.budget : size;
        if (compress_cold_locked(need2, protect) > 0)
            if (auto pa = try_take(p, size, align)) return *pa;
    }

    // 3) Sem saída: o título passou do limite do Series S.
    ++counters_.oom_events;
    const std::uint64_t avail = s.budget > s.used ? s.budget - s.used : 0;
    log_line(LogLevel::Warn, "", "Memory threshold exceeded on Series S backend.");
    log_warn("MEM", "{}: pedido {} | em uso {} de {} | livre no orçamento {}", to_string(p), human_bytes(size),
             human_bytes(s.used), human_bytes(s.budget), human_bytes(s.budget > s.used ? s.budget - s.used : 0));
    throw OutOfMemory(p, size, avail);
}

// ============================================================================
//  Alocações
// ============================================================================
AllocationId MemoryManager::allocate(Partition p, std::uint64_t size, std::string tag, std::uint32_t flags) {
    if (size == 0) throw std::invalid_argument("allocate: tamanho zero");
    size = round_up(size, kPage);
    const std::uint64_t align = size >= kLarge ? kLarge : kPage;   // permite páginas de 2 MB

    std::unique_lock lk(mu_);
    AllocationInfo a;
    a.extents = obtain_frames(p, size, align, lk, 0);
    a.id = next_id_++;
    a.partition = p;
    a.pa = a.extents.front().pa;
    a.size = size;
    a.tag = std::move(tag);
    a.flags = flags;
    a.last_use = ++clock_;
    ++part(p).allocations;
    const AllocationId id = a.id;
    allocs_.emplace(id, std::move(a));
    publish_pressure(lk);
    return id;
}

void MemoryManager::free(AllocationId id) {
    std::unique_lock lk(mu_);
    auto it = allocs_.find(id);
    if (it == allocs_.end()) throw std::invalid_argument(std::format("free: alocação {} inexistente", id));
    AllocationInfo& a = it->second;
    if (a.mapped) throw std::logic_error(std::format("free: alocação '{}' ainda está mapeada", a.tag));
    give_back(a.partition, a.compressed ? a.compressed_extents : a.extents);
    --part(a.partition).allocations;
    allocs_.erase(it);
    publish_pressure(lk);
}

void MemoryManager::touch(AllocationId id) {
    std::lock_guard lk(mu_);
    if (auto it = allocs_.find(id); it != allocs_.end()) it->second.last_use = ++clock_;
}

std::optional<AllocationInfo> MemoryManager::info(AllocationId id) const {
    std::lock_guard lk(mu_);
    if (auto it = allocs_.find(id); it != allocs_.end()) return it->second;
    return std::nullopt;
}

std::vector<std::span<std::uint8_t>> MemoryManager::host_spans(AllocationId id) {
    std::lock_guard lk(mu_);
    const auto& a = allocs_.at(id);
    if (a.compressed) throw std::logic_error("host_spans: alocação comprimida (use lock() antes)");
    if (a.locks == 0 && (a.flags & alloc_flags::kCompressible) && !a.mapped)
        throw std::logic_error("host_spans: alocação compressível precisa de lock()");
    std::vector<std::span<std::uint8_t>> v;
    for (const auto& e : a.extents) v.emplace_back(base_ + e.pa, e.size);
    return v;
}

void MemoryManager::read_allocation(AllocationId id, std::uint64_t offset, void* dst, std::uint64_t len) {
    lock(id);
    auto* out = static_cast<std::uint8_t*>(dst);
    {
        std::lock_guard lk(mu_);
        const auto& a = allocs_.at(id);
        if (offset + len > a.size) throw std::out_of_range("read_allocation: fora da alocação");
        for (const auto& e : a.extents) {
            if (offset >= e.size) { offset -= e.size; continue; }
            const auto n = std::min(len, e.size - offset);
            std::memcpy(out, base_ + e.pa + offset, n);
            out += n; len -= n; offset = 0;
            if (!len) break;
        }
    }
    unlock(id);
}

void MemoryManager::write_allocation(AllocationId id, std::uint64_t offset, const void* src, std::uint64_t len) {
    lock(id);
    auto* in = static_cast<const std::uint8_t*>(src);
    {
        std::lock_guard lk(mu_);
        const auto& a = allocs_.at(id);
        if (offset + len > a.size) throw std::out_of_range("write_allocation: fora da alocação");
        for (const auto& e : a.extents) {
            if (offset >= e.size) { offset -= e.size; continue; }
            const auto n = std::min(len, e.size - offset);
            std::memcpy(base_ + e.pa + offset, in, n);
            in += n; len -= n; offset = 0;
            if (!len) break;
        }
    }
    unlock(id);
}

GuestPA MemoryManager::acquire_locked(AllocationId id, std::unique_lock<std::mutex>& lk, bool take_lock) {
    auto it = allocs_.find(id);
    if (it == allocs_.end()) throw std::invalid_argument(std::format("acquire: alocação {} inexistente", id));
    it->second.last_use = ++clock_;
    if (it->second.compressed) {
        decompress_locked(it->second, lk);
        it = allocs_.find(id);                  // o lock pode ter sido liberado
        if (it == allocs_.end()) throw std::runtime_error("acquire: alocação liberada durante a descompressão");
    }
    if (take_lock) ++it->second.locks;
    const GuestPA pa = it->second.pa;
    publish_pressure(lk, id);                   // nunca comprime a própria alocação recém-adquirida
    return pa;
}

GuestPA MemoryManager::acquire(AllocationId id) {
    std::unique_lock lk(mu_);
    return acquire_locked(id, lk, false);
}

GuestPA MemoryManager::lock(AllocationId id) {
    std::unique_lock lk(mu_);
    return acquire_locked(id, lk, true);
}

void MemoryManager::unlock(AllocationId id) {
    std::lock_guard lk(mu_);
    if (auto it = allocs_.find(id); it != allocs_.end() && it->second.locks) --it->second.locks;
}

// ============================================================================
//  Compressão de memória fria
// ============================================================================
std::uint64_t MemoryManager::compress_cold(std::uint64_t target) {
    std::unique_lock lk(mu_);
    const auto saved = compress_cold_locked(target, 0);
    publish_pressure(lk);
    return saved;
}

std::uint64_t MemoryManager::compress_cold_locked(std::uint64_t target, AllocationId protect) {
    std::vector<AllocationInfo*> cand;
    for (auto& [id, a] : allocs_)
        if (id != protect && a.partition == Partition::User && (a.flags & alloc_flags::kCompressible) &&
            !(a.flags & alloc_flags::kPinned) && !a.compressed && a.mapped == 0 && a.locks == 0)
            cand.push_back(&a);
    std::sort(cand.begin(), cand.end(), [](auto* x, auto* y) { return x->last_use < y->last_use; });

    std::uint64_t saved = 0;
    for (AllocationInfo* a : cand) {
        if (saved >= target) break;
        std::vector<std::uint8_t> packed;
        if (a->contiguous()) {
            packed = codec::compress({base_ + a->pa, a->size}, codec::kDefaultBlock, 1);
        } else {
            std::vector<std::uint8_t> flat(a->size);
            gather(a->extents, flat.data(), a->size);
            packed = codec::compress(flat, codec::kDefaultBlock, 1);
        }
        const std::uint64_t frames = round_up(packed.size(), kPage);
        if (frames + kPage > a->size) continue;            // não compensa (dados aleatórios)
        // O payload está num buffer do host: libera o original primeiro (com o User Space
        // cheio não há frame livre antes disso) e então ocupa frames menores para o payload.
        give_back(Partition::User, a->extents);
        ExtentList dst;
        if (auto pa = user_.frames.allocate(frames, kPage)) dst.push_back({*pa, frames});
        else if (!user_.frames.allocate_scattered(frames, dst))
            throw std::logic_error("compress_cold: frames liberados não reutilizáveis");
        for (const auto& e : dst) host_commit(e.pa, e.size);
        scatter(dst, packed.data(), packed.size());
        user_.used += frames;
        a->compressed = true;
        a->compressed_extents = std::move(dst);
        a->compressed_size = packed.size();
        a->compressed_frames = frames;
        a->extents.clear();
        a->pa = 0;
        saved += a->size - frames;
        ++counters_.compressions;
        counters_.compression_saved += a->size - frames;
        log_info("MEM", "compressão forçada: '{}' {} → {} ({:.1f}×)", a->tag, human_bytes(a->size),
                 human_bytes(packed.size()), static_cast<double>(a->size) / static_cast<double>(packed.size()));
    }
    return saved;
}

void MemoryManager::decompress_locked(AllocationInfo& a_ref, std::unique_lock<std::mutex>& lk) {
    const AllocationId id = a_ref.id;
    const std::uint64_t size = a_ref.size;
    ExtentList ex = obtain_frames(Partition::User, size, size >= kLarge ? kLarge : kPage, lk, id);
    auto it = allocs_.find(id);
    if (it == allocs_.end()) { give_back(Partition::User, ex); return; }
    AllocationInfo& a = it->second;
    if (!a.compressed) { give_back(Partition::User, ex); return; }   // outro thread já resolveu
    std::vector<std::uint8_t> packed(a.compressed_size);
    gather(a.compressed_extents, packed.data(), packed.size());
    bool ok;
    if (ex.size() == 1) {
        ok = codec::decompress(packed, {base_ + ex[0].pa, size}, 1);
    } else {
        std::vector<std::uint8_t> flat(size);
        ok = codec::decompress(packed, flat, 1);
        if (ok) scatter(ex, flat.data(), size);
    }
    if (!ok) throw std::runtime_error(std::format("descompressão de '{}' falhou (memória corrompida)", a.tag));
    give_back(Partition::User, a.compressed_extents);
    a.extents = std::move(ex);
    a.pa = a.extents.front().pa;
    a.compressed = false;
    a.compressed_extents.clear();
    a.compressed_size = a.compressed_frames = 0;
    ++counters_.decompressions;
}

// ============================================================================
//  Paginação x86-64
// ============================================================================
GuestPA MemoryManager::alloc_page_table() {
    // Page tables pertencem ao sistema (são estruturas do kernel), não ao título.
    auto pa = system_.frames.allocate(kPage, kPage);
    if (!pa) throw OutOfMemory(Partition::System, kPage, 0);
    host_commit(*pa, kPage);
    std::memset(base_ + *pa, 0, kPage);
    system_.used += kPage;
    system_.peak = std::max(system_.peak, system_.used);
    ++counters_.page_table_pages;
    return *pa;
}

std::uint64_t* MemoryManager::entry(GuestPA table, unsigned i) {
    return reinterpret_cast<std::uint64_t*>(base_ + table) + i;
}

// Desce de PML4 (nível 3) até `stop_level` e devolve a entrada naquele nível.
std::uint64_t* MemoryManager::walk_to(GuestVA va, unsigned stop_level, bool create) {
    GuestPA table = cr3_;
    for (unsigned level = 3; level > stop_level; --level) {
        std::uint64_t* e = entry(table, idx(va, level));
        if (!(*e & pte::kPresent)) {
            if (!create) return nullptr;
            *e = alloc_page_table() | pte::kPresent | pte::kWritable | pte::kUser;
        } else if (*e & pte::kLargePage) {
            if (!create) return e;              // página grande cobre o endereço
            throw std::logic_error(std::format("map: 0x{:X} sobrepõe uma página de 2 MB existente", va));
        }
        table = *e & pte::kAddrMask;
    }
    return entry(table, idx(va, stop_level));
}

void MemoryManager::invalidate_tlb(GuestVA va, std::uint64_t size) {
    if (size >= kTlbSize * kPage) { for (auto& t : tlb_) t.vpn = ~0ull; return; }
    for (std::uint64_t off = 0; off < size; off += kPage) tlb_[((va + off) >> 12) % kTlbSize].vpn = ~0ull;
}

void MemoryManager::map(GuestVA va, GuestPA pa, std::uint64_t size, std::uint64_t flags) {
    if ((va | pa | size) & (kPage - 1)) throw std::invalid_argument("map: endereços/tamanho devem ser múltiplos de 4 KiB");
    std::lock_guard lk(mu_);
    std::uint64_t off = 0;
    while (off < size) {
        const GuestVA v = va + off;
        const GuestPA p = pa + off;
        if (!canonical(v)) throw std::invalid_argument("map: VA não canônico");
        if (((v | p) & (kLarge - 1)) == 0 && size - off >= kLarge) {
            std::uint64_t* pde = walk_to(v, 1, true);
            if ((*pde & pte::kPresent) && !(*pde & pte::kLargePage))
                throw std::logic_error(std::format("map: 0x{:X} já tem page table de 4 KB", v));
            *pde = (p & pte::kLargeMask) | flags | pte::kPresent | pte::kLargePage;
            ++counters_.large_mappings;
            off += kLarge;
        } else {
            std::uint64_t* e = walk_to(v, 0, true);
            *e = (p & pte::kAddrMask) | flags | pte::kPresent;
            ++counters_.small_mappings;
            off += kPage;
        }
    }
    invalidate_tlb(va, size);
}

GuestVA MemoryManager::map_allocation(GuestVA va, AllocationId id, std::uint64_t flags) {
    lock(id);                       // trava já na aquisição: nada comprime entre aqui e o ++mapped
    ExtentList ex;
    {
        std::lock_guard lk(mu_);
        auto& a = allocs_.at(id);
        ++a.mapped;                 // mapeadas não são comprimidas (o PA precisa ser estável)
        --a.locks;
        ex = a.extents;
    }
    // Extensões espalhadas viram um intervalo virtual contínuo.
    GuestVA v = va;
    for (const auto& e : ex) { map(v, e.pa, e.size, flags); v += e.size; }
    return va;
}

void MemoryManager::unmap(GuestVA va, std::uint64_t size) {
    std::lock_guard lk(mu_);
    std::uint64_t off = 0;
    while (off < size) {
        const GuestVA v = va + off;
        std::uint64_t* pde = walk_to(v, 1, false);
        if (pde && (*pde & pte::kPresent) && (*pde & pte::kLargePage)) {
            *pde = 0;
            off += kLarge - (v & (kLarge - 1));
            continue;
        }
        if (std::uint64_t* e = walk_to(v, 0, false)) *e = 0;
        off += kPage;
    }
    invalidate_tlb(va, size);
}

void MemoryManager::unmap_allocation(GuestVA va, AllocationId id) {
    std::uint64_t size;
    {
        std::lock_guard lk(mu_);
        auto& a = allocs_.at(id);
        if (a.mapped) --a.mapped;
        size = a.size;
    }
    unmap(va, size);
}

void MemoryManager::flush_tlb() { std::lock_guard lk(mu_); for (auto& t : tlb_) t.vpn = ~0ull; }

GuestPA MemoryManager::translate(GuestVA va, Access access) {
    const GuestVA vpn = va >> 12;
    std::lock_guard lk(mu_);
    TlbEntry& t = tlb_[vpn % kTlbSize];
    if (t.vpn != vpn) {
        ++counters_.tlb_misses;
        if (!canonical(va)) throw PageFault(va, access, "endereço não canônico");
        GuestPA table = cr3_;
        std::uint64_t e = 0;
        GuestPA frame = 0;
        for (unsigned level = 3;; --level) {
            std::uint64_t* pe = entry(table, idx(va, level));
            e = *pe;
            if (!(e & pte::kPresent)) throw PageFault(va, access, "página não presente");
            *pe |= pte::kAccessed;
            if (level == 1 && (e & pte::kLargePage)) {
                frame = (e & pte::kLargeMask) | (va & (kLarge - 1) & ~(kPage - 1));
                break;
            }
            if (level == 0) { frame = e & pte::kAddrMask; break; }
            table = e & pte::kAddrMask;
        }
        t = {vpn, frame, e};
    } else {
        ++counters_.tlb_hits;
    }
    if (access == Access::Write && !(t.flags & pte::kWritable)) throw PageFault(va, access, "página somente leitura");
    if (access == Access::Execute && (t.flags & pte::kNoExec))  throw PageFault(va, access, "página NX");
    return t.pfn | (va & (kPage - 1));
}

void MemoryManager::read_virtual(GuestVA va, void* dst, std::uint64_t len, Access a) {
    auto* out = static_cast<std::uint8_t*>(dst);
    while (len) {
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

// ============================================================================
//  Estatísticas
// ============================================================================
MemoryManager::Stats MemoryManager::stats() const {
    std::lock_guard lk(mu_);
    Stats s = counters_;
    auto fill = [](const PartitionState& p, PartitionStats& o) {
        o = {p.budget, p.used, p.peak, p.allocations, p.frames.free_bytes(), p.frames.largest_free_block(),
             p.frames.fragments()};
    };
    fill(system_, s.system);
    fill(user_, s.user);
    s.host_committed_estimate = system_.used + user_.used;
    return s;
}

std::string MemoryManager::report() const {
    const Stats s = stats();
    auto line = [](const char* name, const PartitionStats& p) {
        return std::format("  {:<16} {:>10} / {:<10} ({:5.1f}%)  pico {:>10}  {} alocações, {} fragmentos livres\n",
                           name, human_bytes(p.used), human_bytes(p.budget),
                           100.0 * static_cast<double>(p.used) / static_cast<double>(p.budget),
                           human_bytes(p.peak), p.allocations, p.fragments);
    };
    std::string r = "Memória unificada (10 GB GDDR6):\n";
    r += line("User Space", s.user);
    r += line("System Reserved", s.system);
    r += std::format("  Paginação: {} page tables, {} mapeamentos de 2 MB, {} de 4 KB | TLB {} hits / {} misses\n",
                     s.page_table_pages, s.large_mappings, s.small_mappings, s.tlb_hits, s.tlb_misses);
    r += std::format("  Pressão: {} reclaims ({} liberados), {} compressões ({} economizados), {} descompressões, "
                     "{} eventos de limite excedido | {} alocações scatter-gather\n",
                     s.reclaim_runs, human_bytes(s.reclaimed_bytes), s.compressions, human_bytes(s.compression_saved),
                     s.decompressions, s.oom_events, s.scattered_allocations);
    return r;
}

}  // namespace xss
