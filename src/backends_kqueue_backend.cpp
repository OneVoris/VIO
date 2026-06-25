#include <voris/io/backends/kqueue_backend.hpp>

namespace voris::io::backends {

io_result<backend_handle_token> kqueue_backend::register_handle(std::size_t native_handle) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return fallback_.register_handle(native_handle);
#else
    (void)native_handle;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "kqueue backend is unavailable"));
#endif
}

void_result kqueue_backend::submit(backend_operation operation) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return fallback_.submit(operation);
#else
    (void)operation;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "kqueue backend is unavailable"));
#endif
}

void_result kqueue_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return fallback_.cancel(operation_id, reason);
#else
    (void)operation_id;
    (void)reason;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "kqueue backend is unavailable"));
#endif
}

void_result kqueue_backend::close_handle(backend_handle_token token) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return fallback_.close_handle(token);
#else
    (void)token;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "kqueue backend is unavailable"));
#endif
}

io_result<std::size_t> kqueue_backend::poll() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return fallback_.poll();
#else
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "kqueue backend is unavailable"));
#endif
}

io_result<std::size_t> kqueue_backend::drain_completions(
    std::span<backend_completion> out) {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return fallback_.drain_completions(out);
#else
    (void)out;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "kqueue backend is unavailable"));
#endif
}

void_result kqueue_backend::wake() {
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    return fallback_.wake();
#else
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "kqueue backend is unavailable"));
#endif
}

void_result kqueue_backend::shutdown() {
    return fallback_.shutdown();
}

} // namespace voris::io::backends
