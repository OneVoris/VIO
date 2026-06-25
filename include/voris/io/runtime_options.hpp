#pragma once

#include <cstddef>
#include <optional>

#include <voris/io/error.hpp>
#include <voris/io/loop_budget.hpp>
#include <voris/io/runtime_metrics.hpp>

namespace voris::io {

struct runtime_options {
    std::size_t shard_count{1};
    std::size_t queue_limit{1024};
    std::optional<std::size_t> cpu_affinity_start{};
    loop_budget loop_budget{};
    runtime_metrics_config metrics_config{};

    [[nodiscard]] void_result validate() const;
};

} // namespace voris::io
