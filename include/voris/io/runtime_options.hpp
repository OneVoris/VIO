#pragma once

#include <cstddef>
#include <optional>

#include <voris/io/error.hpp>

namespace voris::io {

struct runtime_options {
    std::size_t shard_count{1};
    std::size_t queue_limit{1024};
    std::optional<std::size_t> cpu_affinity_start{};

    [[nodiscard]] void_result validate() const;
};

} // namespace voris::io
