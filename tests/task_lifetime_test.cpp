#include <voris/io/task.hpp>

#include "test_assert.hpp"
#include <type_traits>

namespace {

struct post_once {
    voris::io::scheduler_ref scheduler;

    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }

    void await_suspend(std::coroutine_handle<> continuation) const {
        (void)scheduler.schedule([continuation] { continuation.resume(); });
    }

    void await_resume() const noexcept {}
};

voris::io::task<void> suspended_lifetime_task(voris::io::scheduler_ref scheduler,
                                              int& entered,
                                              int& destroyed) {
    struct frame_guard {
        int& destroyed;

        ~frame_guard() {
            ++destroyed;
        }
    };

    frame_guard guard{destroyed};
    ++entered;
    co_await post_once{scheduler};
    co_return;
}

} // namespace

int main() {
    using namespace voris::io;

    static_assert(!std::is_copy_constructible_v<task<int>>);
    static_assert(!std::is_copy_assignable_v<task<int>>);
    static_assert(std::is_move_constructible_v<task<int>>);
    static_assert(std::is_move_assignable_v<task<int>>);

    static_assert(!std::is_copy_constructible_v<task<void>>);
    static_assert(!std::is_copy_assignable_v<task<void>>);
    static_assert(std::is_move_constructible_v<task<void>>);
    static_assert(std::is_move_assignable_v<task<void>>);

    default_scheduler scheduler;
    scheduler_ref ref(scheduler);
    int entered = 0;
    int destroyed = 0;
    {
        current_scheduler_scope scope(ref);
        auto abandoned = suspended_lifetime_task(ref, entered, destroyed);
        assert(entered == 1);
        assert(destroyed == 0);
        assert(!abandoned.is_ready());
    }
    assert(destroyed == 1);

    return 0;
}
