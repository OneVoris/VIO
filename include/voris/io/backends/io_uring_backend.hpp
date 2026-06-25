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

namespace detail {

enum class pending_io_uring_submission_kind {
    operation,
    cancel,
};

struct pending_io_uring_submission {
    pending_io_uring_submission_kind kind{};
    std::size_t operation_id{};
};

void discard_submitted_io_uring_submissions(
    std::deque<pending_io_uring_submission>& pending,
    unsigned submitted_count) noexcept;
[[nodiscard]] std::vector<pending_io_uring_submission>
take_unsubmitted_io_uring_submissions(
    std::deque<pending_io_uring_submission>& pending);
[[nodiscard]] bool io_uring_cancel_retry_required(
    bool close_requested,
    bool cancel_submitted) noexcept;

} // namespace detail

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
    struct kernel_operation {
        backend_operation operation{};
        bool close_requested{};
        bool cancel_submitted{};
    };
    struct kernel_ring;

    [[nodiscard]] bool use_kernel_submission() const noexcept;
    [[nodiscard]] bool has_inflight_kernel_work() const noexcept;
    [[nodiscard]] void_result validate_kernel_operation(
        const backend_operation& operation) const;
    [[nodiscard]] void_result submit_to_fallback(queued_submission queued);
    [[nodiscard]] void_result submit_to_kernel(const queued_submission& queued);
    [[nodiscard]] void_result request_kernel_cancellations_for(
        backend_handle_token token);
    [[nodiscard]] void_result request_kernel_cancellation_for(
        std::size_t operation_id,
        kernel_operation& operation);
    [[nodiscard]] io_result<std::size_t> progress_kernel_submissions();
    [[nodiscard]] io_result<std::size_t> retry_missing_kernel_cancellations();
    std::size_t complete_unsubmitted_kernel_submissions(const vio_error& error);
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
    std::deque<detail::pending_io_uring_submission> pending_kernel_submissions_{};
    std::unordered_map<std::size_t, kernel_operation> kernel_operations_{};
    std::unordered_set<std::size_t> kernel_cancel_operation_ids_{};
    std::unordered_set<std::size_t> active_operation_ids_{};
    std::size_t registered_buffers_{0};
    std::size_t registered_files_{0};
    bool closed_{false};
};

} // namespace voris::io::backends
