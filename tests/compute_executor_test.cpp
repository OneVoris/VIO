#include <voris/io/compute_executor.hpp>

#include "test_assert.hpp"

#include <type_traits>
#include <utility>

int main() {
    using namespace voris::io;

    static_assert(!std::is_copy_constructible_v<compute_executor>);
    static_assert(!std::is_copy_assignable_v<compute_executor>);
    static_assert(!std::is_move_constructible_v<compute_executor>);
    static_assert(!std::is_move_assignable_v<compute_executor>);

    {
        compute_executor executor(1);
        bool saw_executor_scheduler = false;
        assert(executor
                   .submit([&] {
                       auto current = require_current_scheduler();
                       assert(current.has_value());
                       assert(current->identity() == &executor);
                       saw_executor_scheduler = true;
                   })
                   .has_value());
        assert(executor.run_until_idle() == 1);
        assert(saw_executor_scheduler);
    }

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
        int ran = 0;
        assert(executor.submit([&ran] { ++ran; }).has_value());
        for (int attempt = 0; attempt != 4; ++attempt) {
            auto reservation = executor.try_reserve_capacity();
            assert(!reservation.has_value());
            assert(reservation.error().classification == vio_error_code::resource_exhausted);
            assert(executor.capacity_waiters() <= 1);
        }
        assert(executor.capacity_waiters() == 1);
        assert(executor.run_until_idle() == 1);
        assert(ran == 1);
        assert(executor.capacity_waiters() == 0);

        auto reservation = executor.try_reserve_capacity();
        assert(reservation.has_value());
        auto stolen = executor.submit([&ran] { ran += 100; });
        assert(!stolen.has_value());
        assert(stolen.error().classification == vio_error_code::resource_exhausted);
        assert(executor.capacity_waiters() == 1);
        assert(executor.queued() == 0);
        assert(executor.submit_reserved(std::move(*reservation), [&ran] { ran += 10; })
                   .has_value());
        assert(executor.capacity_waiters() == 0);
        assert(executor.run_until_idle() == 1);
        assert(ran == 11);
    }

    {
        compute_executor executor(1);
        {
            auto reservation = executor.try_reserve_capacity();
            assert(reservation.has_value());
            auto blocked = executor.submit([] {});
            assert(!blocked.has_value());
            assert(blocked.error().classification == vio_error_code::resource_exhausted);
        }
        assert(executor.submit([] {}).has_value());
        assert(executor.run_until_idle() == 1);

        auto reservation = executor.try_reserve_capacity();
        assert(reservation.has_value());
        reservation->release();
        assert(executor.submit([] {}).has_value());
        assert(executor.run_until_idle() == 1);
    }

    {
        compute_executor executor(2);
        int ran = 0;
        auto first = executor.try_reserve_capacity();
        auto second = executor.try_reserve_capacity();
        assert(first.has_value());
        assert(second.has_value());
        assert(!executor.submit([&ran] { ran += 100; }).has_value());

        *first = std::move(*second);
        second->release();
        assert(executor.submit([&ran] { ran += 1; }).has_value());
        assert(executor.submit_reserved(std::move(*first), [&ran] { ran += 10; }).has_value());
        assert(executor.run_until_idle() == 2);
        assert(ran == 11);
        assert(executor.queued() == 0);
    }

    {
        compute_executor executor(0);
        auto rejected = executor.submit([] {});
        assert(!rejected.has_value());
        assert(rejected.error().classification == vio_error_code::resource_exhausted);
        for (int attempt = 0; attempt != 4; ++attempt) {
            auto reservation = executor.try_reserve_capacity();
            assert(!reservation.has_value());
            assert(reservation.error().classification == vio_error_code::resource_exhausted);
            assert(executor.capacity_waiters() <= 1);
        }
        assert(executor.capacity_waiters() == 1);
        assert(executor.queued() == 0);
        assert(executor.run_until_idle() == 0);
    }

    {
        compute_executor executor(1);
        auto reservation = executor.try_reserve_capacity();
        assert(reservation.has_value());
        executor.request_shutdown();
        assert(executor.capacity_waiters() == 0);
        auto rejected_submit = executor.submit([] {});
        assert(!rejected_submit.has_value());
        assert(rejected_submit.error().classification == vio_error_code::closed);
        auto rejected_reservation = executor.try_reserve_capacity();
        assert(!rejected_reservation.has_value());
        assert(rejected_reservation.error().classification == vio_error_code::closed);
        auto rejected_reserved = executor.submit_reserved(std::move(*reservation), [] {});
        assert(!rejected_reserved.has_value());
        assert(rejected_reserved.error().classification == vio_error_code::closed);
        assert(executor.capacity_waiters() == 0);
    }

    {
        compute_executor owner(1);
        compute_executor other(1);
        auto reservation = owner.try_reserve_capacity();
        assert(reservation.has_value());
        auto wrong_owner = other.submit_reserved(std::move(*reservation), [] {});
        assert(!wrong_owner.has_value());
        assert(wrong_owner.error().classification == vio_error_code::invalid_state);
        assert(owner.submit([] {}).has_value());
        assert(owner.run_until_idle() == 1);
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
        auto reservation = executor.try_reserve_capacity();
        assert(!reservation.has_value());
        assert(executor.capacity_waiters() == 1);
        executor.request_shutdown();
        assert(executor.capacity_waiters() == 0);
        auto rejected_reservation = executor.try_reserve_capacity();
        assert(!rejected_reservation.has_value());
        assert(rejected_reservation.error().classification == vio_error_code::closed);
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
