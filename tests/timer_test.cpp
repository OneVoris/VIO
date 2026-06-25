#include <voris/io/timer.hpp>
#include <voris/io/loop_budget.hpp>

#include "test_assert.hpp"

#include <algorithm>
#include <chrono>
#include <type_traits>
#include <utility>
#include <vector>

using namespace voris::io;
using namespace std::chrono_literals;

static_assert(!std::is_copy_constructible_v<timer_heap>);
static_assert(!std::is_copy_assignable_v<timer_heap>);
static_assert(std::is_nothrow_move_constructible_v<timer_heap>);
static_assert(std::is_nothrow_move_assignable_v<timer_heap>);

namespace {

[[nodiscard]] bool contains_handle(const std::vector<timer_handle>& handles,
                                   timer_handle expected) {
    return std::ranges::any_of(handles, [expected](timer_handle current) {
        return current.id() == expected.id();
    });
}

[[nodiscard]] loop_budget_slice make_timer_budget_slice(std::size_t timer_budget) {
    loop_budget budget;
    budget.timer_budget = timer_budget;

    auto slice = loop_budget_slice::create(budget);
    assert(slice.has_value());
    return std::move(slice).value();
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

void budgeted_pop_keeps_same_deadline_batch_together() {
    timer_heap heap;
    auto first = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto second = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto third = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto later = heap.add(virtual_monotonic_clock::time_point{20ms});

    auto slice = make_timer_budget_slice(1);
    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{100ms}, slice);

    assert(ready.size() == 3);
    assert(ready[0].id() == first.id());
    assert(ready[1].id() == second.id());
    assert(ready[2].id() == third.id());
    assert(!contains_handle(ready, later));
    assert(slice.consumed_timers() == 1);
    assert(slice.remaining_timers() == 0);
    assert(heap.size() == 1);
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{20ms});
}

void budgeted_pop_limits_distinct_deadline_batches_after_forward_jump() {
    timer_heap heap;
    auto first = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto second = heap.add(virtual_monotonic_clock::time_point{20ms});
    auto third = heap.add(virtual_monotonic_clock::time_point{30ms});

    auto slice = make_timer_budget_slice(2);
    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{1h}, slice);

    assert(ready.size() == 2);
    assert(ready[0].id() == first.id());
    assert(ready[1].id() == second.id());
    assert(!contains_handle(ready, third));
    assert(slice.consumed_timers() == 2);
    assert(slice.remaining_timers() == 0);
    assert(heap.size() == 1);
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{30ms});

    auto next_slice = make_timer_budget_slice(1);
    ready = heap.pop_ready(virtual_monotonic_clock::time_point{1h}, next_slice);

    assert(ready.size() == 1);
    assert(ready.front().id() == third.id());
    assert(next_slice.consumed_timers() == 1);
    assert(heap.size() == 0);
    assert(!heap.next_deadline().has_value());
}

void budgeted_pop_with_zero_remaining_timer_budget_leaves_heap_unchanged() {
    timer_heap heap;
    auto first = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto second = heap.add(virtual_monotonic_clock::time_point{20ms});

    auto slice = make_timer_budget_slice(1);
    assert(slice.consume_timer());
    assert(slice.remaining_timers() == 0);

    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{1h}, slice);

    assert(ready.empty());
    assert(slice.consumed_timers() == 1);
    assert(heap.size() == 2);
    assert(heap.next_deadline() == virtual_monotonic_clock::time_point{10ms});

    auto unbudgeted_ready = heap.pop_ready(virtual_monotonic_clock::time_point{1h});
    assert(unbudgeted_ready.size() == 2);
    assert(unbudgeted_ready[0].id() == first.id());
    assert(unbudgeted_ready[1].id() == second.id());
}

void budgeted_pop_skips_cancelled_handles_and_fired_handles_cannot_cancel() {
    timer_heap heap;
    auto cancelled = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto fired = heap.add(virtual_monotonic_clock::time_point{10ms});
    auto later = heap.add(virtual_monotonic_clock::time_point{20ms});

    assert(heap.cancel(cancelled));

    auto slice = make_timer_budget_slice(1);
    auto ready = heap.pop_ready(virtual_monotonic_clock::time_point{10ms}, slice);

    assert(ready.size() == 1);
    assert(ready.front().id() == fired.id());
    assert(!contains_handle(ready, cancelled));
    assert(!heap.cancel(fired));
    assert(heap.cancel(later));
    assert(heap.size() == 0);
}

void move_construct_transfers_owner_and_resets_source() {
    timer_heap source;
    auto first = source.add(virtual_monotonic_clock::time_point{10ms});
    auto second = source.add(virtual_monotonic_clock::time_point{20ms});

    timer_heap moved(std::move(source));

    assert(source.size() == 0);
    assert(!source.cancel(first));
    assert(moved.cancel(first));
    assert(moved.size() == 1);

    auto replacement = source.add(virtual_monotonic_clock::time_point{5ms});
    assert(!source.cancel(second));
    assert(source.cancel(replacement));
    assert(source.size() == 0);

    auto ready = moved.pop_ready(virtual_monotonic_clock::time_point{20ms});
    assert(ready.size() == 1);
    assert(ready.front().id() == second.id());
    assert(moved.size() == 0);
}

void move_assign_transfers_owner_and_resets_source() {
    timer_heap source;
    auto first = source.add(virtual_monotonic_clock::time_point{10ms});
    auto second = source.add(virtual_monotonic_clock::time_point{20ms});

    timer_heap target;
    auto old_target = target.add(virtual_monotonic_clock::time_point{1ms});

    target = std::move(source);

    assert(source.size() == 0);
    assert(!source.cancel(first));
    assert(!target.cancel(old_target));
    assert(target.cancel(first));
    assert(target.size() == 1);

    auto replacement = source.add(virtual_monotonic_clock::time_point{5ms});
    assert(!source.cancel(second));
    assert(source.cancel(replacement));
    assert(source.size() == 0);

    auto ready = target.pop_ready(virtual_monotonic_clock::time_point{20ms});
    assert(ready.size() == 1);
    assert(ready.front().id() == second.id());
    assert(target.size() == 0);
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
    budgeted_pop_keeps_same_deadline_batch_together();
    budgeted_pop_limits_distinct_deadline_batches_after_forward_jump();
    budgeted_pop_with_zero_remaining_timer_budget_leaves_heap_unchanged();
    budgeted_pop_skips_cancelled_handles_and_fired_handles_cannot_cancel();
    move_construct_transfers_owner_and_resets_source();
    move_assign_transfers_owner_and_resets_source();

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
