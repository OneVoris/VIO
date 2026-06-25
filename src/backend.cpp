#include <voris/io/backend.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace voris::io {

namespace {

[[nodiscard]] detail::native_handle_token to_native_token(backend_handle_token token) noexcept {
    return detail::native_handle_token{token.native_handle, token.generation};
}

[[nodiscard]] backend_handle_token to_backend_token(detail::native_handle_token token) noexcept {
    return backend_handle_token{token.id, token.generation};
}

[[nodiscard]] void_result closed_completion() {
    return std::unexpected(make_error(vio_error_code::closed));
}

[[nodiscard]] void_result invalid_state(std::string diagnostic = {}) {
    return std::unexpected(make_error(vio_error_code::invalid_state, std::move(diagnostic)));
}

} // namespace

io_result<backend_handle_token> virtual_backend::register_handle(std::size_t native_handle) {
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }

    auto registered = registry_.register_handle(native_handle);
    if (!registered.has_value()) {
        return std::unexpected(registered.error());
    }

    ++registered_;
    return to_backend_token(*registered);
}

void_result virtual_backend::submit(backend_operation operation) {
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (operation.id == 0) {
        return invalid_state("operation id must be non-zero");
    }
    if (!is_current_handle(operation.handle)) {
        return invalid_state("operation handle token is not current");
    }
    const auto operation_id = operation.id;
    const auto duplicate = std::ranges::find_if(pending_, [operation_id](const auto& pending) {
        return pending.operation.id == operation_id;
    });
    if (duplicate != pending_.end()) {
        return invalid_state("operation id is already pending");
    }

    pending_.push_back(pending_operation{operation});
    ++submitted_;
    return {};
}

void_result virtual_backend::cancel(std::size_t operation_id, cancellation_reason) {
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    if (operation_id == 0) {
        return invalid_state("operation id must be non-zero");
    }

    const auto found = std::ranges::find_if(pending_, [operation_id](const auto& pending) {
        return pending.operation.id == operation_id;
    });
    if (found == pending_.end()) {
        return invalid_state("operation id is not pending");
    }

    ++cancelled_;
    return {};
}

void_result virtual_backend::close_handle(backend_handle_token token) {
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }

    auto closed = registry_.close(to_native_token(token));
    if (!closed.has_value()) {
        return closed;
    }

    complete_pending(token, closed_completion());
    return {};
}

io_result<std::size_t> virtual_backend::poll() {
    return completions_.size();
}

io_result<std::size_t> virtual_backend::drain_completions(
    std::span<backend_completion> out) {
    if (out.empty()) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "completion drain output must not be empty"));
    }

    const auto count = std::min(out.size(), completions_.size());
    for (std::size_t index = 0; index < count; ++index) {
        out[index] = std::move(completions_.front());
        completions_.pop_front();
    }
    return count;
}

void_result virtual_backend::wake() {
    if (stopped_) {
        return std::unexpected(make_error(vio_error_code::closed));
    }

    ++wakeups_;
    return {};
}

void_result virtual_backend::shutdown() {
    if (stopped_) {
        return {};
    }

    stopped_ = true;
    while (!pending_.empty()) {
        completions_.push_back(backend_completion{pending_.front().operation.id,
                                                  closed_completion()});
        pending_.erase(pending_.begin());
    }
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

bool virtual_backend::is_current_handle(backend_handle_token token) const noexcept {
    return registry_.is_current(to_native_token(token));
}

void_result virtual_backend::complete_ready(backend_handle_token token) {
    if (!is_current_handle(token)) {
        return invalid_state("operation handle token is not current");
    }

    complete_pending(token, {});
    return {};
}

void virtual_backend::complete_pending(backend_handle_token token, const void_result& result) {
    for (auto iterator = pending_.begin(); iterator != pending_.end();) {
        if (iterator->operation.handle == token) {
            completions_.push_back(backend_completion{iterator->operation.id, result});
            iterator = pending_.erase(iterator);
            continue;
        }
        ++iterator;
    }
}

} // namespace voris::io
