#include <voris/io/cancellation.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    for (int i = 0; i != 1000; ++i) {
        cancellation_source source;
        cancellation_token token = source.token();
        int calls = 0;

        auto registration = token.register_callback([&](cancellation_reason reason) {
            ++calls;
            assert(reason == cancellation_reason::manual);
        });

        assert(source.request_cancellation(cancellation_reason::manual));
        assert(!source.request_cancellation(cancellation_reason::deadline));
        assert(calls == 1);
        assert(!registration.active());
        assert(token.reason() == cancellation_reason::manual);
    }

    for (int i = 0; i != 1000; ++i) {
        cancellation_source source;
        cancellation_token token = source.token();
        auto registration = token.register_callback([](cancellation_reason) {
            assert(false);
        });
        registration.unregister();
        assert(source.request_cancellation(cancellation_reason::backend_abort));
        assert(token.reason() == cancellation_reason::backend_abort);
    }

    return 0;
}
