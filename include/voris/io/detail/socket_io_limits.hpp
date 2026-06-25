#pragma once

#include <cstddef>
#include <limits>

namespace voris::io::detail {

[[nodiscard]] constexpr std::size_t max_safe_socket_io_size() noexcept {
#if defined(__linux__)
    return std::size_t{0x7ffff000};
#else
    return static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
#endif
}

[[nodiscard]] constexpr std::size_t cap_socket_io_size(std::size_t requested) noexcept {
    const std::size_t cap = max_safe_socket_io_size();
    return requested < cap ? requested : cap;
}

struct socket_iovec_plan {
    std::size_t iovec_count = 0;
    std::size_t requested_size = 0;
};

[[nodiscard]] constexpr bool socket_iovec_plan_full(
    const socket_iovec_plan& plan,
    std::size_t iovec_limit,
    std::size_t request_limit = max_safe_socket_io_size()) noexcept {
    return plan.iovec_count >= iovec_limit || plan.requested_size >= request_limit;
}

[[nodiscard]] constexpr std::size_t append_socket_iovec_segment(
    socket_iovec_plan& plan,
    std::size_t segment_size,
    std::size_t iovec_limit,
    std::size_t request_limit = max_safe_socket_io_size()) noexcept {
    if (segment_size == 0 || socket_iovec_plan_full(plan, iovec_limit, request_limit)) {
        return 0;
    }

    const std::size_t remaining = request_limit - plan.requested_size;
    const std::size_t accepted = segment_size < remaining ? segment_size : remaining;
    ++plan.iovec_count;
    plan.requested_size += accepted;
    return accepted;
}

} // namespace voris::io::detail
