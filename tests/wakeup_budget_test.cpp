#include <voris/io/backend_wakeup.hpp>
#include <voris/io/loop_budget.hpp>
#include <voris/io/shard.hpp>

#include "test_assert.hpp"

#include <array>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <thread>

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

    return 0;
}
