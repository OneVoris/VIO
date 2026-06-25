#include <voris/io/async_scope.hpp>
#include <voris/io/scheduler.hpp>
#include <voris/io/task.hpp>
#include <voris/io/test_scheduler.hpp>

#include "benchmark_support.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace {

voris::io::task<void> spawned_noop() {
    co_return;
}

} // namespace

int main() {
    using namespace voris::io;

    constexpr std::size_t count = 4096;
    test_scheduler scheduler;
    current_scheduler_scope scheduler_scope{scheduler_ref(scheduler)};
    async_scope scope;
    std::vector<std::int64_t> samples;
    samples.reserve(count);
    std::size_t spawned = 0;
    std::size_t errors = 0;

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i != count; ++i) {
        const auto operation_started = std::chrono::steady_clock::now();
        auto result = scope.spawn(spawned_noop());
        samples.push_back(vio_bench::elapsed_ns(operation_started,
                                                std::chrono::steady_clock::now()));
        if (result.has_value()) {
            ++spawned;
        } else {
            ++errors;
        }
    }
    auto joined = scope.join();
    if (!joined.has_value()) {
        ++errors;
    }
    const auto finished = std::chrono::steady_clock::now();

    const bool ok = spawned == count && errors == 0 && scope.pending_count() == 0;
    vio_bench::record record{
        .benchmark = "task_spawn",
        .workload = "task_spawn_completed_noop",
        .result = ok ? "ok" : "failed",
        .reason = ok ? "ok" : "spawn_or_join_failed",
        .operations = spawned,
        .elapsed_ns = vio_bench::elapsed_ns(started, finished),
        .errors = errors + (ok ? 0U : 1U),
        .extra = {{"spawned", std::to_string(spawned)},
                  {"pending", std::to_string(scope.pending_count())}}};
    vio_bench::set_latency_percentiles(record, samples);
    vio_bench::emit(record);
    return ok ? 0 : 1;
}
