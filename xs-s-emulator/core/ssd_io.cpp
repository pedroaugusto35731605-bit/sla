// =============================================================================
//  SeriesS-Emu — ssd_io.cpp
// =============================================================================
#include "ssd_io.hpp"

#include "codec.hpp"
#include "log.hpp"
#include "memory_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <numeric>
#include <thread>

namespace xss {

namespace {

using Clock = std::chrono::steady_clock;
double us_since(Clock::time_point t) { return std::chrono::duration<double, std::micro>(Clock::now() - t).count(); }

double transfer_us(std::uint64_t bytes, std::uint64_t bytes_s) {
    return static_cast<double>(bytes) * 1e6 / static_cast<double>(bytes_s);
}

// Modelo de tempo do pipeline. Entradas por requisição; saídas: issue/xfer/complete.
struct Timeline { std::vector<double> issue, xfer_done, complete; };

Timeline model(const spec::StorageProfile& p, double t0, const std::vector<std::uint64_t>& disk,
               const std::vector<std::uint64_t>& out, const std::vector<bool>& compressed,
               const std::vector<double>& cpu_decomp_us) {
    const std::size_t n = disk.size();
    Timeline tl{std::vector<double>(n), std::vector<double>(n), std::vector<double>(n)};
    const double latency = static_cast<double>(p.latency_ns) / 1e3;
    // NVMe e AHCI sobrepõem comandos; o HDD serializa a busca mecânica.
    const bool overlapped = p.latency_ns < 1'000'000;
    const std::size_t qd = overlapped ? (p.hw_decompression ? spec::kSsd.queue_depth : 32) : 1;
    double bus_free = t0, dec_free = t0;

    for (std::size_t i = 0; i < n; ++i) {
        // Um slot da fila só reabre quando a requisição i-QD termina.
        double issue = t0;
        if (i >= qd) issue = std::max(issue, tl.complete[i - qd]);
        double start;
        if (overlapped) start = std::max(issue + latency, bus_free);
        else            start = std::max(issue, bus_free) + latency;
        const double xfer = start + transfer_us(disk[i], p.bytes_s);
        bus_free = xfer;

        double done = xfer;
        if (compressed[i]) {
            // HW: bloco dedicado em pipeline; sem HW: a CPU descomprime (tempo real medido, 1 núcleo).
            const double dur = p.hw_decompression ? transfer_us(out[i], spec::kSsd.decompressed_max_bytes_s)
                                                  : cpu_decomp_us[i];
            const double ds = std::max(xfer, dec_free);
            done = ds + dur;
            dec_free = done;
        }
        tl.issue[i] = issue;
        tl.xfer_done[i] = xfer;
        tl.complete[i] = done;
    }
    return tl;
}

BatchReport make_report(const spec::StorageProfile& p, double t0, const Timeline& tl,
                        const std::vector<IoResult>& res) {
    BatchReport r;
    r.profile = std::string(p.name);
    r.requests = res.size();
    double end = t0, first = -1;
    for (std::size_t i = 0; i < res.size(); ++i) {
        if (!res[i].ok) { ++r.failed; continue; }
        r.bytes_read += res[i].bytes_read;
        r.bytes_out  += res[i].bytes_out;
        r.host_decompress_us += res[i].host_decompress_us;
        end = std::max(end, tl.complete[i]);
        first = first < 0 ? tl.complete[i] : std::min(first, tl.complete[i]);
    }
    r.sim_us = end - t0;
    r.first_data_us = first < 0 ? 0 : first - t0;
    return r;
}

}  // namespace

std::string BatchReport::summary() const {
    return std::format("{:<28} {:>4} req | {:>10} lidos → {:>10} entregues ({:.2f}×) | {:>10.3f} ms | "
                       "bruto {:5.2f} GB/s | efetivo {:5.2f} GB/s | 1º dado em {:.3f} ms",
                       profile, requests - failed, human_bytes(bytes_read), human_bytes(bytes_out), ratio(),
                       sim_us / 1e3, raw_gbs(), effective_gbs(), first_data_us / 1e3);
}

IoRequest& IoRequest::to_guest(MemoryManager& mem, AllocationId id) {
    const auto info = mem.info(id);
    if (!info) throw std::invalid_argument("IoRequest::to_guest: alocação inexistente");
    mem.lock(id);
    auto spans = mem.host_spans(id);
    if (spans.size() == 1) {
        dst = spans[0].data();
    } else {
        dst = nullptr;
        scatter = std::move(spans);
    }
    dst_capacity = info->size;
    guest_mem = &mem;
    guest_alloc = id;
    return *this;
}

// ============================================================================
SsdDevice::SsdDevice(spec::StorageProfile profile) : profile_(profile) {}

SsdDevice::~SsdDevice() {
    for (auto& f : files_) if (f && f->f) std::fclose(f->f);
}

int SsdDevice::open(const std::filesystem::path& p) {
    std::FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) throw std::runtime_error("SSD: não foi possível abrir " + p.string());
    auto of = std::make_unique<OpenFile>();
    of->f = f;
    of->path = p.string();
    std::error_code ec;
    of->size = std::filesystem::file_size(p, ec);
    std::lock_guard lk(mu_);
    files_.push_back(std::move(of));
    return static_cast<int>(files_.size() - 1);
}

void SsdDevice::close(int h) {
    std::lock_guard lk(mu_);
    if (h >= 0 && static_cast<std::size_t>(h) < files_.size() && files_[h]) {
        std::fclose(files_[h]->f);
        files_[h].reset();
    }
}

std::uint64_t SsdDevice::file_size(int h) const {
    std::lock_guard lk(mu_);
    if (h < 0 || static_cast<std::size_t>(h) >= files_.size() || !files_[h]) return 0;
    return files_[h]->size;
}

bool SsdDevice::host_read(int h, std::uint64_t offset, void* dst, std::uint64_t size) {
    OpenFile* f;
    {
        std::lock_guard lk(mu_);
        if (h < 0 || static_cast<std::size_t>(h) >= files_.size() || !files_[h]) return false;
        f = files_[h].get();
    }
    if (offset > f->size || size > f->size - offset) return false;
    std::lock_guard lk(f->mu);
#if defined(_WIN32)
    if (_fseeki64(f->f, static_cast<long long>(offset), SEEK_SET) != 0) return false;
#else
    if (fseeko(f->f, static_cast<off_t>(offset), SEEK_SET) != 0) return false;
#endif
    return std::fread(dst, 1, size, f->f) == size;
}

IoResult SsdDevice::read(int file, std::uint64_t offset, void* dst, std::uint64_t size) {
    std::vector<IoRequest> b(1);
    b[0].file = file; b[0].offset = offset; b[0].size = size;
    b[0].dst = static_cast<std::uint8_t*>(dst); b[0].dst_capacity = size;
    IoResult out;
    b[0].on_complete = [&](const IoResult& r) { out = r; };
    submit(b);
    return out;
}

BatchReport SsdDevice::submit(std::vector<IoRequest>& batch) {
    const auto wall0 = Clock::now();
    std::vector<IoResult> res(batch.size());
    std::vector<std::uint8_t> staging;

    // ---- execução real: lê do arquivo e descomprime no destino
    for (std::size_t i = 0; i < batch.size(); ++i) {
        IoRequest& q = batch[i];
        IoResult& r = res[i];
        const std::uint64_t out = q.compression == IoCompression::None ? q.size : q.uncompressed_size;
        const bool scattered = !q.scatter.empty();
        if ((!q.dst && !scattered) || out > q.dst_capacity) { r.error = "destino ausente ou pequeno demais"; continue; }

        // Destino espalhado: monta num buffer plano e distribui no fim (1 cópia extra).
        std::vector<std::uint8_t> flat;
        std::uint8_t* final_dst = q.dst;
        if (scattered) { flat.resize(out); final_dst = flat.data(); }

        const auto t_read = Clock::now();
        std::uint8_t* target = final_dst;
        if (q.compression != IoCompression::None) { staging.resize(q.size); target = staging.data(); }
        if (!host_read(q.file, q.offset, target, q.size)) { r.error = "erro de leitura / fora do arquivo"; continue; }
        r.host_read_us = us_since(t_read);
        r.bytes_read = q.size;

        if (q.compression == IoCompression::Xlz) {
            const auto t_dec = Clock::now();
            if (!codec::decompress({staging.data(), q.size}, {final_dst, out}, 1)) {
                r.error = "stream XLZ corrompido";
                continue;
            }
            r.host_decompress_us = us_since(t_dec);
        }
        if (scattered) {
            std::uint64_t off = 0;
            for (auto& s : q.scatter) {
                const auto n = std::min<std::uint64_t>(s.size(), out - off);
                std::memcpy(s.data(), flat.data() + off, n);
                off += n;
                if (off == out) break;
            }
        }
        r.bytes_out = out;
        r.ok = true;
    }

    // ---- relógio simulado do console
    std::vector<std::uint64_t> disk(batch.size()), outb(batch.size());
    std::vector<bool> comp(batch.size());
    std::vector<double> cpu(batch.size());
    for (std::size_t i = 0; i < batch.size(); ++i) {
        disk[i] = res[i].ok ? res[i].bytes_read : 0;
        outb[i] = res[i].bytes_out;
        comp[i] = batch[i].compression != IoCompression::None;
        cpu[i]  = res[i].host_decompress_us;
    }
    double t0;
    { std::lock_guard lk(mu_); t0 = clock_us_; }
    const Timeline tl = model(profile_, t0, disk, outb, comp, cpu);
    for (std::size_t i = 0; i < res.size(); ++i) {
        res[i].issue_us = tl.issue[i];
        res[i].transfer_done_us = tl.xfer_done[i];
        res[i].complete_us = tl.complete[i];
    }
    BatchReport rep = make_report(profile_, t0, tl, res);
    {
        std::lock_guard lk(mu_);
        clock_us_ = t0 + rep.sim_us;
        stats_.commands += batch.size();
        stats_.bytes_read += rep.bytes_read;
        stats_.bytes_out += rep.bytes_out;
        stats_.errors += rep.failed;
        stats_.busy_us += rep.sim_us;
        for (std::size_t i = 0; i < batch.size(); ++i) if (comp[i] && res[i].ok) ++stats_.decompressed_requests;
    }

    if (realtime_) {   // espera o tempo de parede alcançar o tempo simulado
        const double remain = rep.sim_us - us_since(wall0);
        if (remain > 0) std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(remain)));
    }

    // ---- conclusões na ordem em que o hardware terminaria
    std::vector<std::size_t> order(batch.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](auto a, auto b) { return res[a].complete_us < res[b].complete_us; });
    for (auto i : order) {
        if (!res[i].ok) log_warn("SSD", "falha em '{}': {}", batch[i].name, res[i].error);
        if (batch[i].guest_mem) {            // libera o lock do destino antes do callback
            batch[i].guest_mem->unlock(batch[i].guest_alloc);
            batch[i].guest_mem = nullptr;
        }
        if (batch[i].on_complete) batch[i].on_complete(res[i]);
    }
    rep.host_wall_us = us_since(wall0);
    return rep;
}

BatchReport SsdDevice::simulate(const spec::StorageProfile& p, const std::vector<IoResult>& executed,
                                const std::vector<IoRequest>& reqs) {
    std::vector<std::uint64_t> disk(executed.size()), out(executed.size());
    std::vector<bool> comp(executed.size());
    std::vector<double> cpu(executed.size());
    for (std::size_t i = 0; i < executed.size(); ++i) {
        disk[i] = executed[i].ok ? executed[i].bytes_read : 0;
        out[i]  = executed[i].bytes_out;
        comp[i] = reqs[i].compression != IoCompression::None;
        cpu[i]  = executed[i].host_decompress_us;
    }
    const Timeline tl = model(p, 0.0, disk, out, comp, cpu);
    return make_report(p, 0.0, tl, executed);
}

SsdDevice::Stats SsdDevice::stats() const { std::lock_guard lk(mu_); return stats_; }

}  // namespace xss
