#include <voris/io/timer.hpp>

#include "test_assert.hpp"

#include <algorithm>
#include <chrono>
#include <vector>

using namespace voris::io;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] bool contains_handle(const std::vector<timer_handle>& handles,
                                   timer_handle expected) {
    return std::ranges::any_of(handles, [expected](timer_handle current) {
        return current.id() == expected.id();
    });
}

void cancel_earliest_updates_deadline_and_size() {
    timer_heap heap;
    auto first = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto second = heap.add(virtual_monotonic_clock::time_point{20ms});
    auto third = heap.add(virtual_monotonic_clock::time_point{30ms});

    assert(heap.size() == 3);
    assert(heap.cancel(first));
    assert(heap.size() == 2);
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{20ms});

    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{30ms});
    assert(ready.size() == 2);
    assert(ready[0].id() == second.id());
    assert(ready[1].id() == third.id());
    assert(!contains_handle(ready, first));
    assert(heap.size() == 0);
    assert(!heap.next_deadline().has_value());
}

void cancel_non_root_removes_handle_from_ready_results() {
    timer_heap heap;
    auto first = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto cancelled = heap.add(virtual_monotonic_clock::time_point{30ms});
    auto second = heap.add(virtual_monotonic_clock::time_point{20ms});
    auto third = heap.add(virtual_monotonic_clock::time_point{40ms});

    assert(heap.cancel(cancelled));
    assert(heap.size() == 3);
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{10ms});

    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{40ms});
    assert(ready.size() == 3);
    assert(ready[0].id() == first.id());
    assert(ready[1].id() == second.id());
    assert(ready[2].id() == third.id());
    assert(!contains_handle(ready, cancelled));
    assert(heap.size() == 0);
}

void cancel_rejects_invalid_duplicate_and_removed_handles() {
    timer_heap heap;
    auto first = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto second = heap.add(virtual_monotonic_clock::time_point{20ms});

    assert(!heap.cancel(timer_handle{}));
    assert(heap.cancel(second));
    assert(!heap.cancel(second));

    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{10ms});
    assert(ready.size() == 1);
    assert(ready.front().id() == first.id());
    assert(!heap.cancel(first));
    assert(!heap.cancel(second));
}

void cancel_rejects_handle_from_another_heap() {
    timer_heap first_heap;
    timer_heap second_heap;

    auto first_handle = first_heap.add(virtual_monotonic_clock::time_point{10ms});
    auto second_handle = second_heap.add(virtual_monotonic_clock::time_point{20ms});

    assert(first_handle.id() == second_handle.id());
    assert(!second_heap.cancel(first_handle));
    assert(second_heap.size() == 1);
    assert(second_heap.next_deadline() == virtual_monotonic_clock::time_point{20ms});

    auto ready = second_heap.pop_ready(virtual_monotonic_clock::time_point{20ms});
    assert(ready.size() == 1);
    assert(ready.front().id() == second_handle.id());
    assert(second_heap.size() == 0);
    assert(!second_heap.next_deadline().has_value());
}

void interleaved_add_cancel_pop_keeps_accounting() {
    timer_heap heap;
    auto first = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto cancelled = heap.add(virtual_monotonic_clock::time_point{50ms});
    auto second = heap.add(virtual_monotonic_clock::time_point{20ms});

    assert(heap.cancel(cancelled));
    assert(heap.size() == 2);

    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{15ms});
    assert(ready.size() == 1);
    assert(ready.front().id() == first.id());
    assert(heap.size() == 1);
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{20ms});

    auto third = heap.add(virtual_monotonic_clock::time_point{18ms});
    assert(heap.size() == 2);
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{18ms});

    ready = heap.pop_ready(virtual_monotonic_clock::time_point{25ms});
    assert(ready.size() == 2);
    assert(ready[0].id() == third.id());
    assert(ready[1].id() == second.id());
    assert(heap.size() == 0);
}

void erase_at_sifts_moved_last_entry_up_and_tracks_index() {
    timer_heap heap;
    auto root = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto high_parent = heap.add(virtual_monotonic_clock::time_point{50ms});
    auto low_parent = heap.add(virtual_monotonic_clock::time_point{20ms});
    auto removed = heap.add(virtual_monotonic_clock::time_point{60ms});
    auto sibling = heap.add(virtual_monotonic_clock::time_point{70ms});
    auto leaf = heap.add(virtual_monotonic_clock::time_point{80ms});
    auto moved = heap.add(virtual_monotonic_clock::time_point{30ms});

    assert(heap.cancel(removed));
    assert(heap.size() == 6);
    assert(heap.cancel(root));
    assert(heap.cancel(low_parent));
    assert(heap.size() == 4);
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{30ms});
    assert(heap.cancel(moved));
    assert(heap.size() == 3);

    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{80ms});
    assert(ready.size() == 3);
    assert(ready[0].id() == high_parent.id());
    assert(ready[1].id() == sibling.id());
    assert(ready[2].id() == leaf.id());
    assert(heap.size() == 0);
}

void sleep_uses_virtual_clock() {
    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    current_scheduler_scope scope(ref);
    virtual_monotonic_clock clock;
    auto slept = sleep_for(clock, 5ms);
    auto result = std::move(slept).take_result();
    assert(result.has_value());
    assert(clock.now() == virtual_monotonic_clock::time_point{5ms});

    auto slept_until = sleep_until(clock, virtual_monotonic_clock::time_point{12ms});
    auto until_result = std::move(slept_until).take_result();
    assert(until_result.has_value());
    assert(clock.now() == virtual_monotonic_clock::time_point{12ms});
}

} // namespace

int main() {
    cancel_earliest_updates_deadline_and_size();
    cancel_non_root_removes_handle_from_ready_results();
    cancel_rejects_invalid_duplicate_and_removed_handles();
    cancel_rejects_handle_from_another_heap();
    interleaved_add_cancel_pop_keeps_accounting();
    erase_at_sifts_moved_last_entry_up_and_tracks_index();

    timer_heap heap;
    auto second = heap.add(virtual_monotonic_clock::time_point{20ms});
    auto first = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto cancelled = heap.add(virtual_monotonic_clock::time_point{10ms});
    assert(heap.cancel(cancelled));
    assert(!heap.cancel(timer_handle{}));
    assert(heap.size() == 2);

    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{10ms});
    assert(ready.size() == 1);
    assert(ready.front().id() == first.id());
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{20ms});

    ready = heap.pop_ready(virtual_monotonic_clock::time_point{20ms});
    assert(ready.size() == 1);
    assert(ready.front().id() == second.id());
    assert(!heap.next_deadline().has_value());

    sleep_uses_virtual_clock();

    return 0;
}
