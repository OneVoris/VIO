#pragma once

#include <optional>

#include <voris/io/backend.hpp>

namespace voris::io::backends {

class epoll_backend final : public backend {
public:
    epoll_backend();
    ~epoll_backend() override;

    epoll_backend(const epoll_backend&) = delete;
    epoll_backend& operator=(const epoll_backend&) = delete;

    [[nodiscard]] void_result register_handle(std::size_t native_handle) override;
    [[nodiscard]] void_result submit(backend_operation operation) override;
    [[nodiscard]] void_result cancel(std::size_t operation_id,
                                     cancellation_reason reason) override;
    [[nodiscard]] io_result<std::size_t> poll() override;
    [[nodiscard]] void_result wake() override;
    [[nodiscard]] void_result shutdown() override;

private:
#if defined(__linux__)
    void close_owned_descriptors() noexcept;
    [[nodiscard]] void_result initialization_result() const;
#endif

    virtual_backend fallback_;
    int epoll_fd_{-1};
    int wake_fd_{-1};
    bool stopped_{false};
    std::optional<vio_error> initialization_error_{};
};

} // namespace voris::io::backends
