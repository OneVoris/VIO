#include <voris/io/shard.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    shard current(4);
    runtime_metrics initial = current.metrics();
    assert(initial.submitted_tasks == 0);
    assert(current.submit([] {}).has_value());
    runtime_metrics after_submit = current.metrics();
    assert(after_submit.submitted_tasks == 1);
    assert(after_submit.queue_depth == 1);
    assert(current.drain() == 1);
    runtime_metrics after_drain = current.metrics();
    assert(after_drain.submitted_tasks == 1);
    assert(after_drain.completed_tasks == 1);
    assert(after_drain.queue_depth == 0);

    return 0;
}
