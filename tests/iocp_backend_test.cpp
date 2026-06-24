#include <voris/io/backends/iocp_backend.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    backends::overlapped_operation_lifetime lifetime{true, true, false};
    assert(lifetime.storage_retained());
    lifetime.completion_observed = true;
    assert(!lifetime.storage_retained());

    backends::iocp_backend backend;
#if defined(_WIN32)
    assert(backend.register_handle(1).has_value());
    assert(backend.submit(backend_operation{1, backend_operation_kind::read, {}}).has_value());
    assert(backend.cancel(1, cancellation_reason::manual).has_value());
    assert(backend.poll().has_value());
#else
    auto result = backend.register_handle(1);
    assert(!result.has_value());
    assert(result.error().classification == vio_error_code::unsupported);
#endif
    assert(backend.shutdown().has_value());
    return 0;
}
