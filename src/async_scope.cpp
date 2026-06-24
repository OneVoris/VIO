#include <voris/io/async_scope.hpp>

#include <utility>

namespace voris::io {

void background_error_sink::record(vio_error error) {
    errors_.push_back(std::move(error));
}

bool background_error_sink::empty() const noexcept {
    return errors_.empty();
}

std::size_t background_error_sink::size() const noexcept {
    return errors_.size();
}

const std::vector<vio_error>& background_error_sink::errors() const noexcept {
    return errors_;
}

async_scope::async_scope(background_error_sink* sink)
    : sink_(sink) {}

cancellation_token async_scope::token() const noexcept {
    return stop_source_.token();
}

bool async_scope::stop_requested() const {
    return stop_source_.cancellation_requested();
}

std::size_t async_scope::pending_count() const noexcept {
    return pending_;
}

bool async_scope::request_stop(cancellation_reason reason) {
    return stop_source_.request_cancellation(reason);
}

void_result async_scope::spawn(task<void> child) {
    if (stop_requested()) {
        return std::unexpected(make_error(vio_error_code::cancelled,
                                          "scope is stopping"));
    }

    ++pending_;
    auto result = std::move(child).take_result();
    --pending_;
    if (!result.has_value()) {
        errors_.push_back(result.error());
        if (sink_ != nullptr) {
            sink_->record(result.error());
        }
        return std::unexpected(result.error());
    }
    return {};
}

void_result async_scope::join() {
    if (pending_ != 0) {
        return std::unexpected(make_error(vio_error_code::invalid_state,
                                          "scope has pending tasks"));
    }
    if (!errors_.empty()) {
        return std::unexpected(errors_.front());
    }
    return {};
}

const std::vector<vio_error>& async_scope::errors() const noexcept {
    return errors_;
}

} // namespace voris::io
