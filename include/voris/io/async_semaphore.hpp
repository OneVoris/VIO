#pragma once

#include <cstddef>

#include <voris/io/error.hpp>

namespace voris::io {

class async_semaphore {
public:
    explicit async_semaphore(std::size_t permits)
        : permits_(permits) {}

    [[nodiscard]] void_result acquire() {
        if (permits_ == 0) {
            ++waiters_;
            return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                              "no semaphore permits available"));
        }
        --permits_;
        return {};
    }

    void release(std::size_t permits = 1) noexcept {
        permits_ += permits;
        while (permits_ != 0 && waiters_ != 0) {
            --permits_;
            --waiters_;
        }
    }

    [[nodiscard]] std::size_t permits() const noexcept {
        return permits_;
    }

    [[nodiscard]] std::size_t waiters() const noexcept {
        return waiters_;
    }

private:
    std::size_t permits_;
    std::size_t waiters_{0};
};

} // namespace voris::io
