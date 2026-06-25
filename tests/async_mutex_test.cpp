#include <voris/io/async_mutex.hpp>
#include <voris/io/task.hpp>

#include "test_assert.hpp"
#include <coroutine>
#include <deque>
#include <vector>

namespace {

template<class Result>
void assert_error(const Result& result, voris::io::vio_error_code code) {
    assert(!result.has_value());
    assert(result.error().classification == code);
}

voris::io::task<voris::io::vio_error_code> lock_code(voris::io::async_mutex& mutex,
                                                     bool& resumed) {
    auto locked = co_await mutex.lock();
    resumed = true;
    if (locked.has_value()) {
        co_return voris::io::vio_error_code::none;
    }
    co_return locked.error().classification;
}

voris::io::task<voris::io::vio_error_code> lock_code(voris::io::async_mutex& mutex,
                                                     bool& resumed,
                                                     std::vector<int>& order,
                                                     int id) {
    auto locked = co_await mutex.lock();
    resumed = true;
    if (locked.has_value()) {
        order.push_back(id);
        co_return voris::io::vio_error_code::none;
    }
    co_return locked.error().classification;
}

voris::io::task<voris::io::vio_error_code> lock_code_on_scheduler(
    voris::io::async_mutex& mutex,
    bool& resumed,
    voris::io::scheduler_ref expected) {
    auto locked = co_await mutex.lock();
    resumed = true;
    if (!locked.has_value()) {
        co_return locked.error().classification;
    }

    auto current = voris::io::require_current_scheduler();
    if (!current.has_value() || current->identity() != expected.identity()) {
        co_return voris::io::vio_error_code::invalid_state;
    }
    co_return voris::io::vio_error_code::none;
}

void assert_task_code(voris::io::task<voris::io::vio_error_code>& task,
                      voris::io::vio_error_code expected) {
    auto result = std::move(task).take_result();
    assert(result.has_value());
    assert(*result == expected);
}

class recording_queue_scheduler {
public:
    [[nodiscard]] voris::io::void_result submit(voris::io::continuation next) {
        ++user_submits_;
        ready_.push_back(std::move(next));
        return {};
    }

    [[nodiscard]] voris::io::void_result submit_system(voris::io::continuation next) {
        ++system_submits_;
        ready_.push_back(std::move(next));
        return {};
    }

    [[nodiscard]] bool run_one() {
        if (ready_.empty()) {
            return false;
        }

        auto next = std::move(ready_.front());
        ready_.pop_front();
        if (next) {
            voris::io::current_scheduler_scope scope(voris::io::scheduler_ref(*this));
            next();
        }
        return true;
    }

    [[nodiscard]] std::size_t run_until_idle() {
        std::size_t ran = 0;
        while (run_one()) {
            ++ran;
        }
        return ran;
    }

    [[nodiscard]] std::size_t ready_count() const noexcept {
        return ready_.size();
    }

    [[nodiscard]] std::size_t system_submits() const noexcept {
        return system_submits_;
    }

    [[nodiscard]] std::size_t user_submits() const noexcept {
        return user_submits_;
    }

private:
    std::deque<voris::io::continuation> ready_;
    std::size_t user_submits_{0};
    std::size_t system_submits_{0};
};

} // namespace

int main() {
    using namespace voris::io;

    set_current_scheduler_for_testing(std::nullopt);

    {
        async_mutex mutex;
        assert(mutex.max_waiters() == 1);
        assert(mutex.try_lock().has_value());
        assert(mutex.locked());

        auto blocked = mutex.try_lock();
        assert_error(blocked, vio_error_code::operation_in_progress);
        assert(mutex.waiters() == 0);

        mutex.unlock();
        assert(!mutex.locked());
    }

    {
        async_mutex mutex(1);
        assert(mutex.try_lock().has_value());

        auto missing_scheduler = mutex.lock();
        assert(!missing_scheduler.await_ready());
        assert(!missing_scheduler.await_suspend(std::noop_coroutine()));
        assert_error(missing_scheduler.await_resume(), vio_error_code::invalid_state);
        assert(mutex.waiters() == 0);
        assert(mutex.locked());

        mutex.unlock();
    }

    {
        default_scheduler scheduler;
        scheduler_ref ref(scheduler);
        current_scheduler_scope scope(ref);
        async_mutex mutex;
        bool resumed = false;

        auto waiter = lock_code(mutex, resumed);

        assert(resumed);
        assert(waiter.is_ready());
        assert(mutex.locked());
        assert(mutex.waiters() == 0);
        assert_task_code(waiter, vio_error_code::none);

        mutex.unlock();
        assert(!mutex.locked());
    }

    {
        recording_queue_scheduler scheduler;
        scheduler_ref ref(scheduler);
        current_scheduler_scope scope(ref);
        async_mutex mutex(1);
        bool resumed = false;

        assert(mutex.try_lock().has_value());
        auto waiter = lock_code(mutex, resumed);
        assert(!waiter.is_ready());
        assert(!resumed);
        assert(mutex.waiters() == 1);

        mutex.unlock();
        assert(!resumed);
        assert(!waiter.is_ready());
        assert(mutex.locked());
        assert(mutex.waiters() == 0);
        assert(scheduler.user_submits() == 0);
        assert(scheduler.system_submits() == 1);
        assert(scheduler.ready_count() == 1);

        assert(scheduler.run_until_idle() == 1);
        assert(resumed);
        assert(waiter.is_ready());
        assert(mutex.locked());
        assert_task_code(waiter, vio_error_code::none);

        mutex.unlock();
        assert(!mutex.locked());
    }

    {
        default_scheduler scheduler;
        scheduler_ref ref(scheduler);
        current_scheduler_scope scope(ref);
        async_mutex mutex(2);
        std::vector<int> order;
        bool first_resumed = false;
        bool second_resumed = false;

        assert(mutex.try_lock().has_value());
        auto first = lock_code(mutex, first_resumed, order, 1);
        auto second = lock_code(mutex, second_resumed, order, 2);

        assert(mutex.waiters() == 2);
        mutex.unlock();
        assert(mutex.locked());
        assert(mutex.waiters() == 1);
        assert(!first_resumed);
        assert(!second_resumed);

        assert(scheduler.run_one());
        assert(first_resumed);
        assert(!second_resumed);
        assert((order == std::vector<int>{1}));
        assert(mutex.locked());
        assert_task_code(first, vio_error_code::none);

        mutex.unlock();
        assert(mutex.locked());
        assert(mutex.waiters() == 0);
        assert(scheduler.run_one());
        assert(second_resumed);
        assert((order == std::vector<int>{1, 2}));
        assert(mutex.locked());
        assert_task_code(second, vio_error_code::none);

        mutex.unlock();
        assert(!mutex.locked());
    }

    {
        default_scheduler scheduler;
        scheduler_ref ref(scheduler);
        current_scheduler_scope scope(ref);
        async_mutex mutex(1);
        bool first_resumed = false;
        bool second_resumed = false;

        assert(mutex.try_lock().has_value());
        auto first = lock_code(mutex, first_resumed);
        auto second = lock_code(mutex, second_resumed);

        assert(!first.is_ready());
        assert(second.is_ready());
        assert(mutex.waiters() == 1);
        assert(!first_resumed);
        assert(second_resumed);
        assert_task_code(second, vio_error_code::resource_exhausted);

        mutex.unlock();
        assert(scheduler.run_until_idle() == 1);
        assert(first_resumed);
        assert_task_code(first, vio_error_code::none);

        mutex.unlock();
    }

    {
        default_scheduler scheduler;
        scheduler_ref ref(scheduler);
        current_scheduler_scope scope(ref);
        async_mutex mutex(0);
        bool resumed = false;

        assert(mutex.max_waiters() == 0);
        assert(mutex.try_lock().has_value());
        auto waiter = lock_code(mutex, resumed);

        assert(waiter.is_ready());
        assert(resumed);
        assert(mutex.waiters() == 0);
        assert_task_code(waiter, vio_error_code::resource_exhausted);

        mutex.unlock();
    }

    {
        default_scheduler waiter_scheduler;
        default_scheduler unlock_scheduler;
        scheduler_ref waiter_ref(waiter_scheduler);
        scheduler_ref unlock_ref(unlock_scheduler);
        async_mutex mutex(1);
        bool resumed = false;

        assert(mutex.try_lock().has_value());
        task<vio_error_code> waiter;
        {
            current_scheduler_scope scope(waiter_ref);
            waiter = lock_code_on_scheduler(mutex, resumed, waiter_ref);
        }

        assert(!waiter.is_ready());
        assert(mutex.waiters() == 1);

        {
            current_scheduler_scope scope(unlock_ref);
            mutex.unlock();
        }

        assert(waiter_scheduler.ready_count() == 1);
        assert(unlock_scheduler.ready_count() == 0);
        assert(!resumed);

        assert(waiter_scheduler.run_until_idle() == 1);
        assert(resumed);
        assert_task_code(waiter, vio_error_code::none);

        mutex.unlock();
    }

    {
        default_scheduler scheduler;
        scheduler_ref ref(scheduler);
        current_scheduler_scope scope(ref);
        async_mutex mutex(2);
        bool first_resumed = false;
        bool second_resumed = false;
        task<vio_error_code> second;

        assert(mutex.try_lock().has_value());
        {
            auto first = lock_code(mutex, first_resumed);
            second = lock_code(mutex, second_resumed);
            assert(!first.is_ready());
            assert(!second.is_ready());
            assert(mutex.waiters() == 2);

            mutex.unlock();
            assert(mutex.locked());
            assert(mutex.waiters() == 1);
            assert(!first_resumed);
            assert(!second_resumed);
        }

        assert(!first_resumed);
        assert(mutex.locked());
        assert(mutex.waiters() == 0);
        assert(scheduler.run_until_idle() >= 1);
        assert(!first_resumed);
        assert(second_resumed);
        assert(mutex.locked());
        assert_task_code(second, vio_error_code::none);

        mutex.unlock();
        assert(!mutex.locked());
    }

    {
        default_scheduler scheduler;
        scheduler_ref ref(scheduler);
        current_scheduler_scope scope(ref);
        async_mutex mutex(1);
        bool resumed = false;

        assert(mutex.try_lock().has_value());
        {
            auto waiter = lock_code(mutex, resumed);
            assert(!waiter.is_ready());
            mutex.unlock();
            assert(mutex.locked());
            assert(!resumed);
        }

        assert(!mutex.locked());
        assert(scheduler.run_until_idle() == 1);
        assert(!resumed);
    }

    {
        default_scheduler scheduler;
        scheduler_ref ref(scheduler);
        task<vio_error_code> waiter;
        bool resumed = false;

        {
            async_mutex mutex(1);
            assert(mutex.try_lock().has_value());
            {
                current_scheduler_scope scope(ref);
                waiter = lock_code(mutex, resumed);
            }
            assert(!waiter.is_ready());
            assert(mutex.waiters() == 1);
        }

        assert(!resumed);
        assert(scheduler.ready_count() == 1);
        assert(scheduler.run_until_idle() == 1);
        assert(resumed);
        assert(waiter.is_ready());
        assert_task_code(waiter, vio_error_code::invalid_state);
    }

    return 0;
}
