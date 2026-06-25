#include <voris/io/shard.hpp>

#include <utility>

namespace voris::io {

shard::shard(std::size_t queue_limit, loop_budget budget)
    : mailbox_(queue_limit),
      budget_(budget) {}

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
    if (result.has_value()) {
        wakeup_.wake();
    }
    return result;
}

void_result shard::submit_system(continuation next) {
    auto result = mailbox_.submit_system(std::move(next));
    {
        std::lock_guard lock(metrics_mutex_);
        metrics_.queue_depth = mailbox_.size();
        if (result.has_value()) {
            ++metrics_.submitted_tasks;
        }
    }
    if (result.has_value()) {
        wakeup_.wake();
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

io_result<std::size_t> shard::run_one_loop_iteration() {
    auto slice = loop_budget_slice::create(budget_);
    if (!slice.has_value()) {
        return std::unexpected(slice.error());
    }

    const std::size_t ran = mailbox_.run_for_budget(*slice);
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
    {
        std::lock_guard lock(metrics_mutex_);
        loop_error_.reset();
    }
    stop_requested_ = false;
    thread_ = std::thread([this] { run_loop(); });
}

void shard::request_stop() {
    stop_requested_ = true;
    wakeup_.wake();
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

std::optional<vio_error> shard::last_loop_error() const {
    std::lock_guard lock(metrics_mutex_);
    return loop_error_;
}

void shard::run_loop() {
    thread_id_ = std::this_thread::get_id();
    current_scheduler_scope scope(scheduler());
    bool drain_on_exit = true;
    while (!stop_requested_) {
        auto ran = run_one_loop_iteration();
        if (!ran.has_value()) {
            {
                std::lock_guard lock(metrics_mutex_);
                loop_error_ = ran.error();
                metrics_.queue_depth = mailbox_.size();
            }
            stop_requested_ = true;
            drain_on_exit = false;
            break;
        }
        if (*ran == 0) {
            wakeup_.wait();
            (void)wakeup_.consume_all();
        }
    }
    if (drain_on_exit) {
        (void)drain();
    }
}

} // namespace voris::io
