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
    event.data.fd = wake_fd_;
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

void_result epoll_backend::register_handle(std::size_t native_handle) {
#if defined(__linux__)
    if (stopped_) {
        return closed_error();
    }
    if (native_handle == 0 ||
        native_handle > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "native handle id must be a valid file descriptor"));
    }
    if (auto initialized = initialization_result(); !initialized.has_value()) {
        return initialized;
    }

    epoll_event event{};
    event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    event.data.fd = static_cast<int>(native_handle);
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, event.data.fd, &event) != 0) {
        return std::unexpected(provider_failure(errno));
    }
    return {};
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

    for (int index = 0; index < ready; ++index) {
        if (events[static_cast<std::size_t>(index)].data.fd != wake_fd_) {
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
    }

    return static_cast<std::size_t>(ready);
#else
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
    stopped_ = true;
    close_owned_descriptors();
    return {};
#else
    return {};
#endif
}

} // namespace voris::io::backends
