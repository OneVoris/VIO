#include <voris/io/timer.hpp>

#include <atomic>
#include <utility>

namespace voris::io {

std::size_t timer_heap::allocate_owner_id() noexcept {
    static std::atomic<std::size_t> next_owner_id{1};
    return next_owner_id.fetch_add(1);
}

timer_heap::timer_heap() noexcept
    : owner_id_(allocate_owner_id()) {}

timer_heap::timer_heap(timer_heap&& other) noexcept
    : owner_id_(std::exchange(other.owner_id_, allocate_owner_id())),
      next_id_(std::exchange(other.next_id_, 1)),
      entries_(std::move(other.entries_)),
      indices_(std::move(other.indices_)) {
    other.entries_.clear();
    other.indices_.clear();
}

timer_heap& timer_heap::operator=(timer_heap&& other) noexcept {
    if (this == &other) {
        return *this;
    }

    owner_id_ = std::exchange(other.owner_id_, allocate_owner_id());
    next_id_ = std::exchange(other.next_id_, 1);
    entries_ = std::move(other.entries_);
    indices_ = std::move(other.indices_);
    other.entries_.clear();
    other.indices_.clear();
    return *this;
}

bool timer_heap::entry_less(const entry& lhs, const entry& rhs) noexcept {
    if (lhs.deadline < rhs.deadline) {
        return true;
    }
    if (rhs.deadline < lhs.deadline) {
        return false;
    }
    return lhs.id < rhs.id;
}

void timer_heap::swap_entries(std::size_t lhs, std::size_t rhs) noexcept {
    if (lhs == rhs) {
        return;
    }

    using std::swap;
    swap(entries_[lhs], entries_[rhs]);
    indices_.find(entries_[lhs].id)->second = lhs;
    indices_.find(entries_[rhs].id)->second = rhs;
}

void timer_heap::sift_up(std::size_t index) noexcept {
    while (index > 0) {
        const auto parent = (index - 1) / 2;
        if (!entry_less(entries_[index], entries_[parent])) {
            break;
        }
        swap_entries(index, parent);
        index = parent;
    }
}

void timer_heap::sift_down(std::size_t index) noexcept {
    const auto count = entries_.size();
    while (true) {
        const auto left = index * 2 + 1;
        if (left >= count) {
            break;
        }

        const auto right = left + 1;
        auto smallest = left;
        if (right < count && entry_less(entries_[right], entries_[left])) {
            smallest = right;
        }

        if (!entry_less(entries_[smallest], entries_[index])) {
            break;
        }

        swap_entries(index, smallest);
        index = smallest;
    }
}

void timer_heap::repair_at(std::size_t index) noexcept {
    if (index >= entries_.size()) {
        return;
    }

    if (index > 0 && entry_less(entries_[index], entries_[(index - 1) / 2])) {
        sift_up(index);
        return;
    }
    sift_down(index);
}

void timer_heap::erase_at(std::size_t index) noexcept {
    const auto removed_id = entries_[index].id;
    const auto last = entries_.size() - 1;

    if (index != last) {
        swap_entries(index, last);
    }

    indices_.erase(removed_id);
    entries_.pop_back();
    repair_at(index);
}

void timer_heap::pop_deadline_batch(time_point deadline, std::vector<timer_handle>& ready) {
    while (!entries_.empty() && entries_.front().deadline == deadline) {
        const auto id = entries_.front().id;
        ready.push_back(timer_handle(owner_id_, id));
        erase_at(0);
    }
}

timer_handle timer_heap::add(time_point deadline) {
    const timer_handle handle(owner_id_, next_id_++);
    const auto index = entries_.size();
    entries_.push_back(entry{deadline, handle.id()});
    try {
        indices_.emplace(handle.id(), index);
    } catch (...) {
        entries_.pop_back();
        throw;
    }
    sift_up(index);
    return handle;
}

bool timer_heap::cancel(timer_handle handle) noexcept {
    if (!handle.valid() || handle.owner_id_ != owner_id_) {
        return false;
    }

    const auto found = indices_.find(handle.id());
    if (found == indices_.end()) {
        return false;
    }

    erase_at(found->second);
    return true;
}

std::vector<timer_handle> timer_heap::pop_ready(time_point now) {
    std::vector<timer_handle> ready;

    while (!entries_.empty() && entries_.front().deadline <= now) {
        pop_deadline_batch(entries_.front().deadline, ready);
    }
    return ready;
}

std::vector<timer_handle> timer_heap::pop_ready(time_point now, loop_budget_slice& budget) {
    std::vector<timer_handle> ready;

    while (!entries_.empty() && entries_.front().deadline <= now) {
        if (!budget.consume_timer()) {
            break;
        }
        pop_deadline_batch(entries_.front().deadline, ready);
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
