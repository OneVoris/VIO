#include <voris/io/backends/kqueue_backend.hpp>

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <limits>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace voris::io::backends {

namespace {

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
[[nodiscard]] vio_error provider_failure(int provider_code) {
    return make_error(vio_error_code::backend_failure, static_cast<std::int64_t>(provider_code));
}

[[nodiscard]] void_result closed_error() {
    return std::unexpected(make_error(vio_error_code::closed));
}

[[nodiscard]] vio_error invalid_state_error(const char* diagnostic) {
    return make_error(vio_error_code::invalid_state, diagnostic);
}

constexpr std::uintptr_t wake_event_ident = 1U;
constexpr std::uintptr_t wake_event_cookie = std::numeric_limits<std::uintptr_t>::max();
constexpr unsigned event_cookie_bits = sizeof(std::uintptr_t) * CHAR_BIT;
constexpr unsigned token_generation_shift = event_cookie_bits / 2U;
constexpr std::uintptr_t token_low_mask =
    (std::uintptr_t{1} << token_generation_shift) - 1U;

[[nodiscard]] io_result<std::uintptr_t> pack_event_cookie(backend_handle_token token) {
    if (token.native_handle == 0 || token.generation == 0 ||
        token.native_handle > token_low_mask || token.generation > token_low_mask) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "backend handle token cannot be encoded"));
    }

    const auto cookie =
        (static_cast<std::uintptr_t>(token.generation) << token_generation_shift) |
        static_cast<std::uintptr_t>(token.native_handle);
    if (cookie == wake_event_cookie) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "backend handle token conflicts with wake cookie"));
    }
    return cookie;
}

[[nodiscard]] backend_handle_token unpack_event_cookie(std::uintptr_t cookie) noexcept {
    return backend_handle_token{
        static_cast<std::size_t>(cookie & token_low_mask),
        static_cast<std::size_t>(cookie >> token_generation_shift),
    };
}

#if defined(__NetBSD__)
[[nodiscard]] intptr_t cookie_to_udata(std::uintptr_t cookie) noexcept {
    return static_cast<intptr_t>(cookie);
}

[[nodiscard]] std::uintptr_t cookie_from_udata(const struct kevent& event) noexcept {
    return static_cast<std::uintptr_t>(event.udata);
}
#else
[[nodiscard]] void* cookie_to_udata(std::uintptr_t cookie) noexcept {
    return reinterpret_cast<void*>(cookie);
}

[[nodiscard]] std::uintptr_t cookie_from_udata(const struct kevent& event) noexcept {
    return reinterpret_cast<std::uintptr_t>(event.udata);
}
#endif

void set_event(struct kevent& event,
               std::uintptr_t ident,
               int16_t filter,
               uint16_t flags,
               uint32_t fflags,
               intptr_t data,
               std::uintptr_t cookie) noexcept {
    EV_SET(&event, ident, filter, flags, fflags, data, cookie_to_udata(cookie));
}

[[nodiscard]] void_result apply_filter_change(int kqueue_fd, struct kevent& change) {
    if (::kevent(kqueue_fd, &change, 1, nullptr, 0, nullptr) == 0) {
        return {};
    }
    return std::unexpected(provider_failure(errno));
}

[[nodiscard]] void_result delete_filter(int kqueue_fd, std::uintptr_t ident, int16_t filter) {
    struct kevent change{};
    set_event(change, ident, filter, EV_DELETE, 0, 0, 0);
    if (::kevent(kqueue_fd, &change, 1, nullptr, 0, nullptr) == 0) {
        return {};
    }

    const int provider_code = errno;
    if (provider_code == EBADF || provider_code == ENOENT) {
        return {};
    }
    return std::unexpected(provider_failure(provider_code));
}

[[nodiscard]] bool is_read_readiness(const struct kevent& event) noexcept {
    return event.filter == EVFILT_READ;
}

[[nodiscard]] bool is_write_readiness(const struct kevent& event) noexcept {
    return event.filter == EVFILT_WRITE;
}
#else
[[nodiscard]] vio_error unsupported_error() {
    return make_error(vio_error_code::unsupported, "kqueue backend is unavailable");
}
#endif

} // namespace

kqueue_backend::kqueue_backend() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    kqueue_fd_ = ::kqueue();
    if (kqueue_fd_ < 0) {
        initialization_error_ = provider_failure(errno);
        return;
    }

    struct kevent wake_event{};
    set_event(wake_event, wake_event_ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0,
              wake_event_cookie);
    if (::kevent(kqueue_fd_, &wake_event, 1, nullptr, 0, nullptr) != 0) {
        initialization_error_ = provider_failure(errno);
        close_owned_descriptor();
    }
#endif
}

kqueue_backend::~kqueue_backend() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    close_owned_descriptor();
#endif
}

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
void kqueue_backend::close_owned_descriptor() noexcept {
    if (kqueue_fd_ >= 0) {
        (void)::close(kqueue_fd_);
        kqueue_fd_ = -1;
    }
}

void_result kqueue_backend::initialization_result() const {
    if (initialization_error_.has_value()) {
        return std::unexpected(*initialization_error_);
    }
    return {};
}
#endif

io_result<backend_handle_token> kqueue_backend::register_handle(std::size_t native_handle) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (native_handle == 0 ||
        native_handle > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(invalid_state_error(
            "native handle id must be a valid file descriptor"));
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return std::unexpected(initialized.error());
    }

    auto token = fallback_.register_handle(native_handle);
    if (!token.has_value()) {
        return token;
    }

    const auto cookie = pack_event_cookie(*token);
    if (!cookie.has_value()) {
        (void)fallback_.close_handle(*token);
        return std::unexpected(cookie.error());
    }

    const auto ident = static_cast<std::uintptr_t>(native_handle);
    struct kevent read_event{};
    set_event(read_event, ident, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, *cookie);
    if (auto registered_read = apply_filter_change(kqueue_fd_, read_event);
        !registered_read.has_value()) {
        (void)fallback_.close_handle(*token);
        return std::unexpected(registered_read.error());
    }

    struct kevent write_event{};
    set_event(write_event, ident, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, *cookie);
    if (auto registered_write = apply_filter_change(kqueue_fd_, write_event);
        !registered_write.has_value()) {
        (void)delete_filter(kqueue_fd_, ident, EVFILT_READ);
        (void)fallback_.close_handle(*token);
        return std::unexpected(registered_write.error());
    }

    return token;
#else
    (void)native_handle;
    return std::unexpected(unsupported_error());
#endif
}

void_result kqueue_backend::submit(backend_operation operation) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }
    if (!is_socket_backend_operation(operation)) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "kqueue backend accepts socket readiness operations only"));
    }
    return fallback_.submit(operation);
#else
    (void)operation;
    return std::unexpected(unsupported_error());
#endif
}

void_result kqueue_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
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

void_result kqueue_backend::close_handle(backend_handle_token token) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
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

    const auto ident = static_cast<std::uintptr_t>(token.native_handle);
    if (auto deleted_read = delete_filter(kqueue_fd_, ident, EVFILT_READ);
        !deleted_read.has_value()) {
        return deleted_read;
    }
    if (auto deleted_write = delete_filter(kqueue_fd_, ident, EVFILT_WRITE);
        !deleted_write.has_value()) {
        return deleted_write;
    }

    return fallback_.close_handle(token);
#else
    (void)token;
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> kqueue_backend::poll() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (stopped_) {
        return 0U;
    }
    if (initialization_error_.has_value()) {
        return std::unexpected(*initialization_error_);
    }

    std::array<struct kevent, 64> events{};
    const timespec timeout{0, 0};
    const int ready =
        ::kevent(kqueue_fd_, nullptr, 0, events.data(), static_cast<int>(events.size()), &timeout);
    if (ready < 0) {
        if (errno == EINTR) {
            return 0U;
        }
        return std::unexpected(provider_failure(errno));
    }

    std::size_t observed = 0;
    for (int index = 0; index < ready; ++index) {
        const auto& event = events[static_cast<std::size_t>(index)];
        if (event.filter == EVFILT_USER && event.ident == wake_event_ident) {
            ++observed;
            continue;
        }

        if ((event.flags & EV_ERROR) != 0 && event.data != 0) {
            return std::unexpected(provider_failure(static_cast<int>(event.data)));
        }

        const auto token = unpack_event_cookie(cookie_from_udata(event));
        if (!fallback_.is_current_handle(token)) {
            continue;
        }

        ++observed;
        if (is_read_readiness(event)) {
            if (auto completed = fallback_.complete_ready(token, backend_operation_kind::read);
                !completed.has_value()) {
                return std::unexpected(completed.error());
            }
        } else if (is_write_readiness(event)) {
            if (auto completed = fallback_.complete_ready(token, backend_operation_kind::write);
                !completed.has_value()) {
                return std::unexpected(completed.error());
            }
        }
    }

    return observed;
#else
    return std::unexpected(unsupported_error());
#endif
}

io_result<std::size_t> kqueue_backend::drain_completions(
    std::span<backend_completion> out) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return fallback_.drain_completions(out);
#else
    (void)out;
    return std::unexpected(unsupported_error());
#endif
}

void_result kqueue_backend::wake() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (stopped_) {
        return closed_error();
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }

    for (;;) {
        struct kevent event{};
        set_event(event, wake_event_ident, EVFILT_USER, 0, NOTE_TRIGGER, 0,
                  wake_event_cookie);
        if (::kevent(kqueue_fd_, &event, 1, nullptr, 0, nullptr) == 0) {
            return {};
        }
        if (errno == EINTR) {
            continue;
        }
        return std::unexpected(provider_failure(errno));
    }
#else
    return std::unexpected(unsupported_error());
#endif
}

void_result kqueue_backend::shutdown() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (stopped_) {
        return {};
    }
    stopped_ = true;
    (void)fallback_.shutdown();
    close_owned_descriptor();
    return {};
#else
    return fallback_.shutdown();
#endif
}

} // namespace voris::io::backends
