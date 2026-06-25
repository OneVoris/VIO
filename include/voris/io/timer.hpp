#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

#include <voris/io/task.hpp>
#include <voris/io/virtual_clock.hpp>

namespace voris::io {

class timer_handle {
public:
    timer_handle() noexcept = default;

    [[nodiscard]] std::size_t id() const noexcept {
        return id_;
    }

    [[nodiscard]] bool valid() const noexcept {
        return id_ != 0;
    }

private:
    friend class timer_heap;

    explicit timer_handle(std::size_t id) noexcept
        : id_(id) {}

    std::size_t id_{0};
};

class timer_heap {
public:
    using time_point = virtual_monotonic_clock::time_point;

    [[nodiscard]] timer_handle add(time_point deadline);
    [[nodiscard]] bool cancel(timer_handle handle) noexcept;
    [[nodiscard]] std::vector<timer_handle> pop_ready(time_point now);
    [[nodiscard]] std::optional<time_point> next_deadline() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct entry {
        time_point deadline;
        std::size_t id;
    };

    [[nodiscard]] static bool entry_less(const entry& lhs, const entry& rhs) noexcept;
    void swap_entries(std::size_t lhs, std::size_t rhs) noexcept;
    void sift_up(std::size_t index) noexcept;
    void sift_down(std::size_t index) noexcept;
    void repair_at(std::size_t index) noexcept;
    void erase_at(std::size_t index) noexcept;

    std::size_t next_id_{1};
    std::vector<entry> entries_;
    std::unordered_map<std::size_t, std::size_t> indices_;
};

[[nodiscard]] task<void> sleep_until(virtual_monotonic_clock& clock,
                                     virtual_monotonic_clock::time_point deadline);

[[nodiscard]] task<void> sleep_for(virtual_monotonic_clock& clock,
                                   virtual_monotonic_clock::duration duration);

} // namespace voris::io
