#pragma once

#include <cstddef>
#include <chrono>

namespace voris::io {

struct runtime_metrics {
    std::size_t submitted_tasks{};
    std::size_t completed_tasks{};
    std::size_t queue_depth{};
    std::size_t long_tasks{};
    std::chrono::nanoseconds scheduler_lag{};
};

} // namespace voris::io
