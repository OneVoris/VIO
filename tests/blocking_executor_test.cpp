#include <voris/io/blocking_executor.hpp>

#include <cassert>

int main() {
    using namespace voris::io;

    blocking_executor executor(1);
    int ran = 0;
    assert(executor.submit([&ran] { ++ran; }).has_value());
    auto full = executor.submit([&ran] { ran += 10; });
    assert(!full.has_value());
    assert(full.error().classification == vio_error_code::resource_exhausted);
    assert(executor.drain() == 1);
    assert(ran == 1);
    executor.shutdown();
    assert(executor.shutting_down());

    return 0;
}
