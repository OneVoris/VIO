#include <voris/io/compute_executor.hpp>

#include "test_assert.hpp"

int main() {
    using namespace voris::io;

    {
        compute_executor executor(1);
        int ran = 0;
        assert(executor.submit([&ran] { ++ran; }).has_value());
        auto full = executor.submit([&ran] { ran += 10; });
        assert(!full.has_value());
        assert(full.error().classification == vio_error_code::resource_exhausted);
        assert(ran == 0);
        assert(executor.capacity_waiters() == 1);
        assert(executor.run_until_idle() == 1);
        assert(ran == 1);
        assert(executor.queued() == 0);
        assert(executor.capacity_waiters() == 0);
    }

    {
        compute_executor executor(1);
        assert(executor.submit([] {}).has_value());
        for (int attempt = 0; attempt != 4; ++attempt) {
            auto wait = executor.try_register_capacity_waiter();
            assert(!wait.has_value());
            assert(wait.error().classification == vio_error_code::resource_exhausted);
            assert(executor.capacity_waiters() <= 1);
        }
        assert(executor.capacity_waiters() == 1);
        executor.release_capacity_waiter();
        assert(executor.capacity_waiters() == 0);

        auto wait = executor.try_register_capacity_waiter();
        assert(!wait.has_value());
        assert(wait.error().classification == vio_error_code::resource_exhausted);
        assert(executor.capacity_waiters() == 1);
        assert(executor.run_until_idle() == 1);
        assert(executor.capacity_waiters() == 0);
        assert(executor.try_register_capacity_waiter().has_value());
        assert(executor.capacity_waiters() == 0);
    }

    {
        compute_executor executor(2);
        int ran = 0;
        assert(executor.submit([&ran] { ++ran; }).has_value());
        executor.request_shutdown();
        assert(executor.shutting_down());
        auto rejected = executor.submit([&ran] { ran += 10; });
        assert(!rejected.has_value());
        assert(rejected.error().classification == vio_error_code::closed);
        assert(executor.queued() == 1);
        assert(executor.run_until_idle() == 1);
        assert(ran == 1);
        assert(executor.queued() == 0);
    }

    {
        compute_executor executor(2);
        int ran = 0;
        assert(executor.submit([&ran] { ran += 1; }).has_value());
        assert(executor.submit([&ran] { ran += 10; }).has_value());
        executor.shutdown();
        assert(executor.shutting_down());
        assert(executor.run_until_idle() == 2);
        assert(ran == 11);
        assert(executor.queued() == 0);
    }

    {
        compute_executor executor(1);
        assert(executor.submit([] {}).has_value());
        auto wait = executor.try_register_capacity_waiter();
        assert(!wait.has_value());
        assert(executor.capacity_waiters() == 1);
        executor.request_shutdown();
        assert(executor.capacity_waiters() == 0);
        auto rejected_wait = executor.try_register_capacity_waiter();
        assert(!rejected_wait.has_value());
        assert(rejected_wait.error().classification == vio_error_code::closed);
        assert(executor.capacity_waiters() == 0);
    }

    {
        compute_executor executor(2);
        int ran = 0;
        assert(executor
                   .submit([&] {
                       ++ran;
                       assert(executor.submit([&] { ran += 10; }).has_value());
                       assert(executor.run_until_idle() == 1);
                   })
                   .has_value());
        assert(executor.run_until_idle() == 1);
        assert(ran == 11);
    }

    return 0;
}
