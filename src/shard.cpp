#include <voris/io/shard.hpp>

#include <chrono>
#include <utility>

namespace voris::io {

shard::shard(std::size_t queue_limit, loop_budget budget, runtime_metrics_config metrics_config)
    : mailbox_(queue_limit),
      budget_(budget),
      metrics_config_(metrics_config) {}

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
    if (!next) {
        return std::unexpected(make_error(vio_error_code::invalid_state));
    }
    if (stop_requested_) {
        return std::unexpected(make_error(vio_error_code::invalid_state, "shard is stopped"));
    }

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
    if (!next) {
        return std::unexpected(make_error(vio_error_code::invalid_state));
    }
    if (stop_requested_) {
        return std::unexpected(make_error(vio_error_code::invalid_state, "shard is stopped"));
    }

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
    std::size_t ran = 0;
    while (run_one_queued_message()) {
        ++ran;
    }
    return ran;
}

io_result<std::size_t> shard::run_one_loop_iteration() {
    current_scheduler_scope scope(scheduler());
    return run_one_loop_iteration_under_current_scheduler();
}

io_result<std::size_t> shard::run_one_loop_iteration_under_current_scheduler() {
    auto slice = loop_budget_slice::create(budget_);
    if (!slice.has_value()) {
        return std::unexpected(slice.error());
    }

    std::size_t ran = 0;
    while (slice->remaining_tasks() > 0) {
        auto next = mailbox_.pop_next();
        if (!next.has_value()) {
            break;
        }
        (void)slice->consume_task();
        run_queued_message(std::move(*next));
        ++ran;
    }
    return ran;
}

bool shard::run_one_queued_message() {
    auto next = mailbox_.pop_next();
    if (!next.has_value()) {
        std::lock_guard lock(metrics_mutex_);
        metrics_.queue_depth = mailbox_.size();
        return false;
    }

    run_queued_message(std::move(*next));
    return true;
}

void shard::run_queued_message(detail::mailbox::message message) {
    const auto run_started = detail::mailbox::clock::now();
    const auto lag =
        std::chrono::duration_cast<std::chrono::nanoseconds>(run_started - message.enqueued_at);

    {
        std::lock_guard lock(metrics_mutex_);
        metrics_.queue_depth = mailbox_.size();
        if (metrics_.scheduler_lag < lag) {
            metrics_.scheduler_lag = lag;
        }
    }

    std::chrono::nanoseconds task_duration{};
    if (message.work) {
        const auto task_started = detail::mailbox::clock::now();
        message.work();
        task_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            detail::mailbox::clock::now() - task_started);
    }

    {
        std::lock_guard lock(metrics_mutex_);
        ++metrics_.completed_tasks;
        if (task_duration > metrics_config_.long_task_threshold) {
            ++metrics_.long_tasks;
        }
    }
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
        auto ran = run_one_loop_iteration_under_current_scheduler();
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
