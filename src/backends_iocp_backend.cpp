#include <voris/io/backends/iocp_backend.hpp>

namespace voris::io::backends {

void_result iocp_backend::register_handle(std::size_t native_handle) {
#if defined(_WIN32)
    return fallback_.register_handle(native_handle);
#else
    (void)native_handle;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "IOCP backend is unavailable"));
#endif
}

void_result iocp_backend::submit(backend_operation operation) {
#if defined(_WIN32)
    return fallback_.submit(operation);
#else
    (void)operation;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "IOCP backend is unavailable"));
#endif
}

void_result iocp_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
#if defined(_WIN32)
    return fallback_.cancel(operation_id, reason);
#else
    (void)operation_id;
    (void)reason;
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "IOCP backend is unavailable"));
#endif
}

io_result<std::size_t> iocp_backend::poll() {
#if defined(_WIN32)
    return fallback_.poll();
#else
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "IOCP backend is unavailable"));
#endif
}

void_result iocp_backend::wake() {
#if defined(_WIN32)
    return fallback_.wake();
#else
    return std::unexpected(make_error(vio_error_code::unsupported,
                                      "IOCP backend is unavailable"));
#endif
}

void_result iocp_backend::shutdown() {
    return fallback_.shutdown();
}

} // namespace voris::io::backends
