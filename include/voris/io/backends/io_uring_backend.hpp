#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <voris/io/backend.hpp>

namespace voris::io::backends {

class io_uring_backend;

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
    // Candidate support based on the files-update opcode. M6-006 defines the
    // lifecycle contract; M6-008 still gates default fixed-file use.
    bool supports_registered_files{};
};

enum class linux_backend_choice {
    epoll,
    io_uring,
};

// Pre-1.0 Linux backend release-gating policy helpers. They are pure and
// testable, but they do not construct backends or switch runtime defaults
// automatically.
struct io_uring_default_enable_evidence {
    bool cancellation_races_passed{};
    bool differential_tests_passed{};
    bool benchmarks_passed{};
    bool linux_real_provider_tests_passed{};
};

struct io_uring_backend_options {
    std::size_t submission_queue_capacity{64};
    std::size_t submit_batch_limit{32};
    std::size_t completion_batch_limit{32};
    bool enable_kernel_submission{true};
};

struct io_uring_registered_buffer {
    // VIO does not own this storage. Kernel registration may pin or map it
    // until explicit unregister, shutdown, or ring teardown, so the caller must
    // keep the memory alive for the whole registration lifetime.
    std::span<std::byte> bytes{};
};

enum class io_uring_backend_state {
    unavailable,
    active,
    closed,
};

[[nodiscard]] io_uring_capabilities detect_io_uring_capabilities() noexcept;
[[nodiscard]] bool io_uring_default_enable_eligible(
    io_uring_capabilities capabilities,
    io_uring_default_enable_evidence evidence) noexcept;
[[nodiscard]] linux_backend_choice select_default_linux_backend(
    io_uring_capabilities capabilities,
    io_uring_default_enable_evidence evidence) noexcept;

namespace detail {

enum class pending_io_uring_submission_kind {
    operation,
    cancel,
};

enum class io_uring_completion_result_class {
    kernel_result,
    cancelled,
    closed,
};

struct pending_io_uring_submission {
    pending_io_uring_submission_kind kind{};
    std::size_t operation_id{};
};

enum class io_uring_test_completion_kind {
    operation,
    cancel_ack,
};

struct io_uring_test_completion {
    io_uring_test_completion_kind kind{};
    std::size_t operation_id{};
    int result{};
};

struct io_uring_test_submission {
    std::size_t operation_id{};
    bool used_registered_buffer{};
    bool used_registered_file{};
};

struct io_uring_test_kernel {
    std::size_t cancel_submission_failures{};
    std::deque<io_uring_test_completion> completions{};
    std::vector<std::size_t> submitted_cancel_operation_ids{};
    std::vector<io_uring_test_submission> submissions{};
    std::size_t register_buffers_calls{};
    std::size_t unregister_buffers_calls{};
    std::size_t register_files_calls{};
    std::size_t unregister_files_calls{};
    std::size_t register_buffers_provider_failures{};
    std::size_t register_files_provider_failures{};
};

void discard_submitted_io_uring_submissions(
    std::deque<pending_io_uring_submission>& pending,
    unsigned submitted_count) noexcept;
[[nodiscard]] std::vector<pending_io_uring_submission>
take_unsubmitted_io_uring_submissions(
    std::deque<pending_io_uring_submission>& pending);
[[nodiscard]] bool io_uring_cancel_retry_required(
    bool cancel_requested,
    bool close_requested,
    bool cancel_request_submitted,
    bool cancel_sqe_in_flight) noexcept;
[[nodiscard]] io_uring_completion_result_class io_uring_completion_result_for(
    backend_operation_target target,
    bool cancel_requested,
    bool close_requested,
    bool handle_current,
    int cqe_result) noexcept;
[[nodiscard]] bool io_uring_completion_should_report_closed(
    backend_operation_target target,
    bool close_requested,
    bool handle_current,
    int cqe_result) noexcept;
void attach_io_uring_test_kernel(
    io_uring_backend& backend,
    io_uring_test_kernel& kernel) noexcept;

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
    [[nodiscard]] std::size_t registered_buffer_count() const noexcept;
    [[nodiscard]] std::size_t registered_file_count() const noexcept;

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
    [[nodiscard]] void_result register_buffers(
        std::span<const io_uring_registered_buffer> buffers);
    [[nodiscard]] void_result unregister_buffers();
    [[nodiscard]] void_result register_files(std::size_t count);
    [[nodiscard]] void_result register_files(
        std::span<const backend_handle_token> files);
    [[nodiscard]] void_result unregister_files();

private:
    friend void detail::attach_io_uring_test_kernel(
        io_uring_backend& backend,
        detail::io_uring_test_kernel& kernel) noexcept;

    struct queued_submission {
        backend_operation operation{};
    };
    struct kernel_operation {
        backend_operation operation{};
        std::optional<cancellation_reason> cancellation{};
        bool close_requested{};
        // True once a cancel request was submitted and not proven unsubmitted.
        bool cancel_request_submitted{};
        // True while a cancel CQE may still arrive for bookkeeping cleanup.
        bool cancel_sqe_in_flight{};
    };
    struct kernel_ring;

    [[nodiscard]] bool use_kernel_submission() const noexcept;
    [[nodiscard]] bool use_test_kernel_submission() const noexcept;
    [[nodiscard]] bool has_inflight_kernel_work() const noexcept;
    [[nodiscard]] bool registered_files_contains(
        backend_handle_token token) const noexcept;
    [[nodiscard]] void_result ensure_kernel_ring();
    [[nodiscard]] void_result validate_kernel_operation(
        const backend_operation& operation) const;
    [[nodiscard]] void_result register_buffers_with_provider(
        std::span<const io_uring_registered_buffer> buffers);
    [[nodiscard]] void_result unregister_buffers_with_provider();
    [[nodiscard]] void_result register_files_with_provider(
        std::span<const backend_handle_token> files);
    [[nodiscard]] void_result unregister_files_with_provider();
    [[nodiscard]] void_result release_registered_resources();
    [[nodiscard]] void_result release_registered_files_for(
        backend_handle_token token);
    [[nodiscard]] void_result submit_to_fallback(queued_submission queued);
    [[nodiscard]] void_result submit_to_kernel(const queued_submission& queued);
    [[nodiscard]] void_result submit_to_test_kernel(const backend_operation& operation);
    [[nodiscard]] void_result request_kernel_cancellations_for(
        backend_handle_token token);
    [[nodiscard]] void_result request_kernel_cancellation_for(
        std::size_t operation_id,
        kernel_operation& operation);
    [[nodiscard]] void_result request_test_kernel_cancellation_for(
        std::size_t operation_id,
        kernel_operation& operation);
    [[nodiscard]] io_result<std::size_t> progress_kernel_submissions();
    [[nodiscard]] io_result<std::size_t> retry_missing_kernel_cancellations();
    std::size_t complete_unsubmitted_kernel_submissions(const vio_error& error);
    [[nodiscard]] io_result<std::size_t> flush_submission_batch();
    [[nodiscard]] io_result<std::size_t> observe_completion_batch();
    [[nodiscard]] io_result<std::size_t> observe_kernel_completions();
    [[nodiscard]] io_result<std::size_t> observe_test_kernel_completions();
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
    detail::io_uring_test_kernel* test_kernel_{};
    std::vector<io_uring_registered_buffer> registered_buffers_{};
    std::vector<backend_handle_token> registered_files_{};
    bool closed_{false};
};

} // namespace voris::io::backends
