#include <voris/io/async_mutex.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    async_mutex mutex;
    assert(mutex.lock().has_value());
    assert(mutex.locked());
    auto blocked = mutex.lock();
    assert(!blocked.has_value());
    assert(blocked.error().classification == vio_error_code::operation_in_progress);
    assert(mutex.waiters() == 1);
    mutex.unlock();
    assert(mutex.locked());
    assert(mutex.waiters() == 0);
    mutex.unlock();
    assert(!mutex.locked());

    return 0;
}
