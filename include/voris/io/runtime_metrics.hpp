#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>

namespace voris::io {

struct runtime_metrics {
    std::atomic<std::size_t> submitted_tasks{};
    std::atomic<std::size_t> completed_tasks{};
    std::atomic<std::size_t> queue_depth{};
    std::atomic<std::size_t> long_tasks{};
    std::chrono::nanoseconds scheduler_lag{};
};

} // namespace voris::io
