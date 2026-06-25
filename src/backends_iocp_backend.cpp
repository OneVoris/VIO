#include <voris/io/backends/iocp_backend.hpp>

#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#endif

namespace voris::io::backends {

namespace {

#if defined(_WIN32)
[[nodiscard]] vio_error provider_failure(DWORD provider_code) {
    return make_error(vio_error_code::backend_failure,
                      static_cast<std::int64_t>(provider_code));
}

[[nodiscard]] void_result closed_error() {
    return std::unexpected(make_error(vio_error_code::closed));
}
#else
[[nodiscard]] vio_error unsupported_error() {
    return make_error(vio_error_code::unsupported, "IOCP backend is unavailable");
}
#endif

} // namespace

iocp_backend::iocp_backend(iocp_backend_options options) : options_(options) {
    if (options_.completion_batch_limit == 0) {
        options_.completion_batch_limit = 1;
    }

#if defined(_WIN32)
    completion_port_ =
        ::CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
    if (completion_port_ == nullptr) {
        initialization_error_ = provider_failure(::GetLastError());
    }
#endif
}

iocp_backend::~iocp_backend() {
#if defined(_WIN32)
    close_owned_port();
#endif
}

std::size_t iocp_backend::completion_batch_limit() const noexcept {
    return options_.completion_batch_limit;
}

#if defined(_WIN32)
void iocp_backend::close_owned_port() noexcept {
    if (completion_port_ != nullptr) {
        (void)::CloseHandle(static_cast<HANDLE>(completion_port_));
        completion_port_ = nullptr;
    }
}

void_result iocp_backend::initialization_result() const {
    if (initialization_error_.has_value()) {
        return std::unexpected(*initialization_error_);
    }
    return {};
}

io_result<std::size_t> iocp_backend::observe_native_completions() {
    if (completion_port_ == nullptr) {
        return std::unexpected(make_error(vio_error_code::closed));
    }

    std::vector<OVERLAPPED_ENTRY> entries(options_.completion_batch_limit);
    ULONG removed = 0;
    if (::GetQueuedCompletionStatusEx(static_cast<HANDLE>(completion_port_),
                                      entries.data(),
                                      static_cast<ULONG>(entries.size()),
                                      &removed, 0, FALSE) == 0) {
        const DWORD provider_code = ::GetLastError();
        if (provider_code == WAIT_TIMEOUT) {
            return 0U;
        }
        return std::unexpected(provider_failure(provider_code));
    }

    std::size_t observed = 0;
    for (ULONG index = 0; index < removed; ++index) {
        const auto& entry = entries[static_cast<std::size_t>(index)];
        const auto key = static_cast<std::uintptr_t>(entry.lpCompletionKey);
        if (detail::is_iocp_wake_completion_key(key) &&
            entry.lpOverlapped == nullptr) {
            ++observed;
            continue;
        }

        const auto token = detail::unpack_iocp_completion_key(key);
        if (token.has_value() && !fallback_.is_current_handle(*token)) {
            ++observed;
            continue;
        }
        if (!token.has_value()) {
            ++observed;
            continue;
        }

        // M7-003 will map current OVERLAPPED storage to user completions. M7-002
        // only preserves the native packet and proves stale generation handling.
        native_packets_.push_back(detail::iocp_native_completion_packet{
            static_cast<std::size_t>(entry.dwNumberOfBytesTransferred),
            key,
            entry.lpOverlapped,
            static_cast<std::uintptr_t>(entry.Internal),
        });
        ++observed;
    }
    return observed;
}
#endif

io_result<backend_handle_token> iocp_backend::register_handle(std::size_t native_handle) {
#if defined(_WIN32)
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return std::unexpected(initialized.error());
    }

    auto token = fallback_.register_handle(native_handle);
    if (!token.has_value()) {
        return token;
    }

    auto completion_key = detail::pack_iocp_completion_key(*token);
    if (!completion_key.has_value()) {
        (void)fallback_.close_handle(*token);
        return std::unexpected(completion_key.error());
    }

    const auto associated = ::CreateIoCompletionPort(
        reinterpret_cast<HANDLE>(native_handle),
        static_cast<HANDLE>(completion_port_),
        static_cast<ULONG_PTR>(*completion_key), 0);
    if (associated == nullptr) {
        const DWORD provider_code = ::GetLastError();
        (void)fallback_.close_handle(*token);
        return std::unexpected(provider_failure(provider_code));
    }

    return token;
#else
    (void)native_handle;
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::submit(backend_operation operation) {
#if defined(_WIN32)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    return fallback_.submit(operation);
#else
    (void)operation;
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
#if defined(_WIN32)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    return fallback_.cancel(operation_id, reason);
#else
    (void)operation_id;
    (void)reason;
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::close_handle(backend_handle_token token) {
#if defined(_WIN32)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    return fallback_.close_handle(token);
#else
    (void)token;
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> iocp_backend::poll() {
#if defined(_WIN32)
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (initialization_error_.has_value()) {
        return std::unexpected(*initialization_error_);
    }

    auto native_observed = observe_native_completions();
    if (!native_observed.has_value()) {
        return std::unexpected(native_observed.error());
    }
    auto fallback_visible = fallback_.poll();
    if (!fallback_visible.has_value()) {
        return std::unexpected(fallback_visible.error());
    }
    return *native_observed + *fallback_visible;
#else
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> iocp_backend::drain_completions(
    std::span<backend_completion> out) {
#if defined(_WIN32)
    return fallback_.drain_completions(out);
#else
    (void)out;
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::wake() {
#if defined(_WIN32)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }

    if (::PostQueuedCompletionStatus(static_cast<HANDLE>(completion_port_), 0,
                                     static_cast<ULONG_PTR>(
                                         detail::iocp_wake_completion_key),
                                     nullptr) == 0) {
        return std::unexpected(provider_failure(::GetLastError()));
    }
    return {};
#else
    return std::unexpected(unsupported_error());
#endif
}

void_result iocp_backend::shutdown() {
#if defined(_WIN32)
    if (stopped_) {
        return {};
    }

    stopped_ = true;
    auto drained = fallback_.shutdown();
    native_packets_.clear();
    close_owned_port();
    return drained;
#else
    return fallback_.shutdown();
#endif
}

} // namespace voris::io::backends
