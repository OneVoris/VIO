#pragma once

#include <cstddef>

#include <voris/io/cancellation.hpp>
#include <voris/io/error.hpp>
#include <voris/io/scheduler.hpp>

namespace voris::io {

enum class backend_operation_kind {
    read,
    write,
    accept,
    connect,
    close,
    wake,
};

struct backend_operation {
    std::size_t id{};
    backend_operation_kind kind{};
    scheduler_ref scheduler{};
};

struct backend_completion {
    std::size_t operation_id{};
    void_result result{};
};

class backend {
public:
    virtual ~backend() = default;

    [[nodiscard]] virtual void_result register_handle(std::size_t native_handle) = 0;
    [[nodiscard]] virtual void_result submit(backend_operation operation) = 0;
    [[nodiscard]] virtual void_result cancel(std::size_t operation_id,
                                             cancellation_reason reason) = 0;
    [[nodiscard]] virtual io_result<std::size_t> poll() = 0;
    [[nodiscard]] virtual void_result wake() = 0;
    [[nodiscard]] virtual void_result shutdown() = 0;
};

class virtual_backend final : public backend {
public:
    [[nodiscard]] void_result register_handle(std::size_t native_handle) override;
    [[nodiscard]] void_result submit(backend_operation operation) override;
    [[nodiscard]] void_result cancel(std::size_t operation_id,
                                     cancellation_reason reason) override;
    [[nodiscard]] io_result<std::size_t> poll() override;
    [[nodiscard]] void_result wake() override;
    [[nodiscard]] void_result shutdown() override;

    [[nodiscard]] std::size_t submitted() const noexcept;
    [[nodiscard]] std::size_t cancelled() const noexcept;
    [[nodiscard]] bool stopped() const noexcept;

private:
    std::size_t registered_{0};
    std::size_t submitted_{0};
    std::size_t cancelled_{0};
    std::size_t wakeups_{0};
    bool stopped_{false};
};

} // namespace voris::io
