#include <voris/io/backends/io_uring_backend.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <thread>
#include <utility>

#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "test_assert.hpp"

namespace voris::io::backends::detail {

[[nodiscard]] io_uring_capabilities capabilities_from_io_uring_probe_opcodes(
    std::span<const unsigned> supported_opcodes) noexcept;

} // namespace voris::io::backends::detail

namespace {

constexpr unsigned uapi_op_read = 22U;
constexpr unsigned uapi_op_write = 23U;
constexpr unsigned uapi_op_readv = 1U;
constexpr unsigned uapi_op_writev = 2U;
constexpr unsigned uapi_op_fsync = 3U;
constexpr unsigned uapi_op_read_fixed = 4U;
constexpr unsigned uapi_op_write_fixed = 5U;
constexpr unsigned uapi_op_accept = 13U;
constexpr unsigned uapi_op_async_cancel = 14U;
constexpr unsigned uapi_op_connect = 16U;
constexpr unsigned uapi_op_files_update = 20U;

voris::io::backend_operation operation(std::size_t id,
                                       voris::io::backend_operation_kind kind,
                                       voris::io::backend_handle_token token) {
    voris::io::backend_operation result{};
    result.id = id;
    result.kind = kind;
    result.handle = token;
    return result;
}

voris::io::backend_operation read_operation(std::size_t id,
                                            voris::io::backend_handle_token token,
                                            std::span<std::byte> buffer) {
    auto result = operation(id, voris::io::backend_operation_kind::read, token);
    result.read_buffer = buffer;
    return result;
}

voris::io::backend_operation write_operation(std::size_t id,
                                             voris::io::backend_handle_token token,
                                             std::span<const std::byte> buffer) {
    auto result = operation(id, voris::io::backend_operation_kind::write, token);
    result.write_buffer = buffer;
    return result;
}

voris::io::backend_operation connect_operation(std::size_t id,
                                               voris::io::backend_handle_token token,
                                               std::span<const std::byte> address) {
    auto result = operation(id, voris::io::backend_operation_kind::connect, token);
    result.socket_address = address;
    return result;
}

void assert_void_unsupported(const voris::io::void_result& result) {
    assert(!result.has_value());
    assert(result.error().classification == voris::io::vio_error_code::unsupported);
}

void assert_void_error(const voris::io::void_result& result,
                       voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

template <class T>
void assert_io_unsupported(const voris::io::io_result<T>& result) {
    assert(!result.has_value());
    assert(result.error().classification == voris::io::vio_error_code::unsupported);
}

template <class T>
void assert_io_error(const voris::io::io_result<T>& result,
                     voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_completion_error(const voris::io::backend_completion& completion,
                             std::size_t operation_id,
                             voris::io::vio_error_code expected) {
    assert(completion.operation_id == operation_id);
    assert(!completion.result.has_value());
    assert(completion.result.error().classification == expected);
}

void assert_drained_closed_completion(voris::io::backend_completion completion,
                                      std::size_t operation_id) {
    assert_completion_error(completion, operation_id, voris::io::vio_error_code::closed);
}

voris::io::backends::io_uring_capabilities core_capabilities() {
    return voris::io::backends::io_uring_capabilities{
        .available = true,
        .supports_read = true,
        .supports_write = true,
        .supports_accept = true,
        .supports_connect = true,
        .supports_files = true,
        .supports_fsync = true,
        .supports_cancel = true,
    };
}

voris::io::backends::io_uring_capabilities capability_for(
    voris::io::backend_operation_kind kind) {
    voris::io::backends::io_uring_capabilities caps{.available = true};
    switch (kind) {
    case voris::io::backend_operation_kind::read:
        caps.supports_read = true;
        break;
    case voris::io::backend_operation_kind::write:
        caps.supports_write = true;
        break;
    case voris::io::backend_operation_kind::accept:
        caps.supports_accept = true;
        break;
    case voris::io::backend_operation_kind::connect:
        caps.supports_connect = true;
        break;
    case voris::io::backend_operation_kind::close:
    case voris::io::backend_operation_kind::wake:
        break;
    }
    return caps;
}

constexpr std::array socket_operation_kinds{
    voris::io::backend_operation_kind::read,
    voris::io::backend_operation_kind::write,
    voris::io::backend_operation_kind::accept,
    voris::io::backend_operation_kind::connect,
};

voris::io::backends::io_uring_backend_options deterministic_options(
    std::size_t submission_queue_capacity = 64,
    std::size_t submit_batch_limit = 32,
    std::size_t completion_batch_limit = 32) {
    return voris::io::backends::io_uring_backend_options{
        .submission_queue_capacity = submission_queue_capacity,
        .submit_batch_limit = submit_batch_limit,
        .completion_batch_limit = completion_batch_limit,
        .enable_kernel_submission = false,
    };
}

void test_backend_contract_carries_socket_payloads_and_results() {
    std::array<std::byte, 4> read_buffer{};
    const std::array<std::byte, 3> write_buffer{
        std::byte{0x76}, std::byte{0x69}, std::byte{0x6f}};
    const std::array<std::byte, 2> address{std::byte{0x01}, std::byte{0x02}};
    const voris::io::backend_handle_token token{1, 1};

    const auto read = read_operation(1, token, read_buffer);
    assert(read.read_buffer.data() == read_buffer.data());
    assert(read.read_buffer.size() == read_buffer.size());

    const auto write = write_operation(2, token, write_buffer);
    assert(write.write_buffer.data() == write_buffer.data());
    assert(write.write_buffer.size() == write_buffer.size());

    const auto connect = connect_operation(3, token, address);
    assert(connect.socket_address.data() == address.data());
    assert(connect.socket_address.size() == address.size());

    voris::io::backend_completion completion{};
    completion.bytes_transferred = 7;
    completion.accepted_native_handle = 9;
    assert(completion.bytes_transferred == 7);
    assert(completion.accepted_native_handle == 9);
}

#if defined(__linux__)
class unique_fd {
public:
    explicit unique_fd(int fd = -1) noexcept : fd_(fd) {}

    ~unique_fd() {
        reset();
    }

    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    unique_fd(unique_fd&& other) noexcept : fd_(other.release()) {}

    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    [[nodiscard]] int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            (void)::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_{-1};
};

std::array<unique_fd, 2> make_socket_pair() {
    std::array<int, 2> fds{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                        fds.data()) == 0);
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

sockaddr_in socket_address(int fd) {
    sockaddr_in address{};
    socklen_t length = static_cast<socklen_t>(sizeof(address));
    assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    assert(length == static_cast<socklen_t>(sizeof(address)));
    return address;
}

std::span<const std::byte> address_bytes(const sockaddr_in& address) noexcept {
    return std::as_bytes(std::span<const sockaddr_in>(&address, 1));
}

int release_nonzero_fd(unique_fd& fd) {
    const int released = fd.release();
    if (released != 0) {
        return released;
    }

    const int duplicate = ::fcntl(released, F_DUPFD_CLOEXEC, 1);
    assert(duplicate > 0);
    assert(::close(released) == 0);
    return duplicate;
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

void finish_nonblocking_connect(int fd) {
    wait_for_events(fd, POLLOUT);

    int socket_error = 0;
    socklen_t length = static_cast<socklen_t>(sizeof(socket_error));
    assert(::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) == 0);
    assert(socket_error == 0);
}

void assert_provider_code(const voris::io::backend_completion& completion,
                          std::size_t operation_id,
                          int expected_provider_code) {
    assert(completion.operation_id == operation_id);
    assert(!completion.result.has_value());
    assert(completion.result.error().classification == voris::io::vio_error_code::backend_failure);
    assert(completion.result.error().provider_code.has_value());
    assert(*completion.result.error().provider_code == expected_provider_code);
}

voris::io::backend_completion wait_for_real_completion(voris::io::backend& backend) {
    using namespace std::chrono_literals;

    std::array<voris::io::backend_completion, 1> completions{};
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        auto polled = backend.poll();
        assert(polled.has_value());

        auto drained = backend.drain_completions(completions);
        assert(drained.has_value());
        if (*drained == 1) {
            return completions[0];
        }

        std::this_thread::sleep_for(1ms);
    }

    assert(false);
    return {};
}

voris::io::backends::io_uring_backend_options real_kernel_options() {
    return voris::io::backends::io_uring_backend_options{
        .submission_queue_capacity = 16,
        .submit_batch_limit = 8,
        .completion_batch_limit = 8,
        .enable_kernel_submission = true,
    };
}

void test_linux_real_io_uring_read_write_transfer_returns_byte_counts() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    if (!caps.available || !caps.supports_read || !caps.supports_write ||
        !caps.supports_cancel) {
        return;
    }

    voris::io::backends::io_uring_backend backend(caps, real_kernel_options());
    auto sockets = make_socket_pair();
    const auto writer =
        backend.register_handle(static_cast<std::size_t>(sockets[0].get()));
    const auto reader =
        backend.register_handle(static_cast<std::size_t>(sockets[1].get()));
    assert(writer.has_value());
    assert(reader.has_value());

    const std::array<std::byte, 4> payload{
        std::byte{0x76}, std::byte{0x69}, std::byte{0x6f}, std::byte{0x21}};
    assert(backend.submit(write_operation(901, *writer, payload)).has_value());
    auto completion = wait_for_real_completion(backend);
    assert(completion.operation_id == 901);
    assert(completion.result.has_value());
    assert(completion.bytes_transferred == payload.size());

    std::array<std::byte, 4> output{};
    assert(backend.submit(read_operation(902, *reader, output)).has_value());
    completion = wait_for_real_completion(backend);
    assert(completion.operation_id == 902);
    assert(completion.result.has_value());
    assert(completion.bytes_transferred == output.size());
    assert(output == payload);
    assert(backend.shutdown().has_value());
}

void test_linux_real_io_uring_accept_returns_usable_nonblocking_socket() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    if (!caps.available || !caps.supports_accept || !caps.supports_cancel) {
        return;
    }

    voris::io::backends::io_uring_backend backend(caps, real_kernel_options());
    auto listener = make_loopback_listener();
    const sockaddr_in address = socket_address(listener.get());
    const auto listener_token =
        backend.register_handle(static_cast<std::size_t>(listener.get()));
    assert(listener_token.has_value());

    assert(backend.submit(operation(911, voris::io::backend_operation_kind::accept,
                                    *listener_token))
               .has_value());

    auto client = make_tcp_socket();
    const int connected =
        ::connect(client.get(), reinterpret_cast<const sockaddr*>(&address),
                  static_cast<socklen_t>(sizeof(address)));
    assert(connected == 0 || errno == EINPROGRESS);

    auto completion = wait_for_real_completion(backend);
    assert(completion.operation_id == 911);
    assert(completion.result.has_value());
    assert(completion.accepted_native_handle != 0);
    unique_fd accepted(static_cast<int>(completion.accepted_native_handle));

    finish_nonblocking_connect(client.get());
    const int status_flags = ::fcntl(accepted.get(), F_GETFL, 0);
    assert(status_flags != -1);
    assert((status_flags & O_NONBLOCK) != 0);
    const int descriptor_flags = ::fcntl(accepted.get(), F_GETFD, 0);
    assert(descriptor_flags != -1);
    assert((descriptor_flags & FD_CLOEXEC) != 0);

    const std::array<std::byte, 1> payload{std::byte{0x61}};
    assert(::send(client.get(), payload.data(), payload.size(), MSG_NOSIGNAL) == 1);
    wait_for_events(accepted.get(), POLLIN);
    std::array<std::byte, 1> output{};
    assert(::recv(accepted.get(), output.data(), output.size(), 0) == 1);
    assert(output == payload);
    assert(backend.shutdown().has_value());
}

void test_linux_real_io_uring_connect_completes() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    if (!caps.available || !caps.supports_connect || !caps.supports_cancel) {
        return;
    }

    voris::io::backends::io_uring_backend backend(caps, real_kernel_options());
    auto listener = make_loopback_listener();
    const sockaddr_in address = socket_address(listener.get());
    auto client = make_tcp_socket();
    const auto client_token =
        backend.register_handle(static_cast<std::size_t>(client.get()));
    assert(client_token.has_value());

    assert(backend.submit(connect_operation(921, *client_token, address_bytes(address)))
               .has_value());
    auto completion = wait_for_real_completion(backend);
    assert(completion.operation_id == 921);
    assert(completion.result.has_value());

    wait_for_events(listener.get(), POLLIN);
    unique_fd accepted(::accept4(listener.get(), nullptr, nullptr,
                                 SOCK_NONBLOCK | SOCK_CLOEXEC));
    assert(accepted.get() >= 0);
    assert(backend.shutdown().has_value());
}

void test_linux_real_io_uring_read_provider_error_is_reported() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    if (!caps.available || !caps.supports_read || !caps.supports_cancel) {
        return;
    }

    voris::io::backends::io_uring_backend backend(caps, real_kernel_options());
    auto sockets = make_socket_pair();
    const int closed_fd = release_nonzero_fd(sockets[0]);
    assert(::close(closed_fd) == 0);

    const auto token = backend.register_handle(static_cast<std::size_t>(closed_fd));
    assert(token.has_value());
    std::array<std::byte, 1> output{};
    assert(backend.submit(read_operation(931, *token, output)).has_value());
    const auto completion = wait_for_real_completion(backend);
    assert_provider_code(completion, 931, EBADF);
    assert(backend.shutdown().has_value());
}

void test_linux_real_io_uring_close_completes_queued_socket_operation() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    if (!caps.available || !caps.supports_read || !caps.supports_cancel) {
        return;
    }

    voris::io::backends::io_uring_backend backend(caps, real_kernel_options());
    auto sockets = make_socket_pair();
    const auto token = backend.register_handle(static_cast<std::size_t>(sockets[0].get()));
    assert(token.has_value());

    std::array<std::byte, 1> output{};
    assert(backend.submit(read_operation(941, *token, output)).has_value());
    assert(backend.close_handle(*token).has_value());

    std::array<voris::io::backend_completion, 1> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_completion_error(completions[0], 941, voris::io::vio_error_code::closed);

    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);
    assert(backend.shutdown().has_value());
}

void test_linux_real_io_uring_close_waits_for_submitted_cqe_before_completion() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    if (!caps.available || !caps.supports_read || !caps.supports_cancel) {
        return;
    }

    voris::io::backends::io_uring_backend backend(caps, real_kernel_options());
    auto sockets = make_socket_pair();
    const auto token = backend.register_handle(static_cast<std::size_t>(sockets[0].get()));
    assert(token.has_value());

    std::array<std::byte, 1> output{};
    assert(backend.submit(read_operation(951, *token, output)).has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    assert(backend.close_handle(*token).has_value());

    std::array<voris::io::backend_completion, 1> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);

    const auto completion = wait_for_real_completion(backend);
    assert_completion_error(completion, 951, voris::io::vio_error_code::closed);

    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        auto polled = backend.poll();
        if (!polled.has_value()) {
            assert(polled.error().classification == voris::io::vio_error_code::closed);
            break;
        }

        auto drained = backend.drain_completions(completions);
        assert(drained.has_value());
        assert(*drained == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(backend.shutdown().has_value());
}

void test_linux_real_io_uring_shutdown_cancels_submitted_read_without_data() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    if (!caps.available || !caps.supports_read || !caps.supports_cancel) {
        return;
    }

    voris::io::backends::io_uring_backend backend(caps, real_kernel_options());
    auto sockets = make_socket_pair();
    const auto token = backend.register_handle(static_cast<std::size_t>(sockets[0].get()));
    assert(token.has_value());

    std::array<std::byte, 1> output{};
    assert(backend.submit(read_operation(961, *token, output)).has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    assert(backend.shutdown().has_value());
    assert(backend.state() == voris::io::backends::io_uring_backend_state::closed);

    const auto completion = wait_for_real_completion(backend);
    assert_completion_error(completion, 961, voris::io::vio_error_code::closed);

    std::array<voris::io::backend_completion, 1> completions{};
    for (std::size_t attempt = 0; attempt < 16; ++attempt) {
        polled = backend.poll();
        if (!polled.has_value()) {
            assert(polled.error().classification == voris::io::vio_error_code::closed);
            break;
        }

        auto drained = backend.drain_completions(completions);
        assert(drained.has_value());
        assert(*drained == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void test_linux_real_io_uring_submit_requires_cancel_for_kernel_liveness() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    if (!caps.available || !caps.supports_read) {
        return;
    }

    caps.supports_cancel = false;

    voris::io::backends::io_uring_backend backend(caps, real_kernel_options());
    auto sockets = make_socket_pair();
    const auto token = backend.register_handle(static_cast<std::size_t>(sockets[0].get()));
    assert(token.has_value());

    std::array<std::byte, 1> output{};
    assert_void_unsupported(backend.submit(read_operation(971, *token, output)));
    assert(backend.shutdown().has_value());
}
#endif

void assert_not_default_eligible_when(
    bool voris::io::backends::io_uring_capabilities::*field) {
    auto caps = core_capabilities();
    caps.*field = false;

    const voris::io::backends::io_uring_backend backend(caps, deterministic_options());
    assert(!backend.default_eligible());
}

void assert_submit_unsupported_when(
    voris::io::backend_operation_kind kind,
    bool voris::io::backends::io_uring_capabilities::*field) {
    auto caps = core_capabilities();
    caps.*field = false;

    voris::io::backends::io_uring_backend backend(caps, deterministic_options());
    auto token = backend.register_handle(1);
    assert(token.has_value());

    assert_void_unsupported(backend.submit(operation(10, kind, *token)));
    assert(backend.shutdown().has_value());
}

void assert_no_capabilities(const voris::io::backends::io_uring_capabilities& caps) {
    assert(!caps.available);
    assert(!caps.supports_read);
    assert(!caps.supports_write);
    assert(!caps.supports_accept);
    assert(!caps.supports_connect);
    assert(!caps.supports_files);
    assert(!caps.supports_fsync);
    assert(!caps.supports_cancel);
    assert(!caps.supports_registered_buffers);
    assert(!caps.supports_registered_files);
}

void test_default_eligibility_requires_core_capabilities() {
    const voris::io::backends::io_uring_backend eligible(core_capabilities(),
                                                         deterministic_options());
    assert(eligible.default_eligible());

    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::available);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_read);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_write);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_accept);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_connect);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_files);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_fsync);
    assert_not_default_eligible_when(
        &voris::io::backends::io_uring_capabilities::supports_cancel);
}

void test_probe_opcode_mapping_is_deterministic() {
    constexpr std::array supported{
        uapi_op_read,         uapi_op_write,     uapi_op_fsync,
        uapi_op_accept,       uapi_op_connect,   uapi_op_async_cancel,
        uapi_op_read_fixed,   uapi_op_write_fixed,
        uapi_op_files_update,
    };

    const auto caps =
        voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
            supported);

    assert(caps.available);
    assert(caps.supports_read);
    assert(caps.supports_write);
    assert(caps.supports_fsync);
    assert(caps.supports_files);
    assert(caps.supports_accept);
    assert(caps.supports_connect);
    assert(caps.supports_cancel);
    assert(caps.supports_registered_buffers);
    assert(caps.supports_registered_files);
}

void test_probe_files_require_read_write_and_fsync() {
    {
        constexpr std::array supported{uapi_op_read, uapi_op_write, uapi_op_fsync};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_read);
        assert(caps.supports_write);
        assert(caps.supports_fsync);
        assert(caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_write, uapi_op_fsync};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_read);
        assert(caps.supports_write);
        assert(caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_read, uapi_op_fsync};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_read);
        assert(!caps.supports_write);
        assert(caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_read, uapi_op_write};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_read);
        assert(caps.supports_write);
        assert(!caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_readv};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_read);
        assert(!caps.supports_write);
        assert(!caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_writev};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_read);
        assert(!caps.supports_write);
        assert(!caps.supports_fsync);
        assert(!caps.supports_files);
    }

    {
        constexpr std::array supported{uapi_op_readv, uapi_op_writev, uapi_op_fsync};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_read);
        assert(!caps.supports_write);
        assert(caps.supports_fsync);
        assert(!caps.supports_files);
    }
}

void test_probe_registered_capabilities_are_independent_candidates() {
    {
        constexpr std::array supported{uapi_op_read_fixed};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_registered_buffers);
        assert(!caps.supports_registered_files);
    }

    {
        constexpr std::array supported{uapi_op_write_fixed};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_registered_buffers);
        assert(!caps.supports_registered_files);
    }

    {
        constexpr std::array supported{uapi_op_read_fixed, uapi_op_write_fixed};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(caps.supports_registered_buffers);
        assert(!caps.supports_registered_files);
    }

    {
        constexpr std::array supported{uapi_op_files_update};
        const auto caps =
            voris::io::backends::detail::capabilities_from_io_uring_probe_opcodes(
                supported);
        assert(!caps.supports_registered_buffers);
        assert(caps.supports_registered_files);
        assert(!caps.supports_files);
    }
}

void test_submit_rejects_missing_operation_opcodes() {
    assert_submit_unsupported_when(
        voris::io::backend_operation_kind::read,
        &voris::io::backends::io_uring_capabilities::supports_read);
    assert_submit_unsupported_when(
        voris::io::backend_operation_kind::write,
        &voris::io::backends::io_uring_capabilities::supports_write);
    assert_submit_unsupported_when(
        voris::io::backend_operation_kind::accept,
        &voris::io::backends::io_uring_capabilities::supports_accept);
    assert_submit_unsupported_when(
        voris::io::backend_operation_kind::connect,
        &voris::io::backends::io_uring_capabilities::supports_connect);
}

void test_socket_operation_kinds_accept_only_with_matching_capability() {
    std::size_t operation_id = 30;
    for (const auto kind : socket_operation_kinds) {
        {
            voris::io::backends::io_uring_backend backend(capability_for(kind),
                                                          deterministic_options());
            auto token = backend.register_handle(operation_id);
            assert(token.has_value());
            assert(backend.submit(operation(operation_id, kind, *token)).has_value());
            assert(backend.shutdown().has_value());
        }

        {
            auto caps = capability_for(kind);
            switch (kind) {
            case voris::io::backend_operation_kind::read:
                caps.supports_read = false;
                break;
            case voris::io::backend_operation_kind::write:
                caps.supports_write = false;
                break;
            case voris::io::backend_operation_kind::accept:
                caps.supports_accept = false;
                break;
            case voris::io::backend_operation_kind::connect:
                caps.supports_connect = false;
                break;
            case voris::io::backend_operation_kind::close:
            case voris::io::backend_operation_kind::wake:
                break;
            }

            voris::io::backends::io_uring_backend backend(caps, deterministic_options());
            auto token = backend.register_handle(operation_id + 100);
            assert(token.has_value());
            assert_void_unsupported(backend.submit(operation(operation_id, kind, *token)));
            assert(backend.shutdown().has_value());
        }
        ++operation_id;
    }
}

void test_submit_rejects_non_socket_operation_kinds() {
    voris::io::backends::io_uring_backend backend(core_capabilities(),
                                                  deterministic_options());
    auto token = backend.register_handle(1);
    assert(token.has_value());

    assert_void_error(backend.submit(operation(40, voris::io::backend_operation_kind::close,
                                               *token)),
                      voris::io::vio_error_code::invalid_state);
    assert_void_error(backend.submit(operation(41, voris::io::backend_operation_kind::wake,
                                               *token)),
                      voris::io::vio_error_code::invalid_state);
    assert(backend.shutdown().has_value());
}

void test_optional_registrations_follow_capabilities() {
    {
        voris::io::backends::io_uring_backend backend(core_capabilities(),
                                                      deterministic_options());
        assert_void_unsupported(backend.register_buffers(2));
        assert_void_unsupported(backend.register_files(2));
    }

    {
        auto caps = core_capabilities();
        caps.available = false;
        caps.supports_registered_buffers = true;
        caps.supports_registered_files = true;

        voris::io::backends::io_uring_backend backend(caps, deterministic_options());
        assert_void_unsupported(backend.register_buffers(2));
        assert_void_unsupported(backend.register_files(2));
    }

    {
        auto caps = core_capabilities();
        caps.supports_registered_buffers = true;

        voris::io::backends::io_uring_backend backend(caps, deterministic_options());
        assert(backend.register_buffers(2).has_value());
        assert_void_unsupported(backend.register_files(2));
    }

    {
        auto caps = core_capabilities();
        caps.supports_registered_files = true;

        voris::io::backends::io_uring_backend backend(caps, deterministic_options());
        assert_void_unsupported(backend.register_buffers(2));
        assert(backend.register_files(2).has_value());
    }
}

void test_lifecycle_state_tracks_availability_and_shutdown() {
    {
        auto caps = core_capabilities();
        caps.available = false;

        voris::io::backends::io_uring_backend backend(caps, deterministic_options());
        assert(backend.state() == voris::io::backends::io_uring_backend_state::unavailable);
        assert_io_unsupported(backend.register_handle(1));
        assert_io_unsupported(backend.poll());

        std::array<voris::io::backend_completion, 1> completions{};
        assert_io_unsupported(backend.drain_completions(completions));

        assert(backend.shutdown().has_value());
        assert(backend.state() == voris::io::backends::io_uring_backend_state::closed);
        assert_io_error(backend.register_handle(1), voris::io::vio_error_code::closed);
    }

    {
        voris::io::backends::io_uring_backend backend(core_capabilities(),
                                                      deterministic_options());
        assert(backend.state() == voris::io::backends::io_uring_backend_state::active);

        const auto token = backend.register_handle(1);
        assert(token.has_value());

        assert(backend.shutdown().has_value());
        assert(backend.shutdown().has_value());
        assert(backend.state() == voris::io::backends::io_uring_backend_state::closed);

        assert_io_error(backend.register_handle(2), voris::io::vio_error_code::closed);
        assert_void_error(backend.submit(operation(11, voris::io::backend_operation_kind::read,
                                                   *token)),
                          voris::io::vio_error_code::closed);
        assert_void_error(backend.wake(), voris::io::vio_error_code::closed);
        assert_io_error(backend.poll(), voris::io::vio_error_code::closed);
        assert_void_error(backend.register_buffers(1), voris::io::vio_error_code::closed);

        std::array<voris::io::backend_completion, 1> completions{};
        const auto drained = backend.drain_completions(completions);
        assert(drained.has_value());
        assert(*drained == 0);
    }
}

void test_empty_completion_drain_is_invalid_state() {
    auto caps = core_capabilities();
    caps.available = false;
    voris::io::backends::io_uring_backend unavailable(caps, deterministic_options());
    assert_io_error(
        unavailable.drain_completions(std::span<voris::io::backend_completion>{}),
        voris::io::vio_error_code::invalid_state);

    voris::io::backends::io_uring_backend backend(core_capabilities(),
                                                  deterministic_options());
    assert_io_error(backend.drain_completions(std::span<voris::io::backend_completion>{}),
                    voris::io::vio_error_code::invalid_state);
}

void test_submit_validates_operation_before_queueing() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), deterministic_options(2, 1, 1));
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert_void_error(backend.submit(operation(0, voris::io::backend_operation_kind::read,
                                               *token)),
                      voris::io::vio_error_code::invalid_state);
    assert_void_error(backend.submit(operation(1, voris::io::backend_operation_kind::read,
                                               {})),
                      voris::io::vio_error_code::invalid_state);

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert_void_error(backend.submit(operation(1, voris::io::backend_operation_kind::read,
                                               *token)),
                      voris::io::vio_error_code::invalid_state);
    assert(backend.shutdown().has_value());
}

void test_submission_queue_is_bounded() {
    {
        voris::io::backends::io_uring_backend backend(
            core_capabilities(), deterministic_options(0, 2, 2));
        const auto token = backend.register_handle(1);
        assert(token.has_value());
        assert_void_error(backend.submit(operation(1, voris::io::backend_operation_kind::read,
                                                   *token)),
                          voris::io::vio_error_code::resource_exhausted);
        assert(backend.shutdown().has_value());
    }

    {
        voris::io::backends::io_uring_backend backend(
            core_capabilities(), deterministic_options(2, 2, 2));
        const auto token = backend.register_handle(1);
        assert(token.has_value());

        assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
                   .has_value());
        assert(backend.submit(operation(2, voris::io::backend_operation_kind::read, *token))
                   .has_value());
        assert_void_error(backend.submit(operation(3, voris::io::backend_operation_kind::read,
                                                   *token)),
                          voris::io::vio_error_code::resource_exhausted);
        assert(backend.shutdown().has_value());
    }
}

void test_queued_operation_can_be_cancelled_before_flush() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), deterministic_options(4, 1, 4));
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(2, voris::io::backend_operation_kind::read, *token))
               .has_value());

    assert(backend.cancel(2, voris::io::cancellation_reason::manual).has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert(backend.cancel(1, voris::io::cancellation_reason::manual).has_value());

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    assert(backend.close_handle(*token).has_value());
    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 2);

    std::array<voris::io::backend_completion, 2> completions{};
    const auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 2);
    assert_completion_error(completions[0], 1, voris::io::vio_error_code::closed);
    assert_completion_error(completions[1], 2, voris::io::vio_error_code::closed);
}

void test_kernel_mode_queued_cancellation_is_rejected_before_recording() {
    voris::io::backends::io_uring_backend backend(core_capabilities());
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    std::array<std::byte, 1> output{};
    assert(backend.submit(read_operation(101, *token, output)).has_value());

    assert_void_unsupported(backend.cancel(101, voris::io::cancellation_reason::manual));
    assert(backend.close_handle(*token).has_value());

    std::array<voris::io::backend_completion, 1> completions{};
    const auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_completion_error(completions[0], 101, voris::io::vio_error_code::closed);
}

void test_kernel_submission_requires_cancel_capability_for_close_liveness() {
    auto caps = core_capabilities();
    caps.supports_cancel = false;

    voris::io::backends::io_uring_backend backend(caps);
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    std::array<std::byte, 1> output{};
    assert_void_unsupported(backend.submit(read_operation(102, *token, output)));
    assert(backend.shutdown().has_value());
}

void test_poll_flushes_submissions_in_batches() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), deterministic_options(8, 2, 8));
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(2, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(3, voris::io::backend_operation_kind::read, *token))
               .has_value());

    std::array<voris::io::backend_completion, 4> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert(backend.cancel(1, voris::io::cancellation_reason::manual).has_value());
    assert(backend.cancel(2, voris::io::cancellation_reason::manual).has_value());
    assert(backend.cancel(3, voris::io::cancellation_reason::manual).has_value());

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert(backend.shutdown().has_value());
}

void test_socket_operations_flow_through_submission_batches_and_close_fifo() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), deterministic_options(8, 2, 8));
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(61, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(62, voris::io::backend_operation_kind::write, *token))
               .has_value());
    assert(backend.submit(operation(63, voris::io::backend_operation_kind::accept, *token))
               .has_value());
    assert(backend.submit(operation(64, voris::io::backend_operation_kind::connect, *token))
               .has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    std::array<voris::io::backend_completion, 4> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);

    assert(backend.close_handle(*token).has_value());
    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 4);

    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 4);
    assert_drained_closed_completion(completions[0], 61);
    assert_drained_closed_completion(completions[1], 62);
    assert_drained_closed_completion(completions[2], 63);
    assert_drained_closed_completion(completions[3], 64);

    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);
}

void test_poll_observes_completions_in_batches_and_drain_preserves_order() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), deterministic_options(8, 8, 2));
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(2, voris::io::backend_operation_kind::write, *token))
               .has_value());
    assert(backend.submit(operation(3, voris::io::backend_operation_kind::read, *token))
               .has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    assert(backend.close_handle(*token).has_value());

    std::array<voris::io::backend_completion, 1> one_completion{};
    auto drained = backend.drain_completions(one_completion);
    assert(drained.has_value());
    assert(*drained == 0);

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 2);

    drained = backend.drain_completions(one_completion);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_completion_error(one_completion[0], 1, voris::io::vio_error_code::closed);

    drained = backend.drain_completions(one_completion);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_completion_error(one_completion[0], 2, voris::io::vio_error_code::closed);

    drained = backend.drain_completions(one_completion);
    assert(drained.has_value());
    assert(*drained == 0);

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);

    std::array<voris::io::backend_completion, 4> completions{};
    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_completion_error(completions[0], 3, voris::io::vio_error_code::closed);
}

void test_socket_operation_completions_drain_once_with_completion_batch_limit() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), deterministic_options(8, 8, 2));
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(71, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(72, voris::io::backend_operation_kind::accept, *token))
               .has_value());
    assert(backend.submit(operation(73, voris::io::backend_operation_kind::write, *token))
               .has_value());
    assert(backend.submit(operation(74, voris::io::backend_operation_kind::connect, *token))
               .has_value());
    assert(backend.poll().has_value());
    assert(backend.close_handle(*token).has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 2);

    std::array<voris::io::backend_completion, 4> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 2);
    assert_drained_closed_completion(completions[0], 71);
    assert_drained_closed_completion(completions[1], 72);

    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 2);

    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 2);
    assert_drained_closed_completion(completions[0], 73);
    assert_drained_closed_completion(completions[1], 74);

    polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);
}

void test_shutdown_closes_queued_and_pending_operations() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), deterministic_options(8, 1, 8));
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(1, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(2, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(3, voris::io::backend_operation_kind::read, *token))
               .has_value());

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    assert(backend.shutdown().has_value());
    assert(backend.state() == voris::io::backends::io_uring_backend_state::closed);
    assert_io_error(backend.poll(), voris::io::vio_error_code::closed);
    assert_void_error(backend.submit(operation(4, voris::io::backend_operation_kind::read,
                                               *token)),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.wake(), voris::io::vio_error_code::closed);

    std::array<voris::io::backend_completion, 4> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 3);
    assert_completion_error(completions[0], 1, voris::io::vio_error_code::closed);
    assert_completion_error(completions[1], 2, voris::io::vio_error_code::closed);
    assert_completion_error(completions[2], 3, voris::io::vio_error_code::closed);

    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);
}

void test_socket_operations_interact_with_queued_cancellation_and_shutdown() {
    voris::io::backends::io_uring_backend backend(
        core_capabilities(), deterministic_options(8, 1, 8));
    const auto token = backend.register_handle(1);
    assert(token.has_value());

    assert(backend.submit(operation(81, voris::io::backend_operation_kind::read, *token))
               .has_value());
    assert(backend.submit(operation(82, voris::io::backend_operation_kind::accept, *token))
               .has_value());
    assert(backend.submit(operation(83, voris::io::backend_operation_kind::write, *token))
               .has_value());
    assert(backend.submit(operation(84, voris::io::backend_operation_kind::connect, *token))
               .has_value());

    assert(backend.cancel(82, voris::io::cancellation_reason::manual).has_value());
    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert(backend.cancel(81, voris::io::cancellation_reason::manual).has_value());
    assert(backend.cancel(84, voris::io::cancellation_reason::manual).has_value());

    assert(backend.shutdown().has_value());

    std::array<voris::io::backend_completion, 4> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 4);
    assert_drained_closed_completion(completions[0], 81);
    assert_drained_closed_completion(completions[1], 82);
    assert_drained_closed_completion(completions[2], 83);
    assert_drained_closed_completion(completions[3], 84);

    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);
}

void test_detected_capabilities_are_conservative() {
    auto caps = voris::io::backends::detect_io_uring_capabilities();
    voris::io::backends::io_uring_backend backend(caps);
    assert(backend.capabilities().available == caps.available);

#if defined(__linux__)
    if (!caps.available) {
        assert_no_capabilities(caps);
        assert(!backend.default_eligible());
        assert_io_unsupported(backend.register_handle(1));
        return;
    }

    if (backend.default_eligible()) {
        assert(caps.supports_read);
        assert(caps.supports_write);
        assert(caps.supports_accept);
        assert(caps.supports_connect);
        assert(caps.supports_files);
        assert(caps.supports_fsync);
        assert(caps.supports_cancel);
    }

    if (caps.supports_registered_buffers) {
        assert(backend.register_buffers(2).has_value());
    } else {
        assert_void_unsupported(backend.register_buffers(2));
    }

    if (caps.supports_registered_files) {
        assert(backend.register_files(2).has_value());
    } else {
        assert_void_unsupported(backend.register_files(2));
    }
#else
    assert_no_capabilities(caps);
    assert(!backend.default_eligible());
    assert_io_unsupported(backend.register_handle(1));
#endif
}

} // namespace

int main() {
    using namespace voris::io;

    test_default_eligibility_requires_core_capabilities();
    test_backend_contract_carries_socket_payloads_and_results();
    test_probe_opcode_mapping_is_deterministic();
    test_probe_files_require_read_write_and_fsync();
    test_probe_registered_capabilities_are_independent_candidates();
    test_submit_rejects_missing_operation_opcodes();
    test_socket_operation_kinds_accept_only_with_matching_capability();
    test_submit_rejects_non_socket_operation_kinds();
    test_optional_registrations_follow_capabilities();
    test_lifecycle_state_tracks_availability_and_shutdown();
    test_empty_completion_drain_is_invalid_state();
    test_submit_validates_operation_before_queueing();
    test_submission_queue_is_bounded();
    test_queued_operation_can_be_cancelled_before_flush();
    test_kernel_mode_queued_cancellation_is_rejected_before_recording();
    test_kernel_submission_requires_cancel_capability_for_close_liveness();
    test_poll_flushes_submissions_in_batches();
    test_socket_operations_flow_through_submission_batches_and_close_fifo();
    test_poll_observes_completions_in_batches_and_drain_preserves_order();
    test_socket_operation_completions_drain_once_with_completion_batch_limit();
    test_shutdown_closes_queued_and_pending_operations();
    test_socket_operations_interact_with_queued_cancellation_and_shutdown();
    test_detected_capabilities_are_conservative();
#if defined(__linux__)
    test_linux_real_io_uring_read_write_transfer_returns_byte_counts();
    test_linux_real_io_uring_accept_returns_usable_nonblocking_socket();
    test_linux_real_io_uring_connect_completes();
    test_linux_real_io_uring_read_provider_error_is_reported();
    test_linux_real_io_uring_close_completes_queued_socket_operation();
    test_linux_real_io_uring_close_waits_for_submitted_cqe_before_completion();
    test_linux_real_io_uring_shutdown_cancels_submitted_read_without_data();
    test_linux_real_io_uring_submit_requires_cancel_for_kernel_liveness();
#endif

    auto caps = backends::io_uring_capabilities{
        .available = true,
        .supports_read = true,
        .supports_write = true,
        .supports_accept = true,
        .supports_connect = true,
        .supports_files = true,
        .supports_fsync = true,
        .supports_cancel = true,
    };
    backends::io_uring_backend backend(caps, deterministic_options());
    auto token = backend.register_handle(1);
    assert(token.has_value());
    assert(backend.submit(operation(1, backend_operation_kind::read, *token)).has_value());
    assert(backend.poll().has_value());
    assert(backend.cancel(1, cancellation_reason::manual).has_value());
    assert(backend.close_handle(*token).has_value());
    assert(backend.poll().has_value());
    std::array<backend_completion, 2> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
    assert(backend.shutdown().has_value());

    return 0;
}
