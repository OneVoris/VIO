#pragma once

#include <cerrno>

namespace voris::io::detail {

[[nodiscard]] constexpr bool is_accept_retryable_pending_network_error(
    int provider_code) noexcept {
#if defined(ENETDOWN)
    if (provider_code == ENETDOWN) {
        return true;
    }
#endif
#if defined(EPROTO)
    if (provider_code == EPROTO) {
        return true;
    }
#endif
#if defined(ENOPROTOOPT)
    if (provider_code == ENOPROTOOPT) {
        return true;
    }
#endif
#if defined(EHOSTDOWN)
    if (provider_code == EHOSTDOWN) {
        return true;
    }
#endif
#if defined(ENONET)
    if (provider_code == ENONET) {
        return true;
    }
#endif
#if defined(EHOSTUNREACH)
    if (provider_code == EHOSTUNREACH) {
        return true;
    }
#endif
#if defined(EOPNOTSUPP)
    if (provider_code == EOPNOTSUPP) {
        return true;
    }
#endif
#if defined(ENETUNREACH)
    if (provider_code == ENETUNREACH) {
        return true;
    }
#endif
    return false;
}

} // namespace voris::io::detail
