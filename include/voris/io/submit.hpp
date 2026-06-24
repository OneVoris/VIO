#pragma once

#include <voris/io/shard.hpp>

namespace voris::io {

[[nodiscard]] inline void_result submit_to(shard& target, continuation message) {
    return target.submit(std::move(message));
}

} // namespace voris::io
