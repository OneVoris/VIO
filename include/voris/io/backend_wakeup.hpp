#pragma once

#include <cstddef>

namespace voris::io {

class backend_wakeup {
public:
    void wake() noexcept {
        ++wake_count_;
    }

    [[nodiscard]] std::size_t wake_count() const noexcept {
        return wake_count_;
    }

    void consume() noexcept {
        if (wake_count_ != 0) {
            --wake_count_;
        }
    }

private:
    std::size_t wake_count_{0};
};

} // namespace voris::io
