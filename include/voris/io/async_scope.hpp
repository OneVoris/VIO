#pragma once

#include <cstddef>
#include <memory>
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
    void_result spawn(task<T> child);

    void_result spawn(task<void> child);
    void_result join();

    [[nodiscard]] const std::vector<vio_error>& errors() const noexcept;

private:
    struct child_operation {
        child_operation() = default;
        child_operation(const child_operation&) = delete;
        child_operation& operator=(const child_operation&) = delete;
        virtual ~child_operation() = default;

        [[nodiscard]] virtual bool is_ready() const noexcept = 0;
        virtual void_result finish() = 0;
    };

    template<class T>
    class task_child final : public child_operation {
    public:
        explicit task_child(task<T> child)
            : child_(std::move(child)) {}

        [[nodiscard]] bool is_ready() const noexcept override {
            return child_.is_ready();
        }

        void_result finish() override {
            auto result = std::move(child_).take_result();
            if (!result.has_value()) {
                return std::unexpected(result.error());
            }
            return {};
        }

    private:
        task<T> child_;
    };

    void record_error(vio_error error);
    void_result observe_child_result(void_result result);

    cancellation_source stop_source_;
    background_error_sink* sink_{nullptr};
    std::vector<vio_error> errors_;
    std::vector<std::unique_ptr<child_operation>> children_;
};

template<class T>
void_result async_scope::spawn(task<T> child) {
    if (stop_requested()) {
        return std::unexpected(make_error(vio_error_code::cancelled,
                                          "scope is stopping"));
    }

    auto owned_child = std::make_unique<task_child<T>>(std::move(child));
    if (owned_child->is_ready()) {
        return observe_child_result(owned_child->finish());
    }

    children_.push_back(std::move(owned_child));
    return {};
}

} // namespace voris::io
