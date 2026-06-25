#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include <voris/io/backend.hpp>

namespace voris::io::backends {

struct io_uring_capabilities {
    bool available{};
    bool supports_read{};
    bool supports_write{};
    bool supports_accept{};
    bool supports_connect{};
    bool supports_files{};
    bool supports_fsync{};
    bool supports_cancel{};
    bool supports_registered_buffers{};
    // Candidate support based on the files-update opcode. M6-006 must still
    // validate registered-file lifecycle before this becomes a default path.
    bool supports_registered_files{};
};

struct io_uring_backend_options {
    std::size_t submission_queue_capacity{64};
    std::size_t submit_batch_limit{32};
    std::size_t completion_batch_limit{32};
    bool enable_kernel_submission{true};
};

enum class io_uring_backend_state {
    unavailable,
    active,
    closed,
};

[[nodiscard]] io_uring_capabilities detect_io_uring_capabilities() noexcept;

class io_uring_backend final : public backend {
public:
    explicit io_uring_backend(io_uring_capabilities capabilities =
                                  detect_io_uring_capabilities(),
                              io_uring_backend_options options = {});
    ~io_uring_backend() override;

    [[nodiscard]] const io_uring_capabilities& capabilities() const noexcept;
    [[nodiscard]] io_uring_backend_state state() const noexcept;
    [[nodiscard]] bool default_eligible() const noexcept;

    [[nodiscard]] io_result<backend_handle_token> register_handle(
        std::size_t native_handle) override;
    [[nodiscard]] void_result submit(backend_operation operation) override;
    [[nodiscard]] void_result cancel(std::size_t operation_id,
                                     cancellation_reason reason) override;
    [[nodiscard]] void_result close_handle(backend_handle_token token) override;
    [[nodiscard]] io_result<std::size_t> poll() override;
    [[nodiscard]] io_result<std::size_t> drain_completions(
        std::span<backend_completion> out) override;
    [[nodiscard]] void_result wake() override;
    [[nodiscard]] void_result shutdown() override;

    [[nodiscard]] void_result register_buffers(std::size_t count);
    [[nodiscard]] void_result register_files(std::size_t count);

private:
    struct queued_submission {
        backend_operation operation{};
        std::optional<cancellation_reason> cancellation{};
    };
    struct kernel_ring;

    [[nodiscard]] bool use_kernel_submission() const noexcept;
    [[nodiscard]] void_result validate_kernel_socket_operation(
        const backend_operation& operation) const;
    [[nodiscard]] void_result submit_to_fallback(queued_submission queued);
    [[nodiscard]] void_result submit_to_kernel(const queued_submission& queued);
    [[nodiscard]] io_result<std::size_t> flush_submission_batch();
    [[nodiscard]] io_result<std::size_t> observe_completion_batch();
    [[nodiscard]] io_result<std::size_t> observe_kernel_completions();
    [[nodiscard]] void_result drain_fallback_completions();
    [[nodiscard]] void_result flush_queued_submissions_for(backend_handle_token token);
    void complete_queued_submissions_for(backend_handle_token token,
                                         const void_result& result);

    io_uring_capabilities capabilities_;
    io_uring_backend_options options_;
    virtual_backend fallback_;
    std::unique_ptr<kernel_ring> kernel_ring_{};
    std::deque<queued_submission> submission_queue_{};
    std::deque<backend_completion> completion_queue_{};
    std::unordered_map<std::size_t, backend_operation> kernel_operations_{};
    std::unordered_set<std::size_t> active_operation_ids_{};
    std::size_t registered_buffers_{0};
    std::size_t registered_files_{0};
    bool closed_{false};
};

} // namespace voris::io::backends
