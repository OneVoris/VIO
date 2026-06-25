#include <voris/io/compute_executor.hpp>
#include <voris/io/shard.hpp>

#include "test_assert.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace {

template<class T>
void assert_resource_exhausted(const voris::io::io_result<T>& result) {
    assert(!result.has_value());
    assert(result.error().classification == voris::io::vio_error_code::resource_exhausted);
}

void scheduler_lag_is_recorded_under_real_shard_backlog() {
    using namespace voris::io;
    using namespace std::chrono_literals;

    shard runtime(4);
    std::mutex mutex;
    std::condition_variable first_started;
    std::condition_variable release_first;
    std::condition_variable second_finished;
    bool first_is_running = false;
    bool release = false;
    bool second_ran = false;

    runtime.start();
    assert(runtime.wait_until_parked_for(2s));

    assert(runtime.submit([&] {
                      {
                          std::lock_guard lock(mutex);
                          first_is_running = true;
                      }
                      first_started.notify_one();

                      std::unique_lock lock(mutex);
                      assert(release_first.wait_for(lock, 2s, [&] { return release; }));
                  })
               .has_value());

    {
        std::unique_lock lock(mutex);
        assert(first_started.wait_for(lock, 2s, [&] { return first_is_running; }));
    }

    assert(runtime.submit([&] {
                      {
                          std::lock_guard lock(mutex);
                          second_ran = true;
                      }
                      second_finished.notify_one();
                  })
               .has_value());

    {
        std::unique_lock lock(mutex);
        (void)second_finished.wait_for(lock, 35ms, [&] { return second_ran; });
        assert(!second_ran);
        release = true;
    }
    release_first.notify_one();

    {
        std::unique_lock lock(mutex);
        assert(second_finished.wait_for(lock, 2s, [&] { return second_ran; }));
    }

    runtime.request_stop();
    runtime.join();

    const runtime_metrics metrics = runtime.metrics();
    assert(metrics.completed_tasks == 2);
    assert(metrics.queue_depth == 0);
    assert(metrics.scheduler_lag >= 20ms);
}

void shard_accepts_work_after_overload_drains() {
    using namespace voris::io;

    shard limited(1);
    int completed = 0;
    bool rejected_ran = false;

    assert(limited.submit([&] { ++completed; }).has_value());
    assert_resource_exhausted(limited.submit([&] { rejected_ran = true; }));
    assert(limited.metrics().queue_depth == 1);

    assert(limited.drain() == 1);
    assert(completed == 1);
    assert(!rejected_ran);
    assert(limited.metrics().queue_depth == 0);

    assert(limited.submit([&] { ++completed; }).has_value());
    assert(limited.drain() == 1);
    assert(completed == 2);
    assert(!rejected_ran);
    assert(limited.metrics().completed_tasks == 2);
}

void executor_capacity_ceiling_rejects_without_leaking_state() {
    using namespace voris::io;

    compute_executor executor(1);
    int completed = 0;
    bool rejected_ran = false;

    assert(executor.submit([&] { ++completed; }).has_value());
    assert_resource_exhausted(executor.submit([&] { rejected_ran = true; }));
    assert(executor.queued() == 1);
    assert(executor.capacity_waiters() == 1);

    assert(executor.run_until_idle() == 1);
    assert(completed == 1);
    assert(!rejected_ran);
    assert(executor.queued() == 0);
    assert(executor.capacity_waiters() == 0);

    auto reservation = executor.try_reserve_capacity();
    assert(reservation.has_value());
    auto rejected_reservation = executor.try_reserve_capacity();
    assert_resource_exhausted(rejected_reservation);
    assert(executor.capacity_waiters() == 1);

    assert(executor.submit_reserved(std::move(*reservation), [&] { ++completed; }).has_value());
    assert(executor.capacity_waiters() == 0);
    assert(executor.run_until_idle() == 1);
    assert(completed == 2);
    assert(executor.queued() == 0);
}

} // namespace

int main() {
    scheduler_lag_is_recorded_under_real_shard_backlog();
    shard_accepts_work_after_overload_drains();
    executor_capacity_ceiling_rejects_without_leaking_state();

    return 0;
}
