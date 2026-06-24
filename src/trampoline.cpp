#include <voris/io/trampoline.hpp>

#include <deque>
#include <utility>

namespace voris::io {
namespace {

struct trampoline_state {
    bool draining{false};
    std::deque<continuation> deferred;
};

thread_local trampoline_state state;

void run_continuation(continuation next) {
    if (!next) {
        return;
    }

    if (state.draining) {
        state.deferred.push_back(std::move(next));
        return;
    }

    state.draining = true;
    next();
    while (!state.deferred.empty()) {
        continuation deferred = std::move(state.deferred.front());
        state.deferred.pop_front();
        if (deferred) {
            deferred();
        }
    }
    state.draining = false;
}

} // namespace

void_result trampoline::schedule(scheduler_ref scheduler, continuation next) {
    return scheduler.schedule([next = std::move(next)] mutable {
        run_continuation(std::move(next));
    });
}

void_result trampoline::schedule_system(scheduler_ref scheduler, continuation next) {
    return scheduler.schedule_system([next = std::move(next)] mutable {
        run_continuation(std::move(next));
    });
}

} // namespace voris::io
