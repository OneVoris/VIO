#include <voris/io/compute_executor.hpp>
#include <voris/io/shard.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    shard limited(1);
    assert(limited.submit([] {}).has_value());
    auto overload = limited.submit([] {});
    assert(!overload.has_value());
    assert(overload.error().classification == vio_error_code::resource_exhausted);
    assert(limited.drain() == 1);

    compute_executor executor(1);
    assert(executor.submit([] {}).has_value());
    assert(!executor.submit([] {}).has_value());
    assert(executor.run_until_idle() == 1);

    return 0;
}
