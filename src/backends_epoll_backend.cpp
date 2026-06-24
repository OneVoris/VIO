#include <voris/io/backends/epoll_backend.hpp>

#if defined(__linux__)
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <unistd.h>
#endif

namespace voris::io::backends {

epoll_backend::epoll_backend() = default;
epoll_backend::~epoll_backend() = default;

void_result epoll_backend::register_handle(std::size_t native_handle) {
#if defined(__linux__)
    return fallback_.register_handle(native_handle);
#else
    (void)native_handle;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "epoll backend is only available on Linux"));
#endif
}

void_result epoll_backend::submit(backend_operation operation) {
#if defined(__linux__)
    return fallback_.submit(operation);
#else
    (void)operation;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "epoll backend is only available on Linux"));
#endif
}

void_result epoll_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
#if defined(__linux__)
    return fallback_.cancel(operation_id, reason);
#else
    (void)operation_id;
    (void)reason;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "epoll backend is only available on Linux"));
#endif
}

io_result<std::size_t> epoll_backend::poll() {
#if defined(__linux__)
    return fallback_.poll();
#else
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "epoll backend is only available on Linux"));
#endif
}

void_result epoll_backend::wake() {
#if defined(__linux__)
    return fallback_.wake();
#else
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "epoll backend is only available on Linux"));
#endif
}

void_result epoll_backend::shutdown() {
#if defined(__linux__)
    return fallback_.shutdown();
#else
    return {};
#endif
}

} // namespace voris::io::backends
