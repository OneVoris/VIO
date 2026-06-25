#include <voris/io/backend_wakeup.hpp>
#include <voris/io/detail/mailbox.hpp>
#include <voris/io/loop_budget.hpp>
#include <voris/io/scheduler.hpp>
#include <voris/io/shard.hpp>

#include "test_assert.hpp"

#include <array>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>

int main() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    {
        backend_wakeup wakeup;
        wakeup.wake();
        assert(wakeup.wait_for(0ms));
        assert(wakeup.pending_count() == 0);
        assert(!wakeup.try_consume());
    }

    {
        backend_wakeup wakeup;
        wakeup.wake();
        wakeup.wake();
        wakeup.wake();
        assert(wakeup.wake_count() == 3);
        wakeup.consume();
        assert(wakeup.pending_count() == 2);
        assert(wakeup.try_consume());
        assert(wakeup.pending_count() == 1);
        assert(wakeup.consume_all() == 1);
        assert(wakeup.pending_count() == 0);
        assert(!wakeup.try_consume());
    }

    {
        backend_wakeup wakeup;
        constexpr std::size_t thread_count = 4;
        constexpr std::size_t wake_count = 16;
        std::barrier start(static_cast<std::ptrdiff_t>(thread_count + 1));
        std::array<std::thread, thread_count> threads;

        for (auto& thread : threads) {
            thread = std::thread([&] {
                start.arrive_and_wait();
                for (std::size_t i = 0; i < wake_count; ++i) {
                    wakeup.wake();
                }
            });
        }

        start.arrive_and_wait();
        for (auto& thread : threads) {
            thread.join();
        }

        assert(wakeup.consume_all() == thread_count * wake_count);
        assert(wakeup.pending_count() == 0);
    }

    {
        backend_wakeup wakeup;
        std::mutex mutex;
        std::condition_variable ready;
        std::condition_variable done;
        bool waiter_ready = false;
        bool waiter_done = false;

        std::thread waiter([&] {
            {
                std::lock_guard lock(mutex);
                waiter_ready = true;
            }
            ready.notify_one();

            const bool consumed = wakeup.wait_for(2s);
            {
                std::lock_guard lock(mutex);
                waiter_done = consumed;
            }
            done.notify_one();
        });

        {
            std::unique_lock lock(mutex);
            assert(ready.wait_for(lock, 2s, [&] { return waiter_ready; }));
        }

        wakeup.wake();

        {
            std::unique_lock lock(mutex);
            assert(done.wait_for(lock, 2s, [&] { return waiter_done; }));
        }

        waiter.join();
        assert(wakeup.pending_count() == 0);
    }

    {
        shard worker(4);
        worker.start();

        std::mutex mutex;
        std::condition_variable done;
        bool ran = false;

        assert(worker.submit([&] {
            {
                std::lock_guard lock(mutex);
                ran = true;
            }
            done.notify_one();
        }).has_value());

        {
            std::unique_lock lock(mutex);
            assert(done.wait_for(lock, 2s, [&] { return ran; }));
        }

        assert(worker.submit([&worker] { worker.request_stop(); }).has_value());
        worker.join();
        assert(!worker.running());
    }

    {
        shard worker(4);
        worker.start();
        assert(worker.wait_until_parked_for(2s));

        std::mutex mutex;
        std::condition_variable joined;
        bool join_returned = false;

        std::thread joiner([&] {
            worker.join();
            {
                std::lock_guard lock(mutex);
                join_returned = true;
            }
            joined.notify_one();
        });

        worker.request_stop();

        {
            std::unique_lock lock(mutex);
            assert(joined.wait_for(lock, 2s, [&] { return join_returned; }));
        }

        joiner.join();
        assert(!worker.running());
    }

    loop_budget budget;
    assert(budget.task_budget > 0);
    assert(budget.completion_budget > 0);
    assert(budget.timer_budget > 0);
    assert(budget.validate().has_value());

    {
        loop_budget invalid_task{.task_budget = 0, .completion_budget = 1, .timer_budget = 1};
        auto result = invalid_task.validate();
        assert(!result.has_value());
        assert(result.error().classification == vio_error_code::invalid_state);

        loop_budget invalid_completion{.task_budget = 1,
                                       .completion_budget = 0,
                                       .timer_budget = 1};
        result = invalid_completion.validate();
        assert(!result.has_value());
        assert(result.error().classification == vio_error_code::invalid_state);

        loop_budget invalid_timer{.task_budget = 1, .completion_budget = 1, .timer_budget = 0};
        result = invalid_timer.validate();
        assert(!result.has_value());
        assert(result.error().classification == vio_error_code::invalid_state);

        auto slice = loop_budget_slice::create(invalid_timer);
        assert(!slice.has_value());
        assert(slice.error().classification == vio_error_code::invalid_state);
    }

    {
        auto slice_result =
            loop_budget_slice::create(loop_budget{.task_budget = 2,
                                                  .completion_budget = 1,
                                                  .timer_budget = 3});
        assert(slice_result.has_value());
        auto slice = *slice_result;

        assert(slice.remaining_tasks() == 2);
        assert(slice.remaining_completions() == 1);
        assert(slice.remaining_timers() == 3);

        assert(slice.consume_task());
        assert(slice.consume_task());
        assert(!slice.consume_task());
        assert(slice.consumed_tasks() == 2);
        assert(slice.remaining_tasks() == 0);

        assert(slice.consume_completion());
        assert(!slice.consume_completion());
        assert(slice.consumed_completions() == 1);
        assert(slice.remaining_completions() == 0);

        assert(slice.consume_timer());
        assert(slice.consume_timer());
        assert(slice.consume_timer());
        assert(!slice.consume_timer());
        assert(slice.consumed_timers() == 3);
        assert(slice.remaining_timers() == 0);
    }

    {
        detail::mailbox budgeted(4);
        std::vector<int> order;
        assert(budgeted.submit([&order] { order.push_back(1); }).has_value());
        assert(budgeted.submit([&order] { order.push_back(2); }).has_value());
        assert(budgeted.submit([&order] { order.push_back(3); }).has_value());

        auto first_slice =
            loop_budget_slice::create(loop_budget{.task_budget = 2,
                                                  .completion_budget = 8,
                                                  .timer_budget = 8});
        assert(first_slice.has_value());
        assert(budgeted.run_for_budget(*first_slice) == 2);
        assert(order == std::vector<int>({1, 2}));
        assert(first_slice->consumed_tasks() == 2);

        auto second_slice =
            loop_budget_slice::create(loop_budget{.task_budget = 2,
                                                  .completion_budget = 8,
                                                  .timer_budget = 8});
        assert(second_slice.has_value());
        assert(budgeted.run_for_budget(*second_slice) == 1);
        assert(order == std::vector<int>({1, 2, 3}));
        assert(second_slice->consumed_tasks() == 1);
    }

    {
        shard worker(4, loop_budget{.task_budget = 2, .completion_budget = 8, .timer_budget = 8});
        std::vector<int> order;
        assert(worker.submit([&order] { order.push_back(1); }).has_value());
        assert(worker.submit([&order] { order.push_back(2); }).has_value());
        assert(worker.submit([&order] { order.push_back(3); }).has_value());

        auto first_turn = worker.run_one_loop_iteration();
        assert(first_turn.has_value());
        assert(*first_turn == 2);
        assert(order == std::vector<int>({1, 2}));

        auto second_turn = worker.run_one_loop_iteration();
        assert(second_turn.has_value());
        assert(*second_turn == 1);
        assert(order == std::vector<int>({1, 2, 3}));
    }

    {
        shard worker(4, loop_budget{.task_budget = 1, .completion_budget = 8, .timer_budget = 8});
        std::vector<int> order;
        assert(worker.submit([&order] { order.push_back(2); }).has_value());
        assert(worker.submit_system([&order] { order.push_back(1); }).has_value());

        auto first_turn = worker.run_one_loop_iteration();
        assert(first_turn.has_value());
        assert(*first_turn == 1);
        assert(order == std::vector<int>({1}));

        auto second_turn = worker.run_one_loop_iteration();
        assert(second_turn.has_value());
        assert(*second_turn == 1);
        assert(order == std::vector<int>({1, 2}));
    }

    {
        shard worker(4, loop_budget{.task_budget = 1, .completion_budget = 8, .timer_budget = 8});
        const auto expected_scheduler = worker.scheduler();
        bool saw_scheduler = false;
        bool matched_scheduler = false;
        assert(worker.submit([&] {
                         auto current = require_current_scheduler();
                         saw_scheduler = current.has_value();
                         if (current.has_value()) {
                             matched_scheduler = *current == expected_scheduler;
                         }
                     })
                   .has_value());

        auto turn = worker.run_one_loop_iteration();
        assert(turn.has_value());
        assert(*turn == 1);
        assert(saw_scheduler);
        assert(matched_scheduler);
    }

    {
        shard worker(4, loop_budget{.task_budget = 0, .completion_budget = 8, .timer_budget = 8});
        bool ran = false;
        assert(worker.submit([&ran] { ran = true; }).has_value());

        worker.start();

        std::mutex mutex;
        std::condition_variable joined;
        bool join_returned = false;
        std::thread joiner([&] {
            worker.join();
            {
                std::lock_guard lock(mutex);
                join_returned = true;
            }
            joined.notify_one();
        });

        {
            std::unique_lock lock(mutex);
            assert(joined.wait_for(lock, 2s, [&] { return join_returned; }));
        }

        joiner.join();

        auto error = worker.last_loop_error();
        assert(error.has_value());
        assert(error->classification == vio_error_code::invalid_state);
        assert(!ran);
        assert(worker.metrics().completed_tasks == 0);
    }

    return 0;
}
