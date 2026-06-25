#pragma once

#include <voris/io/backend.hpp>
#include <voris/io/backends/epoll_backend.hpp>
#include <voris/io/backends/io_uring_backend.hpp>
#include <voris/io/backends/iocp_backend.hpp>
#include <voris/io/backends/kqueue_backend.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

#include "test_assert.hpp"

#if defined(__linux__)
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace vio_backend_contract_tests {

inline voris::io::backend_operation operation(std::size_t id,
                                              voris::io::backend_operation_kind kind,
                                              voris::io::backend_handle_token token) {
    voris::io::backend_operation result{};
    result.id = id;
    result.kind = kind;
    result.handle = token;
    return result;
}

inline voris::io::backend_operation file_operation(std::size_t id,
                                                   voris::io::backend_operation_kind kind,
                                                   voris::io::backend_handle_token token) {
    auto result = operation(id, kind, token);
    result.target = voris::io::backend_operation_target::file;
    return result;
}

inline void assert_void_error(const voris::io::void_result& result,
                              voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

inline void assert_token_error(
    const voris::io::io_result<voris::io::backend_handle_token>& result,
    voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

inline void assert_size_error(const voris::io::io_result<std::size_t>& result,
                              voris::io::vio_error_code expected) {
    assert(!result.has_value());
    assert(result.error().classification == expected);
}

inline voris::io::backend_handle_token require_token(
    voris::io::io_result<voris::io::backend_handle_token> result) {
    assert(result.has_value());
    return *result;
}

inline std::size_t drain(voris::io::backend& backend,
                         std::span<voris::io::backend_completion> completions) {
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    return *drained;
}

inline void assert_poll_count(voris::io::backend& backend, std::size_t expected) {
    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == expected);
}

inline void assert_completion_error(const voris::io::backend_completion& completion,
                                    std::size_t operation_id,
                                    voris::io::vio_error_code expected) {
    assert(completion.operation_id == operation_id);
    assert(!completion.result.has_value());
    assert(completion.result.error().classification == expected);
}

class virtual_contract_fixture {
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

    [[nodiscard]] std::size_t expected_poll_count_after_close(
        std::size_t queued_completions) const noexcept {
        return queued_completions;
    }

    void make_pending_completions_visible() noexcept {}

    voris::io::virtual_backend backend{};

private:
    std::size_t next_native_handle_{1};
};

template <class Fixture>
inline constexpr bool can_force_same_numeric_handle_reuse = true;

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

inline unique_fd make_event_fd() {
    unique_fd fd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    assert(fd.get() >= 0);
    return fd;
}

class epoll_contract_fixture {
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

    [[nodiscard]] std::size_t expected_poll_count_after_close(
        std::size_t) const noexcept {
        return 0;
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
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
class kqueue_unique_fd {
public:
    explicit kqueue_unique_fd(int fd = -1) noexcept : fd_(fd) {}

    ~kqueue_unique_fd() {
        reset();
    }

    kqueue_unique_fd(const kqueue_unique_fd&) = delete;
    kqueue_unique_fd& operator=(const kqueue_unique_fd&) = delete;

    kqueue_unique_fd(kqueue_unique_fd&& other) noexcept : fd_(other.release()) {}

    kqueue_unique_fd& operator=(kqueue_unique_fd&& other) noexcept {
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

struct kqueue_socket_pair {
    kqueue_unique_fd first{};
    kqueue_unique_fd second{};
};

inline kqueue_socket_pair make_kqueue_socket_pair() {
    int sockets[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    return kqueue_socket_pair{kqueue_unique_fd{sockets[0]}, kqueue_unique_fd{sockets[1]}};
}

inline void move_away_from_fd_number(int target_fd, kqueue_unique_fd& fd) {
    if (fd.get() != target_fd) {
        return;
    }

    const int duplicate = ::dup(fd.get());
    assert(duplicate >= 0);
    fd.reset(duplicate);
}

inline void make_kqueue_same_number_fd(int target_fd,
                                       kqueue_unique_fd& fd,
                                       kqueue_unique_fd& peer) {
    if (fd.get() == target_fd) {
        return;
    }

    move_away_from_fd_number(target_fd, peer);

    const int duplicate = ::dup2(fd.get(), target_fd);
    assert(duplicate == target_fd);
    fd.reset();
    fd.reset(duplicate);
}

class kqueue_contract_fixture {
public:
    [[nodiscard]] std::size_t make_native_handle() {
        handles_.push_back(make_kqueue_socket_pair());
        return static_cast<std::size_t>(handles_.back().first.get());
    }

    [[nodiscard]] std::size_t invalid_native_handle() const noexcept {
        return 0;
    }

    [[nodiscard]] std::size_t recreate_native_handle_with_same_number(
        std::size_t native_handle) {
        close_native_handle(native_handle);

        auto replacement = make_kqueue_socket_pair();
        const auto target = static_cast<int>(native_handle);
        make_kqueue_same_number_fd(target, replacement.first, replacement.second);

        handles_.push_back(std::move(replacement));
        return native_handle;
    }

    [[nodiscard]] std::size_t expected_poll_count_after_close(
        std::size_t) const noexcept {
        return 0;
    }

    void make_pending_completions_visible() noexcept {}

    voris::io::backends::kqueue_backend backend{};

private:
    void close_native_handle(std::size_t native_handle) noexcept {
        for (auto& handle : handles_) {
            if (handle.first.get() == static_cast<int>(native_handle)) {
                handle.first.reset();
                handle.second.reset();
                return;
            }
        }
        assert(false);
    }

    std::vector<kqueue_socket_pair> handles_{};
};
#endif

#if defined(_WIN32)
class iocp_unique_handle {
public:
    explicit iocp_unique_handle(HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}

    ~iocp_unique_handle() {
        reset();
    }

    iocp_unique_handle(const iocp_unique_handle&) = delete;
    iocp_unique_handle& operator=(const iocp_unique_handle&) = delete;

    iocp_unique_handle(iocp_unique_handle&& other) noexcept : handle_(other.release()) {}

    iocp_unique_handle& operator=(iocp_unique_handle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    [[nodiscard]] HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return handle;
    }

    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
            (void)::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

inline iocp_unique_handle make_iocp_overlapped_temp_file() {
    std::array<wchar_t, MAX_PATH> directory{};
    const DWORD directory_length =
        ::GetTempPathW(static_cast<DWORD>(directory.size()), directory.data());
    assert(directory_length > 0);
    assert(directory_length < directory.size());

    std::array<wchar_t, MAX_PATH> path{};
    assert(::GetTempFileNameW(directory.data(), L"vio", 0, path.data()) != 0);

    iocp_unique_handle file(::CreateFileW(
        path.data(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OVERLAPPED,
        nullptr));
    assert(file.get() != INVALID_HANDLE_VALUE);
    return file;
}

inline std::size_t iocp_native_handle_value(HANDLE handle) noexcept {
    return reinterpret_cast<std::size_t>(handle);
}

class iocp_contract_fixture {
public:
    [[nodiscard]] std::size_t make_native_handle() {
        handles_.push_back(make_iocp_overlapped_temp_file());
        return iocp_native_handle_value(handles_.back().get());
    }

    [[nodiscard]] std::size_t invalid_native_handle() const noexcept {
        return 0;
    }

    [[nodiscard]] std::size_t recreate_native_handle_with_same_number(
        std::size_t native_handle) {
        close_native_handle(native_handle);
        handles_.push_back(make_iocp_overlapped_temp_file());
        return iocp_native_handle_value(handles_.back().get());
    }

    [[nodiscard]] std::size_t expected_poll_count_after_close(
        std::size_t queued_completions) const noexcept {
        return queued_completions;
    }

    void make_pending_completions_visible() noexcept {}

    voris::io::backends::iocp_backend backend{};

private:
    void close_native_handle(std::size_t native_handle) noexcept {
        for (auto& handle : handles_) {
            if (iocp_native_handle_value(handle.get()) == native_handle) {
                handle.reset();
                return;
            }
        }
        assert(false);
    }

    std::vector<iocp_unique_handle> handles_{};
};

template <>
inline constexpr bool can_force_same_numeric_handle_reuse<iocp_contract_fixture> = false;
#endif

inline voris::io::backends::io_uring_capabilities io_uring_contract_capabilities()
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

inline voris::io::backends::io_uring_backend_options io_uring_contract_options()
    noexcept {
    return voris::io::backends::io_uring_backend_options{
        .submission_queue_capacity = 64,
        .submit_batch_limit = 32,
        .completion_batch_limit = 32,
        .enable_kernel_submission = false,
    };
}

class io_uring_contract_fixture {
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

    [[nodiscard]] std::size_t expected_poll_count_after_close(
        std::size_t queued_completions) const noexcept {
        return queued_completions;
    }

    void make_pending_completions_visible() {
        auto polled = backend.poll();
        assert(polled.has_value());
    }

    voris::io::backends::io_uring_backend backend{
        io_uring_contract_capabilities(),
        io_uring_contract_options(),
    };

private:
    std::size_t next_native_handle_{1};
};

template <class Fixture>
void test_register_valid_handle_returns_token_and_invalid_handle_is_rejected() {
    using namespace voris::io;

    Fixture fixture;
    const auto first_native = fixture.make_native_handle();
    const auto second_native = fixture.make_native_handle();

    const auto first = require_token(fixture.backend.register_handle(first_native));
    const auto second = require_token(fixture.backend.register_handle(second_native));

    assert(first.native_handle == first_native);
    assert(first.generation == 1);
    assert(second.native_handle == second_native);
    assert(second.generation == 1);
    assert_token_error(fixture.backend.register_handle(fixture.invalid_native_handle()),
                       vio_error_code::invalid_state);
}

template <class Fixture>
void test_submit_accepts_current_token_and_rejects_default_and_closed_tokens() {
    using namespace voris::io;

    Fixture fixture;
    const auto token = require_token(fixture.backend.register_handle(fixture.make_native_handle()));

    assert(fixture.backend.submit(operation(10, backend_operation_kind::read, token)).has_value());
    assert_void_error(fixture.backend.submit(operation(0, backend_operation_kind::read, token)),
                      vio_error_code::invalid_state);
    assert_void_error(fixture.backend.submit(operation(11, backend_operation_kind::read, {})),
                      vio_error_code::invalid_state);

    assert(fixture.backend.close_handle(token).has_value());
    assert_void_error(fixture.backend.submit(operation(12, backend_operation_kind::write, token)),
                      vio_error_code::invalid_state);
    assert_poll_count(fixture.backend, fixture.expected_poll_count_after_close(1));

    std::array<backend_completion, 4> completions{};
    assert(drain(fixture.backend, completions) == 1);
    assert_completion_error(completions[0], 10, vio_error_code::closed);
    assert_poll_count(fixture.backend, 0);
    assert(drain(fixture.backend, completions) == 0);
}

template <class Fixture>
void test_submit_rejects_invalid_operation_shapes() {
    using namespace voris::io;

    Fixture fixture;
    const auto token = require_token(fixture.backend.register_handle(fixture.make_native_handle()));

    assert_void_error(fixture.backend.submit(operation(51, backend_operation_kind::fsync, token)),
                      vio_error_code::invalid_state);
    assert_void_error(
        fixture.backend.submit(file_operation(52, backend_operation_kind::accept, token)),
        vio_error_code::invalid_state);
    assert_void_error(
        fixture.backend.submit(file_operation(53, backend_operation_kind::connect, token)),
        vio_error_code::invalid_state);
}

template <class Fixture>
void test_close_handle_completes_all_pending_operations_once() {
    using namespace voris::io;

    Fixture fixture;
    const auto token = require_token(fixture.backend.register_handle(fixture.make_native_handle()));

    assert(fixture.backend.submit(operation(11, backend_operation_kind::read, token)).has_value());
    assert(fixture.backend.submit(operation(12, backend_operation_kind::write, token)).has_value());

    assert(fixture.backend.close_handle(token).has_value());
    assert_poll_count(fixture.backend, fixture.expected_poll_count_after_close(2));

    std::array<backend_completion, 8> completions{};
    assert(drain(fixture.backend, completions) == 2);
    assert_completion_error(completions[0], 11, vio_error_code::closed);
    assert_completion_error(completions[1], 12, vio_error_code::closed);
    assert_poll_count(fixture.backend, 0);
    assert(drain(fixture.backend, completions) == 0);

    assert_void_error(fixture.backend.cancel(11, cancellation_reason::manual),
                      vio_error_code::invalid_state);
}

template <class Fixture>
void test_close_one_handle_does_not_complete_another_handles_pending_operations() {
    using namespace voris::io;

    Fixture fixture;
    const auto first = require_token(fixture.backend.register_handle(fixture.make_native_handle()));
    const auto second =
        require_token(fixture.backend.register_handle(fixture.make_native_handle()));

    assert(fixture.backend.submit(operation(21, backend_operation_kind::read, first)).has_value());
    assert(fixture.backend.submit(operation(22, backend_operation_kind::read, second)).has_value());

    assert(fixture.backend.close_handle(first).has_value());
    fixture.make_pending_completions_visible();

    std::array<backend_completion, 8> completions{};
    assert(drain(fixture.backend, completions) == 1);
    assert_completion_error(completions[0], 21, vio_error_code::closed);
    assert(drain(fixture.backend, completions) == 0);

    assert(fixture.backend.close_handle(second).has_value());
    fixture.make_pending_completions_visible();
    assert(drain(fixture.backend, completions) == 1);
    assert_completion_error(completions[0], 22, vio_error_code::closed);
}

template <class Fixture>
void test_operation_id_cannot_reuse_until_queued_completion_is_drained() {
    using namespace voris::io;

    Fixture fixture;
    const auto closing =
        require_token(fixture.backend.register_handle(fixture.make_native_handle()));
    const auto other = require_token(fixture.backend.register_handle(fixture.make_native_handle()));

    assert(fixture.backend.submit(operation(77, backend_operation_kind::read, closing)).has_value());
    assert(fixture.backend.close_handle(closing).has_value());

    assert_void_error(fixture.backend.submit(operation(77, backend_operation_kind::write, other)),
                      vio_error_code::invalid_state);

    fixture.make_pending_completions_visible();
    std::array<backend_completion, 4> completions{};
    assert(drain(fixture.backend, completions) == 1);
    assert_completion_error(completions[0], 77, vio_error_code::closed);

    assert(fixture.backend.submit(operation(77, backend_operation_kind::write, other)).has_value());
    assert(fixture.backend.close_handle(other).has_value());
    fixture.make_pending_completions_visible();
    assert(drain(fixture.backend, completions) == 1);
    assert_completion_error(completions[0], 77, vio_error_code::closed);
}

template <class Fixture>
void test_handle_reuse_gets_safe_generation_when_possible_and_rejects_stale_token() {
    using namespace voris::io;

    Fixture fixture;
    const auto native_handle = fixture.make_native_handle();
    const auto first = require_token(fixture.backend.register_handle(native_handle));
    assert(fixture.backend.close_handle(first).has_value());

    const auto reused_native = fixture.recreate_native_handle_with_same_number(native_handle);
    const auto reused = require_token(fixture.backend.register_handle(reused_native));

    if constexpr (can_force_same_numeric_handle_reuse<Fixture>) {
        assert(reused.native_handle == first.native_handle);
        assert(reused.generation > first.generation);
    } else if (reused.native_handle == first.native_handle) {
        assert(reused.generation > first.generation);
    } else {
        assert(reused.generation == 1);
    }

    assert_void_error(fixture.backend.submit(operation(31, backend_operation_kind::read, first)),
                      vio_error_code::invalid_state);
    assert_void_error(fixture.backend.close_handle(first), vio_error_code::invalid_state);

    assert(fixture.backend.submit(operation(32, backend_operation_kind::read, reused)).has_value());
    assert(fixture.backend.close_handle(reused).has_value());
    assert_poll_count(fixture.backend, fixture.expected_poll_count_after_close(1));

    std::array<backend_completion, 4> completions{};
    assert(drain(fixture.backend, completions) == 1);
    assert_completion_error(completions[0], 32, vio_error_code::closed);
    assert_poll_count(fixture.backend, 0);
}

template <class Fixture>
void test_shutdown_rejects_new_work_and_drains_pending_as_closed() {
    using namespace voris::io;

    Fixture fixture;
    const auto token = require_token(fixture.backend.register_handle(fixture.make_native_handle()));
    assert(fixture.backend.submit(operation(41, backend_operation_kind::read, token)).has_value());

    assert(fixture.backend.shutdown().has_value());
    assert_void_error(fixture.backend.submit(operation(42, backend_operation_kind::write, token)),
                      vio_error_code::closed);
    assert_void_error(fixture.backend.cancel(41, cancellation_reason::manual),
                      vio_error_code::closed);
    assert_void_error(fixture.backend.close_handle(token), vio_error_code::closed);
    assert_token_error(fixture.backend.register_handle(fixture.make_native_handle()),
                       vio_error_code::closed);

    std::array<backend_completion, 4> completions{};
    assert(drain(fixture.backend, completions) == 1);
    assert_completion_error(completions[0], 41, vio_error_code::closed);
    assert(drain(fixture.backend, completions) == 0);
}

template <class Fixture>
void test_drain_empty_and_empty_span_behavior() {
    using namespace voris::io;

    Fixture fixture;
    std::array<backend_completion, 2> completions{};

    assert_poll_count(fixture.backend, 0);
    assert(drain(fixture.backend, completions) == 0);
    assert_poll_count(fixture.backend, 0);
    assert_size_error(fixture.backend.drain_completions(std::span<backend_completion>{}),
                      vio_error_code::invalid_state);
}

template <class Fixture>
void run_backend_contract_suite() {
    test_register_valid_handle_returns_token_and_invalid_handle_is_rejected<Fixture>();
    test_submit_accepts_current_token_and_rejects_default_and_closed_tokens<Fixture>();
    test_submit_rejects_invalid_operation_shapes<Fixture>();
    test_close_handle_completes_all_pending_operations_once<Fixture>();
    test_close_one_handle_does_not_complete_another_handles_pending_operations<Fixture>();
    test_operation_id_cannot_reuse_until_queued_completion_is_drained<Fixture>();
    test_handle_reuse_gets_safe_generation_when_possible_and_rejects_stale_token<Fixture>();
    test_shutdown_rejects_new_work_and_drains_pending_as_closed<Fixture>();
    test_drain_empty_and_empty_span_behavior<Fixture>();
}

inline void run_virtual_backend_contract_suite() {
    run_backend_contract_suite<virtual_contract_fixture>();

    using namespace voris::io;

    virtual_contract_fixture fixture;
    const auto token = require_token(fixture.backend.register_handle(fixture.make_native_handle()));
    assert(fixture.backend.submit(file_operation(54, backend_operation_kind::read, token))
               .has_value());
    assert(fixture.backend.submit(file_operation(55, backend_operation_kind::write, token))
               .has_value());
    assert(fixture.backend.submit(file_operation(56, backend_operation_kind::fsync, token))
               .has_value());

    assert(fixture.backend.close_handle(token).has_value());
    assert_poll_count(fixture.backend, fixture.expected_poll_count_after_close(3));

    std::array<backend_completion, 4> completions{};
    assert(drain(fixture.backend, completions) == 3);
    assert_completion_error(completions[0], 54, vio_error_code::closed);
    assert_completion_error(completions[1], 55, vio_error_code::closed);
    assert_completion_error(completions[2], 56, vio_error_code::closed);
}

inline void run_epoll_backend_contract_suite() {
#if defined(__linux__)
    run_backend_contract_suite<epoll_contract_fixture>();
#else
    using namespace voris::io;

    backends::epoll_backend backend;
    assert_token_error(backend.register_handle(1), vio_error_code::unsupported);
    assert_void_error(backend.submit(operation(1, backend_operation_kind::read, {1, 1})),
                      vio_error_code::unsupported);
    assert_void_error(backend.cancel(1, cancellation_reason::manual), vio_error_code::unsupported);
    assert_void_error(backend.close_handle({1, 1}), vio_error_code::unsupported);
    assert_size_error(backend.poll(), vio_error_code::unsupported);
    std::array<backend_completion, 1> completions{};
    assert_size_error(backend.drain_completions(completions), vio_error_code::unsupported);
    assert_void_error(backend.wake(), vio_error_code::unsupported);
    assert(backend.shutdown().has_value());
#endif
}

inline void run_kqueue_backend_contract_suite() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    run_backend_contract_suite<kqueue_contract_fixture>();
#else
    using namespace voris::io;

    backends::kqueue_backend backend;
    assert_token_error(backend.register_handle(1), vio_error_code::unsupported);
    assert_void_error(backend.submit(operation(1, backend_operation_kind::read, {1, 1})),
                      vio_error_code::unsupported);
    assert_void_error(backend.cancel(1, cancellation_reason::manual), vio_error_code::unsupported);
    assert_void_error(backend.close_handle({1, 1}), vio_error_code::unsupported);
    assert_size_error(backend.poll(), vio_error_code::unsupported);
    std::array<backend_completion, 1> completions{};
    assert_size_error(backend.drain_completions(completions), vio_error_code::unsupported);
    assert_void_error(backend.wake(), vio_error_code::unsupported);
    assert(backend.shutdown().has_value());
#endif
}

inline void run_iocp_backend_contract_suite() {
#if defined(_WIN32)
    run_backend_contract_suite<iocp_contract_fixture>();

    using namespace voris::io;

    iocp_contract_fixture fixture;
    const auto token = require_token(fixture.backend.register_handle(fixture.make_native_handle()));
    assert(fixture.backend.submit(file_operation(254, backend_operation_kind::read, token))
               .has_value());
    assert(fixture.backend.submit(file_operation(255, backend_operation_kind::write, token))
               .has_value());
    assert(fixture.backend.submit(file_operation(256, backend_operation_kind::fsync, token))
               .has_value());

    assert(fixture.backend.close_handle(token).has_value());
    assert_poll_count(fixture.backend, fixture.expected_poll_count_after_close(3));

    std::array<backend_completion, 4> completions{};
    assert(drain(fixture.backend, completions) == 3);
    assert_completion_error(completions[0], 254, vio_error_code::closed);
    assert_completion_error(completions[1], 255, vio_error_code::closed);
    assert_completion_error(completions[2], 256, vio_error_code::closed);
#else
    using namespace voris::io;

    backends::iocp_backend backend;
    assert_token_error(backend.register_handle(1), vio_error_code::unsupported);
    assert_void_error(backend.submit(operation(1, backend_operation_kind::read, {1, 1})),
                      vio_error_code::unsupported);
    assert_void_error(backend.cancel(1, cancellation_reason::manual), vio_error_code::unsupported);
    assert_void_error(backend.close_handle({1, 1}), vio_error_code::unsupported);
    assert_size_error(backend.poll(), vio_error_code::unsupported);
    std::array<backend_completion, 1> completions{};
    assert_size_error(backend.drain_completions(completions), vio_error_code::unsupported);
    assert_void_error(backend.wake(), vio_error_code::unsupported);
    assert(backend.shutdown().has_value());
#endif
}

inline void run_io_uring_backend_contract_suite() {
    run_backend_contract_suite<io_uring_contract_fixture>();

    using namespace voris::io;

    io_uring_contract_fixture fixture;
    const auto token = require_token(fixture.backend.register_handle(fixture.make_native_handle()));
    std::array<std::byte, 1> read_buffer{};
    const std::array<std::byte, 1> write_buffer{std::byte{'x'}};
    auto file_read = file_operation(154, backend_operation_kind::read, token);
    file_read.read_buffer = read_buffer;
    auto file_write = file_operation(155, backend_operation_kind::write, token);
    file_write.write_buffer = write_buffer;

    assert(fixture.backend.submit(file_read).has_value());
    assert(fixture.backend.submit(file_write).has_value());
    assert(fixture.backend.submit(file_operation(156, backend_operation_kind::fsync, token))
               .has_value());

    assert(fixture.backend.close_handle(token).has_value());
    assert_poll_count(fixture.backend, fixture.expected_poll_count_after_close(3));

    std::array<backend_completion, 4> completions{};
    assert(drain(fixture.backend, completions) == 3);
    assert_completion_error(completions[0], 154, vio_error_code::closed);
    assert_completion_error(completions[1], 155, vio_error_code::closed);
    assert_completion_error(completions[2], 156, vio_error_code::closed);

#if !defined(__linux__)
    backends::io_uring_backend unavailable;
    assert_token_error(unavailable.register_handle(1), vio_error_code::unsupported);
    assert_void_error(unavailable.submit(operation(1, backend_operation_kind::read, {1, 1})),
                      vio_error_code::unsupported);
    assert_void_error(unavailable.cancel(1, cancellation_reason::manual),
                      vio_error_code::unsupported);
    assert_void_error(unavailable.close_handle({1, 1}), vio_error_code::unsupported);
    assert_size_error(unavailable.poll(), vio_error_code::unsupported);
    std::array<backend_completion, 1> unavailable_completions{};
    assert_size_error(unavailable.drain_completions(unavailable_completions),
                      vio_error_code::unsupported);
    assert_void_error(unavailable.wake(), vio_error_code::unsupported);
    assert(unavailable.shutdown().has_value());
#endif
}

} // namespace vio_backend_contract_tests
