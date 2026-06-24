#include <voris/io/timer.hpp>

#include <utility>

namespace voris::io {

timer_handle timer_heap::add(time_point deadline) {
    const timer_handle handle(next_id_++);
    entries_.push_back(entry{deadline, handle.id(), false});
    std::ranges::push_heap(entries_, std::ranges::greater{}, &entry::deadline);
    return handle;
}

bool timer_heap::cancel(timer_handle handle) noexcept {
    for (auto& current : entries_) {
        if (current.id == handle.id() && !current.cancelled) {
            current.cancelled = true;
            return true;
        }
    }
    return false;
}

std::vector<timer_handle> timer_heap::pop_ready(time_point now) {
    std::vector<timer_handle> ready;
    std::erase_if(entries_, [](const entry& current) { return current.cancelled; });
    std::ranges::make_heap(entries_, std::ranges::greater{}, &entry::deadline);

    while (!entries_.empty() && entries_.front().deadline <= now) {
        std::ranges::pop_heap(entries_, std::ranges::greater{}, &entry::deadline);
        entry current = entries_.back();
        entries_.pop_back();
        if (!current.cancelled) {
            ready.push_back(timer_handle(current.id));
        }
    }
    return ready;
}

std::optional<timer_heap::time_point> timer_heap::next_deadline() const noexcept {
    if (entries_.empty()) {
        return std::nullopt;
    }
    return entries_.front().deadline;
}

std::size_t timer_heap::size() const noexcept {
    return entries_.size();
}

task<void> sleep_until(virtual_monotonic_clock& clock,
                       virtual_monotonic_clock::time_point deadline) {
    if (clock.now() < deadline) {
        auto advanced = clock.advance_to(deadline);
        (void)advanced;
    }
    co_return;
}

task<void> sleep_for(virtual_monotonic_clock& clock,
                     virtual_monotonic_clock::duration duration) {
    if (duration < virtual_monotonic_clock::duration::zero()) {
        co_return;
    }
    auto advanced = clock.advance_by(duration);
    (void)advanced;
    co_return;
}

} // namespace voris::io
