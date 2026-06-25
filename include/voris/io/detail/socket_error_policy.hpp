#pragma once

#include <cerrno>

#include <voris/io/detail/socket_accept_errors.hpp>

namespace voris::io::detail {

enum class socket_errno_action {
    retry,
    operation_in_progress,
    provider_failure,
};

[[nodiscard]] constexpr bool is_socket_interrupted_errno(int provider_code) noexcept {
#if defined(EINTR)
    if (provider_code == EINTR) {
        return true;
    }
#endif
    return false;
}

[[nodiscard]] constexpr bool is_socket_would_block_errno(int provider_code) noexcept {
#if defined(EAGAIN)
    if (provider_code == EAGAIN) {
        return true;
    }
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || EWOULDBLOCK != EAGAIN)
    if (provider_code == EWOULDBLOCK) {
        return true;
    }
#endif
    return false;
}

[[nodiscard]] constexpr bool is_socket_connect_in_progress_errno(
    int provider_code) noexcept {
#if defined(EINPROGRESS)
    if (provider_code == EINPROGRESS) {
        return true;
    }
#endif
#if defined(EALREADY)
    if (provider_code == EALREADY) {
        return true;
    }
#endif
    return is_socket_would_block_errno(provider_code);
}

[[nodiscard]] constexpr socket_errno_action classify_socket_transfer_errno(
    int provider_code) noexcept {
    if (is_socket_interrupted_errno(provider_code)) {
        return socket_errno_action::retry;
    }
    if (is_socket_would_block_errno(provider_code)) {
        return socket_errno_action::operation_in_progress;
    }
    return socket_errno_action::provider_failure;
}

[[nodiscard]] constexpr socket_errno_action classify_socket_accept_errno(
    int provider_code) noexcept {
    if (is_socket_interrupted_errno(provider_code) ||
        is_accept_retryable_pending_network_error(provider_code)) {
        return socket_errno_action::retry;
    }
    if (is_socket_would_block_errno(provider_code)) {
        return socket_errno_action::operation_in_progress;
    }
    return socket_errno_action::provider_failure;
}

[[nodiscard]] constexpr socket_errno_action classify_socket_connect_errno(
    int provider_code) noexcept {
    if (is_socket_interrupted_errno(provider_code) ||
        is_socket_connect_in_progress_errno(provider_code)) {
        return socket_errno_action::operation_in_progress;
    }
    return socket_errno_action::provider_failure;
}

} // namespace voris::io::detail
