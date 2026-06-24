#include <voris/io/async_semaphore.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    async_semaphore semaphore(1);
    assert(semaphore.acquire().has_value());
    auto blocked = semaphore.acquire();
    assert(!blocked.has_value());
    assert(blocked.error().classification == vio_error_code::resource_exhausted);
    assert(semaphore.waiters() == 1);
    semaphore.release();
    assert(semaphore.waiters() == 0);
    assert(semaphore.permits() == 0);
    semaphore.release();
    assert(semaphore.permits() == 1);

    return 0;
}
