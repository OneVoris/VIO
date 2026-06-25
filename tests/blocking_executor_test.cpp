#include <voris/io/blocking_executor.hpp>

#include "test_assert.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <thread>
#include <type_traits>

namespace {

using namespace std::chrono_literals;

class gate {
public:
    void open() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

    void wait_until_open() {
        assert(wait_until_open_for(2s));
    }

    template<class Rep, class Period>
    [[nodiscard]] bool wait_until_open_for(std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] { return open_; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_{false};
};

class manual_gate {
public:
    void arrive() {
        {
            std::lock_guard lock(mutex_);
            arrived_ = true;
        }
        cv_.notify_all();
    }

    void wait_for_arrival() {
        std::unique_lock lock(mutex_);
        assert(cv_.wait_for(lock, 2s, [&] { return arrived_; }));
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

    void wait_for_release() {
        std::unique_lock lock(mutex_);
        assert(cv_.wait_for(lock, 2s, [&] { return released_; }));
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool arrived_{false};
    bool released_{false};
};

void test_full_queue_returns_resource_error_while_worker_is_blocked() {
    using namespace voris::io;

    blocking_executor executor(1, 1);
    manual_gate blocker;
    gate queued_work_ran;

    assert(executor
               .submit([&] {
                   blocker.arrive();
                   blocker.wait_for_release();
               })
               .has_value());
    blocker.wait_for_arrival();

    assert(executor.submit([&] { queued_work_ran.open(); }).has_value());
    auto full = executor.submit([] {});
    assert(!full.has_value());
    assert(full.error().classification == vio_error_code::resource_exhausted);
    assert(executor.queued() == 1);

    blocker.release();
    queued_work_ran.wait_until_open();
    executor.shutdown();
}

void test_work_runs_on_background_worker_thread() {
    using namespace voris::io;

    blocking_executor executor(1, 1);
    const auto submitter_thread = std::this_thread::get_id();
    std::thread::id worker_thread;
    gate work_ran;

    assert(executor
               .submit([&] {
                   worker_thread = std::this_thread::get_id();
                   work_ran.open();
               })
               .has_value());

    work_ran.wait_until_open();
    assert(worker_thread != submitter_thread);
    executor.shutdown();
}

void test_shutdown_drains_queued_work_and_rejects_new_submit() {
    using namespace voris::io;

    blocking_executor executor(1, 2);
    manual_gate blocker;
    bool queued_ran = false;

    assert(executor
               .submit([&] {
                   blocker.arrive();
                   blocker.wait_for_release();
               })
               .has_value());
    blocker.wait_for_arrival();
    assert(executor.submit([&] { queued_ran = true; }).has_value());

    blocker.release();
    executor.shutdown();

    assert(executor.shutting_down());
    assert(queued_ran);
    assert(executor.queued() == 0);

    auto rejected = executor.submit([] {});
    assert(!rejected.has_value());
    assert(rejected.error().classification == vio_error_code::closed);
}

bool test_concurrent_shutdown_callers_all_wait_for_join_barrier() {
    using namespace voris::io;

    blocking_executor executor(1, 1);
    manual_gate blocker;
    std::mutex mutex;
    std::condition_variable cv;
    bool first_entered = false;
    bool first_returned = false;
    bool second_returned = false;

    assert(executor
               .submit([&] {
                   blocker.arrive();
                   blocker.wait_for_release();
               })
               .has_value());
    blocker.wait_for_arrival();

    std::thread first_shutdown([&] {
        {
            std::lock_guard lock(mutex);
            first_entered = true;
        }
        cv.notify_all();
        executor.shutdown();
        {
            std::lock_guard lock(mutex);
            first_returned = true;
        }
        cv.notify_all();
    });

    {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock, 2s, [&] { return first_entered; }));
        assert(!cv.wait_for(lock, 50ms, [&] { return first_returned; }));
    }

    std::thread second_shutdown([&] {
        executor.shutdown();
        {
            std::lock_guard lock(mutex);
            second_returned = true;
        }
        cv.notify_all();
    });

    {
        std::unique_lock lock(mutex);
        const bool returned_before_release =
            cv.wait_for(lock, 50ms, [&] { return first_returned || second_returned; });
        blocker.release();
        lock.unlock();
        first_shutdown.join();
        second_shutdown.join();
        if (returned_before_release) {
            return false;
        }
    }
    assert(first_returned);
    assert(second_returned);
    return true;
}

bool test_throwing_user_work_keeps_worker_alive_for_later_work() {
    using namespace voris::io;

    blocking_executor executor(1, 2);
    gate later_work_ran;

    assert(executor.submit([] { throw 7; }).has_value());
    assert(executor.submit([&] { later_work_ran.open(); }).has_value());

    const bool later_ran = later_work_ran.wait_until_open_for(2s);
    executor.shutdown();
    return later_ran;
}

bool test_worker_thread_shutdown_returns_and_external_shutdown_joins() {
    using namespace voris::io;

    blocking_executor executor(1, 1);
    gate worker_shutdown_returned;

    assert(executor
               .submit([&] {
                   executor.shutdown();
                   worker_shutdown_returned.open();
               })
               .has_value());

    const bool returned_from_worker_shutdown = worker_shutdown_returned.wait_until_open_for(2s);
    executor.shutdown();
    return returned_from_worker_shutdown;
}

void test_destructor_joins_workers() {
    using namespace voris::io;

    manual_gate blocker;
    gate destructor_returned;
    std::mutex destroy_mutex;
    std::condition_variable destroy_cv;
    bool delete_started = false;
    bool delete_returned = false;
    auto executor = new blocking_executor(1, 1);

    assert(executor
               ->submit([&] {
                   blocker.arrive();
                   blocker.wait_for_release();
               })
               .has_value());
    blocker.wait_for_arrival();

    std::thread destroyer([&] {
        {
            std::lock_guard lock(destroy_mutex);
            delete_started = true;
        }
        destroy_cv.notify_all();
        delete executor;
        {
            std::lock_guard lock(destroy_mutex);
            delete_returned = true;
        }
        destroy_cv.notify_all();
        destructor_returned.open();
    });

    {
        std::unique_lock lock(destroy_mutex);
        assert(destroy_cv.wait_for(lock, 2s, [&] { return delete_started; }));
        assert(!destroy_cv.wait_for(lock, 50ms, [&] { return delete_returned; }));
    }

    blocker.release();
    destructor_returned.wait_until_open();
    destroyer.join();
}

void test_zero_capacity_rejects_submit() {
    using namespace voris::io;

    blocking_executor executor(1, 0);
    auto rejected = executor.submit([] {});
    assert(!rejected.has_value());
    assert(rejected.error().classification == vio_error_code::resource_exhausted);
    assert(executor.queued() == 0);
    executor.shutdown();
}

void test_zero_worker_count_is_normalized_to_one_worker() {
    using namespace voris::io;

    blocking_executor executor(0, 1);
    gate work_ran;

    assert(executor.submit([&] { work_ran.open(); }).has_value());
    work_ran.wait_until_open();
    executor.shutdown();
}

void test_user_work_can_submit_without_internal_mutex_deadlock() {
    using namespace voris::io;

    blocking_executor executor(1, 2);
    gate nested_ran;

    assert(executor
               .submit([&] {
                   assert(executor.queued() == 0);
                   assert(executor.submit([&] { nested_ran.open(); }).has_value());
               })
               .has_value());

    nested_ran.wait_until_open();
    executor.shutdown();
}

} // namespace

int main() {
    using namespace voris::io;

    std::set_terminate([] { std::_Exit(99); });

    static_assert(!std::is_copy_constructible_v<blocking_executor>);
    static_assert(!std::is_copy_assignable_v<blocking_executor>);
    static_assert(!std::is_move_constructible_v<blocking_executor>);
    static_assert(!std::is_move_assignable_v<blocking_executor>);

    test_full_queue_returns_resource_error_while_worker_is_blocked();
    test_work_runs_on_background_worker_thread();
    test_shutdown_drains_queued_work_and_rejects_new_submit();
    if (!test_concurrent_shutdown_callers_all_wait_for_join_barrier()) {
        return 1;
    }
    if (!test_throwing_user_work_keeps_worker_alive_for_later_work()) {
        return 2;
    }
    if (!test_worker_thread_shutdown_returns_and_external_shutdown_joins()) {
        return 3;
    }
    test_destructor_joins_workers();
    test_zero_capacity_rejects_submit();
    test_zero_worker_count_is_normalized_to_one_worker();
    test_user_work_can_submit_without_internal_mutex_deadlock();

    return 0;
}
