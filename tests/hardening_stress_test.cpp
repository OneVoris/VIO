#include <voris/io/cancellation.hpp>
#include <voris/io/runtime.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    for (int i = 0; i != 1000; ++i) {
        cancellation_source source;
        auto token = source.token();
        assert(source.request_cancellation(cancellation_reason::manual));
        assert(!source.request_cancellation(cancellation_reason::runtime_shutdown));
        assert(token.reason() == cancellation_reason::manual);
    }

    runtime_options options;
    options.shard_count = 1;
    options.queue_limit = 8;
    auto created = runtime::create(options);
    assert(created.has_value());
    created->start();
    created->request_stop();
    created->join();

    return 0;
}
