#include <voris/io/backend_wakeup.hpp>
#include <voris/io/loop_budget.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    backend_wakeup wakeup;
    wakeup.wake();
    wakeup.wake();
    assert(wakeup.wake_count() == 2);
    wakeup.consume();
    assert(wakeup.wake_count() == 1);

    loop_budget budget;
    assert(budget.task_budget > 0);
    assert(budget.completion_budget > 0);
    assert(budget.timer_budget > 0);

    return 0;
}
