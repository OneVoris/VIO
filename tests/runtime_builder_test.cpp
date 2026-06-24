#include <voris/io/runtime.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    runtime_options invalid;
    invalid.shard_count = 0;
    auto invalid_runtime = runtime::create(invalid);
    assert(!invalid_runtime.has_value());
    assert(invalid_runtime.error().classification == vio_error_code::invalid_state);

    runtime_options options;
    options.shard_count = 2;
    options.queue_limit = 4;
    auto created = runtime::create(options);
    assert(created.has_value());
    assert(created->shard_count() == 2);
    assert(created->options().queue_limit == 4);

    return 0;
}
