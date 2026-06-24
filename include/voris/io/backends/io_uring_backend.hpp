#pragma once

#include <cstddef>
#include <vector>

#include <voris/io/backend.hpp>

namespace voris::io::backends {

struct io_uring_capabilities {
    bool available{};
    bool supports_accept{};
    bool supports_connect{};
    bool supports_files{};
    bool supports_cancel{};
    bool supports_registered_buffers{};
    bool supports_registered_files{};
};

[[nodiscard]] io_uring_capabilities detect_io_uring_capabilities() noexcept;

class io_uring_backend final : public backend {
public:
    explicit io_uring_backend(io_uring_capabilities capabilities =
                                  detect_io_uring_capabilities());

    [[nodiscard]] const io_uring_capabilities& capabilities() const noexcept;
    [[nodiscard]] bool default_eligible() const noexcept;

    [[nodiscard]] void_result register_handle(std::size_t native_handle) override;
    [[nodiscard]] void_result submit(backend_operation operation) override;
    [[nodiscard]] void_result cancel(std::size_t operation_id,
                                     cancellation_reason reason) override;
    [[nodiscard]] io_result<std::size_t> poll() override;
    [[nodiscard]] void_result wake() override;
    [[nodiscard]] void_result shutdown() override;

    [[nodiscard]] void_result register_buffers(std::size_t count);
    [[nodiscard]] void_result register_files(std::size_t count);

private:
    io_uring_capabilities capabilities_;
    virtual_backend fallback_;
    std::size_t registered_buffers_{0};
    std::size_t registered_files_{0};
};

} // namespace voris::io::backends
