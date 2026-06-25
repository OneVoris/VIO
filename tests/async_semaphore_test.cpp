#include <voris/io/async_semaphore.hpp>
#include <voris/io/task.hpp>

#include "test_assert.hpp"
#include <limits>
#include <vector>

namespace {

template<class Result>
void assert_error(const Result& result, voris::io::vio_error_code code) {
    assert(!result.has_value());
    assert(result.error().classification == code);
}

voris::io::task<voris::io::vio_error_code> acquire_code(voris::io::async_semaphore& semaphore,
                                                        bool& resumed,
                                                        std::vector<int>& order,
                                                        int id) {
    auto acquired = co_await semaphore.acquire();
    resumed = true;
    if (acquired.has_value()) {
        order.push_back(id);
        co_return voris::io::vio_error_code::none;
    }
    co_return acquired.error().classification;
}

voris::io::task<voris::io::vio_error_code> acquire_code(voris::io::async_semaphore& semaphore,
                                                        bool& resumed) {
    auto acquired = co_await semaphore.acquire();
    resumed = true;
    if (acquired.has_value()) {
        co_return voris::io::vio_error_code::none;
    }
    co_return acquired.error().classification;
}

void assert_task_code(voris::io::task<voris::io::vio_error_code>& task,
                      voris::io::vio_error_code expected) {
    auto result = std::move(task).take_result();
    assert(result.has_value());
    assert(*result == expected);
}

} // namespace

int main() {
    using namespace voris::io;

    set_current_scheduler_for_testing(std::nullopt);

    {
        async_semaphore semaphore(1);
        assert(semaphore.max_waiters() == 1);
        assert(semaphore.try_acquire().has_value());
        assert(semaphore.permits() == 0);

        auto failed = semaphore.try_acquire();
        assert_error(failed, vio_error_code::resource_exhausted);
        assert(semaphore.waiters() == 0);
    }

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);

    {
        current_scheduler_scope scope(ref);
        async_semaphore semaphore(1);
        bool resumed = false;

        auto waiter = acquire_code(semaphore, resumed);

        assert(resumed);
        assert(waiter.is_ready());
        assert(semaphore.permits() == 0);
        assert(semaphore.waiters() == 0);
        assert_task_code(waiter, vio_error_code::none);
    }

    {
        current_scheduler_scope scope(ref);
        async_semaphore semaphore(0, 1);
        bool resumed = false;

        auto waiter = acquire_code(semaphore, resumed);

        assert(!waiter.is_ready());
        assert(!resumed);
        assert(semaphore.waiters() == 1);

        semaphore.release(0);
        assert(!resumed);
        assert(semaphore.waiters() == 1);
        assert(scheduler.ready_count() == 0);

        semaphore.release();
        assert(!resumed);
        assert(!waiter.is_ready());
        assert(semaphore.waiters() == 0);
        assert(scheduler.ready_count() == 1);

        assert(scheduler.run_until_idle() == 1);
        assert(resumed);
        assert(waiter.is_ready());
        assert_task_code(waiter, vio_error_code::none);
    }

    {
        current_scheduler_scope scope(ref);
        async_semaphore semaphore(0, 2);
        std::vector<int> order;
        bool first_resumed = false;
        bool second_resumed = false;

        auto first = acquire_code(semaphore, first_resumed, order, 1);
        auto second = acquire_code(semaphore, second_resumed, order, 2);

        assert(semaphore.waiters() == 2);
        semaphore.release(1);
        assert(semaphore.waiters() == 1);
        assert(!first_resumed);
        assert(!second_resumed);
        assert(order.empty());

        assert(scheduler.run_until_idle() == 1);
        assert(first_resumed);
        assert(!second_resumed);
        assert((order == std::vector<int>{1}));

        semaphore.release(2);
        assert(semaphore.waiters() == 0);
        assert(semaphore.permits() == 1);
        assert(scheduler.run_until_idle() == 1);
        assert(second_resumed);
        assert((order == std::vector<int>{1, 2}));
        assert(semaphore.try_acquire().has_value());
        assert(semaphore.permits() == 0);

        assert_task_code(first, vio_error_code::none);
        assert_task_code(second, vio_error_code::none);
    }

    {
        default_scheduler waiter_scheduler;
        default_scheduler release_scheduler;
        scheduler_ref waiter_ref(waiter_scheduler);
        scheduler_ref release_ref(release_scheduler);
        async_semaphore semaphore(0, 1);
        bool resumed = false;

        task<vio_error_code> waiter;
        {
            current_scheduler_scope scope(waiter_ref);
            waiter = acquire_code(semaphore, resumed);
        }

        assert(!waiter.is_ready());
        assert(semaphore.waiters() == 1);

        {
            current_scheduler_scope scope(release_ref);
            semaphore.release();
        }

        assert(waiter_scheduler.ready_count() == 1);
        assert(release_scheduler.ready_count() == 0);
        assert(!resumed);

        assert(waiter_scheduler.run_until_idle() == 1);
        assert(resumed);
        assert_task_code(waiter, vio_error_code::none);
    }

    {
        current_scheduler_scope scope(ref);
        async_semaphore semaphore(0, 1);
        bool first_resumed = false;
        bool second_resumed = false;

        auto first = acquire_code(semaphore, first_resumed);
        auto second = acquire_code(semaphore, second_resumed);

        assert(!first.is_ready());
        assert(second.is_ready());
        assert(semaphore.waiters() == 1);
        assert(!first_resumed);
        assert(second_resumed);
        assert_task_code(second, vio_error_code::resource_exhausted);

        semaphore.release();
        assert(scheduler.run_until_idle() == 1);
        assert(first_resumed);
        assert_task_code(first, vio_error_code::none);
    }

    {
        current_scheduler_scope scope(ref);
        async_semaphore semaphore(0, 1);
        bool resumed = false;

        {
            auto waiter = acquire_code(semaphore, resumed);
            assert(!waiter.is_ready());
            assert(semaphore.waiters() == 1);
        }

        assert(semaphore.waiters() == 0);
        semaphore.release();
        assert(scheduler.run_until_idle() == 0);
        assert(!resumed);
        assert(semaphore.permits() == 1);
    }

    {
        current_scheduler_scope scope(ref);
        async_semaphore semaphore(0, 1);
        bool resumed = false;

        {
            auto waiter = acquire_code(semaphore, resumed);
            assert(!waiter.is_ready());
            semaphore.release();
            assert(semaphore.waiters() == 0);
            assert(!resumed);
            assert(scheduler.ready_count() == 1);
        }

        assert(scheduler.run_until_idle() == 1);
        assert(!resumed);
    }

    {
        async_semaphore semaphore(1, 0);
        semaphore.release(0);
        assert(semaphore.permits() == 1);

        semaphore.release(std::numeric_limits<std::size_t>::max());
        assert(semaphore.permits() == std::numeric_limits<std::size_t>::max());
    }

    return 0;
}
