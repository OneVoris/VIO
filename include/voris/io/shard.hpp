#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <thread>

#include <voris/io/backend_wakeup.hpp>
#include <voris/io/detail/mailbox.hpp>
#include <voris/io/error.hpp>
#include <voris/io/loop_budget.hpp>
#include <voris/io/runtime_metrics.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io {

class shard {
public:
    explicit shard(std::size_t queue_limit = 1024,
                   loop_budget budget = {},
                   runtime_metrics_config metrics_config = {});
    ~shard();

    shard(const shard&) = delete;
    shard& operator=(const shard&) = delete;

    [[nodiscard]] scheduler_ref scheduler() noexcept;
    [[nodiscard]] void_result enqueue(continuation next);
    [[nodiscard]] void_result submit(continuation next);
    [[nodiscard]] void_result submit_system(continuation next);
    [[nodiscard]] std::size_t drain();

    // Runs one nonblocking scheduler-loop iteration for manual drivers and deterministic tests.
    [[nodiscard]] io_result<std::size_t> run_one_loop_iteration();

    void start();
    void request_stop();
    void join();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::thread::id thread_id() const noexcept;
    [[nodiscard]] runtime_metrics metrics() const;
    [[nodiscard]] std::optional<vio_error> last_loop_error() const;

    // Observes worker parked state at the wakeup wait; not a loop budget or metrics API.
    template<class Rep, class Period>
    [[nodiscard]] bool wait_until_parked_for(
        const std::chrono::duration<Rep, Period>& timeout) const {
        return wakeup_.wait_for_waiter_count_for(1, timeout);
    }

private:
    void run_loop();
    [[nodiscard]] io_result<std::size_t> run_one_loop_iteration_under_current_scheduler();
    [[nodiscard]] bool run_one_queued_message();
    void run_queued_message(detail::mailbox::message message);

    detail::mailbox mailbox_;
    loop_budget budget_;
    runtime_metrics_config metrics_config_;
    backend_wakeup wakeup_;
    runtime_metrics metrics_;
    std::optional<vio_error> loop_error_;
    mutable std::mutex metrics_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
    std::thread::id thread_id_{};
};

} // namespace voris::io
