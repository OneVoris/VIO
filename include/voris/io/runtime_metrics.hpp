#pragma once

#include <chrono>
#include <cstddef>

namespace voris::io {

struct runtime_metrics_config {
    // Zero is a valid threshold: any task measured above 0ns is counted as long.
    std::chrono::nanoseconds long_task_threshold{std::chrono::seconds(1)};
};

struct runtime_metrics {
    std::size_t submitted_tasks{};
    std::size_t completed_tasks{};
    std::size_t queue_depth{};
    std::size_t long_tasks{};
    // Max observed delay from accepted enqueue to the continuation starting on the shard.
    std::chrono::nanoseconds scheduler_lag{};
};

} // namespace voris::io
