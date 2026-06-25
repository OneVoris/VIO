#include <voris/io/backends/epoll_backend.hpp>

#if defined(__linux__)
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>
#endif

namespace voris::io::backends {

namespace {

#if defined(__linux__)
[[nodiscard]] vio_error provider_failure(int provider_code) {
    return make_error(vio_error_code::backend_failure, static_cast<std::int64_t>(provider_code));
}

[[nodiscard]] void_result closed_error() {
    return std::unexpected(make_error(vio_error_code::closed));
}

constexpr std::uint64_t wake_event_cookie = std::numeric_limits<std::uint64_t>::max();
constexpr std::uint64_t token_generation_shift = 32U;
constexpr std::uint64_t token_low_mask = (std::uint64_t{1} << token_generation_shift) - 1U;

[[nodiscard]] io_result<std::uint64_t> pack_event_cookie(backend_handle_token token) {
    // epoll_event stores one uint64_t cookie. VIO M4 accepts int file descriptors and
    // bounds the encoded generation to 32 bits so stale events can be classified
    // without dereferencing backend-owned storage.
    if (token.native_handle == 0 || token.generation == 0 ||
        token.native_handle > token_low_mask || token.generation > token_low_mask) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "backend handle token cannot be encoded"));
    }
    return (static_cast<std::uint64_t>(token.generation) << token_generation_shift) |
           static_cast<std::uint64_t>(token.native_handle);
}

[[nodiscard]] backend_handle_token unpack_event_cookie(std::uint64_t cookie) noexcept {
    return backend_handle_token{
        static_cast<std::size_t>(cookie & token_low_mask),
        static_cast<std::size_t>(cookie >> token_generation_shift),
    };
}

[[nodiscard]] bool has_read_readiness(std::uint32_t events) noexcept {
    return (events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0;
}

[[nodiscard]] bool has_write_readiness(std::uint32_t events) noexcept {
    return (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0;
}
#else
[[nodiscard]] vio_error unsupported_error() {
    return make_error(vio_error_code::unsupported, "epoll backend is only available on Linux");
}
#endif

} // namespace

epoll_backend::epoll_backend() {
#if defined(__linux__)
    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        initialization_error_ = provider_failure(errno);
        return;
    }

    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        initialization_error_ = provider_failure(errno);
        close_owned_descriptors();
        return;
    }

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.u64 = wake_event_cookie;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &event) != 0) {
        initialization_error_ = provider_failure(errno);
        close_owned_descriptors();
    }
#endif
}

epoll_backend::~epoll_backend() {
#if defined(__linux__)
    close_owned_descriptors();
#endif
}

#if defined(__linux__)
void epoll_backend::close_owned_descriptors() noexcept {
    if (wake_fd_ >= 0) {
        (void)::close(wake_fd_);
        wake_fd_ = -1;
    }
    if (epoll_fd_ >= 0) {
        (void)::close(epoll_fd_);
        epoll_fd_ = -1;
    }
}

void_result epoll_backend::initialization_result() const {
    if (initialization_error_.has_value()) {
        return std::unexpected(*initialization_error_);
    }
    return {};
}
#endif

io_result<backend_handle_token> epoll_backend::register_handle(std::size_t native_handle) {
#if defined(__linux__)
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (native_handle == 0 ||
        native_handle > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "native handle id must be a valid file descriptor"));
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return std::unexpected(initialized.error());
    }

    auto token = fallback_.register_handle(native_handle);
    if (!token.has_value()) {
        return token;
    }

    auto cookie = pack_event_cookie(*token);
    if (!cookie.has_value()) {
        (void)fallback_.close_handle(*token);
        return std::unexpected(cookie.error());
    }

    epoll_event event{};
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    event.data.u64 = *cookie;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, static_cast<int>(native_handle), &event) != 0) {
        (void)fallback_.close_handle(*token);
        return std::unexpected(provider_failure(errno));
    }
    return token;
#else
    (void)native_handle;
    return std::unexpected(unsupported_error());
#endif
}

void_result epoll_backend::submit(backend_operation operation) {
#if defined(__linux__)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    if (!is_socket_backend_operation(operation)) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "epoll backend accepts socket readiness operations only"));
    }
    return fallback_.submit(operation);
#else
    (void)operation;
    return std::unexpected(unsupported_error());
#endif
}

void_result epoll_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
#if defined(__linux__)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    return fallback_.cancel(operation_id, reason);
#else
    (void)operation_id;
    (void)reason;
    return std::unexpected(unsupported_error());
#endif
}

void_result epoll_backend::close_handle(backend_handle_token token) {
#if defined(__linux__)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    if (token.native_handle == 0 ||
        token.native_handle > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !fallback_.is_current_handle(token)) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "backend handle token is not current"));
    }

    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, static_cast<int>(token.native_handle), nullptr) !=
        0) {
        const int provider_code = errno;
        if (provider_code != EBADF && provider_code != ENOENT) {
            return std::unexpected(provider_failure(provider_code));
        }
    }

    return fallback_.close_handle(token);
#else
    (void)token;
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> epoll_backend::poll() {
#if defined(__linux__)
    if (stopped_) {
        return 0U;
    }
    if (initialization_error_.has_value()) {
        return std::unexpected(*initialization_error_);
    }

    std::array<epoll_event, 64> events{};
    const int ready = ::epoll_wait(epoll_fd_, events.data(), static_cast<int>(events.size()), 0);
    if (ready < 0) {
        if (errno == EINTR) {
            return 0U;
        }
        return std::unexpected(provider_failure(errno));
    }

    std::size_t observed = 0;
    for (int index = 0; index < ready; ++index) {
        const auto& event = events[static_cast<std::size_t>(index)];
        const auto cookie = event.data.u64;
        if (cookie != wake_event_cookie) {
            const auto token = unpack_event_cookie(cookie);
            if (!fallback_.is_current_handle(token)) {
                continue;
            }
            ++observed;
            if (has_read_readiness(event.events)) {
                if (auto completed =
                        fallback_.complete_ready(token, backend_operation_kind::read);
                    !completed.has_value()) {
                    return std::unexpected(completed.error());
                }
            }
            if (has_write_readiness(event.events)) {
                if (auto completed =
                        fallback_.complete_ready(token, backend_operation_kind::write);
                    !completed.has_value()) {
                    return std::unexpected(completed.error());
                }
            }
            continue;
        }

        for (;;) {
            eventfd_t value = 0;
            if (::eventfd_read(wake_fd_, &value) == 0) {
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                break;
            }
            return std::unexpected(provider_failure(errno));
        }
        ++observed;
    }

    return observed;
#else
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> epoll_backend::drain_completions(std::span<backend_completion> out) {
#if defined(__linux__)
    return fallback_.drain_completions(out);
#else
    (void)out;
    return std::unexpected(unsupported_error());
#endif
}

void_result epoll_backend::wake() {
#if defined(__linux__)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }

    for (;;) {
        if (::eventfd_write(wake_fd_, 1) == 0) {
            return {};
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN) {
            return {};
        }
        return std::unexpected(provider_failure(errno));
    }
#else
    return std::unexpected(unsupported_error());
#endif
}

void_result epoll_backend::shutdown() {
#if defined(__linux__)
    if (stopped_) {
        return {};
    }
    stopped_ = true;
    (void)fallback_.shutdown();
    close_owned_descriptors();
    return {};
#else
    return {};
#endif
}

} // namespace voris::io::backends
