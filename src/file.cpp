#include <voris/io/file.hpp>
#include <voris/io/blocking_executor.hpp>
#include <voris/io/trampoline.hpp>

#include <algorithm>
#include <coroutine>
#include <exception>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <utility>

namespace voris::io {

namespace detail {

struct file_state {
    file_state(std::filesystem::path file_path, std::fstream file_stream)
        : path(std::move(file_path)),
          stream(std::move(file_stream)) {}

    std::filesystem::path path;
    mutable std::mutex mutex;
    std::fstream stream;
};

} // namespace detail

namespace {

std::ios::openmode to_openmode(file_open_mode mode) {
    switch (mode) {
    case file_open_mode::read:
        return std::ios::binary | std::ios::in;
    case file_open_mode::write:
        return std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc;
    case file_open_mode::read_write:
        return std::ios::binary | std::ios::in | std::ios::out;
    }
    return std::ios::binary | std::ios::in;
}

const std::filesystem::path& empty_path() noexcept {
    static const std::filesystem::path empty;
    return empty;
}

[[nodiscard]] bool is_open_state(const std::shared_ptr<detail::file_state>& state) noexcept {
    if (!state) {
        return false;
    }
    std::scoped_lock lock(state->mutex);
    return state->stream.is_open();
}

[[nodiscard]] void_result close_state(const std::shared_ptr<detail::file_state>& state) {
    if (!state) {
        return {};
    }
    std::scoped_lock lock(state->mutex);
    if (state->stream.is_open()) {
        state->stream.close();
    }
    return {};
}

[[nodiscard]] io_result<std::vector<std::byte>> read_at_state(
    const std::shared_ptr<detail::file_state>& state,
    std::uint64_t offset,
    std::size_t read_size) {
    if (!state) {
        return std::unexpected(make_error(vio_error_code::closed));
    }

    std::scoped_lock lock(state->mutex);
    if (!state->stream.is_open()) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    std::vector<std::byte> buffer(read_size);
    state->stream.clear();
    state->stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    state->stream.read(reinterpret_cast<char*>(buffer.data()),
                       static_cast<std::streamsize>(read_size));
    buffer.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, state->stream.gcount())));
    return buffer;
}

[[nodiscard]] io_result<std::size_t> write_at_state(
    const std::shared_ptr<detail::file_state>& state,
    std::uint64_t offset,
    std::span<const std::byte> data) {
    if (!state) {
        return std::unexpected(make_error(vio_error_code::closed));
    }

    std::scoped_lock lock(state->mutex);
    if (!state->stream.is_open()) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    state->stream.clear();
    state->stream.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    state->stream.write(reinterpret_cast<const char*>(data.data()),
                        static_cast<std::streamsize>(data.size()));
    if (!state->stream) {
        return std::unexpected(make_error(vio_error_code::backend_failure,
                                          "file write failed"));
    }
    return data.size();
}

[[nodiscard]] io_result<std::uint64_t> size_state(
    const std::shared_ptr<detail::file_state>& state) {
    const auto size_path = [](const std::filesystem::path& path) -> io_result<std::uint64_t> {
        std::error_code ec;
        const auto value = std::filesystem::file_size(path, ec);
        if (ec) {
            return std::unexpected(make_error(vio_error_code::backend_failure,
                                              static_cast<std::int64_t>(ec.value()),
                                              ec.message()));
        }
        return static_cast<std::uint64_t>(value);
    };

    if (!state) {
        return size_path(empty_path());
    }

    std::scoped_lock lock(state->mutex);
    return size_path(state->path);
}

[[nodiscard]] void_result truncate_state(const std::shared_ptr<detail::file_state>& state,
                                         std::uint64_t new_size) {
    const auto resize_path = [](const std::filesystem::path& path,
                                std::uint64_t size) -> void_result {
        std::error_code ec;
        std::filesystem::resize_file(path, size, ec);
        if (ec) {
            return std::unexpected(make_error(vio_error_code::backend_failure,
                                              static_cast<std::int64_t>(ec.value()),
                                              ec.message()));
        }
        return {};
    };

    if (!state) {
        return resize_path(empty_path(), new_size);
    }

    std::scoped_lock lock(state->mutex);
    return resize_path(state->path, new_size);
}

[[nodiscard]] void_result allocate_state(const std::shared_ptr<detail::file_state>& state,
                                         std::uint64_t allocation_size) {
    auto current_size = size_state(state);
    if (!current_size.has_value()) {
        return std::unexpected(current_size.error());
    }
    if (*current_size >= allocation_size) {
        return {};
    }
    return truncate_state(state, allocation_size);
}

[[nodiscard]] void_result sync_data_state(const std::shared_ptr<detail::file_state>& state) {
    if (!state) {
        return std::unexpected(make_error(vio_error_code::closed));
    }

    std::scoped_lock lock(state->mutex);
    if (!state->stream.is_open()) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    state->stream.flush();
    return {};
}

template<class T>
class blocking_file_operation {
    struct operation_state;

public:
    using result_type = io_result<T>;
    using work_type = detail::move_only_function<result_type()>;

    blocking_file_operation(blocking_executor& executor, scheduler_ref scheduler, work_type work)
        : state_(std::make_shared<operation_state>(scheduler, std::move(work))) {
        auto submitted = executor.submit([state = state_] { state->run(); });
        if (!submitted.has_value()) {
            state_->complete_submit_failure(submitted.error());
        }
    }

    class awaiter {
    public:
        explicit awaiter(std::shared_ptr<operation_state> state) noexcept
            : state_(std::move(state)) {}

        awaiter(const awaiter&) = delete;
        awaiter& operator=(const awaiter&) = delete;

        awaiter(awaiter&& other) noexcept
            : state_(std::move(other.state_)) {}

        ~awaiter() {
            if (state_) {
                state_->detach();
            }
        }

        [[nodiscard]] bool await_ready() const noexcept {
            return state_->ready_inline();
        }

        [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation) const {
            return state_->install(continuation);
        }

        result_type await_resume() {
            return state_->take_result();
        }

    private:
        std::shared_ptr<operation_state> state_;
    };

    awaiter operator co_await() && noexcept {
        return awaiter(std::move(state_));
    }

private:
    struct operation_state : std::enable_shared_from_this<operation_state> {
        operation_state(scheduler_ref resume_scheduler, work_type operation_work)
            : scheduler(resume_scheduler),
              work(std::move(operation_work)) {}

        [[nodiscard]] bool ready_inline() const noexcept {
            std::scoped_lock lock(mutex);
            return !submitted && result.has_value();
        }

        [[nodiscard]] bool install(std::coroutine_handle<> next) {
            bool schedule_now = false;
            {
                std::scoped_lock lock(mutex);
                if (detached) {
                    return true;
                }
                continuation = next;
                schedule_now = result.has_value();
            }

            if (schedule_now) {
                schedule_resume();
            }
            return true;
        }

        void detach() noexcept {
            std::scoped_lock lock(mutex);
            detached = true;
            continuation = {};
        }

        result_type take_result() {
            std::scoped_lock lock(mutex);
            if (!result.has_value()) {
                return std::unexpected(make_error(vio_error_code::invalid_state,
                                                  "file operation has not completed"));
            }
            return std::move(*result);
        }

        void complete_submit_failure(vio_error error) {
            std::scoped_lock lock(mutex);
            submitted = false;
            if (!result.has_value()) {
                result.emplace(std::unexpected(std::move(error)));
            }
        }

        void run() noexcept {
            result_type outcome = invoke_work();
            publish(std::move(outcome));
        }

        result_type invoke_work() noexcept {
            if (!work) {
                return std::unexpected(make_error(vio_error_code::invalid_state,
                                                  "missing blocking file operation"));
            }

            try {
                return work();
            } catch (...) {
                return std::unexpected(make_error(vio_error_code::invalid_state,
                                                  "blocking file operation threw"));
            }
        }

        void publish(result_type outcome) noexcept {
            bool schedule_now = false;
            {
                std::scoped_lock lock(mutex);
                if (result.has_value()) {
                    return;
                }
                result.emplace(std::move(outcome));
                schedule_now = !detached && continuation;
            }

            if (schedule_now) {
                schedule_resume();
            }
        }

        void schedule_resume() noexcept {
            auto self = this->shared_from_this();
            auto scheduled = trampoline::schedule_system(scheduler, [state = std::move(self)] {
                state->resume_if_claimed();
            });
            if (!scheduled.has_value()) {
                std::terminate();
            }
        }

        void resume_if_claimed() noexcept {
            std::coroutine_handle<> next{};
            {
                std::scoped_lock lock(mutex);
                if (detached || !continuation) {
                    return;
                }
                next = continuation;
                continuation = {};
            }

            current_scheduler_scope scope(scheduler);
            next.resume();
        }

        scheduler_ref scheduler;
        mutable std::mutex mutex;
        std::optional<result_type> result;
        std::coroutine_handle<> continuation{};
        bool submitted{true};
        bool detached{false};
        work_type work;
    };

    std::shared_ptr<operation_state> state_;
};

} // namespace

file::file(std::shared_ptr<detail::file_state> state) noexcept
    : state_(std::move(state)) {}

file::~file() {
    (void)close();
}

file::file(file&& other) noexcept
    : state_(std::move(other.state_)) {}

file& file::operator=(file&& other) noexcept {
    if (this != &other) {
        (void)close();
        state_ = std::move(other.state_);
    }
    return *this;
}

io_result<file> file::open(const std::filesystem::path& path, file_open_mode mode) {
    std::fstream stream(path, to_openmode(mode));
    if (!stream.is_open()) {
        return std::unexpected(make_error(vio_error_code::backend_failure,
                                          "file open failed"));
    }
    return file(std::make_shared<detail::file_state>(path, std::move(stream)));
}

task<file> file::async_open(blocking_executor& executor,
                            std::filesystem::path path,
                            file_open_mode mode) {
    auto scheduler = require_current_scheduler();
    if (!scheduler.has_value()) {
        co_return std::unexpected(scheduler.error());
    }

    auto result = co_await blocking_file_operation<file>(
        executor, *scheduler, [path = std::move(path), mode]() mutable -> io_result<file> {
            return file::open(path, mode);
        });
    co_return std::move(result);
}

bool file::is_open() const noexcept {
    return is_open_state(state_);
}

void_result file::close() {
    return close_state(state_);
}

task<file_close_result> file::async_close(blocking_executor& executor) {
    auto scheduler = require_current_scheduler();
    if (!scheduler.has_value()) {
        co_return std::unexpected(scheduler.error());
    }

    auto state = state_;
    auto result = co_await blocking_file_operation<file_close_result>(
        executor, *scheduler, [state = std::move(state)]() -> io_result<file_close_result> {
            auto closed = close_state(state);
            if (!closed.has_value()) {
                return std::unexpected(closed.error());
            }
            return file_close_result{};
        });
    co_return std::move(result);
}

io_result<std::vector<std::byte>> file::read_at(std::uint64_t offset, std::size_t read_size) {
    return read_at_state(state_, offset, read_size);
}

io_result<std::size_t> file::write_at(std::uint64_t offset, std::span<const std::byte> data) {
    return write_at_state(state_, offset, data);
}

task<std::vector<std::byte>> file::async_read_at(blocking_executor& executor,
                                                 std::uint64_t offset,
                                                 std::size_t read_size) {
    auto scheduler = require_current_scheduler();
    if (!scheduler.has_value()) {
        co_return std::unexpected(scheduler.error());
    }

    auto state = state_;
    auto result = co_await blocking_file_operation<std::vector<std::byte>>(
        executor, *scheduler, [state = std::move(state), offset, read_size]() {
            return read_at_state(state, offset, read_size);
        });
    co_return std::move(result);
}

task<std::size_t> file::async_write_at(blocking_executor& executor,
                                       std::uint64_t offset,
                                       std::span<const std::byte> data) {
    auto scheduler = require_current_scheduler();
    if (!scheduler.has_value()) {
        co_return std::unexpected(scheduler.error());
    }

    std::vector<std::byte> owned_data(data.begin(), data.end());
    auto state = state_;
    auto result = co_await blocking_file_operation<std::size_t>(
        executor, *scheduler,
        [state = std::move(state), offset, data = std::move(owned_data)]() mutable {
            return write_at_state(state, offset, data);
        });
    co_return std::move(result);
}

io_result<std::uint64_t> file::size() const {
    return size_state(state_);
}

void_result file::truncate(std::uint64_t new_size) {
    return truncate_state(state_, new_size);
}

void_result file::allocate(std::uint64_t allocation_size) {
    return allocate_state(state_, allocation_size);
}

void_result file::sync_data() {
    return sync_data_state(state_);
}

void_result file::sync_all() {
    return sync_data();
}

const std::filesystem::path& file::path() const noexcept {
    return state_ ? state_->path : empty_path();
}

io_result<sendfile_view> make_sendfile_view(const file& source,
                                            std::uint64_t offset,
                                            std::uint64_t length) {
    if (!source.is_open()) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    return sendfile_view{&source, offset, length};
}

} // namespace voris::io
