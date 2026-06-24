#include <voris/io/compute_executor.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    compute_executor executor(1);
    int ran = 0;
    assert(executor.submit([&ran] { ++ran; }).has_value());
    auto full = executor.submit([&ran] { ran += 10; });
    assert(!full.has_value());
    assert(full.error().classification == vio_error_code::resource_exhausted);
    assert(executor.capacity_waiters() == 1);
    assert(executor.run_until_idle() == 1);
    assert(ran == 1);
    assert(executor.queued() == 0);

    return 0;
}
