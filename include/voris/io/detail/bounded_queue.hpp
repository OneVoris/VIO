#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <utility>

#include <voris/io/error.hpp>

namespace voris::io::detail {

template<class T>
class bounded_queue {
public:
    explicit bounded_queue(std::size_t capacity)
        : capacity_(capacity) {}

    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return items_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return items_.empty();
    }

    [[nodiscard]] bool full() const noexcept {
        return items_.size() >= capacity_;
    }

    [[nodiscard]] void_result try_push(T value) {
        if (capacity_ == 0 || full()) {
            ++capacity_waiters_;
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "bounded queue is full"));
        }
        items_.push_back(std::move(value));
        return {};
    }

    [[nodiscard]] std::optional<T> pop() {
        if (items_.empty()) {
            return std::nullopt;
        }
        T value = std::move(items_.front());
        items_.pop_front();
        if (capacity_waiters_ != 0) {
            --capacity_waiters_;
        }
        return value;
    }

    [[nodiscard]] std::size_t capacity_waiters() const noexcept {
        return capacity_waiters_;
    }

private:
    std::size_t capacity_;
    std::deque<T> items_;
    std::size_t capacity_waiters_{0};
};

} // namespace voris::io::detail
