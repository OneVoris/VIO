#include <voris/io/socket.hpp>

#include <array>
#include <cstddef>
#include <limits>

#include "test_assert.hpp"

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

void assert_size_error(const voris::io::io_result<std::size_t>& result,
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
    using voris::io::total_size;

    std::array<std::byte, 3> first{};
    std::array<std::byte, 5> second{};
    std::array<buffer_chain_view, 2> buffers{{
        buffer_chain_view{std::span<const std::byte>(first)},
        buffer_chain_view{std::span<const std::byte>(second)},
    }};
    assert(total_size(buffers) == 8);
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

void test_linux_read_write_some_make_short_progress_without_taking_ownership() {
    using voris::io::read_some;
    using voris::io::write_some;

    auto pipe = make_pipe();
    set_nonblocking_fd(pipe[0].get());
    set_nonblocking_fd(pipe[1].get());

    const std::array<std::byte, 3> input{std::byte{0x61}, std::byte{0x62},
                                        std::byte{0x63}};
    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(pipe[1].get()), input);
    assert(written.has_value());
    assert(*written == input.size());

    std::array<std::byte, 3> output{};
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(pipe[0].get()), output);
    assert(read.has_value());
    assert(*read == output.size());
    assert(output == input);

    assert(::fcntl(pipe[0].get(), F_GETFD) != -1);
    assert(::fcntl(pipe[1].get(), F_GETFD) != -1);

    const std::array<std::byte, 1> next_input{std::byte{0x64}};
    written = write_some(static_cast<std::size_t>(pipe[1].get()), next_input);
    assert(written.has_value());
    assert(*written == next_input.size());

    std::array<std::byte, 1> next_output{};
    read = read_some(static_cast<std::size_t>(pipe[0].get()), next_output);
    assert(read.has_value());
    assert(*read == next_output.size());
    assert(next_output == next_input);
}

void test_linux_would_block_read_uses_operation_in_progress() {
    using voris::io::read_some;
    using voris::io::vio_error_code;

    auto pipe = make_pipe();
    set_nonblocking_fd(pipe[0].get());

    std::array<std::byte, 1> output{};
    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(pipe[0].get()), output);
    assert_size_error(read, vio_error_code::operation_in_progress);
    assert(!read.error().provider_code.has_value());
}

void test_linux_zero_length_operations_return_zero() {
    using voris::io::read_some;
    using voris::io::write_some;

    auto pipe = make_pipe();
    set_nonblocking_fd(pipe[0].get());
    set_nonblocking_fd(pipe[1].get());

    voris::io::io_result<std::size_t> read =
        read_some(static_cast<std::size_t>(pipe[0].get()), std::span<std::byte>{});
    assert(read.has_value());
    assert(*read == 0);

    voris::io::io_result<std::size_t> written =
        write_some(static_cast<std::size_t>(pipe[1].get()), std::span<const std::byte>{});
    assert(written.has_value());
    assert(*written == 0);
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

    assert_size_error(read_some(0, buffer), vio_error_code::invalid_state);
    assert_size_error(write_some(0, input), vio_error_code::invalid_state);

    voris::io::io_result<std::size_t> empty_read = read_some(1, std::span<std::byte>{});
    assert(empty_read.has_value());
    assert(*empty_read == 0);

    voris::io::io_result<std::size_t> empty_write =
        write_some(1, std::span<const std::byte>{});
    assert(empty_write.has_value());
    assert(*empty_write == 0);

    assert_size_error(read_some(1, buffer), vio_error_code::unsupported);
    assert_size_error(write_some(1, input), vio_error_code::unsupported);
}

#endif

} // namespace

int main() {
    test_socket_operation_queue();
    test_total_size();

#if defined(__linux__)
    test_linux_read_write_some_make_short_progress_without_taking_ownership();
    test_linux_would_block_read_uses_operation_in_progress();
    test_linux_zero_length_operations_return_zero();
    test_linux_invalid_handles_return_invalid_state();
    test_linux_closed_fd_reports_provider_error();
#else
    test_non_linux_read_write_some_validation_and_unsupported();
#endif

    return 0;
}
