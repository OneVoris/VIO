#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include <voris/io/cancellation.hpp>
#include <voris/io/error.hpp>
#include <voris/io/task.hpp>

namespace voris::io {

class background_error_sink {
public:
    void record(vio_error error);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const std::vector<vio_error>& errors() const noexcept;

private:
    std::vector<vio_error> errors_;
};

class async_scope {
public:
    explicit async_scope(background_error_sink* sink = nullptr);

    async_scope(const async_scope&) = delete;
    async_scope& operator=(const async_scope&) = delete;

    async_scope(async_scope&&) = delete;
    async_scope& operator=(async_scope&&) = delete;

    [[nodiscard]] cancellation_token token() const noexcept;
    [[nodiscard]] bool stop_requested() const;
    [[nodiscard]] std::size_t pending_count() const noexcept;

    bool request_stop(cancellation_reason reason = cancellation_reason::scope_shutdown);

    template<class T>
    void_result spawn(task<T> child) {
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

    void_result spawn(task<void> child);
    void_result join();

    [[nodiscard]] const std::vector<vio_error>& errors() const noexcept;

private:
    cancellation_source stop_source_;
    background_error_sink* sink_{nullptr};
    std::size_t pending_{0};
    std::vector<vio_error> errors_;
};

} // namespace voris::io
