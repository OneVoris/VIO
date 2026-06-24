#include <voris/io/backends/epoll_backend.hpp>
#include <voris/io/backends/io_uring_backend.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    virtual_backend reference;
    assert(reference.register_handle(1).has_value());
    assert(reference.submit(backend_operation{1, backend_operation_kind::write, {}}).has_value());
    assert(reference.poll().has_value());

    backends::epoll_backend epoll;
    backends::io_uring_backend uring;

#if defined(__linux__)
    assert(epoll.register_handle(1).has_value());
    assert(uring.register_handle(1).has_value());
#else
    assert(!epoll.register_handle(1).has_value());
    assert(!uring.register_handle(1).has_value());
#endif

    return 0;
}
