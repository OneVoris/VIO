#include <voris/io/manual_reset_event.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    manual_reset_event event;
    auto blocked = event.wait();
    assert(!blocked.has_value());
    assert(blocked.error().classification == vio_error_code::resource_exhausted);
    assert(event.waiters() == 1);
    event.set();
    assert(event.waiters() == 0);
    assert(event.wait().has_value());
    event.reset();
    assert(!event.wait().has_value());

    return 0;
}
