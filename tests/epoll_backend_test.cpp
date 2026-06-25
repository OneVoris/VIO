#include <voris/io/backends/epoll_backend.hpp>

#include "test_assert.hpp"

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace {

void assert_void_error(const voris::io::void_result& result, voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_size_error(const voris::io::io_result<std::size_t>& result,
                       voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
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

void assert_provider_error(const voris::io::void_result& result, int expected_errno) {
    assert(!result.has_value());
    assert(result.error().classification == voris::io::vio_error_code::backend_failure);
    assert(result.error().provider_code.has_value());
    assert(*result.error().provider_code == expected_errno);
}

unique_fd make_event_fd() {
    unique_fd fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    assert(fd.get() >= 0);
    return fd;
}

void test_wake_is_polled_once() {
    voris::io::backends::epoll_backend backend;

    auto idle = backend.poll();
    assert(idle.has_value());
    assert(*idle == 0);

    assert(backend.wake().has_value());
    auto woken = backend.poll();
    assert(woken.has_value());
    assert(*woken == 1);

    auto drained = backend.poll();
    assert(drained.has_value());
    assert(*drained == 0);

    assert(backend.shutdown().has_value());
}

void test_register_validates_and_reports_provider_errors() {
    voris::io::backends::epoll_backend backend;

    assert_void_error(backend.register_handle(0), voris::io::vio_error_code::invalid_state);

    unique_fd closed = make_event_fd();
    const int closed_fd = closed.release();
    assert(::close(closed_fd) == 0);
    assert_provider_error(backend.register_handle(static_cast<std::size_t>(closed_fd)), EBADF);

    assert(backend.shutdown().has_value());
}

void test_registered_fd_readiness_is_reported_without_taking_ownership() {
    int pipe_fds[2]{-1, -1};
    assert(::pipe(pipe_fds) == 0);
    unique_fd read_end(pipe_fds[0]);
    unique_fd write_end(pipe_fds[1]);

    {
        voris::io::backends::epoll_backend backend;
        assert(backend.register_handle(static_cast<std::size_t>(read_end.get())).has_value());

        const char byte = 'x';
        assert(::write(write_end.get(), &byte, sizeof(byte)) == sizeof(byte));

        auto ready = backend.poll();
        assert(ready.has_value());
        assert(*ready == 1);

        assert(backend.shutdown().has_value());
    }

    errno = 0;
    assert(::fcntl(read_end.get(), F_GETFD) != -1);
    assert(errno != EBADF);
}

void test_shutdown_defines_later_behavior() {
    voris::io::backends::epoll_backend backend;

    assert(backend.shutdown().has_value());
    assert(backend.shutdown().has_value());

    unique_fd fd = make_event_fd();
    assert_void_error(backend.register_handle(static_cast<std::size_t>(fd.get())),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.submit(voris::io::backend_operation{
                          1, voris::io::backend_operation_kind::read, {}}),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.cancel(1, voris::io::cancellation_reason::manual),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.wake(), voris::io::vio_error_code::closed);

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
}
#else
void test_non_linux_reports_unsupported() {
    voris::io::backends::epoll_backend backend;

    assert_void_error(backend.register_handle(1), voris::io::vio_error_code::unsupported);
    assert_void_error(backend.submit(voris::io::backend_operation{
                          1, voris::io::backend_operation_kind::read, {}}),
                      voris::io::vio_error_code::unsupported);
    assert_void_error(backend.cancel(1, voris::io::cancellation_reason::manual),
                      voris::io::vio_error_code::unsupported);
    assert_size_error(backend.poll(), voris::io::vio_error_code::unsupported);
    assert_void_error(backend.wake(), voris::io::vio_error_code::unsupported);
    assert(backend.shutdown().has_value());
}
#endif

} // namespace

int main() {
#if defined(__linux__)
    test_wake_is_polled_once();
    test_register_validates_and_reports_provider_errors();
    test_registered_fd_readiness_is_reported_without_taking_ownership();
    test_shutdown_defines_later_behavior();
#else
    test_non_linux_reports_unsupported();
#endif

    return 0;
}
