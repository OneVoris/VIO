#include <voris/io/backends/epoll_backend.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    backends::epoll_backend backend;
#if defined(__linux__)
    assert(backend.register_handle(1).has_value());
#else
    auto result = backend.register_handle(1);
    assert(!result.has_value());
    assert(result.error().classification == vio_error_code::unsupported);
#endif
    assert(backend.shutdown().has_value());

    return 0;
}
