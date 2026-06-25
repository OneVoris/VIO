#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace vio_bench {

[[nodiscard]] inline const char* platform_name() noexcept {
#if defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "other";
#endif
}

[[nodiscard]] inline std::int64_t elapsed_ns(
    std::chrono::steady_clock::time_point started,
    std::chrono::steady_clock::time_point finished) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started)
        .count();
}

[[nodiscard]] inline std::string throughput(std::size_t operations,
                                             std::int64_t elapsed_nanoseconds) {
    if (operations == 0 || elapsed_nanoseconds <= 0) {
        return "0";
    }

    const long double ops = static_cast<long double>(operations);
    const long double seconds =
        static_cast<long double>(elapsed_nanoseconds) / 1000000000.0L;
    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << (ops / seconds);
    return out.str();
}

[[nodiscard]] inline std::string percentile(std::vector<std::int64_t> samples,
                                            long double percentile_value) {
    if (samples.empty()) {
        return "unknown";
    }

    std::sort(samples.begin(), samples.end());
    const auto scaled =
        (percentile_value / 100.0L) * static_cast<long double>(samples.size());
    std::size_t index = static_cast<std::size_t>(scaled);
    if (scaled > static_cast<long double>(index)) {
        ++index;
    }
    if (index == 0) {
        index = 1;
    }
    return std::to_string(samples[index - 1]);
}

[[nodiscard]] inline std::string peak_rss_bytes() {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return "unknown";
    }
#if defined(__APPLE__)
    return std::to_string(static_cast<long long>(usage.ru_maxrss));
#else
    return std::to_string(static_cast<long long>(usage.ru_maxrss) * 1024LL);
#endif
#else
    return "unknown";
#endif
}

struct record {
    std::string_view benchmark;
    std::string_view workload;
    std::string_view result{"ok"};
    std::string_view reason{"ok"};
    std::size_t operations{};
    std::int64_t elapsed_ns{};
    std::string p50_ns{"unknown"};
    std::string p95_ns{"unknown"};
    std::string p99_ns{"unknown"};
    std::string peak_rss_bytes{vio_bench::peak_rss_bytes()};
    std::string allocations_per_operation{"unknown"};
    std::size_t errors{};
    std::size_t timeouts{};
    std::vector<std::pair<std::string_view, std::string>> extra{};
};

inline void emit(const record& current) {
    std::cout << "benchmark=" << current.benchmark
              << " environment=" << platform_name()
              << " platform=" << platform_name()
              << " workload=" << current.workload
              << " result=" << current.result
              << " reason=" << current.reason
              << " operations=" << current.operations
              << " elapsed_ns=" << current.elapsed_ns
              << " throughput_ops_per_sec="
              << throughput(current.operations, current.elapsed_ns)
              << " p50_ns=" << current.p50_ns
              << " p95_ns=" << current.p95_ns
              << " p99_ns=" << current.p99_ns
              << " peak_rss_bytes=" << current.peak_rss_bytes
              << " allocations_per_operation="
              << current.allocations_per_operation
              << " errors=" << current.errors
              << " timeouts=" << current.timeouts;
    for (const auto& [key, value] : current.extra) {
        std::cout << ' ' << key << '=' << value;
    }
    std::cout << '\n';
}

inline void set_latency_percentiles(record& current,
                                    const std::vector<std::int64_t>& samples) {
    current.p50_ns = percentile(samples, 50.0L);
    current.p95_ns = percentile(samples, 95.0L);
    current.p99_ns = percentile(samples, 99.0L);
}

} // namespace vio_bench
