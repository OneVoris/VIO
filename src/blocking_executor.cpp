#include <voris/io/blocking_executor.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace voris::io {
namespace {

struct blocking_executor_state {
    explicit blocking_executor_state(std::size_t queue_limit_value)
        : queue_limit(queue_limit_value) {}

    std::size_t queue_limit;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<continuation> queue;
    bool shutting_down{false};

    std::mutex worker_mutex;
    std::vector<std::thread> workers;
};

void request_shutdown(blocking_executor_state& state) noexcept {
    {
        std::lock_guard lock(state.mutex);
        state.shutting_down = true;
    }
    state.cv.notify_all();
}

void join_workers(blocking_executor_state& state) noexcept {
    std::vector<std::thread> workers;
    {
        std::lock_guard lock(state.worker_mutex);
        workers.swap(state.workers);
    }

    for (auto& worker : workers) {
        if (!worker.joinable()) {
            continue;
        }
        if (worker.get_id() == std::this_thread::get_id()) {
            std::terminate();
        }
        worker.join();
    }
}

void worker_loop(std::shared_ptr<blocking_executor_state> state) {
    for (;;) {
        continuation work;
        {
            std::unique_lock lock(state->mutex);
            state->cv.wait(lock, [&] {
                return state->shutting_down || !state->queue.empty();
            });

            if (state->queue.empty()) {
                if (state->shutting_down) {
                    return;
                }
                continue;
            }

            work = std::move(state->queue.front());
            state->queue.pop_front();
        }

        // User work runs after releasing executor locks.
        if (work) {
            work();
        }
    }
}

} // namespace

struct blocking_executor::state : blocking_executor_state {
    using blocking_executor_state::blocking_executor_state;
};

blocking_executor::blocking_executor(std::size_t worker_count, std::size_t queue_limit)
    : state_(std::make_shared<state>(queue_limit)) {
    // Normalize zero workers to one so accepted work can still make progress.
    const std::size_t normalized_worker_count = std::max<std::size_t>(worker_count, 1);

    try {
        std::lock_guard lock(state_->worker_mutex);
        state_->workers.reserve(normalized_worker_count);
        for (std::size_t i = 0; i != normalized_worker_count; ++i) {
            state_->workers.emplace_back([state = state_] { worker_loop(std::move(state)); });
        }
    } catch (...) {
        request_shutdown(*state_);
        join_workers(*state_);
        throw;
    }
}

blocking_executor::blocking_executor(std::size_t queue_limit)
    : blocking_executor(1, queue_limit) {}

blocking_executor::~blocking_executor() {
    shutdown();
}

void_result blocking_executor::submit(continuation work) {
    {
        std::lock_guard lock(state_->mutex);
        if (state_->shutting_down) {
            return std::unexpected(make_error(vio_error_code::closed,
                                              "blocking executor is shutting down"));
        }
        if (state_->queue_limit == 0 || state_->queue.size() >= state_->queue_limit) {
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "blocking executor queue is full"));
        }

        state_->queue.push_back(std::move(work));
    }
    state_->cv.notify_one();
    return {};
}

void blocking_executor::shutdown() noexcept {
    request_shutdown(*state_);
    join_workers(*state_);
}

bool blocking_executor::shutting_down() const {
    std::lock_guard lock(state_->mutex);
    return state_->shutting_down;
}

std::size_t blocking_executor::queued() const {
    std::lock_guard lock(state_->mutex);
    return state_->queue.size();
}

} // namespace voris::io
