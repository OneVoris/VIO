#include <voris/io/cancellation.hpp>

#include "test_assert.hpp"
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

int main() {
    using namespace voris::io;

    static_assert(std::is_copy_constructible_v<cancellation_token>);
    static_assert(std::is_copy_constructible_v<cancellation_source>);
    static_assert(!std::is_copy_constructible_v<cancellation_registration>);
    static_assert(std::is_move_constructible_v<cancellation_registration>);

    assert(to_string(cancellation_reason::manual) == std::string_view("manual"));
    assert(to_string(cancellation_reason::deadline) == std::string_view("deadline"));
    assert(to_string(cancellation_reason::scope_shutdown) == std::string_view("scope_shutdown"));
    assert(to_string(cancellation_reason::runtime_shutdown) == std::string_view("runtime_shutdown"));
    assert(to_string(cancellation_reason::backend_abort) == std::string_view("backend_abort"));

    {
        cancellation_source source;
        cancellation_token token = source.token();

        assert(token.can_be_cancelled());
        assert(!token.cancellation_requested());
        assert(!token.reason().has_value());

        assert(source.request_cancellation(cancellation_reason::manual));
        assert(token.cancellation_requested());
        assert(token.reason() == cancellation_reason::manual);

        assert(!source.request_cancellation(cancellation_reason::deadline));
        assert(token.reason() == cancellation_reason::manual);
        assert(source.reason() == cancellation_reason::manual);
    }

    {
        cancellation_source source;
        cancellation_source copied_source = source;
        cancellation_token token = source.token();
        cancellation_token copied_token = token;
        cancellation_token moved_token = std::move(copied_token);

        assert(moved_token.can_be_cancelled());
        assert(!copied_token.can_be_cancelled());

        assert(copied_source.request_cancellation(cancellation_reason::runtime_shutdown));
        assert(token.cancellation_requested());
        assert(moved_token.cancellation_requested());
        assert(moved_token.reason() == cancellation_reason::runtime_shutdown);
    }

    {
        cancellation_source source;
        cancellation_token token = source.token();
        int calls = 0;
        std::optional<cancellation_reason> seen;

        auto registration = token.register_callback([&](cancellation_reason reason) {
            ++calls;
            seen = reason;
        });

        assert(registration.active());
        assert(source.request_cancellation(cancellation_reason::deadline));
        assert(calls == 1);
        assert(seen == cancellation_reason::deadline);
        assert(!registration.active());

        assert(!source.request_cancellation(cancellation_reason::backend_abort));
        registration.unregister();
        assert(calls == 1);
    }

    {
        cancellation_source source;
        cancellation_token token = source.token();
        int calls = 0;

        auto registration = token.register_callback([&](cancellation_reason) {
            ++calls;
        });
        assert(registration.active());
        registration.unregister();
        assert(!registration.active());

        assert(source.request_cancellation(cancellation_reason::manual));
        assert(calls == 0);
    }

    {
        cancellation_source source;
        cancellation_token token = source.token();
        assert(source.request_cancellation(cancellation_reason::backend_abort));

        int calls = 0;
        std::optional<cancellation_reason> seen;
        auto registration = token.register_callback([&](cancellation_reason reason) {
            ++calls;
            seen = reason;
            assert(!detail::cancellation_internal_lock_held_for_testing(token));
        });

        assert(calls == 1);
        assert(seen == cancellation_reason::backend_abort);
        assert(!registration.active());
    }

    {
        cancellation_source source;
        cancellation_token token = source.token();
        int calls = 0;
        bool internal_lock_was_held = true;

        auto registration = token.register_callback([&](cancellation_reason) {
            ++calls;
            internal_lock_was_held = detail::cancellation_internal_lock_held_for_testing(token);
        });

        assert(source.request_cancellation(cancellation_reason::scope_shutdown));
        assert(calls == 1);
        assert(!internal_lock_was_held);
        assert(!registration.active());
    }

    return 0;
}
