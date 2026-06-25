#include <voris/io/socket.hpp>
#include <voris/io/detail/socket_accept_errors.hpp>
#include <voris/io/detail/socket_error_policy.hpp>
#include <voris/io/detail/socket_io_limits.hpp>

#include <array>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <utility>

#include "test_assert.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

void assert_size_error(const voris::io::io_result<std::size_t>& result,
                       voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_void_error(const voris::io::void_result& result,
                       voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void test_socket_operation_queue() {
    using voris::io::socket_operation_direction;
    using voris::io::socket_operation_queue;

    socket_operation_queue queue(2);
    assert(queue.enqueue(socket_operation_direction::read, 1).has_value());
    assert(queue.enqueue(socket_operation_direction::read, 2).has_value());
    assert(!queue.enqueue(socket_operation_direction::read, 3).has_value());
    assert(queue.enqueue(socket_operation_direction::write, 4).has_value());
    assert(*queue.pop(socket_operation_direction::read) == 1);
    assert(*queue.pop(socket_operation_direction::read) == 2);
    assert(!queue.pop(socket_operation_direction::read).has_value());
    assert(*queue.pop(socket_operation_direction::write) == 4);
}

void test_total_size() {
    using voris::io::buffer_chain_view;
    using voris::io::mutable_buffer_chain_view;
    using voris::io::total_size;

    std::array<std::byte, 3> first{};
    std::array<std::byte, 5> second{};
    std::array<buffer_chain_view, 2> buffers{{
        buffer_chain_view{std::span<const std::byte>(first)},
        buffer_chain_view{std::span<const std::byte>(second)},
    }};
    assert(total_size(buffers) == 8);

    std::array<mutable_buffer_chain_view, 2> mutable_buffers{{
        mutable_buffer_chain_view{std::span<std::byte>(first)},
        mutable_buffer_chain_view{std::span<std::byte>(second)},
    }};
    assert(total_size(mutable_buffers) == 8);
}

void test_socket_io_size_cap() {
    using voris::io::detail::cap_socket_io_size;
    using voris::io::detail::max_safe_socket_io_size;

    const std::size_t cap = max_safe_socket_io_size();
    assert(cap > 0);
    assert(cap_socket_io_size(0) == 0);
    assert(cap_socket_io_size(1) == 1);
    assert(cap_socket_io_size(cap) == cap);
    if (cap < std::numeric_limits<std::size_t>::max()) {
        assert(cap_socket_io_size(cap + 1) == cap);
    }
    assert(cap_socket_io_size(std::numeric_limits<std::size_t>::max()) == cap);
}

void test_socket_iovec_plan_caps_segments() {
    using voris::io::detail::append_socket_iovec_segment;
    using voris::io::detail::socket_iovec_plan;
    using voris::io::detail::socket_iovec_plan_full;

    socket_iovec_plan plan{};
    assert(append_socket_iovec_segment(plan, 0, 2, 6) == 0);
    assert(plan.iovec_count == 0);
    assert(plan.requested_size == 0);
    assert(!socket_iovec_plan_full(plan, 2, 6));

    assert(append_socket_iovec_segment(plan, 4, 2, 6) == 4);
    assert(plan.iovec_count == 1);
    assert(plan.requested_size == 4);
    assert(append_socket_iovec_segment(plan, 5, 2, 6) == 2);
    assert(plan.iovec_count == 2);
    assert(plan.requested_size == 6);
    assert(socket_iovec_plan_full(plan, 2, 6));
    assert(append_socket_iovec_segment(plan, 1, 2, 6) == 0);
    assert(plan.iovec_count == 2);
    assert(plan.requested_size == 6);

    socket_iovec_plan iovec_capped{};
    assert(append_socket_iovec_segment(iovec_capped, 3, 1, 99) == 3);
    assert(append_socket_iovec_segment(iovec_capped, 3, 1, 99) == 0);
    assert(iovec_capped.iovec_count == 1);
    assert(iovec_capped.requested_size == 3);

    socket_iovec_plan zero_iovec_limit{};
    assert(append_socket_iovec_segment(zero_iovec_limit, 3, 0, 99) == 0);
    assert(zero_iovec_limit.iovec_count == 0);
    assert(zero_iovec_limit.requested_size == 0);
}

void test_accept_pending_network_errors_are_retryable() {
    using voris::io::detail::is_accept_retryable_pending_network_error;

    assert(!is_accept_retryable_pending_network_error(0));

#if defined(ENETDOWN)
    assert(is_accept_retryable_pending_network_error(ENETDOWN));
#endif
#if defined(EPROTO)
    assert(is_accept_retryable_pending_network_error(EPROTO));
#endif
#if defined(ENOPROTOOPT)
    assert(is_accept_retryable_pending_network_error(ENOPROTOOPT));
#endif
#if defined(EHOSTDOWN)
    assert(is_accept_retryable_pending_network_error(EHOSTDOWN));
#endif
#if defined(ENONET)
    assert(is_accept_retryable_pending_network_error(ENONET));
#endif
#if defined(EHOSTUNREACH)
    assert(is_accept_retryable_pending_network_error(EHOSTUNREACH));
#endif
#if defined(EOPNOTSUPP)
    assert(is_accept_retryable_pending_network_error(EOPNOTSUPP));
#endif
#if defined(ENETUNREACH)
    assert(is_accept_retryable_pending_network_error(ENETUNREACH));
#endif
}

void test_socket_error_policy_classifies_retry_and_pending_paths() {
    using voris::io::detail::classify_socket_accept_errno;
    using voris::io::detail::classify_socket_connect_errno;
    using voris::io::detail::classify_socket_transfer_errno;
    using voris::io::detail::socket_errno_action;

#if defined(EINTR)
    assert(classify_socket_transfer_errno(EINTR) == socket_errno_action::retry);
#endif
#if defined(EAGAIN)
    assert(classify_socket_transfer_errno(EAGAIN) ==
           socket_errno_action::operation_in_progress);
#endif
    assert(classify_socket_transfer_errno(EBADF) == socket_errno_action::provider_failure);

#if defined(EINTR)
    assert(classify_socket_accept_errno(EINTR) == socket_errno_action::retry);
#endif
#if defined(ENETDOWN)
    assert(classify_socket_accept_errno(ENETDOWN) == socket_errno_action::retry);
#endif
#if defined(EAGAIN)
    assert(classify_socket_accept_errno(EAGAIN) == socket_errno_action::operation_in_progress);
#endif
    assert(classify_socket_accept_errno(EBADF) == socket_errno_action::provider_failure);

#if defined(EINTR)
    assert(classify_socket_connect_errno(EINTR) ==
           socket_errno_action::operation_in_progress);
#endif
#if defined(EALREADY)
    assert(classify_socket_connect_errno(EALREADY) ==
           socket_errno_action::operation_in_progress);
#endif
    assert(classify_socket_connect_errno(EBADF) == socket_errno_action::provider_failure);
}

#if defined(__linux__)
class unique_fd {
public:
    explicit unique_fd(int fd) noexcept
        : fd_(fd) {}

    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    unique_fd(unique_fd&& other) noexcept
        : fd_(other.release()) {}

    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) {
                (void)::close(fd_);
            }
            fd_ = other.release();
        }
        return *this;
    }

    ~unique_fd() {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

private:
    int fd_;
};

std::array<unique_fd, 2> make_pipe() {
    std::array<int, 2> fds{};
    assert(::pipe(fds.data()) == 0);
    return {unique_fd(fds[0]), unique_fd(fds[1])};
}

std::array<unique_fd, 2> make_socket_pair() {
    std::array<int, 2> fds{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0);
    return {unique_fd(fds[0]), unique_fd(fds[1])};
}

unique_fd make_tcp_socket() {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    assert(fd >= 0);
    if (fd != 0) {
        return unique_fd(fd);
    }

    const int duplicate = ::fcntl(fd, F_DUPFD_CLOEXEC, 1);
    assert(duplicate > 0);
    assert(::close(fd) == 0);
    return unique_fd(duplicate);
}

unique_fd make_loopback_listener() {
    unique_fd listener = make_tcp_socket();
    int reuse = 1;
    assert(::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse,
                        static_cast<socklen_t>(sizeof(reuse))) == 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address),
                  static_cast<socklen_t>(sizeof(address))) == 0);
    assert(::listen(listener.get(), 4) == 0);
    return listener;
}

sockaddr_in get_socket_address(int fd) {
    sockaddr_in address{};
    socklen_t length = static_cast<socklen_t>(sizeof(address));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    assert(length == static_cast<socklen_t>(sizeof(address)));
    return address;
}

voris::io::socket_address_view address_view(const sockaddr_in& address) {
    return voris::io::socket_address_view{
        std::as_bytes(std::span<const sockaddr_in>(&address, 1))};
}

void set_nonblocking_fd(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    assert(flags != -1);
    assert(::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

int release_nonzero_fd(unique_fd& fd) {
    const int released = fd.release();
    if (released != 0) {
        return released;
    }

    const int duplicate = ::fcntl(released, F_DUPFD, 1);
    assert(duplicate > 0);
    assert(::close(released) == 0);
    return duplicate;
}

void fill_socket_until_would_block(int fd) {
    int send_buffer_size = 4096;
    assert(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &send_buffer_size,
                        static_cast<socklen_t>(sizeof(send_buffer_size))) == 0);

    std::array<std::byte, 4096> chunk{};
    for (std::size_t attempt = 0; attempt < 65536; ++attempt) {
        const ssize_t count = ::send(fd, chunk.data(), chunk.size(), MSG_NOSIGNAL);
        if (count > 0) {
            continue;
        }
        assert(count == -1);
        assert(errno == EAGAIN || errno == EWOULDBLOCK);
        return;
    }
    assert(false);
}

void wait_for_events(int fd, short events) {
    pollfd descriptor{.fd = fd, .events = events, .revents = 0};
    for (;;) {
        const int count = ::poll(&descriptor, 1, 2000);
        if (count > 0) {
            assert((descriptor.revents & (events | POLLERR | POLLHUP)) != 0);
            return;
        }
        if (count == -1 && errno == EINTR) {
            continue;
        }
        assert(false);
    }
}

std::array<unique_fd, 2> make_loopback_connection() {
    using voris::io::accept_one;
    using voris::io::finish_connect;
    using voris::io::start_connect;
    using voris::io::vio_error_code;

    auto listener = make_loopback_listener();
    const sockaddr_in address = get_socket_address(listener.get());
    auto client = make_tcp_socket();

    voris::io::void_result started =
        start_connect(static_cast<std::size_t>(client.get()), address_view(address));
    assert(started.has_value() ||
           started.error().classification == vio_error_code::operation_in_progress);
    if (!started.has_value()) {
        wait_for_events(client.get(), POLLOUT);
    }

    voris::io::void_result finished = finish_connect(static_cast<std::size_t>(client.get()));
    assert(finished.has_value());

    wait_for_events(listener.get(), POLLIN);
    voris::io::io_result<std::size_t> accepted =
        accept_one(static_cast<std::size_t>(listener.get()));
    assert(accepted.has_value());
    assert(*accepted != 0);
    unique_fd server(static_cast<int>(*accepted));

    return {std::move(client), std::move(server)};
}

void close_with_reset(unique_fd& fd) {
    linger reset_linger{};
    reset_linger.l_onoff = 1;
    reset_linger.l_linger = 0;
    assert(::setsockopt(fd.get(), SOL_SOCKET, SO_LINGER, &reset_linger,
                        static_cast<socklen_t>(sizeof(reset_linger))) == 0);

    const int reset_fd = fd.release();
    assert(::close(reset_fd) == 0);
}

bool is_peer_reset_write_errno(int provider_code) noexcept {
    return provider_code == ECONNRESET || provider_code == EPIPE;
}

void test_linux_accept_one_without_pending_connection_reports_in_progress() {
    using voris::io::accept_one;
    using voris::io::vio_error_code;

    auto listener = make_loopback_listener();
    voris::io::io_result<std::size_t> accepted =
        accept_one(static_cast<std::size_t>(listener.get()));
    assert_size_error(accepted, vio_error_code::operation_in_progress);
    assert(!accepted.error().provider_code.has_value());
}

void test_linux_nonblocking_connect_accept_and_data_path() {
    using voris::io::accept_one;
    using voris::io::finish_connect;
    using voris::io::read_some;
    using voris::io::start_connect;
    using voris::io::vio_error_code;
    using voris::io::write_some;

    auto listener = make_loopback_listener();
    const sockaddr_in address = get_socket_address(listener.get());
    auto client = make_tcp_socket();

    voris::io::void_result started =
        start_connect(static_cast<std::size_t>(client.get()), address_view(address));
    assert(started.has_value() ||
           started.error().classification == vio_error_code::operation_in_progress);
    if (!started.has_value()) {
        assert(!started.error().provider_code.has_value());
        wait_for_events(client.get(), POLLOUT);
    }

    voris::io::void_result finished = finish_connect(static_cast<std::size_t>(client.get()));
    assert(finished.has_value());

    wait_for_events(listener.get(), POLLIN);
    voris::io::io_result<std::size_t> accepted =
        accept_one(static_cast<std::size_t>(listener.get()));
    assert(accepted.has_value());
    assert(*accepted != 0);
    unique_fd server(static_cast<int>(*accepted));

    const int status_flags = ::fcntl(server.get(), F_GETFL, 0);
    assert(status_flags != -1);
    assert((status_flags & O_NONBLOCK) != 0);
    const int descriptor_flags = ::fcntl(server.get(), F_GETFD, 0);
    assert(descriptor_flags != -1);
    assert((descriptor_flags & FD_CLOEXEC) != 0);

    const std::array<std::byte, 4> input{std::byte{0x76}, std::byte{0x69},
                                        std::byte{0x6f}, std::byte{0x21}};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(client.get()), input);
    assert(written.has_value());
    assert(*written == input.size());

    wait_for_events(server.get(), POLLIN);
    std::array<std::byte, 4> output{};
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(server.get()), output);
    assert(read.has_value());
    assert(*read == output.size());
    assert(output == input);
}

void test_linux_refused_connect_reports_so_error_provider_failure() {
    using voris::io::finish_connect;
    using voris::io::start_connect;
    using voris::io::vio_error_code;

    sockaddr_in address{};
    {
        auto listener = make_loopback_listener();
        address = get_socket_address(listener.get());
    }

    auto client = make_tcp_socket();
    voris::io::void_result started =
        start_connect(static_cast<std::size_t>(client.get()), address_view(address));
    if (!started.has_value()) {
        if (started.error().classification == vio_error_code::backend_failure) {
            assert(started.error().provider_code.has_value());
            assert(*started.error().provider_code == ECONNREFUSED);
            return;
        }
        assert(started.error().classification == vio_error_code::operation_in_progress);
        assert(!started.error().provider_code.has_value());
    }

    wait_for_events(client.get(), POLLOUT);
    voris::io::void_result finished = finish_connect(static_cast<std::size_t>(client.get()));
    assert_void_error(finished, vio_error_code::backend_failure);
    assert(finished.error().provider_code.has_value());
    assert(*finished.error().provider_code == ECONNREFUSED);
}

void test_linux_accept_connect_validation() {
    using voris::io::accept_one;
    using voris::io::finish_connect;
    using voris::io::socket_address_view;
    using voris::io::start_connect;
    using voris::io::vio_error_code;

    const std::size_t too_large =
        static_cast<std::size_t>(std::numeric_limits<int>::max()) + std::size_t{1};
    std::array<std::byte, sizeof(sockaddr_in)> bytes{};
    const socket_address_view address{std::span<const std::byte>(bytes)};
    auto client = make_tcp_socket();

    assert_size_error(accept_one(0), vio_error_code::invalid_state);
    assert_size_error(accept_one(too_large), vio_error_code::invalid_state);
    assert_void_error(start_connect(0, address), vio_error_code::invalid_state);
    assert_void_error(start_connect(too_large, address), vio_error_code::invalid_state);
    assert_void_error(start_connect(static_cast<std::size_t>(client.get()),
                                    socket_address_view{}),
                      vio_error_code::invalid_state);
    assert_void_error(finish_connect(0), vio_error_code::invalid_state);
    assert_void_error(finish_connect(too_large), vio_error_code::invalid_state);
}

void test_linux_read_some_reports_partial_progress_without_taking_ownership() {
    using voris::io::read_some;
    using voris::io::write_some;

    auto sockets = make_socket_pair();
    set_nonblocking_fd(sockets[0].get());
    set_nonblocking_fd(sockets[1].get());

    const std::array<std::byte, 3> input{std::byte{0x61}, std::byte{0x62},
                                        std::byte{0x63}};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(sockets[1].get()), input);
    assert(written.has_value());
    assert(*written == input.size());

    std::array<std::byte, 2> output{};
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(sockets[0].get()), output);
    assert(read.has_value());
    assert(*read == output.size());
    assert(output[0] == input[0]);
    assert(output[1] == input[1]);

    assert(::fcntl(sockets[0].get(), F_GETFD) != -1);
    assert(::fcntl(sockets[1].get(), F_GETFD) != -1);

    std::array<std::byte, 1> rest{};
    read = read_some(static_cast<std::size_t>(sockets[0].get()), rest);
    assert(read.has_value());
    assert(*read == rest.size());
    assert(rest[0] == input[2]);
}

void test_linux_vector_read_write_preserves_buffer_order_and_partial_progress() {
    using voris::io::buffer_chain_view;
    using voris::io::mutable_buffer_chain_view;
    using voris::io::read_some;
    using voris::io::write_some;

    auto sockets = make_socket_pair();
    set_nonblocking_fd(sockets[0].get());
    set_nonblocking_fd(sockets[1].get());

    const std::array<std::byte, 2> first{std::byte{0x76}, std::byte{0x69}};
    const std::array<std::byte, 0> empty{};
    const std::array<std::byte, 3> second{std::byte{0x6f}, std::byte{0x2d},
                                         std::byte{0x76}};
    const std::array<buffer_chain_view, 3> write_buffers{{
        buffer_chain_view{std::span<const std::byte>(first)},
        buffer_chain_view{std::span<const std::byte>(empty)},
        buffer_chain_view{std::span<const std::byte>(second)},
    }};

    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(sockets[1].get()), write_buffers);
    assert(written.has_value());
    assert(*written == first.size() + second.size());

    std::array<std::byte, 1> out_first{};
    std::array<std::byte, 2> out_second{};
    std::array<std::byte, 4> out_third{};
    std::array<mutable_buffer_chain_view, 3> read_buffers{{
        mutable_buffer_chain_view{std::span<std::byte>(out_first)},
        mutable_buffer_chain_view{std::span<std::byte>(out_second)},
        mutable_buffer_chain_view{std::span<std::byte>(out_third)},
    }};

    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(sockets[0].get()), read_buffers);
    assert(read.has_value());
    assert(*read == first.size() + second.size());
    assert(out_first[0] == std::byte{0x76});
    assert(out_second[0] == std::byte{0x69});
    assert(out_second[1] == std::byte{0x6f});
    assert(out_third[0] == std::byte{0x2d});
    assert(out_third[1] == std::byte{0x76});
    assert(out_third[2] == std::byte{0x00});
    assert(out_third[3] == std::byte{0x00});
}

void test_linux_write_some_on_non_socket_reports_provider_error() {
    using voris::io::vio_error_code;
    using voris::io::write_some;

    auto pipe = make_pipe();
    set_nonblocking_fd(pipe[1].get());

    const std::array<std::byte, 1> input{std::byte{0x64}};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(pipe[1].get()), input);
    assert_size_error(written, vio_error_code::backend_failure);
    assert(written.error().provider_code.has_value());
    assert(*written.error().provider_code == ENOTSOCK);
}

void test_linux_vector_write_some_on_non_socket_reports_provider_error() {
    using voris::io::buffer_chain_view;
    using voris::io::vio_error_code;
    using voris::io::write_some;

    auto pipe = make_pipe();
    set_nonblocking_fd(pipe[1].get());

    const std::array<std::byte, 1> first{std::byte{0x64}};
    const std::array<std::byte, 1> second{std::byte{0x65}};
    const std::array<buffer_chain_view, 2> buffers{{
        buffer_chain_view{std::span<const std::byte>(first)},
        buffer_chain_view{std::span<const std::byte>(second)},
    }};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(pipe[1].get()), buffers);
    assert_size_error(written, vio_error_code::backend_failure);
    assert(written.error().provider_code.has_value());
    assert(*written.error().provider_code == ENOTSOCK);
}

void test_linux_would_block_read_uses_operation_in_progress() {
    using voris::io::read_some;
    using voris::io::vio_error_code;

    auto sockets = make_socket_pair();
    set_nonblocking_fd(sockets[0].get());

    std::array<std::byte, 1> output{};
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(sockets[0].get()), output);
    assert_size_error(read, vio_error_code::operation_in_progress);
    assert(!read.error().provider_code.has_value());
}

void test_linux_would_block_write_uses_operation_in_progress() {
    using voris::io::vio_error_code;
    using voris::io::write_some;

    auto sockets = make_socket_pair();
    set_nonblocking_fd(sockets[0].get());
    fill_socket_until_would_block(sockets[0].get());

    const std::array<std::byte, 1> input{std::byte{0x65}};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(sockets[0].get()), input);
    assert_size_error(written, vio_error_code::operation_in_progress);
    assert(!written.error().provider_code.has_value());
}

void test_linux_vector_would_block_read_uses_operation_in_progress() {
    using voris::io::mutable_buffer_chain_view;
    using voris::io::read_some;
    using voris::io::vio_error_code;

    auto sockets = make_socket_pair();
    set_nonblocking_fd(sockets[0].get());

    std::array<std::byte, 1> output{};
    const std::array<mutable_buffer_chain_view, 1> buffers{{
        mutable_buffer_chain_view{std::span<std::byte>(output)},
    }};
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(sockets[0].get()), buffers);
    assert_size_error(read, vio_error_code::operation_in_progress);
    assert(!read.error().provider_code.has_value());
}

void test_linux_vector_would_block_write_uses_operation_in_progress() {
    using voris::io::buffer_chain_view;
    using voris::io::vio_error_code;
    using voris::io::write_some;

    auto sockets = make_socket_pair();
    set_nonblocking_fd(sockets[0].get());
    fill_socket_until_would_block(sockets[0].get());

    const std::array<std::byte, 1> input{std::byte{0x65}};
    const std::array<buffer_chain_view, 1> buffers{{
        buffer_chain_view{std::span<const std::byte>(input)},
    }};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(sockets[0].get()), buffers);
    assert_size_error(written, vio_error_code::operation_in_progress);
    assert(!written.error().provider_code.has_value());
}

void test_linux_closed_peer_write_reports_epipe_without_sigpipe() {
    using voris::io::vio_error_code;
    using voris::io::write_some;

    auto sockets = make_socket_pair();
    const int peer = sockets[1].release();
    assert(::close(peer) == 0);

    const std::array<std::byte, 1> input{std::byte{0x66}};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(sockets[0].get()), input);
    assert_size_error(written, vio_error_code::backend_failure);
    assert(written.error().provider_code.has_value());
    assert(*written.error().provider_code == EPIPE);
}

void test_linux_vector_closed_peer_write_reports_epipe_without_sigpipe() {
    using voris::io::buffer_chain_view;
    using voris::io::vio_error_code;
    using voris::io::write_some;

    auto sockets = make_socket_pair();
    const int peer = sockets[1].release();
    assert(::close(peer) == 0);

    const std::array<std::byte, 1> input{std::byte{0x66}};
    const std::array<buffer_chain_view, 1> buffers{{
        buffer_chain_view{std::span<const std::byte>(input)},
    }};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(sockets[0].get()), buffers);
    assert_size_error(written, vio_error_code::backend_failure);
    assert(written.error().provider_code.has_value());
    assert(*written.error().provider_code == EPIPE);
}

void test_linux_peer_reset_read_reports_provider_failure() {
    using voris::io::read_some;
    using voris::io::vio_error_code;

    auto connection = make_loopback_connection();
    close_with_reset(connection[1]);
    wait_for_events(connection[0].get(), POLLIN);

    std::array<std::byte, 1> output{};
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(connection[0].get()), output);
    assert_size_error(read, vio_error_code::backend_failure);
    assert(read.error().provider_code.has_value());
    assert(*read.error().provider_code == ECONNRESET);
}

void test_linux_peer_reset_write_reports_provider_failure() {
    using voris::io::vio_error_code;
    using voris::io::write_some;

    auto connection = make_loopback_connection();
    close_with_reset(connection[1]);
    wait_for_events(connection[0].get(), POLLIN);

    const std::array<std::byte, 1> input{std::byte{0x72}};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(connection[0].get()), input);
    assert_size_error(written, vio_error_code::backend_failure);
    assert(written.error().provider_code.has_value());
    assert(is_peer_reset_write_errno(static_cast<int>(*written.error().provider_code)));
}

void test_linux_half_close_drains_then_reports_eof_and_keeps_reverse_direction_open() {
    using voris::io::read_some;
    using voris::io::write_some;

    auto sockets = make_socket_pair();
    set_nonblocking_fd(sockets[0].get());
    set_nonblocking_fd(sockets[1].get());

    const std::array<std::byte, 2> payload{std::byte{0x68}, std::byte{0x63}};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(sockets[1].get()), payload);
    assert(written.has_value());
    assert(*written == payload.size());
    assert(::shutdown(sockets[1].get(), SHUT_WR) == 0);

    std::array<std::byte, 2> output{};
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(sockets[0].get()), output);
    assert(read.has_value());
    assert(*read == output.size());
    assert(output == payload);

    std::array<std::byte, 1> eof_probe{};
    read = read_some(static_cast<std::size_t>(sockets[0].get()), eof_probe);
    assert(read.has_value());
    assert(*read == 0);

    const std::array<std::byte, 2> reply{std::byte{0x6f}, std::byte{0x6b}};
    written = write_some(static_cast<std::size_t>(sockets[0].get()), reply);
    assert(written.has_value());
    assert(*written == reply.size());

    std::array<std::byte, 2> reply_output{};
    read = read_some(static_cast<std::size_t>(sockets[1].get()), reply_output);
    assert(read.has_value());
    assert(*read == reply_output.size());
    assert(reply_output == reply);
}

void test_linux_zero_length_operations_return_zero() {
    using voris::io::read_some;
    using voris::io::write_some;

    auto sockets = make_socket_pair();
    set_nonblocking_fd(sockets[0].get());
    set_nonblocking_fd(sockets[1].get());

    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(sockets[0].get()), std::span<std::byte>{});
    assert(read.has_value());
    assert(*read == 0);

    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(sockets[1].get()), std::span<const std::byte>{});
    assert(written.has_value());
    assert(*written == 0);
}

void test_linux_vector_zero_length_operations_return_zero_and_skip_empty_segments() {
    using voris::io::buffer_chain_view;
    using voris::io::mutable_buffer_chain_view;
    using voris::io::read_some;
    using voris::io::write_some;

    auto sockets = make_socket_pair();
    set_nonblocking_fd(sockets[0].get());
    set_nonblocking_fd(sockets[1].get());

    const std::array<std::byte, 0> empty_write{};
    const std::array<buffer_chain_view, 2> zero_write_buffers{{
        buffer_chain_view{std::span<const std::byte>(empty_write)},
        buffer_chain_view{std::span<const std::byte>{}},
    }};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(sockets[1].get()), zero_write_buffers);
    assert(written.has_value());
    assert(*written == 0);

    std::array<std::byte, 0> empty_read{};
    const std::array<mutable_buffer_chain_view, 2> zero_read_buffers{{
        mutable_buffer_chain_view{std::span<std::byte>(empty_read)},
        mutable_buffer_chain_view{std::span<std::byte>{}},
    }};
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(sockets[0].get()), zero_read_buffers);
    assert(read.has_value());
    assert(*read == 0);

    const std::array<std::byte, 2> payload{std::byte{0x61}, std::byte{0x62}};
    const std::array<buffer_chain_view, 3> mixed_write_buffers{{
        buffer_chain_view{std::span<const std::byte>{}},
        buffer_chain_view{std::span<const std::byte>(payload)},
        buffer_chain_view{std::span<const std::byte>(empty_write)},
    }};
    written = write_some(static_cast<std::size_t>(sockets[1].get()), mixed_write_buffers);
    assert(written.has_value());
    assert(*written == payload.size());

    std::array<std::byte, 2> output{};
    read = read_some(static_cast<std::size_t>(sockets[0].get()), output);
    assert(read.has_value());
    assert(*read == output.size());
    assert(output == payload);
}

void test_linux_invalid_handles_return_invalid_state() {
    using voris::io::read_some;
    using voris::io::vio_error_code;
    using voris::io::write_some;

    std::array<std::byte, 1> buffer{};
    const std::array<std::byte, 1> input{std::byte{0x65}};
    const std::size_t too_large =
        static_cast<std::size_t>(std::numeric_limits<int>::max()) + std::size_t{1};

    assert_size_error(read_some(0, buffer), vio_error_code::invalid_state);
    assert_size_error(write_some(0, input), vio_error_code::invalid_state);
    assert_size_error(read_some(too_large, buffer), vio_error_code::invalid_state);
    assert_size_error(write_some(too_large, input), vio_error_code::invalid_state);

    const std::array<voris::io::mutable_buffer_chain_view, 1> read_buffers{{
        voris::io::mutable_buffer_chain_view{std::span<std::byte>(buffer)},
    }};
    const std::array<voris::io::buffer_chain_view, 1> write_buffers{{
        voris::io::buffer_chain_view{std::span<const std::byte>(input)},
    }};
    assert_size_error(read_some(0, read_buffers), vio_error_code::invalid_state);
    assert_size_error(write_some(0, write_buffers), vio_error_code::invalid_state);
    assert_size_error(read_some(too_large, read_buffers), vio_error_code::invalid_state);
    assert_size_error(write_some(too_large, write_buffers), vio_error_code::invalid_state);
}

void test_linux_closed_fd_reports_provider_error() {
    using voris::io::read_some;
    using voris::io::vio_error_code;
    using voris::io::write_some;

    std::array<std::byte, 1> buffer{};
    const std::array<std::byte, 1> input{std::byte{0x66}};

    auto read_pipe = make_pipe();
    int closed_read_fd = release_nonzero_fd(read_pipe[0]);
    assert(::close(closed_read_fd) == 0);
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(closed_read_fd), buffer);
    assert_size_error(read, vio_error_code::backend_failure);
    assert(read.error().provider_code.has_value());
    assert(*read.error().provider_code == EBADF);

    auto write_pipe = make_pipe();
    int closed_write_fd = release_nonzero_fd(write_pipe[1]);
    assert(::close(closed_write_fd) == 0);
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(closed_write_fd), input);
    assert_size_error(written, vio_error_code::backend_failure);
    assert(written.error().provider_code.has_value());
    assert(*written.error().provider_code == EBADF);
}

#else

void test_non_linux_read_write_some_validation_and_unsupported() {
    using voris::io::read_some;
    using voris::io::vio_error_code;
    using voris::io::write_some;

    std::array<std::byte, 1> buffer{};
    const std::array<std::byte, 1> input{std::byte{0x61}};
    const std::array<voris::io::mutable_buffer_chain_view, 1> read_buffers{{
        voris::io::mutable_buffer_chain_view{std::span<std::byte>(buffer)},
    }};
    const std::array<voris::io::buffer_chain_view, 1> write_buffers{{
        voris::io::buffer_chain_view{std::span<const std::byte>(input)},
    }};

    assert_size_error(read_some(0, buffer), vio_error_code::invalid_state);
    assert_size_error(write_some(0, input), vio_error_code::invalid_state);
    assert_size_error(read_some(0, read_buffers), vio_error_code::invalid_state);
    assert_size_error(write_some(0, write_buffers), vio_error_code::invalid_state);

    voris::io::io_result<std::size_t> empty_read = read_some(1, std::span<std::byte>{});
    assert(empty_read.has_value());
    assert(*empty_read == 0);

    voris::io::io_result<std::size_t> empty_write =
        write_some(1, std::span<const std::byte>{});
    assert(empty_write.has_value());
    assert(*empty_write == 0);

    const std::array<voris::io::mutable_buffer_chain_view, 1> empty_read_buffers{{
        voris::io::mutable_buffer_chain_view{std::span<std::byte>{}},
    }};
    empty_read = read_some(1, empty_read_buffers);
    assert(empty_read.has_value());
    assert(*empty_read == 0);

    const std::array<voris::io::buffer_chain_view, 1> empty_write_buffers{{
        voris::io::buffer_chain_view{std::span<const std::byte>{}},
    }};
    empty_write = write_some(1, empty_write_buffers);
    assert(empty_write.has_value());
    assert(*empty_write == 0);

    assert_size_error(read_some(1, buffer), vio_error_code::unsupported);
    assert_size_error(write_some(1, input), vio_error_code::unsupported);
    assert_size_error(read_some(1, read_buffers), vio_error_code::unsupported);
    assert_size_error(write_some(1, write_buffers), vio_error_code::unsupported);
}

void test_non_linux_accept_connect_validation_and_unsupported() {
    using voris::io::accept_one;
    using voris::io::finish_connect;
    using voris::io::socket_address_view;
    using voris::io::start_connect;
    using voris::io::vio_error_code;

    const std::array<std::byte, 1> address_bytes{std::byte{0x01}};
    const socket_address_view address{std::span<const std::byte>(address_bytes)};

    assert_size_error(accept_one(0), vio_error_code::invalid_state);
    assert_void_error(start_connect(0, address), vio_error_code::invalid_state);
    assert_void_error(start_connect(1, socket_address_view{}), vio_error_code::invalid_state);
    assert_void_error(finish_connect(0), vio_error_code::invalid_state);

    assert_size_error(accept_one(1), vio_error_code::unsupported);
    assert_void_error(start_connect(1, address), vio_error_code::unsupported);
    assert_void_error(finish_connect(1), vio_error_code::unsupported);
}

#endif

} // namespace

int main() {
    test_socket_operation_queue();
    test_total_size();
    test_socket_io_size_cap();
    test_socket_iovec_plan_caps_segments();
    test_accept_pending_network_errors_are_retryable();
    test_socket_error_policy_classifies_retry_and_pending_paths();

#if defined(__linux__)
    test_linux_accept_one_without_pending_connection_reports_in_progress();
    test_linux_nonblocking_connect_accept_and_data_path();
    test_linux_refused_connect_reports_so_error_provider_failure();
    test_linux_accept_connect_validation();
    test_linux_read_some_reports_partial_progress_without_taking_ownership();
    test_linux_vector_read_write_preserves_buffer_order_and_partial_progress();
    test_linux_write_some_on_non_socket_reports_provider_error();
    test_linux_vector_write_some_on_non_socket_reports_provider_error();
    test_linux_would_block_read_uses_operation_in_progress();
    test_linux_would_block_write_uses_operation_in_progress();
    test_linux_vector_would_block_read_uses_operation_in_progress();
    test_linux_vector_would_block_write_uses_operation_in_progress();
    test_linux_closed_peer_write_reports_epipe_without_sigpipe();
    test_linux_vector_closed_peer_write_reports_epipe_without_sigpipe();
    test_linux_peer_reset_read_reports_provider_failure();
    test_linux_peer_reset_write_reports_provider_failure();
    test_linux_half_close_drains_then_reports_eof_and_keeps_reverse_direction_open();
    test_linux_zero_length_operations_return_zero();
    test_linux_vector_zero_length_operations_return_zero_and_skip_empty_segments();
    test_linux_invalid_handles_return_invalid_state();
    test_linux_closed_fd_reports_provider_error();
#else
    test_non_linux_read_write_some_validation_and_unsupported();
    test_non_linux_accept_connect_validation_and_unsupported();
#endif

    return 0;
}
