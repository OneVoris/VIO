#pragma once

#include <cstddef>

#include <voris/io/error.hpp>

namespace voris::io {

class manual_reset_event {
public:
    explicit manual_reset_event(bool initially_set = false) noexcept
        : set_(initially_set) {}

    [[nodiscard]] void_result wait() {
        if (set_) {
            return {};
        }
        ++waiters_;
        return std::unexpected(make_error(vio_error_code::resource_exhausted,
                                          "event is not set"));
    }

    void set() noexcept {
        set_ = true;
        waiters_ = 0;
    }

    void reset() noexcept {
        set_ = false;
    }

    [[nodiscard]] bool is_set() const noexcept {
        return set_;
    }

    [[nodiscard]] std::size_t waiters() const noexcept {
        return waiters_;
    }

private:
    bool set_{false};
    std::size_t waiters_{0};
};

} // namespace voris::io
