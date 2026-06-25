#include <voris/io/timer.hpp>

#include "benchmark_support.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    timer_heap heap;
    constexpr std::size_t count = 4096;
    std::vector<std::int64_t> samples;
    samples.reserve(count);

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i != count; ++i) {
        const auto operation_started = std::chrono::steady_clock::now();
        (void)heap.add(virtual_monotonic_clock::time_point{i * 1ms});
        samples.push_back(vio_bench::elapsed_ns(operation_started,
                                                std::chrono::steady_clock::now()));
    }
    const auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{count * 1ms});
    const auto finished = std::chrono::steady_clock::now();

    vio_bench::record record{
        .benchmark = "timer_heap",
        .workload = "timer_heap_add_pop_ready",
        .result = ready.size() == count ? "ok" : "failed",
        .reason = ready.size() == count ? "ok" : "ready_count_mismatch",
        .operations = count + ready.size(),
        .elapsed_ns = vio_bench::elapsed_ns(started, finished),
        .errors = ready.size() == count ? 0U : 1U,
        .extra = {{"timers_added", std::to_string(count)},
                  {"timers_ready", std::to_string(ready.size())}}};
    vio_bench::set_latency_percentiles(record, samples);
    vio_bench::emit(record);
    return record.result == "ok" ? 0 : 1;
}
