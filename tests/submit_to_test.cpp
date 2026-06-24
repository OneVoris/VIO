#include <voris/io/submit.hpp>

#include "test_assert.hpp"
#include <memory>
#include <vector>

namespace {

void assert_error(const voris::io::void_result& result, voris::io::vio_error_code code) {
    assert(!result.has_value());
    assert(result.error().classification == code);
}

} // namespace

int main() {
    using namespace voris::io;

    {
        shard target(2);
        scheduler_ref target_ref = target.scheduler();
        auto value = std::make_unique<int>(7);
        int observed = 0;

        assert(submit_to(target_ref, [owned = std::move(value), &observed] {
            observed = *owned;
        }).has_value());
        assert(value == nullptr);
        assert(target.drain() == 1);
        assert(observed == 7);
    }

    {
        shard target(2);
        auto value = std::make_unique<int>(11);
        int observed = 0;

        assert(submit_to(target, [owned = std::move(value), &observed] {
            observed = *owned;
        }).has_value());
        assert(value == nullptr);
        assert(target.drain() == 1);
        assert(observed == 11);
    }

    {
        shard target(1);
        int observed = 0;

        assert(submit_to(target, [&observed] { observed += 1; }).has_value());
        const auto rejected = submit_to(target, [&observed] { observed += 100; });
        assert_error(rejected, vio_error_code::resource_exhausted);

        assert(target.drain() == 1);
        assert(observed == 1);
    }

    {
        shard owner(2);
        shard target(2);
        std::vector<int> order;

        assert(submit_to(target.scheduler(), [owner_ref = owner.scheduler(), &order] {
            auto return_value = std::make_unique<int>(17);
            auto result = submit_to(owner_ref, [owned = std::move(return_value), &order] {
                order.push_back(*owned);
            });

            assert(return_value == nullptr);
            assert(result.has_value());
            order.push_back(3);
        }).has_value());

        assert(target.drain() == 1);
        assert(order == std::vector<int>({3}));
        assert(owner.drain() == 1);
        assert(order == std::vector<int>({3, 17}));
    }

    {
        shard owner(1);
        shard target(1);
        int owner_ran = 0;
        bool target_observed_rejection = false;

        assert(submit_to(owner, [&owner_ran] { owner_ran += 1; }).has_value());
        assert(submit_to(target.scheduler(),
                         [owner_ref = owner.scheduler(), &target_observed_rejection,
                          &owner_ran] {
                             auto return_value = std::make_unique<int>(50);
                             auto result =
                                 submit_to(owner_ref,
                                           [owned = std::move(return_value), &owner_ran] {
                                               owner_ran += *owned;
                                           });

                             assert(return_value == nullptr);
                             assert_error(result, vio_error_code::resource_exhausted);
                             target_observed_rejection = true;
                         })
                   .has_value());

        assert(target.drain() == 1);
        assert(target_observed_rejection);
        assert(owner_ran == 0);
        assert(owner.drain() == 1);
        assert(owner_ran == 1);
    }

    {
        shard target(1);
        scheduler_ref empty_ref;
        assert_error(submit_to(empty_ref, [] {}), vio_error_code::invalid_state);

        continuation empty_message;
        assert_error(submit_to(target.scheduler(), std::move(empty_message)),
                     vio_error_code::invalid_state);
        assert(target.drain() == 0);

        continuation empty_shard_message;
        assert_error(submit_to(target, std::move(empty_shard_message)),
                     vio_error_code::invalid_state);
        assert(target.drain() == 0);
    }

    return 0;
}
