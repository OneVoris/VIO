#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
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

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock(mutex_);
        return items_.empty();
    }

    [[nodiscard]] bool full() const {
        std::lock_guard lock(mutex_);
        return items_.size() >= capacity_;
    }

    [[nodiscard]] void_result try_push(T value) {
        std::lock_guard lock(mutex_);
        if (capacity_ == 0 || items_.size() >= capacity_) {
            record_full_pressure_locked();
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "bounded queue is full"));
        }
        items_.push_back(std::move(value));
        capacity_waiters_ = 0;
        return {};
    }

    [[nodiscard]] std::optional<T> pop() {
        std::optional<T> value;
        {
            std::lock_guard lock(mutex_);
            if (items_.empty()) {
                return std::nullopt;
            }
            value.emplace(std::move(items_.front()));
            items_.pop_front();
            capacity_waiters_ = 0;
        }
        return value;
    }

    // Saturated failed-push pressure while full, reset when the queue accepts or frees capacity.
    [[nodiscard]] std::size_t full_pressure() const {
        std::lock_guard lock(mutex_);
        return capacity_waiters_;
    }

    [[nodiscard]] std::size_t capacity_waiters() const {
        return full_pressure();
    }

private:
    void record_full_pressure_locked() noexcept {
        const std::size_t pressure_limit = capacity_ == 0 ? 1 : capacity_;
        if (capacity_waiters_ < pressure_limit) {
            ++capacity_waiters_;
        }
    }

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<T> items_;
    std::size_t capacity_waiters_{0};
};

} // namespace voris::io::detail
