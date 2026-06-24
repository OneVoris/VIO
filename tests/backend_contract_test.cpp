#include <voris/io/backend.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    virtual_backend backend;
    assert(backend.register_handle(1).has_value());
    assert(!backend.register_handle(0).has_value());
    assert(backend.submit(backend_operation{1, backend_operation_kind::read, {}}).has_value());
    assert(backend.submitted() == 1);
    assert(backend.cancel(1, cancellation_reason::manual).has_value());
    assert(backend.cancelled() == 1);
    auto polled = backend.poll();
    assert(polled.has_value());
    assert(*polled == 1);
    assert(backend.wake().has_value());
    assert(backend.shutdown().has_value());
    assert(backend.stopped());
    assert(!backend.submit(backend_operation{2, backend_operation_kind::write, {}}).has_value());

    return 0;
}
