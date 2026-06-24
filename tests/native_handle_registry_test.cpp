#include <voris/io/detail/native_handle_registry.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    detail::native_handle_registry registry;
    auto first = registry.register_handle(10);
    assert(first.generation == 1);
    assert(registry.is_current(first));
    assert(registry.close(first).has_value());
    assert(!registry.is_current(first));

    auto reused = registry.register_handle(10);
    assert(reused.generation == 2);
    assert(registry.is_current(reused));
    assert(!registry.is_current(first));
    assert(!registry.close(first).has_value());
    assert(registry.close(reused).has_value());

    return 0;
}
