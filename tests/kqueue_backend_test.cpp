#include <voris/io/backends/kqueue_backend.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "test_assert.hpp"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

struct pointer_udata_event {
    void* udata{};
};

struct integral_udata_event {
    std::intptr_t udata{};
};

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

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
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

void make_socket_pair(unique_fd& first, unique_fd& second) {
    int sockets[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    first.reset(sockets[0]);
    second.reset(sockets[1]);
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

void assert_closed_completion(const voris::io::backend_completion& completion,
                              std::size_t operation_id) {
    assert(completion.operation_id == operation_id);
    assert(!completion.result.has_value());
    assert(completion.result.error().classification == voris::io::vio_error_code::closed);
}

void assert_provider_error(const voris::io::io_result<voris::io::backend_handle_token>& result,
                           int expected_errno) {
    assert(!result.has_value());
    assert(result.error().classification == voris::io::vio_error_code::backend_failure);
    assert(result.error().provider_code.has_value());
    assert(*result.error().provider_code == expected_errno);
}

void move_away_from_fd_number(int target_fd, unique_fd& fd) {
    if (fd.get() != target_fd) {
        return;
    }

    const int duplicate = ::dup(fd.get());
    assert(duplicate >= 0);
    fd.reset(duplicate);
}

void make_same_number_fd(int target_fd, unique_fd& fd, unique_fd& peer) {
    if (fd.get() == target_fd) {
        return;
    }

    move_away_from_fd_number(target_fd, peer);

    const int duplicate = ::dup2(fd.get(), target_fd);
    assert(duplicate == target_fd);
    fd.reset();
    fd.reset(duplicate);
}

void test_wake_is_polled_once() {
    voris::io::backends::kqueue_backend backend;

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
    voris::io::backends::kqueue_backend backend;

    assert_token_error(backend.register_handle(0), voris::io::vio_error_code::invalid_state);
    assert_token_error(backend.register_handle(
                           static_cast<std::size_t>(std::numeric_limits<int>::max()) + 1U),
                       voris::io::vio_error_code::invalid_state);

    unique_fd first;
    unique_fd second;
    make_socket_pair(first, second);
    const int closed_fd = first.release();
    assert(::close(closed_fd) == 0);
    assert_provider_error(backend.register_handle(static_cast<std::size_t>(closed_fd)), EBADF);

    unique_fd replacement_first;
    unique_fd replacement_second;
    make_socket_pair(replacement_first, replacement_second);
    make_same_number_fd(closed_fd, replacement_first, replacement_second);

    auto replacement_token =
        require_token(backend.register_handle(static_cast<std::size_t>(replacement_first.get())));
    assert(replacement_token.native_handle == static_cast<std::size_t>(closed_fd));
    assert(backend.close_handle(replacement_token).has_value());

    assert(backend.shutdown().has_value());
}

void test_same_number_fd_helper_never_aliases_peer() {
    unique_fd first;
    unique_fd second;
    make_socket_pair(first, second);

    const int peer_fd = second.get();
    make_same_number_fd(peer_fd, first, second);

    assert(first.get() == peer_fd);
    assert(second.get() != peer_fd);
    assert(second.get() >= 0);
}

void test_close_deregisters_without_taking_ownership() {
    unique_fd first;
    unique_fd second;
    make_socket_pair(first, second);

    {
        voris::io::backends::kqueue_backend backend;
        auto token = require_token(backend.register_handle(static_cast<std::size_t>(first.get())));

        assert(backend.close_handle(token).has_value());
        assert(backend.shutdown().has_value());
    }

    errno = 0;
    assert(::fcntl(first.get(), F_GETFD) != -1);
    assert(errno != EBADF);
}

void test_close_after_external_fd_close_still_completes_pending_operation() {
    unique_fd first;
    unique_fd second;
    make_socket_pair(first, second);

    voris::io::backends::kqueue_backend backend;
    auto token = require_token(backend.register_handle(static_cast<std::size_t>(first.get())));
    assert(backend.submit(operation(351, token)).has_value());

    const int raw_fd = first.release();
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
    unique_fd first;
    unique_fd second;
    make_socket_pair(first, second);

    voris::io::backends::kqueue_backend backend;
    auto token = require_token(backend.register_handle(static_cast<std::size_t>(first.get())));

    assert(backend.submit(operation(361, voris::io::backend_operation_kind::read, token))
               .has_value());

    auto write_ready_only = backend.poll();
    assert(write_ready_only.has_value());

    std::array<voris::io::backend_completion, 8> completions{};
    assert(drain(backend, completions) == 0);

    assert(backend.submit(operation(362, voris::io::backend_operation_kind::write, token))
               .has_value());
    assert(backend.submit(operation(363, voris::io::backend_operation_kind::connect, token))
               .has_value());

    write_ready_only = backend.poll();
    assert(write_ready_only.has_value());
    assert(drain(backend, completions) == 2);
    assert(completions[0].operation_id == 362);
    assert(completions[0].result.has_value());
    assert(completions[1].operation_id == 363);
    assert(completions[1].result.has_value());

    const char byte = 'x';
    assert(::write(second.get(), &byte, sizeof(byte)) == sizeof(byte));
    auto read_ready = backend.poll();
    assert(read_ready.has_value());
    assert(drain(backend, completions) == 1);
    assert(completions[0].operation_id == 361);
    assert(completions[0].result.has_value());

    assert(backend.submit(operation(364, voris::io::backend_operation_kind::accept, token))
               .has_value());
    assert(::write(second.get(), &byte, sizeof(byte)) == sizeof(byte));
    read_ready = backend.poll();
    assert(read_ready.has_value());
    assert(drain(backend, completions) == 1);
    assert(completions[0].operation_id == 364);
    assert(completions[0].result.has_value());

    assert(backend.close_handle(token).has_value());
    assert(backend.shutdown().has_value());
}

void test_submit_rejects_file_operations_for_readiness_backend() {
    unique_fd first;
    unique_fd second;
    make_socket_pair(first, second);

    voris::io::backends::kqueue_backend backend;
    auto token = require_token(backend.register_handle(static_cast<std::size_t>(first.get())));

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
    unique_fd old_first;
    unique_fd old_second;
    make_socket_pair(old_first, old_second);

    voris::io::backends::kqueue_backend backend;
    const int reused_number = old_first.get();
    auto old_token =
        require_token(backend.register_handle(static_cast<std::size_t>(old_first.get())));
    assert(backend.submit(operation(401, old_token)).has_value());

    const char byte = 'x';
    assert(::write(old_second.get(), &byte, sizeof(byte)) == sizeof(byte));
    assert(backend.close_handle(old_token).has_value());

    std::array<voris::io::backend_completion, 8> completions{};
    assert(drain(backend, completions) == 1);
    assert_closed_completion(completions[0], 401);

    old_first.reset();
    old_second.reset();

    unique_fd new_first;
    unique_fd new_second;
    make_socket_pair(new_first, new_second);
    make_same_number_fd(reused_number, new_first, new_second);

    auto new_token =
        require_token(backend.register_handle(static_cast<std::size_t>(new_first.get())));
    assert(new_token.native_handle == old_token.native_handle);
    assert(new_token.generation > old_token.generation);

    assert_void_error(backend.submit(operation(402, old_token)),
                      voris::io::vio_error_code::invalid_state);
    assert(backend.submit(operation(403, new_token)).has_value());

    auto stale_poll = backend.poll();
    assert(stale_poll.has_value());
    assert(drain(backend, completions) == 0);

    assert(::write(new_second.get(), &byte, sizeof(byte)) == sizeof(byte));
    auto current_poll = backend.poll();
    assert(current_poll.has_value());

    assert(drain(backend, completions) == 1);
    assert(completions[0].operation_id == 403);
    assert(completions[0].result.has_value());

    assert(backend.close_handle(new_token).has_value());
    assert(backend.shutdown().has_value());
}

void test_shutdown_defines_later_behavior() {
    unique_fd first;
    unique_fd second;
    make_socket_pair(first, second);

    voris::io::backends::kqueue_backend backend;
    auto token = require_token(backend.register_handle(static_cast<std::size_t>(first.get())));
    assert(backend.submit(operation(501, token)).has_value());

    assert(backend.shutdown().has_value());
    assert(backend.shutdown().has_value());

    assert_void_error(backend.register_handle(static_cast<std::size_t>(second.get())),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.submit(operation(502, token)), voris::io::vio_error_code::closed);
    assert_void_error(backend.cancel(501, voris::io::cancellation_reason::manual),
                      voris::io::vio_error_code::closed);
    assert_void_error(backend.close_handle(token), voris::io::vio_error_code::closed);
    assert_void_error(backend.wake(), voris::io::vio_error_code::closed);

    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 0);

    std::array<voris::io::backend_completion, 8> completions{};
    assert(drain(backend, completions) == 1);
    assert_closed_completion(completions[0], 501);
}
#endif

} // namespace

int main() {
    using namespace voris::io;

    static_assert(std::is_same_v<
                  decltype(backends::detail::kqueue_cookie_to_udata<pointer_udata_event>(1U)),
                  void*>);
    static_assert(std::is_same_v<
                  decltype(backends::detail::kqueue_cookie_to_udata<integral_udata_event>(1U)),
                  std::intptr_t>);
    assert(backends::detail::kqueue_cookie_from_udata(integral_udata_event{7}) == 7U);

    static_assert(!std::is_copy_constructible_v<backends::kqueue_backend>);
    static_assert(!std::is_copy_assignable_v<backends::kqueue_backend>);

    backends::kqueue_backend backend;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    (void)backend;
    test_wake_is_polled_once();
    test_register_validates_and_reports_provider_errors();
    test_close_deregisters_without_taking_ownership();
    test_close_after_external_fd_close_still_completes_pending_operation();
    test_same_number_fd_helper_never_aliases_peer();
    test_readiness_mask_completes_only_compatible_operation_kind();
    test_submit_rejects_file_operations_for_readiness_backend();
    test_same_numeric_fd_reuse_uses_new_generation();
    test_shutdown_defines_later_behavior();
#else
    assert_token_error(backend.register_handle(1), vio_error_code::unsupported);
    assert_void_error(backend.submit(operation(1, backend_operation_kind::read, {1, 1})),
                      vio_error_code::unsupported);
    assert_void_error(backend.cancel(1, cancellation_reason::manual), vio_error_code::unsupported);
    assert_size_error(backend.poll(), vio_error_code::unsupported);
    assert_void_error(backend.wake(), vio_error_code::unsupported);
    assert_void_error(backend.close_handle({1, 1}), vio_error_code::unsupported);
    std::array<backend_completion, 1> completions{};
    assert_size_error(backend.drain_completions(completions), vio_error_code::unsupported);
#endif
    assert(backend.shutdown().has_value());
    return 0;
}
