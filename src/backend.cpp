#include <voris/io/backend.hpp>

namespace voris::io {

void_result virtual_backend::register_handle(std::size_t native_handle) {
    if (native_handle == 0) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "native handle id must be non-zero"));
    }
    ++registered_;
    return {};
}

void_result virtual_backend::submit(backend_operation operation) {
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (operation.id == 0) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "operation id must be non-zero"));
    }
    ++submitted_;
    return {};
}

void_result virtual_backend::cancel(std::size_t operation_id, cancellation_reason) {
    if (operation_id == 0) {
        return std::unexpected(make_error(vio_error_code::invalid_state));
    }
    ++cancelled_;
    return {};
}

io_result<std::size_t> virtual_backend::poll() {
    return submitted_;
}

void_result virtual_backend::wake() {
    ++wakeups_;
    return {};
}

void_result virtual_backend::shutdown() {
    stopped_ = true;
    return {};
}

std::size_t virtual_backend::submitted() const noexcept {
    return submitted_;
}

std::size_t virtual_backend::cancelled() const noexcept {
    return cancelled_;
}

bool virtual_backend::stopped() const noexcept {
    return stopped_;
}

} // namespace voris::io
