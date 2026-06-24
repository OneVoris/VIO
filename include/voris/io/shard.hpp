#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <thread>

#include <voris/io/backend_wakeup.hpp>
#include <voris/io/detail/mailbox.hpp>
#include <voris/io/runtime_metrics.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io {

class shard {
public:
    explicit shard(std::size_t queue_limit = 1024);
    ~shard();

    shard(const shard&) = delete;
    shard& operator=(const shard&) = delete;

    [[nodiscard]] scheduler_ref scheduler() noexcept;
    [[nodiscard]] void_result enqueue(continuation next);
    [[nodiscard]] void_result submit(continuation next);
    [[nodiscard]] void_result submit_system(continuation next);
    [[nodiscard]] std::size_t drain();

    void start();
    void request_stop();
    void join();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::thread::id thread_id() const noexcept;
    [[nodiscard]] runtime_metrics metrics() const;
    template<class Rep, class Period>
    [[nodiscard]] bool wait_for_idle_wait_for(
        const std::chrono::duration<Rep, Period>& timeout) const {
        return wakeup_.wait_for_waiter_count_for(1, timeout);
    }

private:
    void run_loop();

    detail::mailbox mailbox_;
    backend_wakeup wakeup_;
    runtime_metrics metrics_;
    mutable std::mutex metrics_mutex_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::thread thread_;
    std::thread::id thread_id_{};
};

} // namespace voris::io
