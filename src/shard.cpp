#include <voris/io/shard.hpp>

#include <chrono>
#include <utility>

namespace voris::io {

shard::shard(std::size_t queue_limit)
    : mailbox_(queue_limit) {}

shard::~shard() {
    request_stop();
    join();
}

scheduler_ref shard::scheduler() noexcept {
    return scheduler_ref(*this);
}

void_result shard::enqueue(continuation next) {
    return submit(std::move(next));
}

void_result shard::submit(continuation next) {
    auto result = mailbox_.submit(std::move(next));
    {
        std::lock_guard lock(metrics_mutex_);
        metrics_.queue_depth = mailbox_.size();
        if (result.has_value()) {
            ++metrics_.submitted_tasks;
        }
    }
    return result;
}

std::size_t shard::drain() {
    const std::size_t ran = mailbox_.run_until_idle();
    {
        std::lock_guard lock(metrics_mutex_);
        metrics_.completed_tasks += ran;
        metrics_.queue_depth = mailbox_.size();
    }
    return ran;
}

void shard::start() {
    if (running_.exchange(true)) {
        return;
    }
    stop_requested_ = false;
    thread_ = std::thread([this] { run_loop(); });
}

void shard::request_stop() {
    stop_requested_ = true;
}

void shard::join() {
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

bool shard::running() const noexcept {
    return running_;
}

std::thread::id shard::thread_id() const noexcept {
    return thread_id_;
}

runtime_metrics shard::metrics() const {
    std::lock_guard lock(metrics_mutex_);
    return metrics_;
}

void shard::run_loop() {
    thread_id_ = std::this_thread::get_id();
    current_scheduler_scope scope(scheduler());
    while (!stop_requested_) {
        if (drain() == 0) {
            std::this_thread::yield();
        }
    }
    (void)drain();
}

} // namespace voris::io
