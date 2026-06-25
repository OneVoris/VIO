#include <voris/io/shard.hpp>

#include "test_assert.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <thread>

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    shard current(4);
    runtime_metrics initial = current.metrics();
    assert(initial.submitted_tasks == 0);
    assert(initial.long_tasks == 0);
    assert(initial.scheduler_lag == 0ns);
    assert(current.submit([] {}).has_value());
    runtime_metrics after_submit = current.metrics();
    assert(after_submit.submitted_tasks == 1);
    assert(after_submit.queue_depth == 1);
    assert(current.drain() == 1);
    runtime_metrics after_drain = current.metrics();
    assert(after_drain.submitted_tasks == 1);
    assert(after_drain.completed_tasks == 1);
    assert(after_drain.queue_depth == 0);

    shard budgeted(8, loop_budget{.task_budget = 2, .completion_budget = 8, .timer_budget = 8});
    std::vector<int> order;
    assert(budgeted.submit([&order] { order.push_back(1); }).has_value());
    assert(budgeted.submit_system([&order] { order.push_back(0); }).has_value());
    assert(budgeted.submit([&order] { order.push_back(2); }).has_value());
    assert(budgeted.submit([&order] { order.push_back(3); }).has_value());
    assert(budgeted.metrics().queue_depth == 4);

    auto first_turn = budgeted.run_one_loop_iteration();
    assert(first_turn.has_value());
    assert(*first_turn == 2);
    assert(order == std::vector<int>({0, 1}));
    runtime_metrics after_first_turn = budgeted.metrics();
    assert(after_first_turn.completed_tasks == 2);
    assert(after_first_turn.queue_depth == 2);

    auto second_turn = budgeted.run_one_loop_iteration();
    assert(second_turn.has_value());
    assert(*second_turn == 2);
    assert(order == std::vector<int>({0, 1, 2, 3}));
    assert(budgeted.metrics().queue_depth == 0);

    shard lagged(4);
    assert(lagged.submit([] {}).has_value());
    std::this_thread::sleep_for(25ms);
    assert(lagged.drain() == 1);
    assert(lagged.metrics().scheduler_lag >= 10ms);

    runtime_metrics_config metrics_config{.long_task_threshold = 20ms};
    shard measured(4, loop_budget{}, metrics_config);
    assert(measured.submit([] {}).has_value());
    assert(measured.drain() == 1);
    assert(measured.metrics().long_tasks == 0);

    assert(measured.submit([] { std::this_thread::sleep_for(60ms); }).has_value());
    assert(measured.drain() == 1);
    runtime_metrics after_long_task = measured.metrics();
    assert(after_long_task.long_tasks == 1);
    assert(after_long_task.scheduler_lag >= 0ns);

    shard blocked(4);
    std::mutex mutex;
    std::condition_variable started;
    std::condition_variable release;
    bool continuation_started = false;
    bool continuation_released = false;
    std::size_t drained = 0;

    assert(blocked.submit([&] {
                      {
                          std::lock_guard lock(mutex);
                          continuation_started = true;
                      }
                      started.notify_one();

                      std::unique_lock lock(mutex);
                      assert(release.wait_for(lock, 2s, [&] { return continuation_released; }));
                  })
               .has_value());
    std::this_thread::sleep_for(25ms);

    std::thread driver([&] { drained = blocked.drain(); });

    {
        std::unique_lock lock(mutex);
        assert(started.wait_for(lock, 2s, [&] { return continuation_started; }));
    }

    runtime_metrics while_running = blocked.metrics();
    assert(while_running.queue_depth == 0);
    assert(while_running.scheduler_lag >= 10ms);
    assert(while_running.completed_tasks == 0);

    {
        std::lock_guard lock(mutex);
        continuation_released = true;
    }
    release.notify_one();
    driver.join();
    assert(drained == 1);
    runtime_metrics after_blocked = blocked.metrics();
    assert(after_blocked.completed_tasks == 1);
    assert(after_blocked.queue_depth == 0);

    return 0;
}
