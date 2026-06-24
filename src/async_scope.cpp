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
    return children_.size();
}

bool async_scope::request_stop(cancellation_reason reason) {
    return stop_source_.request_cancellation(reason);
}

void_result async_scope::spawn(task<void> child) {
    if (stop_requested()) {
        return std::unexpected(make_error(vio_error_code::cancelled,
                                          "scope is stopping"));
    }

    auto owned_child = std::make_unique<task_child<void>>(std::move(child));
    if (owned_child->is_ready()) {
        return observe_child_result(owned_child->finish());
    }

    children_.push_back(std::move(owned_child));
    return {};
}

void_result async_scope::join() {
    for (auto child = children_.begin(); child != children_.end();) {
        if (!(*child)->is_ready()) {
            ++child;
            continue;
        }

        (void)observe_child_result((*child)->finish());
        child = children_.erase(child);
    }

    if (!children_.empty()) {
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

void async_scope::record_error(vio_error error) {
    errors_.push_back(error);
    if (sink_ != nullptr) {
        sink_->record(std::move(error));
    }
}

void_result async_scope::observe_child_result(void_result result) {
    if (result.has_value()) {
        return {};
    }

    vio_error error = result.error();
    record_error(error);
    return std::unexpected(std::move(error));
}

} // namespace voris::io
