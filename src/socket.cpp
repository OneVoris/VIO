#include <voris/io/socket.hpp>
#include <voris/io/detail/socket_accept_errors.hpp>
#include <voris/io/detail/socket_io_limits.hpp>

#include <cstdint>
#include <limits>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace voris::io {

namespace {

[[nodiscard]] void_result validate_native_handle(std::size_t native_handle) {
    if (native_handle == 0) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "native socket handle must be non-zero"));
    }
#if defined(__linux__)
    if (native_handle > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "native socket handle does not fit int"));
    }
#endif
    return {};
}

#if defined(__linux__)
[[nodiscard]] vio_error provider_failure(int provider_code) {
    return make_error(vio_error_code::backend_failure, static_cast<std::int64_t>(provider_code));
}

[[nodiscard]] vio_error operation_in_progress_error() {
    return make_error(vio_error_code::operation_in_progress, "socket operation is in progress");
}

[[nodiscard]] bool is_operation_in_progress_errno(int provider_code) noexcept {
    return provider_code == EINPROGRESS || provider_code == EALREADY ||
           provider_code == EAGAIN || provider_code == EWOULDBLOCK;
}

[[nodiscard]] void_result validate_socket_address(socket_address_view remote_address) {
    if (remote_address.bytes.empty()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "remote socket address must be non-empty"));
    }
    if (remote_address.bytes.size() >
        static_cast<std::size_t>(std::numeric_limits<socklen_t>::max())) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "remote socket address does not fit socklen_t"));
    }
    return {};
}
#else
[[nodiscard]] void_result validate_socket_address(socket_address_view remote_address) {
    if (remote_address.bytes.empty()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "remote socket address must be non-empty"));
    }
    return {};
}
#endif

[[nodiscard]] vio_error unsupported_socket_io_error() {
    return make_error(vio_error_code::unsupported,
                      "socket native helpers are only available on Linux");
}

} // namespace

void_result socket_operation_queue::enqueue(socket_operation_direction direction,
                                            std::size_t operation_id) {
    auto& queue = direction == socket_operation_direction::read ? reads_ : writes_;
    if (queue.size() >= limit_) {
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "socket operation queue is full"));
    }
    queue.push_back(operation_id);
    return {};
}

std::optional<std::size_t> socket_operation_queue::pop(socket_operation_direction direction) {
    auto& queue = direction == socket_operation_direction::read ? reads_ : writes_;
    if (queue.empty()) {
        return std::nullopt;
    }
    const auto value = queue.front();
    queue.pop_front();
    return value;
}

std::size_t socket_operation_queue::size(socket_operation_direction direction) const noexcept {
    const auto& queue = direction == socket_operation_direction::read ? reads_ : writes_;
    return queue.size();
}

std::size_t total_size(std::span<const buffer_chain_view> buffers) noexcept {
    std::size_t total = 0;
    for (const auto& buffer : buffers) {
        total += buffer.bytes.size();
    }
    return total;
}

io_result<std::size_t> read_some(std::size_t native_handle, std::span<std::byte> buffer) {
    const void_result validation = validate_native_handle(native_handle);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }

#if defined(__linux__)
    const int fd = static_cast<int>(native_handle);
    const std::size_t request_size = detail::cap_socket_io_size(buffer.size());
    for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), request_size);
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }

        const int provider_code = errno;
        if (provider_code == EINTR) {
            continue;
        }
        if (provider_code == EAGAIN || provider_code == EWOULDBLOCK) {
            return std::unexpected(operation_in_progress_error());
        }
        return std::unexpected(provider_failure(provider_code));
    }
#else
    (void)native_handle;
    (void)buffer;
    return std::unexpected(unsupported_socket_io_error());
#endif
}

io_result<std::size_t> write_some(std::size_t native_handle, std::span<const std::byte> buffer) {
    const void_result validation = validate_native_handle(native_handle);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }

#if defined(__linux__)
    const int fd = static_cast<int>(native_handle);
    const std::size_t request_size = detail::cap_socket_io_size(buffer.size());
    for (;;) {
        const ssize_t count = ::send(fd, buffer.data(), request_size, MSG_NOSIGNAL);
        if (count >= 0) {
            return static_cast<std::size_t>(count);
        }

        const int provider_code = errno;
        if (provider_code == EINTR) {
            continue;
        }
        if (provider_code == EAGAIN || provider_code == EWOULDBLOCK) {
            return std::unexpected(operation_in_progress_error());
        }
        return std::unexpected(provider_failure(provider_code));
    }
#else
    (void)native_handle;
    (void)buffer;
    return std::unexpected(unsupported_socket_io_error());
#endif
}

io_result<std::size_t> accept_one(std::size_t native_handle) {
    const void_result validation = validate_native_handle(native_handle);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }

#if defined(__linux__)
    const int fd = static_cast<int>(native_handle);
    for (;;) {
        const int accepted = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted >= 0) {
            if (accepted != 0) {
                return static_cast<std::size_t>(accepted);
            }

            const int duplicate = ::fcntl(accepted, F_DUPFD_CLOEXEC, 1);
            if (duplicate < 0) {
                const int provider_code = errno;
                (void)::close(accepted);
                return std::unexpected(provider_failure(provider_code));
            }
            (void)::close(accepted);
            return static_cast<std::size_t>(duplicate);
        }

        const int provider_code = errno;
        if (provider_code == EINTR) {
            continue;
        }
        if (provider_code == EAGAIN || provider_code == EWOULDBLOCK) {
            return std::unexpected(operation_in_progress_error());
        }
        if (detail::is_accept_retryable_pending_network_error(provider_code)) {
            continue;
        }
        return std::unexpected(provider_failure(provider_code));
    }
#else
    (void)native_handle;
    return std::unexpected(unsupported_socket_io_error());
#endif
}

void_result start_connect(std::size_t native_handle, socket_address_view remote_address) {
    const void_result handle_validation = validate_native_handle(native_handle);
    if (!handle_validation.has_value()) {
        return std::unexpected(handle_validation.error());
    }
    const void_result address_validation = validate_socket_address(remote_address);
    if (!address_validation.has_value()) {
        return std::unexpected(address_validation.error());
    }

#if defined(__linux__)
    const int fd = static_cast<int>(native_handle);
    const auto* address =
        static_cast<const sockaddr*>(static_cast<const void*>(remote_address.bytes.data()));
    const auto address_length = static_cast<socklen_t>(remote_address.bytes.size());
    if (::connect(fd, address, address_length) == 0) {
        return {};
    }

    const int provider_code = errno;
    if (provider_code == EINTR || is_operation_in_progress_errno(provider_code)) {
        return std::unexpected(operation_in_progress_error());
    }
    return std::unexpected(provider_failure(provider_code));
#else
    (void)native_handle;
    (void)remote_address;
    return std::unexpected(unsupported_socket_io_error());
#endif
}

void_result finish_connect(std::size_t native_handle) {
    const void_result validation = validate_native_handle(native_handle);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }

#if defined(__linux__)
    const int fd = static_cast<int>(native_handle);
    int socket_error = 0;
    socklen_t length = static_cast<socklen_t>(sizeof(socket_error));
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
        return std::unexpected(provider_failure(errno));
    }
    if (socket_error == 0) {
        return {};
    }
    if (is_operation_in_progress_errno(socket_error)) {
        return std::unexpected(operation_in_progress_error());
    }
    return std::unexpected(provider_failure(socket_error));
#else
    (void)native_handle;
    return std::unexpected(unsupported_socket_io_error());
#endif
}

} // namespace voris::io
