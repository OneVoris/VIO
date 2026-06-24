#include <voris/io/backends/io_uring_backend.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    auto caps = backends::detect_io_uring_capabilities();
    backends::io_uring_backend backend(caps);
    assert(backend.capabilities().available == caps.available);

#if defined(__linux__)
    assert(backend.default_eligible());
    assert(backend.register_handle(1).has_value());
    assert(backend.submit(backend_operation{1, backend_operation_kind::read, {}}).has_value());
    assert(backend.cancel(1, cancellation_reason::manual).has_value());
    assert(backend.poll().has_value());
    assert(backend.register_buffers(2).has_value());
    assert(backend.register_files(2).has_value());
#else
    assert(!backend.default_eligible());
    auto unsupported = backend.register_handle(1);
    assert(!unsupported.has_value());
    assert(unsupported.error().classification == vio_error_code::unsupported);
    assert(!backend.register_buffers(2).has_value());
    assert(!backend.register_files(2).has_value());
#endif

    assert(backend.shutdown().has_value());
    return 0;
}
