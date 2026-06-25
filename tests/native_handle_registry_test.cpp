#include <voris/io/detail/native_handle_registry.hpp>

#include "test_assert.hpp"

namespace {

void assert_invalid_close(const voris::io::void_result& result) {
    assert(!result.has_value());
    assert(result.error().classification == voris::io::vio_error_code::invalid_state);
}

voris::io::detail::native_handle_token require_token(
    voris::io::io_result<voris::io::detail::native_handle_token> result) {
    assert(result.has_value());
    return *result;
}

} // namespace

int main() {
    using namespace voris::io;

    detail::native_handle_registry registry;
    assert_invalid_close(registry.close({}));
    assert_invalid_close(registry.close({99, 1}));

    auto first = require_token(registry.register_handle(10));
    assert(first.generation == 1);
    assert(registry.is_current(first));
    assert(registry.close(first).has_value());
    assert(!registry.is_current(first));
    assert_invalid_close(registry.close(first));

    auto reused = require_token(registry.register_handle(10));
    assert(reused.generation == 2);
    assert(registry.is_current(reused));
    assert(!registry.is_current(first));
    assert_invalid_close(registry.close(first));
    assert(registry.close(reused).has_value());

    auto other = require_token(registry.register_handle(11));
    assert(other.generation == 1);
    assert(!registry.close(first).has_value());
    assert(registry.is_current(other));
    assert(registry.close(other).has_value());

    return 0;
}
