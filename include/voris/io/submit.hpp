#pragma once

#include <voris/io/shard.hpp>

namespace voris::io {

[[nodiscard]] inline void_result submit_to(scheduler_ref target, continuation message) {
    return target.schedule(std::move(message));
}

[[nodiscard]] inline void_result submit_to(shard& target, continuation message) {
    return submit_to(target.scheduler(), std::move(message));
}

} // namespace voris::io
