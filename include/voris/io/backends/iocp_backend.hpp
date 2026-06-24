#pragma once

#include <voris/io/backend.hpp>

namespace voris::io::backends {

struct overlapped_operation_lifetime {
    bool submitted{};
    bool cancellation_requested{};
    bool completion_observed{};

    [[nodiscard]] bool storage_retained() const noexcept {
        return submitted && !completion_observed;
    }
};

class iocp_backend final : public backend {
public:
    [[nodiscard]] void_result register_handle(std::size_t native_handle) override;
    [[nodiscard]] void_result submit(backend_operation operation) override;
    [[nodiscard]] void_result cancel(std::size_t operation_id,
                                     cancellation_reason reason) override;
    [[nodiscard]] io_result<std::size_t> poll() override;
    [[nodiscard]] void_result wake() override;
    [[nodiscard]] void_result shutdown() override;

private:
    virtual_backend fallback_;
};

} // namespace voris::io::backends
