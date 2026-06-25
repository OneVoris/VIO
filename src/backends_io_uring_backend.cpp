#include <voris/io/backends/io_uring_backend.hpp>

namespace voris::io::backends {

io_uring_capabilities detect_io_uring_capabilities() noexcept {
#if defined(__linux__)
    return io_uring_capabilities{
        .available = true,
        .supports_accept = true,
        .supports_connect = true,
        .supports_files = true,
        .supports_cancel = true,
        .supports_registered_buffers = true,
        .supports_registered_files = true,
    };
#else
    return {};
#endif
}

io_uring_backend::io_uring_backend(io_uring_capabilities capabilities)
    : capabilities_(capabilities) {}

const io_uring_capabilities& io_uring_backend::capabilities() const noexcept {
    return capabilities_;
}

bool io_uring_backend::default_eligible() const noexcept {
    return capabilities_.available && capabilities_.supports_cancel &&
           capabilities_.supports_accept && capabilities_.supports_connect &&
           capabilities_.supports_files;
}

io_result<backend_handle_token> io_uring_backend::register_handle(std::size_t native_handle) {
    if (!capabilities_.available) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring is unavailable"));
    }
    return fallback_.register_handle(native_handle);
}

void_result io_uring_backend::submit(backend_operation operation) {
    if (!capabilities_.available) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring is unavailable"));
    }
    return fallback_.submit(operation);
}

void_result io_uring_backend::cancel(std::size_t operation_id, cancellation_reason reason) {
    if (!capabilities_.supports_cancel) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring cancellation is unavailable"));
    }
    return fallback_.cancel(operation_id, reason);
}

void_result io_uring_backend::close_handle(backend_handle_token token) {
    if (!capabilities_.available) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring is unavailable"));
    }
    return fallback_.close_handle(token);
}

io_result<std::size_t> io_uring_backend::poll() {
    if (!capabilities_.available) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring is unavailable"));
    }
    return fallback_.poll();
}

io_result<std::size_t> io_uring_backend::drain_completions(
    std::span<backend_completion> out) {
    if (!capabilities_.available) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring is unavailable"));
    }
    return fallback_.drain_completions(out);
}

void_result io_uring_backend::wake() {
    if (!capabilities_.available) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "io_uring is unavailable"));
    }
    return fallback_.wake();
}

void_result io_uring_backend::shutdown() {
    return fallback_.shutdown();
}

void_result io_uring_backend::register_buffers(std::size_t count) {
    if (!capabilities_.supports_registered_buffers) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered buffers unavailable"));
    }
    registered_buffers_ = count;
    return {};
}

void_result io_uring_backend::register_files(std::size_t count) {
    if (!capabilities_.supports_registered_files) {
        return std::unexpected(make_error(vio_error_code::unsupported,
                                          "registered files unavailable"));
    }
    registered_files_ = count;
    return {};
}

} // namespace voris::io::backends
