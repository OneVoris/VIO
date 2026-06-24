#include <voris/io/shard.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    shard current(4);
    assert(current.metrics().submitted_tasks == 0);
    assert(current.submit([] {}).has_value());
    assert(current.metrics().submitted_tasks == 1);
    assert(current.metrics().queue_depth == 1);
    assert(current.drain() == 1);
    assert(current.metrics().completed_tasks == 1);
    assert(current.metrics().queue_depth == 0);

    return 0;
}
