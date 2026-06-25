#include <voris/io/shard.hpp>

#include "benchmark_support.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace voris::io;

    constexpr std::size_t count = 4096;
    shard current(count + 16);
    std::vector<std::chrono::steady_clock::time_point> submitted(count);
    std::vector<std::int64_t> hop_latencies(count);
    std::size_t ran = 0;
    std::size_t errors = 0;

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i != count; ++i) {
        submitted[i] = std::chrono::steady_clock::now();
        auto scheduled = current.submit([&, i] {
            ++ran;
            hop_latencies[i] = vio_bench::elapsed_ns(submitted[i],
                                                     std::chrono::steady_clock::now());
        });
        if (!scheduled.has_value()) {
            ++errors;
        }
    }
    const auto drained = current.drain();
    const auto finished = std::chrono::steady_clock::now();

    vio_bench::record record{
        .benchmark = "scheduler_hops",
        .workload = "scheduler_hops",
        .result = (ran == count && drained == count && errors == 0) ? "ok" : "failed",
        .reason = (ran == count && drained == count && errors == 0) ? "ok" : "count_mismatch",
        .operations = ran,
        .elapsed_ns = vio_bench::elapsed_ns(started, finished),
        .errors = errors + (ran == count && drained == count ? 0U : 1U),
        .extra = {{"submitted", std::to_string(count)},
                  {"drained", std::to_string(drained)}}};
    if (ran == count && drained == count) {
        vio_bench::set_latency_percentiles(record, hop_latencies);
    }
    vio_bench::emit(record);
    return record.result == "ok" ? 0 : 1;
}
