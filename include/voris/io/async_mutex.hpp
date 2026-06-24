#pragma once

#include <cstddef>

#include <voris/io/error.hpp>

namespace voris::io {

class async_mutex {
public:
    [[nodiscard]] void_result lock() {
        if (locked_) {
            ++waiters_;
            return std::unexpected(make_error(vio_error_code::operation_in_progress,
                                              "mutex is already locked"));
        }
        locked_ = true;
        return {};
    }

    void unlock() noexcept {
        if (waiters_ != 0) {
            --waiters_;
            locked_ = true;
            return;
        }
        locked_ = false;
    }

    [[nodiscard]] bool locked() const noexcept {
        return locked_;
    }

    [[nodiscard]] std::size_t waiters() const noexcept {
        return waiters_;
    }

private:
    bool locked_{false};
    std::size_t waiters_{0};
};

} // namespace voris::io
