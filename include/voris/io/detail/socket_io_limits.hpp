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

} // namespace voris::io::detail
