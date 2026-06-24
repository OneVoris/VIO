#include <voris/io/runtime.hpp>
#include <voris/io/submit.hpp>

#include "test_assert.hpp"
#include <memory>

int main() {
    using namespace voris::io;

    runtime_options options;
    options.shard_count = 2;
    options.queue_limit = 2;
    auto created = runtime::create(options);
    assert(created.has_value());

    auto& first = created->get_shard(0);
    auto& second = created->get_shard(1);

    int value = 0;
    assert(submit_to(first, [&] {
        value += 1;
        assert(submit_to(second, [&] { value += 2; }).has_value());
    }).has_value());

    assert(first.drain() == 1);
    assert(second.drain() == 1);
    assert(value == 3);

    assert(first.submit([] {}).has_value());
    assert(first.submit([] {}).has_value());
    assert(!first.submit([] {}).has_value());

    return 0;
}
