// =============================================================================
//  SeriesS-Emu — log.hpp
//  Logger thread-safe mínimo. Formato:  [NIVEL] [MÓDULO] mensagem
//  SERIESS_LOG=quiet suprime INFO/DEBUG (WARN/ERROR sempre aparecem).
// =============================================================================
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <string_view>

namespace xss {

enum class LogLevel { Debug, Info, Warn, Error };

namespace detail {
inline std::mutex& log_mutex() { static std::mutex m; return m; }
inline std::atomic<int>& log_threshold() {
    static std::atomic<int> t = [] {
        const char* e = std::getenv("SERIESS_LOG");
        if (e && std::strcmp(e, "quiet") == 0) return static_cast<int>(LogLevel::Warn);
        if (e && std::strcmp(e, "debug") == 0) return static_cast<int>(LogLevel::Debug);
        return static_cast<int>(LogLevel::Info);
    }();
    return t;
}
}  // namespace detail

inline void set_log_level(LogLevel l) { detail::log_threshold() = static_cast<int>(l); }

inline void log_line(LogLevel lvl, std::string_view module, std::string_view msg) {
    if (static_cast<int>(lvl) < detail::log_threshold()) return;
    static constexpr const char* kTag[] = {"DEBUG", "INFO", "WARN", "ERROR"};
    std::lock_guard lk(detail::log_mutex());
    std::FILE* out = lvl >= LogLevel::Warn ? stderr : stdout;
    if (module.empty())
        std::fprintf(out, "[%s] %.*s\n", kTag[static_cast<int>(lvl)], static_cast<int>(msg.size()), msg.data());
    else
        std::fprintf(out, "[%s] [%.*s] %.*s\n", kTag[static_cast<int>(lvl)], static_cast<int>(module.size()),
                     module.data(), static_cast<int>(msg.size()), msg.data());
    std::fflush(out);
}

template <typename... A> void log_info(std::string_view mod, std::format_string<A...> f, A&&... a) {
    log_line(LogLevel::Info, mod, std::format(f, std::forward<A>(a)...));
}
template <typename... A> void log_warn(std::string_view mod, std::format_string<A...> f, A&&... a) {
    log_line(LogLevel::Warn, mod, std::format(f, std::forward<A>(a)...));
}
template <typename... A> void log_error(std::string_view mod, std::format_string<A...> f, A&&... a) {
    log_line(LogLevel::Error, mod, std::format(f, std::forward<A>(a)...));
}
template <typename... A> void log_debug(std::string_view mod, std::format_string<A...> f, A&&... a) {
    log_line(LogLevel::Debug, mod, std::format(f, std::forward<A>(a)...));
}

inline std::string human_bytes(std::uint64_t b) {
    const char* u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(b);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    return i == 0 ? std::format("{} B", b) : std::format("{:.2f} {}", v, u[i]);
}

}  // namespace xss
