#include <voris/io/backends/epoll_backend.hpp>
#include <voris/io/backends/io_uring_backend.hpp>

#include <array>

#include "test_assert.hpp"

#if defined(__linux__)
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace {

voris::io::backend_operation operation(std::size_t id, voris::io::backend_handle_token token) {
    voris::io::backend_operation result{};
    result.id = id;
    result.kind = voris::io::backend_operation_kind::read;
    result.handle = token;
    return result;
}

void assert_closed_completion(const voris::io::backend_completion& completion,
                              std::size_t operation_id) {
    assert(completion.operation_id == operation_id);
    assert(!completion.result.has_value());
    assert(completion.result.error().classification == voris::io::vio_error_code::closed);
}

void exercise_close_contract(voris::io::backend& backend, std::size_t native_handle) {
    auto token_result = backend.register_handle(native_handle);
    assert(token_result.has_value());
    const auto token = *token_result;

    assert(backend.submit(operation(101, token)).has_value());
    assert(backend.close_handle(token).has_value());

    auto polled = backend.poll();
    assert(polled.has_value());

    std::array<voris::io::backend_completion, 2> completions{};
    auto drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 1);
    assert_closed_completion(completions[0], 101);

    drained = backend.drain_completions(completions);
    assert(drained.has_value());
    assert(*drained == 0);
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

    [[nodiscard]] int get() const noexcept {
        return fd_;
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
    return fd;
}
#endif

} // namespace

int main() {
    using namespace voris::io;

    virtual_backend reference;
    exercise_close_contract(reference, 1);

    backends::epoll_backend epoll;
    backends::io_uring_backend uring;

#if defined(__linux__)
    auto epoll_fd = make_event_fd();
    exercise_close_contract(epoll, static_cast<std::size_t>(epoll_fd.get()));

    auto uring_fd = make_event_fd();
    exercise_close_contract(uring, static_cast<std::size_t>(uring_fd.get()));
#else
    assert(!epoll.register_handle(1).has_value());
    assert(!uring.register_handle(1).has_value());
#endif

    assert(epoll.shutdown().has_value());
    assert(uring.shutdown().has_value());

    return 0;
}
