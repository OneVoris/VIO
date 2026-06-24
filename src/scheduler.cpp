#include <voris/io/scheduler.hpp>

#include <cstddef>
#include <utility>

namespace voris::io {
namespace {

thread_local std::optional<scheduler_ref> tls_current_scheduler;

} // namespace

std::optional<scheduler_ref> current_scheduler() noexcept {
    return tls_current_scheduler;
}

io_result<scheduler_ref> require_current_scheduler() {
    if (tls_current_scheduler.has_value()) {
        return *tls_current_scheduler;
    }
    return std::unexpected(make_error(vio_error_code::invalid_state));
}

void set_current_scheduler_for_testing(std::optional<scheduler_ref> scheduler) noexcept {
    tls_current_scheduler = scheduler;
}

current_scheduler_scope::current_scheduler_scope(scheduler_ref scheduler) noexcept
    : previous_(tls_current_scheduler) {
    tls_current_scheduler = scheduler;
}

current_scheduler_scope::~current_scheduler_scope() {
    tls_current_scheduler = previous_;
}

void default_scheduler::enqueue(continuation next) {
    ready_.push_back(std::move(next));
}

bool default_scheduler::run_one() {
    if (ready_.empty()) {
        return false;
    }

    continuation next = std::move(ready_.front());
    ready_.pop_front();
    if (next) {
        current_scheduler_scope scope(scheduler_ref(*this));
        next();
    }
    return true;
}

std::size_t default_scheduler::run_until_idle() {
    std::size_t ran = 0;
    while (run_one()) {
        ++ran;
    }
    return ran;
}

std::size_t default_scheduler::ready_count() const noexcept {
    return ready_.size();
}

} // namespace voris::io
