#include <voris/io/backends/epoll_backend.hpp>

#include <array>

#include "test_assert.hpp"

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace {

voris::io::backend_operation operation(std::size_t id,
                                       voris::io::backend_handle_token token) {
    voris::io::backend_operation result{};
    result.id = id;
    result.kind = voris::io::backend_operation_kind::read;
    result.handle = token;
    return result;
}

voris::io::backend_operation operation(std::size_t id,
                                       voris::io::backend_operation_kind kind,
                                       voris::io::backend_handle_token token) {
    voris::io::backend_operation result{};
    result.id = id;
    result.kind = kind;
    result.handle = token;
    return result;
}

voris::io::backend_operation file_operation(std::size_t id,
                                            voris::io::backend_operation_kind kind,
                                            voris::io::backend_handle_token token) {
    auto result = operation(id, kind, token);
    result.target = voris::io::backend_operation_target::file;
    return result;
}

void assert_void_error(const voris::io::void_result& result, voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_token_error(const voris::io::io_result<voris::io::backend_handle_token>& result,
                        voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_size_error(const voris::io::io_result<std::size_t>& result,
                       voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

void assert_closed_completion(const voris::io::backend_completion& completion,
                              std::size_t operation_id) {
    assert(completion.operation_id == operation_id);
    assert(!completion.result.has_value());
    assert(completion.result.error().classification == voris::io::vio_error_code::closed);
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

void make_pipe(unique_fd& read_end, unique_fd& write_end) {
    int pipe_fds[2]{-1, -1};
    assert(::pipe(pipe_fds) == 0);
    read_end.reset(pipe_fds[0]);
    write_end.reset(pipe_fds[1]);
}

void assert_provider_error(const voris::io::io_result<voris::io::backend_handle_token>& result,
                           int expected_errno) {
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

voris::io::backend_handle_token require_token(
    voris::io::io_result<voris::io::backend_handle_token> result) {
    assert(result.has_value());
    return *result;
}

std::size_t drain(voris::io::backend& backend,
                  std::array<voris::io::backend_completion, 8>& completions) {
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    return *drained;
}

void make_same_number_fd(int target_fd, unique_fd& fd) {
    if (fd.get() == target_fd) {
        return;
    }

    const int duplicate = ::dup2(fd.get(), target_fd);
    assert(duplicate == target_fd);
    fd.reset();
    fd.reset(duplicate);
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

    assert_token_error(backend.register_handle(0), voris::io::vio_error_code::invalid_state);

    unique_fd closed = make_event_fd();
    const int closed_fd = closed.release();
    assert(::close(closed_fd) == 0);
    assert_provider_error(backend.register_handle(static_cast<std::size_t>(closed_fd)), EBADF);

    assert(backend.shutdown().has_value());
}

void test_close_deregisters_without_taking_ownership() {
    unique_fd read_end;
    unique_fd write_end;
    make_pipe(read_end, write_end);

    {
        voris::io::backends::epoll_backend backend;
        auto token = require_token(backend.register_handle(static_cast<std::size_t>(read_end.get())));

        const char byte = 'x';
        assert(::write(write_end.get(), &byte, sizeof(byte)) == sizeof(byte));

        auto ready = backend.poll();
        assert(ready.has_value());
        assert(*ready == 1);

        assert(backend.close_handle(token).has_value());
        assert(backend.shutdown().has_value());
    }

    errno = 0;
    assert(::fcntl(read_end.get(), F_GETFD) != -1);
    assert(errno != EBADF);
}

void test_close_pending_operation_wins_over_queued_readiness() {
    unique_fd read_end;
    unique_fd write_end;
    make_pipe(read_end, write_end);

    voris::io::backends::epoll_backend backend;
    auto token = require_token(backend.register_handle(static_cast<std::size_t>(read_end.get())));
    assert(backend.submit(operation(301, token)).has_value());

    const char byte = 'x';
    assert(::write(write_end.get(), &byte, sizeof(byte)) == sizeof(byte));

    assert(backend.close_handle(token).has_value());
    auto polled = backend.poll();
    assert(polled.has_value());

    std::array<voris::io::backend_completion, 8> completions{};
    assert(drain(backend, completions) == 1);
    assert_closed_completion(completions[0], 301);

    assert_void_error(backend.cancel(301, voris::io::cancellation_reason::manual),
                      voris::io::vio_error_code::invalid_state);
    assert(drain(backend, completions) == 0);

    assert(backend.shutdown().has_value());
}

void test_close_after_external_fd_close_still_completes_pending_operation() {
    unique_fd fd = make_event_fd();

    voris::io::backends::epoll_backend backend;
    auto token = require_token(backend.register_handle(static_cast<std::size_t>(fd.get())));
    assert(backend.submit(operation(351, token)).has_value());

    const int raw_fd = fd.release();
    assert(::close(raw_fd) == 0);

    assert(backend.close_handle(token).has_value());

    std::array<voris::io::backend_completion, 8> completions{};
    assert(drain(backend, completions) == 1);
    assert_closed_completion(completions[0], 351);

    assert_void_error(backend.submit(operation(352, token)), voris::io::vio_error_code::invalid_state);
    assert_void_error(backend.close_handle(token), voris::io::vio_error_code::invalid_state);
    assert(backend.shutdown().has_value());
}

void test_readiness_mask_completes_only_compatible_operation_kind() {
    unique_fd read_end;
    unique_fd write_end;
    make_pipe(read_end, write_end);

    voris::io::backends::epoll_backend backend;
    auto token = require_token(backend.register_handle(static_cast<std::size_t>(write_end.get())));

    assert(backend.submit(
               operation(361, voris::io::backend_operation_kind::read, token))
               .has_value());

    auto write_ready_only = backend.poll();
    assert(write_ready_only.has_value());

    std::array<voris::io::backend_completion, 8> completions{};
    assert(drain(backend, completions) == 0);

    assert(backend.submit(
               operation(362, voris::io::backend_operation_kind::write, token))
               .has_value());

    write_ready_only = backend.poll();
    assert(write_ready_only.has_value());

    assert(drain(backend, completions) == 1);
    assert(completions[0].operation_id == 362);
    assert(completions[0].result.has_value());

    assert(backend.close_handle(token).has_value());
    assert(drain(backend, completions) == 1);
    assert_closed_completion(completions[0], 361);
    assert(backend.shutdown().has_value());
}

void test_submit_rejects_file_operations_for_readiness_backend() {
    unique_fd fd = make_event_fd();

    voris::io::backends::epoll_backend backend;
    auto token = require_token(backend.register_handle(static_cast<std::size_t>(fd.get())));

    assert_void_error(backend.submit(file_operation(371, voris::io::backend_operation_kind::read,
                                                    token)),
                      voris::io::vio_error_code::invalid_state);
    assert_void_error(backend.submit(file_operation(372, voris::io::backend_operation_kind::write,
                                                    token)),
                      voris::io::vio_error_code::invalid_state);
    assert_void_error(backend.submit(file_operation(373, voris::io::backend_operation_kind::fsync,
                                                    token)),
                      voris::io::vio_error_code::invalid_state);

    assert(backend.close_handle(token).has_value());
    assert(backend.shutdown().has_value());
}

void test_same_numeric_fd_reuse_uses_new_generation() {
    unique_fd old_read;
    unique_fd old_write;
    make_pipe(old_read, old_write);

    voris::io::backends::epoll_backend backend;
    const int reused_number = old_read.get();
    auto old_token =
        require_token(backend.register_handle(static_cast<std::size_t>(old_read.get())));
    assert(backend.submit(operation(401, old_token)).has_value());

    const char byte = 'x';
    assert(::write(old_write.get(), &byte, sizeof(byte)) == sizeof(byte));
    assert(backend.close_handle(old_token).has_value());

    std::array<voris::io::backend_completion, 8> completions{};
    assert(drain(backend, completions) == 1);
    assert_closed_completion(completions[0], 401);

    old_read.reset();
    old_write.reset();

    unique_fd new_read;
    unique_fd new_write;
    make_pipe(new_read, new_write);
    make_same_number_fd(reused_number, new_read);

    auto new_token =
        require_token(backend.register_handle(static_cast<std::size_t>(new_read.get())));
    assert(new_token.native_handle == old_token.native_handle);
    assert(new_token.generation > old_token.generation);

    assert_void_error(backend.submit(operation(402, old_token)),
                      voris::io::vio_error_code::invalid_state);
    assert(backend.submit(operation(403, new_token)).has_value());

    auto stale_poll = backend.poll();
    assert(stale_poll.has_value());
    assert(drain(backend, completions) == 0);

    assert(::write(new_write.get(), &byte, sizeof(byte)) == sizeof(byte));
    auto current_poll = backend.poll();
    assert(current_poll.has_value());

    assert(drain(backend, completions) == 1);
    assert(completions[0].operation_id == 403);
    assert(completions[0].result.has_value());

    assert(backend.close_handle(new_token).has_value());
    assert(backend.shutdown().has_value());
}

void test_shutdown_defines_later_behavior() {
    voris::io::backends::epoll_backend backend;

    assert(backend.shutdown().has_value());
    assert(backend.shutdown().has_value());

    unique_fd fd = make_event_fd();
    assert_token_error(backend.register_handle(static_cast<std::size_t>(fd.get())),
                       voris::io::vio_error_code::closed);
    assert_void_error(backend.submit(operation(1, {1, 1})), voris::io::vio_error_code::closed);
    assert_void_error(backend.cancel(1, voris::io::cancellation_reason::manual),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.close_handle({1, 1}), voris::io::vio_error_code::closed);
    assert_void_error(backend.wake(), voris::io::vio_error_code::closed);

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
}
#else
void test_non_linux_reports_unsupported() {
    voris::io::backends::epoll_backend backend;

    assert_token_error(backend.register_handle(1), voris::io::vio_error_code::unsupported);
    assert_void_error(backend.submit(operation(1, {1, 1})), voris::io::vio_error_code::unsupported);
    assert_void_error(backend.cancel(1, voris::io::cancellation_reason::manual),
                      voris::io::vio_error_code::unsupported);
    assert_size_error(backend.poll(), voris::io::vio_error_code::unsupported);
    assert_void_error(backend.wake(), voris::io::vio_error_code::unsupported);
    assert_void_error(backend.close_handle({1, 1}), voris::io::vio_error_code::unsupported);
    std::array<voris::io::backend_completion, 1> completions{};
    assert_size_error(backend.drain_completions(completions),
                      voris::io::vio_error_code::unsupported);
    assert(backend.shutdown().has_value());
}
#endif

} // namespace

int main() {
#if defined(__linux__)
    test_wake_is_polled_once();
    test_register_validates_and_reports_provider_errors();
    test_close_deregisters_without_taking_ownership();
    test_close_pending_operation_wins_over_queued_readiness();
    test_close_after_external_fd_close_still_completes_pending_operation();
    test_readiness_mask_completes_only_compatible_operation_kind();
    test_submit_rejects_file_operations_for_readiness_backend();
    test_same_numeric_fd_reuse_uses_new_generation();
    test_shutdown_defines_later_behavior();
#else
    test_non_linux_reports_unsupported();
#endif

    return 0;
}
