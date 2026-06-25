#include <voris/io/backends/epoll_backend.hpp>
#include <voris/io/backends/io_uring_backend.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "test_assert.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

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

voris::io::backends::io_uring_capabilities deterministic_io_uring_capabilities()
    noexcept {
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

voris::io::backends::io_uring_backend_options deterministic_io_uring_options()
    noexcept {
    return voris::io::backends::io_uring_backend_options{
        .submission_queue_capacity = 64,
        .submit_batch_limit = 32,
        .completion_batch_limit = 32,
        .enable_kernel_submission = false,
    };
}

voris::io::backends::io_uring_backend_options real_io_uring_options() noexcept {
    return voris::io::backends::io_uring_backend_options{
        .submission_queue_capacity = 16,
        .submit_batch_limit = 8,
        .completion_batch_limit = 8,
        .enable_kernel_submission = true,
    };
}

template <class Result>
voris::io::vio_error_code error_classification(const Result& result) {
    assert(!result.has_value());
    return result.error().classification;
}

template <class Result>
void assert_error(const Result& result, voris::io::vio_error_code expected) {
    assert(error_classification(result) == expected);
}

struct normalized_completion {
    std::size_t operation_id{};
    bool success{};
    voris::io::vio_error_code error{voris::io::vio_error_code::none};

    [[nodiscard]] friend bool operator==(normalized_completion lhs,
                                         normalized_completion rhs) noexcept = default;
};

normalized_completion normalize(const voris::io::backend_completion& completion) {
    return normalized_completion{
        .operation_id = completion.operation_id,
        .success = completion.result.has_value(),
        .error = completion.result.has_value()
                     ? voris::io::vio_error_code::none
                     : completion.result.error().classification,
    };
}

std::vector<normalized_completion> drain_all(voris::io::backend& backend) {
    std::array<voris::io::backend_completion, 8> batch{};
    std::vector<normalized_completion> completions{};
    for (;;) {
        auto drained = backend.drain_completions(batch);
        assert(drained.has_value());
        if (*drained == 0) {
            return completions;
        }
        for (std::size_t index = 0; index < *drained; ++index) {
            completions.push_back(normalize(batch[index]));
        }
    }
}

void assert_no_extra_completion(voris::io::backend& backend) {
    assert(drain_all(backend).empty());
}

struct invalid_input_observation {
    std::vector<voris::io::vio_error_code> errors{};

    [[nodiscard]] friend bool operator==(const invalid_input_observation& lhs,
                                         const invalid_input_observation& rhs) = default;
};

struct stale_token_observation {
    std::vector<voris::io::vio_error_code> errors{};
    std::vector<normalized_completion> completions{};

    [[nodiscard]] friend bool operator==(const stale_token_observation& lhs,
                                         const stale_token_observation& rhs) = default;
};

struct shutdown_observation {
    std::vector<normalized_completion> completions{};
    std::vector<voris::io::vio_error_code> shared_late_errors{};
};

voris::io::backend_handle_token require_token(
    voris::io::io_result<voris::io::backend_handle_token> result) {
    assert(result.has_value());
    return *result;
}

constexpr int test_io_uring_canceled_result = -125;

voris::io::backends::io_uring_backend_options test_kernel_io_uring_options()
    noexcept {
    return voris::io::backends::io_uring_backend_options{
        .submission_queue_capacity = 16,
        .submit_batch_limit = 8,
        .completion_batch_limit = 8,
        .enable_kernel_submission = true,
    };
}

class deterministic_io_uring_fixture {
public:
    [[nodiscard]] std::size_t make_native_handle() noexcept {
        return next_native_handle_++;
    }

    [[nodiscard]] std::size_t invalid_native_handle() const noexcept {
        return 0;
    }

    [[nodiscard]] std::size_t recreate_native_handle_with_same_number(
        std::size_t native_handle) noexcept {
        return native_handle;
    }

    void make_pending_completions_visible() {
        auto polled = backend.poll();
        assert(polled.has_value());
    }

    voris::io::backends::io_uring_backend backend{
        deterministic_io_uring_capabilities(),
        deterministic_io_uring_options(),
    };

private:
    std::size_t next_native_handle_{1};
};

class io_uring_test_kernel_fixture {
public:
    io_uring_test_kernel_fixture() {
        voris::io::backends::detail::attach_io_uring_test_kernel(backend, kernel);
    }

    [[nodiscard]] voris::io::backend_handle_token register_handle(
        std::size_t native_handle = 1) {
        return require_token(backend.register_handle(native_handle));
    }

    void flush_submissions(std::size_t expected_submission_count) {
        auto polled = backend.poll();
        assert(polled.has_value());
        assert(*polled == 0);
        assert(kernel.submissions.size() == expected_submission_count);
    }

    void push_operation_completion(std::size_t operation_id, int result) {
        kernel.completions.push_back(voris::io::backends::detail::io_uring_test_completion{
            voris::io::backends::detail::io_uring_test_completion_kind::operation,
            operation_id,
            result,
        });
    }

    void push_cancel_ack(std::size_t operation_id) {
        kernel.completions.push_back(voris::io::backends::detail::io_uring_test_completion{
            voris::io::backends::detail::io_uring_test_completion_kind::cancel_ack,
            operation_id,
            0,
        });
    }

    voris::io::backends::detail::io_uring_test_kernel kernel{};
    voris::io::backends::io_uring_backend backend{
        deterministic_io_uring_capabilities(),
        test_kernel_io_uring_options(),
    };
};

std::vector<normalized_completion> run_queued_io_uring_cancel_completion() {
    using namespace voris::io;

    deterministic_io_uring_fixture fixture;
    const auto token = require_token(fixture.backend.register_handle(1));
    std::array<std::byte, 1> output{};

    assert(fixture.backend.submit(read_operation(801, token, output)).has_value());
    assert(fixture.backend.cancel(801, cancellation_reason::manual).has_value());

    auto completions = drain_all(fixture.backend);
    assert_no_extra_completion(fixture.backend);
    assert(fixture.backend.close_handle(token).has_value());
    return completions;
}

std::vector<normalized_completion> run_submitted_io_uring_cancel_completion() {
    using namespace voris::io;

    io_uring_test_kernel_fixture fixture;
    const auto token = fixture.register_handle();
    std::array<std::byte, 1> output{};

    assert(fixture.backend.submit(read_operation(801, token, output)).has_value());
    fixture.flush_submissions(1);
    assert(fixture.backend.cancel(801, cancellation_reason::manual).has_value());
    assert(fixture.kernel.submitted_cancel_operation_ids.size() == 1);
    assert(fixture.kernel.submitted_cancel_operation_ids[0] == 801);

    fixture.push_cancel_ack(801);
    auto polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert_no_extra_completion(fixture.backend);

    fixture.push_operation_completion(801, test_io_uring_canceled_result);
    polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    auto completions = drain_all(fixture.backend);
    assert_no_extra_completion(fixture.backend);

    fixture.push_cancel_ack(801);
    polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert_no_extra_completion(fixture.backend);
    return completions;
}

void test_io_uring_cancelled_terminal_completion_is_exactly_once() {
    using namespace voris::io;

    const auto expected = std::vector<normalized_completion>{
        normalized_completion{801, false, vio_error_code::cancelled},
    };
    const auto queued = run_queued_io_uring_cancel_completion();
    const auto submitted = run_submitted_io_uring_cancel_completion();

    assert(queued == expected);
    assert(submitted == expected);
    assert(queued == submitted);
}

void test_io_uring_test_kernel_submitted_close_drains_closed_once() {
    using namespace voris::io;

    io_uring_test_kernel_fixture fixture;
    const auto token = fixture.register_handle();
    std::array<std::byte, 1> output{};

    assert(fixture.backend.submit(read_operation(811, token, output)).has_value());
    fixture.flush_submissions(1);
    assert(fixture.backend.close_handle(token).has_value());
    assert(fixture.kernel.submitted_cancel_operation_ids.size() == 1);
    assert(fixture.kernel.submitted_cancel_operation_ids[0] == 811);
    assert_no_extra_completion(fixture.backend);

    fixture.push_cancel_ack(811);
    auto polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert_no_extra_completion(fixture.backend);

    fixture.push_operation_completion(811, 1);
    polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    const auto completions = drain_all(fixture.backend);
    assert(completions == (std::vector<normalized_completion>{
                              normalized_completion{811, false, vio_error_code::closed},
                          }));
    assert_no_extra_completion(fixture.backend);

    fixture.push_cancel_ack(811);
    polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert_no_extra_completion(fixture.backend);
}

void test_io_uring_test_kernel_submitted_shutdown_drains_closed_once() {
    using namespace voris::io;

    io_uring_test_kernel_fixture fixture;
    const auto token = fixture.register_handle();
    std::array<std::byte, 1> output{};

    assert(fixture.backend.submit(read_operation(821, token, output)).has_value());
    fixture.flush_submissions(1);
    assert(fixture.backend.shutdown().has_value());
    assert(fixture.kernel.submitted_cancel_operation_ids.size() == 1);
    assert(fixture.kernel.submitted_cancel_operation_ids[0] == 821);
    assert_no_extra_completion(fixture.backend);

    fixture.push_cancel_ack(821);
    auto polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert_no_extra_completion(fixture.backend);

    fixture.push_operation_completion(821, test_io_uring_canceled_result);
    polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    const auto completions = drain_all(fixture.backend);
    assert(completions == (std::vector<normalized_completion>{
                              normalized_completion{821, false, vio_error_code::closed},
                          }));
    assert_no_extra_completion(fixture.backend);

    polled = fixture.backend.poll();
    assert_error(polled, vio_error_code::closed);
}

void test_io_uring_test_kernel_stale_cqe_after_handle_reuse_is_isolated() {
    using namespace voris::io;

    io_uring_test_kernel_fixture fixture;
    const auto stale = fixture.register_handle(1);
    std::array<std::byte, 1> old_output{};

    assert(fixture.backend.submit(read_operation(831, stale, old_output)).has_value());
    fixture.flush_submissions(1);
    assert(fixture.backend.close_handle(stale).has_value());

    const auto current = fixture.register_handle(1);
    assert(current.native_handle == stale.native_handle);
    assert(current.generation > stale.generation);

    std::array<std::byte, 1> current_output{};
    assert(fixture.backend.submit(read_operation(832, current, current_output)).has_value());

    fixture.push_operation_completion(831, 1);
    auto polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    auto completions = drain_all(fixture.backend);
    assert(completions == (std::vector<normalized_completion>{
                              normalized_completion{831, false, vio_error_code::closed},
                          }));
    assert_no_extra_completion(fixture.backend);

    fixture.push_cancel_ack(831);
    polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);
    assert_no_extra_completion(fixture.backend);

    fixture.push_operation_completion(832, 1);
    polled = fixture.backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    completions = drain_all(fixture.backend);
    assert(completions == (std::vector<normalized_completion>{
                              normalized_completion{832, true, vio_error_code::none},
                          }));
    assert_no_extra_completion(fixture.backend);
    assert(fixture.backend.close_handle(current).has_value());
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

unique_fd make_event_fd() {
    unique_fd fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    assert(fd.get() >= 0);
    if (fd.get() != 0) {
        return fd;
    }

    const int duplicate = ::fcntl(fd.get(), F_DUPFD_CLOEXEC, 1);
    assert(duplicate > 0);
    return unique_fd(duplicate);
}

std::array<unique_fd, 2> make_socket_pair() {
    std::array<int, 2> fds{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                        fds.data()) == 0);
    return {unique_fd(fds[0]), unique_fd(fds[1])};
}

class epoll_fixture {
public:
    [[nodiscard]] std::size_t make_native_handle() {
        handles_.push_back(make_event_fd());
        return static_cast<std::size_t>(handles_.back().get());
    }

    [[nodiscard]] std::size_t invalid_native_handle() const noexcept {
        return 0;
    }

    [[nodiscard]] std::size_t recreate_native_handle_with_same_number(
        std::size_t native_handle) {
        close_native_handle(native_handle);

        auto replacement = make_event_fd();
        const auto target = static_cast<int>(native_handle);
        if (replacement.get() != target) {
            const int duplicate = ::dup2(replacement.get(), target);
            assert(duplicate == target);
            replacement.reset();
            replacement.reset(duplicate);
        }

        handles_.push_back(std::move(replacement));
        return native_handle;
    }

    void make_pending_completions_visible() noexcept {}

    voris::io::backends::epoll_backend backend{};

private:
    void close_native_handle(std::size_t native_handle) noexcept {
        for (auto& handle : handles_) {
            if (handle.get() == static_cast<int>(native_handle)) {
                handle.reset();
                return;
            }
        }
        assert(false);
    }

    std::vector<unique_fd> handles_{};
};

template <class Fixture>
invalid_input_observation exercise_invalid_inputs() {
    using namespace voris::io;

    Fixture fixture;
    const auto token = require_token(
        fixture.backend.register_handle(fixture.make_native_handle()));

    invalid_input_observation observed{};
    observed.errors.push_back(error_classification(
        fixture.backend.register_handle(fixture.invalid_native_handle())));
    observed.errors.push_back(error_classification(
        fixture.backend.submit(operation(0, backend_operation_kind::read, token))));
    observed.errors.push_back(error_classification(
        fixture.backend.submit(operation(11, backend_operation_kind::read, {}))));
    observed.errors.push_back(error_classification(
        fixture.backend.submit(operation(12, backend_operation_kind::fsync, token))));
    observed.errors.push_back(error_classification(
        fixture.backend.submit(file_operation(13, backend_operation_kind::accept, token))));

    assert(fixture.backend.close_handle(token).has_value());
    return observed;
}

template <class Fixture>
voris::io::vio_error_code exercise_duplicate_operation_id() {
    using namespace voris::io;

    Fixture fixture;
    const auto token = require_token(
        fixture.backend.register_handle(fixture.make_native_handle()));

    assert(fixture.backend.submit(operation(101, backend_operation_kind::read, token))
               .has_value());
    const auto duplicate = error_classification(
        fixture.backend.submit(operation(101, backend_operation_kind::read, token)));

    assert(fixture.backend.close_handle(token).has_value());
    fixture.make_pending_completions_visible();
    const auto completions = drain_all(fixture.backend);
    assert(completions.size() == 1);
    assert(completions[0] == (normalized_completion{
                                 .operation_id = 101,
                                 .success = false,
                                 .error = vio_error_code::closed,
                             }));
    assert_no_extra_completion(fixture.backend);

    return duplicate;
}

template <class Fixture>
std::vector<normalized_completion> exercise_close_pending_read_write_once() {
    using namespace voris::io;

    Fixture fixture;
    const auto token = require_token(
        fixture.backend.register_handle(fixture.make_native_handle()));

    assert(fixture.backend.submit(operation(201, backend_operation_kind::read, token))
               .has_value());
    assert(fixture.backend.submit(operation(202, backend_operation_kind::write, token))
               .has_value());

    assert(fixture.backend.close_handle(token).has_value());
    fixture.make_pending_completions_visible();
    auto completions = drain_all(fixture.backend);
    assert_no_extra_completion(fixture.backend);
    return completions;
}

template <class Fixture>
std::vector<normalized_completion> exercise_close_one_handle_only() {
    using namespace voris::io;

    Fixture fixture;
    const auto first = require_token(
        fixture.backend.register_handle(fixture.make_native_handle()));
    const auto second = require_token(
        fixture.backend.register_handle(fixture.make_native_handle()));

    assert(fixture.backend.submit(operation(301, backend_operation_kind::read, first))
               .has_value());
    assert(fixture.backend.submit(operation(302, backend_operation_kind::read, second))
               .has_value());

    assert(fixture.backend.close_handle(first).has_value());
    fixture.make_pending_completions_visible();
    auto completions = drain_all(fixture.backend);
    assert(completions.size() == 1);
    assert(completions[0].operation_id == 301);
    assert_no_extra_completion(fixture.backend);

    assert(fixture.backend.close_handle(second).has_value());
    fixture.make_pending_completions_visible();
    auto second_completions = drain_all(fixture.backend);
    assert(second_completions.size() == 1);
    assert(second_completions[0].operation_id == 302);
    completions.insert(completions.end(), second_completions.begin(),
                       second_completions.end());
    return completions;
}

template <class Fixture>
stale_token_observation exercise_stale_token_after_handle_reuse() {
    using namespace voris::io;

    Fixture fixture;
    const auto native_handle = fixture.make_native_handle();
    const auto stale = require_token(fixture.backend.register_handle(native_handle));
    assert(fixture.backend.close_handle(stale).has_value());

    const auto reused_native = fixture.recreate_native_handle_with_same_number(native_handle);
    const auto current = require_token(
        fixture.backend.register_handle(reused_native));
    assert(current.native_handle == stale.native_handle);
    assert(current.generation > stale.generation);

    stale_token_observation observed{};
    observed.errors.push_back(error_classification(
        fixture.backend.submit(operation(401, backend_operation_kind::read, stale))));
    observed.errors.push_back(error_classification(fixture.backend.close_handle(stale)));

    assert(fixture.backend.submit(operation(402, backend_operation_kind::read, current))
               .has_value());
    assert(fixture.backend.close_handle(current).has_value());
    fixture.make_pending_completions_visible();
    observed.completions = drain_all(fixture.backend);
    assert_no_extra_completion(fixture.backend);
    return observed;
}

template <class Fixture>
shutdown_observation exercise_shutdown_contract() {
    using namespace voris::io;

    Fixture fixture;
    const auto token = require_token(
        fixture.backend.register_handle(fixture.make_native_handle()));
    assert(fixture.backend.submit(operation(501, backend_operation_kind::read, token))
               .has_value());

    assert(fixture.backend.shutdown().has_value());

    shutdown_observation observed{};
    observed.completions = drain_all(fixture.backend);
    observed.shared_late_errors.push_back(error_classification(
        fixture.backend.register_handle(fixture.make_native_handle())));
    observed.shared_late_errors.push_back(error_classification(
        fixture.backend.submit(operation(502, backend_operation_kind::write, token))));
    observed.shared_late_errors.push_back(error_classification(
        fixture.backend.cancel(501, cancellation_reason::manual)));
    observed.shared_late_errors.push_back(error_classification(fixture.backend.close_handle(token)));
    observed.shared_late_errors.push_back(error_classification(fixture.backend.wake()));
    return observed;
}

void test_invalid_inputs_match_between_epoll_and_io_uring() {
    const auto epoll = exercise_invalid_inputs<epoll_fixture>();
    const auto uring = exercise_invalid_inputs<deterministic_io_uring_fixture>();
    assert(epoll == uring);
}

void test_duplicate_operation_id_while_pending_matches() {
    const auto epoll = exercise_duplicate_operation_id<epoll_fixture>();
    const auto uring = exercise_duplicate_operation_id<deterministic_io_uring_fixture>();
    assert(epoll == uring);
    assert(epoll == voris::io::vio_error_code::invalid_state);
}

void test_close_pending_read_write_matches_exactly_once() {
    using namespace voris::io;

    const auto expected = std::vector<normalized_completion>{
        normalized_completion{201, false, vio_error_code::closed},
        normalized_completion{202, false, vio_error_code::closed},
    };
    const auto epoll = exercise_close_pending_read_write_once<epoll_fixture>();
    const auto uring = exercise_close_pending_read_write_once<deterministic_io_uring_fixture>();

    assert(epoll == expected);
    assert(uring == expected);
    assert(epoll == uring);
}

void test_close_one_handle_does_not_complete_another_handle() {
    using namespace voris::io;

    const auto expected = std::vector<normalized_completion>{
        normalized_completion{301, false, vio_error_code::closed},
        normalized_completion{302, false, vio_error_code::closed},
    };
    const auto epoll = exercise_close_one_handle_only<epoll_fixture>();
    const auto uring = exercise_close_one_handle_only<deterministic_io_uring_fixture>();

    assert(epoll == expected);
    assert(uring == expected);
    assert(epoll == uring);
}

void test_stale_token_after_numeric_handle_reuse_matches() {
    using namespace voris::io;

    const auto expected_errors = std::vector{
        vio_error_code::invalid_state,
        vio_error_code::invalid_state,
    };
    const auto expected_completions = std::vector<normalized_completion>{
        normalized_completion{402, false, vio_error_code::closed},
    };
    const auto epoll = exercise_stale_token_after_handle_reuse<epoll_fixture>();
    const auto uring =
        exercise_stale_token_after_handle_reuse<deterministic_io_uring_fixture>();

    assert(epoll.errors == expected_errors);
    assert(uring.errors == expected_errors);
    assert(epoll.completions == expected_completions);
    assert(uring.completions == expected_completions);
    assert(epoll == uring);
}

void test_shutdown_shared_contract_matches_and_poll_difference_is_gated() {
    using namespace voris::io;

    const auto expected_completions = std::vector<normalized_completion>{
        normalized_completion{501, false, vio_error_code::closed},
    };
    const auto expected_late_errors = std::vector{
        vio_error_code::closed,
        vio_error_code::closed,
        vio_error_code::closed,
        vio_error_code::closed,
        vio_error_code::closed,
    };

    const auto epoll = exercise_shutdown_contract<epoll_fixture>();
    const auto uring = exercise_shutdown_contract<deterministic_io_uring_fixture>();

    assert(epoll.completions == expected_completions);
    assert(uring.completions == expected_completions);
    assert(epoll.shared_late_errors == expected_late_errors);
    assert(uring.shared_late_errors == expected_late_errors);

    epoll_fixture stopped_epoll;
    deterministic_io_uring_fixture stopped_uring;
    assert(stopped_epoll.backend.shutdown().has_value());
    assert(stopped_uring.backend.shutdown().has_value());
    auto epoll_poll = stopped_epoll.backend.poll();
    auto uring_poll = stopped_uring.backend.poll();
    assert(epoll_poll.has_value());
    assert(*epoll_poll == 0);
    assert_error(uring_poll, vio_error_code::closed);
}

voris::io::backend_completion wait_for_completion(voris::io::backend& backend) {
    using namespace std::chrono_literals;

    std::array<voris::io::backend_completion, 1> completions{};
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        auto polled = backend.poll();
        assert(polled.has_value());

        auto drained = backend.drain_completions(completions);
        assert(drained.has_value());
        if (*drained == 1) {
            assert_no_extra_completion(backend);
            return completions[0];
        }

        std::this_thread::sleep_for(1ms);
    }

    assert(false);
    return {};
}

normalized_completion run_epoll_read_success(std::size_t operation_id) {
    auto sockets = make_socket_pair();
    voris::io::backends::epoll_backend backend;
    const auto token = backend.register_handle(static_cast<std::size_t>(sockets[1].get()));
    assert(token.has_value());

    const std::array<std::byte, 1> payload{std::byte{'r'}};
    assert(::send(sockets[0].get(), payload.data(), payload.size(), MSG_NOSIGNAL) == 1);

    std::array<std::byte, 1> output{};
    assert(backend.submit(read_operation(operation_id, *token, output)).has_value());
    auto polled = backend.poll();
    assert(polled.has_value());
    const auto completions = drain_all(backend);
    assert(completions.size() == 1);
    assert_no_extra_completion(backend);
    assert(backend.shutdown().has_value());
    return completions[0];
}

normalized_completion run_epoll_write_success(std::size_t operation_id) {
    auto sockets = make_socket_pair();
    voris::io::backends::epoll_backend backend;
    const auto token = backend.register_handle(static_cast<std::size_t>(sockets[0].get()));
    assert(token.has_value());

    const std::array<std::byte, 1> payload{std::byte{'w'}};
    assert(backend.submit(write_operation(operation_id, *token, payload)).has_value());
    auto polled = backend.poll();
    assert(polled.has_value());
    const auto completions = drain_all(backend);
    assert(completions.size() == 1);
    assert_no_extra_completion(backend);
    assert(backend.shutdown().has_value());
    return completions[0];
}

normalized_completion run_real_io_uring_read_success(
    std::size_t operation_id,
    const voris::io::backends::io_uring_capabilities& capabilities) {
    auto sockets = make_socket_pair();
    voris::io::backends::io_uring_backend backend(capabilities, real_io_uring_options());
    const auto token = backend.register_handle(static_cast<std::size_t>(sockets[1].get()));
    assert(token.has_value());

    const std::array<std::byte, 1> payload{std::byte{'r'}};
    assert(::send(sockets[0].get(), payload.data(), payload.size(), MSG_NOSIGNAL) == 1);

    std::array<std::byte, 1> output{};
    assert(backend.submit(read_operation(operation_id, *token, output)).has_value());
    const auto completion = wait_for_completion(backend);
    assert(completion.operation_id == operation_id);
    assert(completion.result.has_value());
    assert(completion.bytes_transferred == payload.size());
    assert(output == payload);
    assert(backend.shutdown().has_value());
    return normalize(completion);
}

normalized_completion run_real_io_uring_write_success(
    std::size_t operation_id,
    const voris::io::backends::io_uring_capabilities& capabilities) {
    auto sockets = make_socket_pair();
    voris::io::backends::io_uring_backend backend(capabilities, real_io_uring_options());
    const auto token = backend.register_handle(static_cast<std::size_t>(sockets[0].get()));
    assert(token.has_value());

    const std::array<std::byte, 1> payload{std::byte{'w'}};
    assert(backend.submit(write_operation(operation_id, *token, payload)).has_value());
    const auto completion = wait_for_completion(backend);
    assert(completion.operation_id == operation_id);
    assert(completion.result.has_value());
    assert(completion.bytes_transferred == payload.size());

    std::array<std::byte, 1> observed{};
    assert(::recv(sockets[1].get(), observed.data(), observed.size(), 0) == 1);
    assert(observed == payload);

    assert(backend.shutdown().has_value());
    return normalize(completion);
}

void test_real_linux_read_write_shared_success_when_available() {
    auto capabilities = voris::io::backends::detect_io_uring_capabilities();
    if (!capabilities.available || !capabilities.supports_read ||
        !capabilities.supports_write || !capabilities.supports_cancel) {
        return;
    }

    const auto epoll_read = run_epoll_read_success(701);
    const auto uring_read = run_real_io_uring_read_success(701, capabilities);
    assert(epoll_read == uring_read);
    assert(epoll_read.success);

    const auto epoll_write = run_epoll_write_success(702);
    const auto uring_write = run_real_io_uring_write_success(702, capabilities);
    assert(epoll_write == uring_write);
    assert(epoll_write.success);
}
#else
void test_non_linux_backends_report_unavailable_or_unsupported() {
    using namespace voris::io;

    backends::epoll_backend epoll;
    backends::io_uring_backend uring;

    assert_error(epoll.register_handle(1), vio_error_code::unsupported);
    assert_error(epoll.submit(operation(1, backend_operation_kind::read, {1, 1})),
                 vio_error_code::unsupported);
    assert_error(epoll.cancel(1, cancellation_reason::manual), vio_error_code::unsupported);
    assert_error(epoll.close_handle({1, 1}), vio_error_code::unsupported);
    assert_error(epoll.poll(), vio_error_code::unsupported);
    std::array<backend_completion, 1> epoll_completions{};
    assert_error(epoll.drain_completions(epoll_completions), vio_error_code::unsupported);
    assert_error(epoll.wake(), vio_error_code::unsupported);
    assert(epoll.shutdown().has_value());

    assert_error(uring.register_handle(1), vio_error_code::unsupported);
    assert_error(uring.submit(operation(1, backend_operation_kind::read, {1, 1})),
                 vio_error_code::unsupported);
    assert_error(uring.cancel(1, cancellation_reason::manual), vio_error_code::unsupported);
    assert_error(uring.close_handle({1, 1}), vio_error_code::unsupported);
    assert_error(uring.poll(), vio_error_code::unsupported);
    std::array<backend_completion, 1> uring_completions{};
    assert_error(uring.drain_completions(uring_completions), vio_error_code::unsupported);
    assert_error(uring.wake(), vio_error_code::unsupported);
    assert(uring.shutdown().has_value());
}
#endif

} // namespace

int main() {
    test_io_uring_cancelled_terminal_completion_is_exactly_once();
    test_io_uring_test_kernel_submitted_close_drains_closed_once();
    test_io_uring_test_kernel_submitted_shutdown_drains_closed_once();
    test_io_uring_test_kernel_stale_cqe_after_handle_reuse_is_isolated();

#if defined(__linux__)
    test_invalid_inputs_match_between_epoll_and_io_uring();
    test_duplicate_operation_id_while_pending_matches();
    test_close_pending_read_write_matches_exactly_once();
    test_close_one_handle_does_not_complete_another_handle();
    test_stale_token_after_numeric_handle_reuse_matches();
    test_shutdown_shared_contract_matches_and_poll_difference_is_gated();
    test_real_linux_read_write_shared_success_when_available();
#else
    test_non_linux_backends_report_unavailable_or_unsupported();
#endif

    return 0;
}
