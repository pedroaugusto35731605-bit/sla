// =============================================================================
//  SeriesS-Emu — texture_streamer.cpp
// =============================================================================
#include "texture_streamer.hpp"

#include "../core/log.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace xss::fs {

TextureStreamer::TextureStreamer(MemoryManager& mem, SsdDevice& ssd, Config cfg) : mem_(mem), ssd_(ssd), cfg_(cfg) {
    // O MemoryManager pede memória → despejamos mips frios.
    reclaim_handle_ = mem_.add_reclaim_handler([this](std::uint64_t needed, PressureLevel) {
        return evict(needed, "reclaim do MemoryManager");
    });
    // Pressão sobe → streaming mais agressivo (mips mais grossos).
    listener_handle_ = mem_.add_pressure_listener([this](PressureLevel, PressureLevel to) {
        const std::uint32_t nb = to == PressureLevel::Critical ? 2 : to == PressureLevel::Elevated ? 1 : 0;
        if (nb != bias_) {
            log_info("STREAM", "pressão {} → mip bias {} ({})", to_string(to), nb,
                     nb ? "streaming agressivo: texturas em resolução reduzida" : "resolução total");
            bias_ = nb;
        }
    });
}

TextureStreamer::~TextureStreamer() {
    mem_.remove_reclaim_handler(reclaim_handle_);
    mem_.remove_pressure_listener(listener_handle_);
    for (auto& t : textures_)
        for (AllocationId id : t.resident)
            if (id) try { mem_.free(id); } catch (...) {}
    if (file_ >= 0) ssd_.close(file_);
}

std::size_t TextureStreamer::register_image(const std::filesystem::path& image, const GameMetadata& meta) {
    if (file_ < 0) file_ = ssd_.open(image);
    std::size_t added = 0;
    for (const auto& f : meta.files) {
        if (f.is_dir || f.size < kXtexHeader || !f.path.ends_with(".xtex")) continue;
        std::uint8_t hdr[kXtexHeader];
        if (!ssd_.read(file_, f.offset, hdr, kXtexHeader).ok) continue;
        std::uint32_t magic, w, h, n;
        std::memcpy(&magic, hdr, 4);
        std::memcpy(&w, hdr + 8, 4);
        std::memcpy(&h, hdr + 12, 4);
        std::memcpy(&n, hdr + 16, 4);
        if (magic != kXtexMagic || n == 0 || n > 16 || kXtexHeader + n * kXtexMipEntry > f.size) {
            log_warn("STREAM", "{}: cabeçalho XTEX inválido", f.path);
            continue;
        }
        std::vector<std::uint8_t> table(n * kXtexMipEntry);
        if (!ssd_.read(file_, f.offset + kXtexHeader, table.data(), table.size()).ok) continue;

        StreamedTexture t;
        t.name = f.path;
        t.width = w;
        t.height = h;
        bool ok = true;
        for (std::uint32_t m = 0; m < n; ++m) {
            const std::uint8_t* e = table.data() + m * kXtexMipEntry;
            MipLevel lv;
            std::uint32_t comp;
            std::memcpy(&lv.offset, e, 8);
            std::memcpy(&lv.disk_size, e + 8, 8);
            std::memcpy(&lv.raw_size, e + 16, 8);
            std::memcpy(&comp, e + 24, 4);
            if (lv.offset + lv.disk_size > f.size) { ok = false; break; }
            lv.offset += f.offset;
            lv.compressed = comp == 1;
            lv.width = std::max(1u, w >> m);
            lv.height = std::max(1u, h >> m);
            t.mips.push_back(lv);
        }
        if (!ok) { log_warn("STREAM", "{}: mip fora do arquivo", f.path); continue; }
        t.resident.assign(n, 0);
        t.last_used.assign(n, 0);
        textures_.push_back(std::move(t));
        ++added;
    }
    log_info("STREAM", "{} texturas registradas ({} em mip 0, pool de {})", added, human_bytes(full_set_bytes()),
             human_bytes(cfg_.pool_budget));
    return added;
}

std::uint64_t TextureStreamer::full_set_bytes() const {
    std::uint64_t s = 0;
    for (const auto& t : textures_) for (const auto& m : t.mips) s += m.raw_size;
    return s;
}

void TextureStreamer::request(std::size_t tex, std::uint32_t mip) {
    if (tex < textures_.size()) requests_.push_back({tex, mip});
}

// Despeja mips residentes não pedidos neste frame, do menos recente para o mais recente.
std::uint64_t TextureStreamer::evict(std::uint64_t needed, const char* reason) {
    struct Victim { std::size_t tex; std::uint32_t mip; std::uint64_t last; };
    std::vector<Victim> v;
    for (std::size_t i = 0; i < textures_.size(); ++i)
        for (std::uint32_t m = 0; m < textures_[i].mips.size(); ++m)
            if (textures_[i].resident[m] && textures_[i].last_used[m] < frame_)
                v.push_back({i, m, textures_[i].last_used[m]});
    // Mais antigo primeiro; empate → o mip mais fino (maior) sai antes.
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.last != b.last ? a.last < b.last : a.mip < b.mip; });

    std::uint64_t freed = 0;
    std::uint32_t count = 0;
    for (const auto& x : v) {
        if (freed >= needed) break;
        auto& t = textures_[x.tex];
        const auto info = mem_.info(t.resident[x.mip]);
        mem_.free(t.resident[x.mip]);
        t.resident[x.mip] = 0;
        const std::uint64_t sz = info ? info->size : t.mips[x.mip].raw_size;
        pool_used_ -= std::min(pool_used_, sz);
        freed += sz;
        ++count;
    }
    if (count) {
        frame_evictions_ += count;
        stats_.evictions += count;
        log_info("STREAM", "despejo ({}): {} mips, {} liberados", reason, count, human_bytes(freed));
    }
    return freed;
}

TextureStreamer::FrameReport TextureStreamer::update() {
    FrameReport rep;
    rep.frame = frame_;
    frame_evictions_ = 0;

    // 1. Resolve o mip efetivo (com bias de pressão) e marca o uso deste frame.
    struct Load { std::size_t tex; std::uint32_t mip; };
    std::vector<Load> wanted;
    for (const auto& r : requests_) {
        auto& t = textures_[r.tex];
        const std::uint32_t mip = std::min<std::uint32_t>(r.mip + bias_, static_cast<std::uint32_t>(t.mips.size() - 1));
        ++rep.requested;
        t.last_used[mip] = frame_;
        if (t.resident[mip]) { ++rep.hits; mem_.touch(t.resident[mip]); continue; }
        if (std::none_of(wanted.begin(), wanted.end(), [&](auto& w) { return w.tex == r.tex && w.mip == mip; }))
            wanted.push_back({r.tex, mip});
    }
    requests_.clear();

    // 2. Aloca memória para cada carga (respeitando o pool e o orçamento global de 7 GB).
    std::vector<IoRequest> batch;
    for (const auto& w : wanted) {
        if (batch.size() >= cfg_.max_loads_per_frame) { ++rep.deferred; continue; }
        auto& t = textures_[w.tex];
        const MipLevel& lv = t.mips[w.mip];
        if (pool_used_ + lv.raw_size > cfg_.pool_budget)
            evict(pool_used_ + lv.raw_size - cfg_.pool_budget, "pool de texturas cheio");
        AllocationId id = 0;
        try {
            id = mem_.allocate(Partition::User, lv.raw_size, std::format("textura {} mip {}", t.name, w.mip));
        } catch (const OutOfMemory&) {
            ++rep.deferred;      // o aviso de limite já foi emitido pelo MemoryManager
            continue;
        }
        t.resident[w.mip] = id;              // reservado; os bytes chegam no lote abaixo
        pool_used_ += mem_.info(id)->size;
        IoRequest q;
        q.file = file_;
        q.offset = lv.offset;
        q.size = lv.disk_size;
        q.compression = lv.compressed ? IoCompression::Xlz : IoCompression::None;
        q.uncompressed_size = lv.raw_size;
        q.name = std::format("{} mip{}", t.name, w.mip);
        q.to_guest(mem_, id);
        // Leitura falhou (imagem truncada/corrompida): devolve a memória reservada.
        q.on_complete = [this, tex = w.tex, mip = w.mip](const IoResult& r) {
            if (r.ok) return;
            auto& tt = textures_[tex];
            if (const auto inf = mem_.info(tt.resident[mip])) pool_used_ -= std::min(pool_used_, inf->size);
            mem_.free(tt.resident[mip]);
            tt.resident[mip] = 0;
        };
        batch.push_back(std::move(q));
    }

    // 3. Um lote DirectStorage para o frame inteiro.
    if (!batch.empty()) {
        const BatchReport br = ssd_.submit(batch);
        rep.loads = static_cast<std::uint32_t>(batch.size() - br.failed);
        rep.disk_bytes = br.bytes_read;
        rep.raw_bytes = br.bytes_out;
        rep.io_ms = br.sim_us / 1e3;
    }

    rep.evictions = frame_evictions_;
    rep.pool_used = pool_used_;
    rep.mip_bias = bias_;
    rep.pressure = mem_.pressure();
    stats_.loads += rep.loads;
    stats_.deferred += rep.deferred;
    stats_.disk_bytes += rep.disk_bytes;
    stats_.raw_bytes += rep.raw_bytes;
    stats_.io_ms += rep.io_ms;
    ++stats_.frames;
    ++frame_;
    return rep;
}

}  // namespace xss::fs
