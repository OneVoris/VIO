#include <voris/io/backends/epoll_backend.hpp>
#include <voris/io/backends/io_uring_backend.hpp>

#include "test_assert.hpp"

#if defined(__linux__)
#include <sys/eventfd.h>
#include <unistd.h>
#endif

int main() {
    using namespace voris::io;

    virtual_backend reference;
    assert(reference.register_handle(1).has_value());
    assert(reference.submit(backend_operation{1, backend_operation_kind::write, {}}).has_value());
    assert(reference.poll().has_value());

    backends::epoll_backend epoll;
    backends::io_uring_backend uring;

#if defined(__linux__)
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    assert(fd >= 0);
    assert(epoll.register_handle(static_cast<std::size_t>(fd)).has_value());
    assert(uring.register_handle(static_cast<std::size_t>(fd)).has_value());
    assert(::close(fd) == 0);
#else
    assert(!epoll.register_handle(1).has_value());
    assert(!uring.register_handle(1).has_value());
#endif

    return 0;
}
