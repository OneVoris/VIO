#include <voris/io/cancellation.hpp>

#include "test_assert.hpp"
#include <atomic>
#include <barrier>
#include <thread>

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

    for (int i = 0; i != 1000; ++i) {
        cancellation_source source;
        cancellation_token token = source.token();
        std::atomic<int> calls{0};
        std::barrier start(3);

        auto registration = token.register_callback([&](cancellation_reason reason) {
            assert(reason == cancellation_reason::manual);
            assert(!detail::cancellation_internal_lock_held_for_testing(token));
            calls.fetch_add(1);
        });

        std::thread canceller([&] {
            start.arrive_and_wait();
            (void)source.request_cancellation(cancellation_reason::manual);
        });
        std::thread unregisterer([&] {
            start.arrive_and_wait();
            registration.unregister();
        });

        start.arrive_and_wait();
        canceller.join();
        unregisterer.join();

        assert(source.cancellation_requested());
        assert(source.reason() == cancellation_reason::manual);
        assert(!registration.active());
        assert(calls.load() <= 1);
    }

    return 0;
}
