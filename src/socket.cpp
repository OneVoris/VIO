#include <voris/io/socket.hpp>
#include <voris/io/detail/socket_accept_errors.hpp>
#include <voris/io/detail/socket_io_limits.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <limits>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
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
constexpr std::size_t local_iovec_capacity = 1024;

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

[[nodiscard]] std::size_t platform_iovec_limit() noexcept {
    std::size_t limit = local_iovec_capacity;
#if defined(IOV_MAX)
    limit = std::min(limit, static_cast<std::size_t>(IOV_MAX));
#endif
#if defined(_SC_IOV_MAX)
    errno = 0;
    const long runtime_limit = ::sysconf(_SC_IOV_MAX);
    if (runtime_limit > 0) {
        limit = std::min(limit, static_cast<std::size_t>(runtime_limit));
    }
#endif
    return std::max<std::size_t>(limit, 1);
}

[[nodiscard]] void* iovec_base(std::span<std::byte> bytes) noexcept {
    return bytes.data();
}

[[nodiscard]] void* iovec_base(std::span<const std::byte> bytes) noexcept {
    return const_cast<std::byte*>(bytes.data());
}

struct iovec_chain {
    std::array<iovec, local_iovec_capacity> entries{};
    detail::socket_iovec_plan plan{};
};

template <class Buffer>
[[nodiscard]] iovec_chain build_iovec_chain(std::span<const Buffer> buffers) noexcept {
    iovec_chain chain{};
    const std::size_t iovec_limit = platform_iovec_limit();

    for (const auto& buffer : buffers) {
        if (buffer.bytes.empty()) {
            continue;
        }
        if (detail::socket_iovec_plan_full(chain.plan, iovec_limit)) {
            break;
        }

        const std::size_t index = chain.plan.iovec_count;
        const std::size_t length =
            detail::append_socket_iovec_segment(chain.plan, buffer.bytes.size(), iovec_limit);
        if (length == 0) {
            break;
        }
        chain.entries[index].iov_base = iovec_base(buffer.bytes);
        chain.entries[index].iov_len = length;
    }

    return chain;
}

[[nodiscard]] int iovec_count(std::size_t count) noexcept {
    return static_cast<int>(count);
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

template <class Buffer>
[[nodiscard]] bool buffer_chain_is_empty(std::span<const Buffer> buffers) noexcept {
    for (const auto& buffer : buffers) {
        if (!buffer.bytes.empty()) {
            return false;
        }
    }
    return true;
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

std::size_t total_size(std::span<const mutable_buffer_chain_view> buffers) noexcept {
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

io_result<std::size_t> read_some(std::size_t native_handle,
                                 std::span<const mutable_buffer_chain_view> buffers) {
    const void_result validation = validate_native_handle(native_handle);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }
    if (buffer_chain_is_empty(buffers)) {
        return std::size_t{0};
    }

#if defined(__linux__)
    auto chain = build_iovec_chain(buffers);
    if (chain.plan.iovec_count == 0 || chain.plan.requested_size == 0) {
        return std::size_t{0};
    }

    const int fd = static_cast<int>(native_handle);
    for (;;) {
        const ssize_t count =
            ::readv(fd, chain.entries.data(), iovec_count(chain.plan.iovec_count));
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
    (void)buffers;
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

io_result<std::size_t> write_some(std::size_t native_handle,
                                  std::span<const buffer_chain_view> buffers) {
    const void_result validation = validate_native_handle(native_handle);
    if (!validation.has_value()) {
        return std::unexpected(validation.error());
    }
    if (buffer_chain_is_empty(buffers)) {
        return std::size_t{0};
    }

#if defined(__linux__)
    auto chain = build_iovec_chain(buffers);
    if (chain.plan.iovec_count == 0 || chain.plan.requested_size == 0) {
        return std::size_t{0};
    }

    const int fd = static_cast<int>(native_handle);
    msghdr message{};
    message.msg_iov = chain.entries.data();
    message.msg_iovlen = chain.plan.iovec_count;

    for (;;) {
        const ssize_t count = ::sendmsg(fd, &message, MSG_NOSIGNAL);
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
    (void)buffers;
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
