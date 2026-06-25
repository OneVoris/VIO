#include <voris/io/file.hpp>
#include <voris/io/blocking_executor.hpp>
#include <voris/io/shard.hpp>

#include <algorithm>
#include <array>
#include "test_assert.hpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

class manual_gate {
public:
    void arrive() {
        {
            std::lock_guard lock(mutex_);
            arrived_ = true;
        }
        cv_.notify_all();
    }

    void wait_for_arrival() {
        std::unique_lock lock(mutex_);
        assert(cv_.wait_for(lock, 2s, [&] { return arrived_; }));
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

    void wait_for_release() {
        std::unique_lock lock(mutex_);
        assert(cv_.wait_for(lock, 2s, [&] { return released_; }));
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool arrived_{false};
    bool released_{false};
};

std::filesystem::path temp_file(std::string name) {
    return std::filesystem::temp_directory_path() / ("vio_file_io_test_" + name + ".bin");
}

template<class Predicate>
bool wait_until(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

template<class Task>
void pump_until_ready(voris::io::shard& owner, Task& task) {
    assert(wait_until([&] {
        if (task.is_ready()) {
            return true;
        }
        auto ran = owner.run_one_loop_iteration();
        assert(ran.has_value());
        return task.is_ready();
    }));
}

void block_worker(voris::io::blocking_executor& executor, manual_gate& blocker) {
    assert(executor
               .submit([&] {
                   blocker.arrive();
                   blocker.wait_for_release();
               })
               .has_value());
    blocker.wait_for_arrival();
}

void test_sync_file_operations() {
    using namespace voris::io;

    const auto path = temp_file("sync");
    std::filesystem::remove(path);

    auto opened = file::open(path, file_open_mode::write);
    assert(opened.has_value());
    auto current = std::move(*opened);

    std::array<std::byte, 4> data{
        std::byte{'V'}, std::byte{'I'}, std::byte{'O'}, std::byte{'!'}};
    auto written = current.write_at(0, data);
    assert(written.has_value());
    assert(*written == data.size());
    assert(current.sync_all().has_value());

    auto size = current.size();
    assert(size.has_value());
    assert(*size == 4);

    auto view = make_sendfile_view(current, 0, *size);
    assert(view.has_value());
    assert(view->source == &current);
    assert(view->length == 4);

    assert(current.truncate(2).has_value());
    assert(*current.size() == 2);
    assert(current.allocate(8).has_value());
    assert(*current.size() == 8);
    assert(current.close().has_value());
    assert(!current.read_at(0, 1).has_value());

    std::filesystem::remove(path);
}

void test_async_open_runs_on_executor_and_completes_on_owner_scheduler() {
    using namespace voris::io;

    const auto path = temp_file("async_open");
    std::filesystem::remove(path);

    blocking_executor executor(1, 2);
    shard owner;
    manual_gate blocker;
    block_worker(executor, blocker);

    task<file> opened;
    {
        current_scheduler_scope scope(owner.scheduler());
        opened = file::async_open(executor, path, file_open_mode::write);
    }

    assert(!opened.is_ready());
    assert(!std::filesystem::exists(path));

    blocker.release();
    assert(wait_until([&] { return std::filesystem::exists(path); }));
    assert(!opened.is_ready());

    pump_until_ready(owner, opened);
    auto result = std::move(opened).take_result();
    assert(result.has_value());
    assert(result->is_open());
    assert(result->close().has_value());

    executor.shutdown();
    std::filesystem::remove(path);
}

void test_async_submit_failure_reports_executor_error() {
    using namespace voris::io;

    const auto path = temp_file("submit_failure");
    std::filesystem::remove(path);

    shard owner;
    blocking_executor zero_capacity(1, 0);
    {
        current_scheduler_scope scope(owner.scheduler());
        auto opened = file::async_open(zero_capacity, path, file_open_mode::write);
        assert(opened.is_ready());
        auto result = std::move(opened).take_result();
        assert(!result.has_value());
        assert(result.error().classification == vio_error_code::resource_exhausted);
    }
    zero_capacity.shutdown();

    auto sync_opened = file::open(path, file_open_mode::write);
    assert(sync_opened.has_value());

    blocking_executor stopped(1, 1);
    stopped.shutdown();
    {
        current_scheduler_scope scope(owner.scheduler());
        auto read = sync_opened->async_read_at(stopped, 0, 1);
        assert(read.is_ready());
        auto result = std::move(read).take_result();
        assert(!result.has_value());
        assert(result.error().classification == vio_error_code::closed);
    }

    assert(sync_opened->close().has_value());
    std::filesystem::remove(path);
}

void test_async_file_operation_reports_resource_exhausted_when_queue_is_full() {
    using namespace voris::io;

    const auto path = temp_file("saturated_pool");
    std::filesystem::remove(path);

    blocking_executor executor(1, 1);
    shard owner;
    manual_gate blocker;
    block_worker(executor, blocker);

    auto filled = executor.submit([] {});
    assert(filled.has_value());
    assert(executor.queued() == 1);

    {
        current_scheduler_scope scope(owner.scheduler());
        auto opened = file::async_open(executor, path, file_open_mode::write);
        assert(opened.is_ready());
        auto result = std::move(opened).take_result();
        assert(!result.has_value());
        assert(result.error().classification == vio_error_code::resource_exhausted);
    }

    assert(!std::filesystem::exists(path));

    blocker.release();
    executor.shutdown();
    std::filesystem::remove(path);
}

void test_executor_shutdown_drains_queued_file_work() {
    using namespace voris::io;

    const auto path = temp_file("shutdown_drains");
    std::filesystem::remove(path);

    auto opened = file::open(path, file_open_mode::write);
    assert(opened.has_value());
    auto current = std::move(*opened);

    blocking_executor executor(1, 2);
    shard owner;
    manual_gate blocker;
    block_worker(executor, blocker);

    std::array<std::byte, 3> data{
        std::byte{'d'}, std::byte{'r'}, std::byte{'n'}};

    task<std::size_t> written;
    {
        current_scheduler_scope scope(owner.scheduler());
        written = current.async_write_at(executor, 0, data);
    }

    assert(!written.is_ready());
    assert(executor.queued() == 1);
    auto initial_size = current.size();
    assert(initial_size.has_value());
    assert(*initial_size == 0);

    std::thread shutdown_thread([&] { executor.shutdown(); });
    assert(wait_until([&] { return executor.shutting_down(); }));
    assert(!written.is_ready());

    blocker.release();
    shutdown_thread.join();

    pump_until_ready(owner, written);
    auto write_result = std::move(written).take_result();
    assert(write_result.has_value());
    assert(*write_result == data.size());

    auto read_back = current.read_at(0, data.size());
    assert(read_back.has_value());
    assert(read_back->size() == data.size());
    assert((*read_back)[0] == std::byte{'d'});
    assert((*read_back)[1] == std::byte{'r'});
    assert((*read_back)[2] == std::byte{'n'});

    assert(current.close().has_value());
    std::filesystem::remove(path);
}

void test_async_open_missing_read_reports_disk_backend_failure() {
    using namespace voris::io;

    const auto path = temp_file("missing_read");
    std::filesystem::remove(path);
    assert(!std::filesystem::exists(path));

    blocking_executor executor(1, 1);
    shard owner;

    task<file> opened;
    {
        current_scheduler_scope scope(owner.scheduler());
        opened = file::async_open(executor, path, file_open_mode::read);
    }

    assert(!opened.is_ready());
    pump_until_ready(owner, opened);
    auto result = std::move(opened).take_result();
    assert(!result.has_value());
    assert(result.error().classification == vio_error_code::backend_failure);

    executor.shutdown();
    std::filesystem::remove(path);
}

void test_async_read_past_eof_returns_short_buffer() {
    using namespace voris::io;

    const auto path = temp_file("short_read");
    std::filesystem::remove(path);

    auto opened = file::open(path, file_open_mode::write);
    assert(opened.has_value());
    auto current = std::move(*opened);

    std::array<std::byte, 4> data{
        std::byte{'V'}, std::byte{'I'}, std::byte{'O'}, std::byte{'!'}};
    auto written = current.write_at(0, data);
    assert(written.has_value());
    assert(*written == data.size());
    assert(current.sync_all().has_value());

    blocking_executor executor(1, 2);
    shard owner;

    {
        current_scheduler_scope scope(owner.scheduler());
        auto short_read = current.async_read_at(executor, 2, 16);
        pump_until_ready(owner, short_read);
        auto result = std::move(short_read).take_result();
        assert(result.has_value());
        assert(result->size() == 2);
        assert((*result)[0] == std::byte{'O'});
        assert((*result)[1] == std::byte{'!'});
    }

    {
        current_scheduler_scope scope(owner.scheduler());
        auto past_eof = current.async_read_at(executor, 32, 8);
        pump_until_ready(owner, past_eof);
        auto result = std::move(past_eof).take_result();
        assert(result.has_value());
        assert(result->empty());
    }

    executor.shutdown();
    assert(current.close().has_value());
    std::filesystem::remove(path);
}

void test_async_zero_length_and_offset_writes_are_successful() {
    using namespace voris::io;

    const auto path = temp_file("zero_offset_writes");
    std::filesystem::remove(path);

    auto opened = file::open(path, file_open_mode::write);
    assert(opened.has_value());
    auto current = std::move(*opened);

    std::array<std::byte, 2> prefix{std::byte{'O'}, std::byte{'K'}};
    auto initial_write = current.write_at(0, prefix);
    assert(initial_write.has_value());
    assert(*initial_write == prefix.size());

    blocking_executor executor(1, 3);
    shard owner;

    std::array<std::byte, 0> empty{};
    {
        current_scheduler_scope scope(owner.scheduler());
        auto zero_write = current.async_write_at(executor, 1, empty);
        pump_until_ready(owner, zero_write);
        auto result = std::move(zero_write).take_result();
        assert(result.has_value());
        assert(*result == 0);
    }

    {
        current_scheduler_scope scope(owner.scheduler());
        auto zero_read = current.async_read_at(executor, 1, 0);
        pump_until_ready(owner, zero_read);
        auto result = std::move(zero_read).take_result();
        assert(result.has_value());
        assert(result->empty());
    }

    std::array<std::byte, 1> suffix{std::byte{'!'}};
    {
        current_scheduler_scope scope(owner.scheduler());
        auto sparse_write = current.async_write_at(executor, 5, suffix);
        pump_until_ready(owner, sparse_write);
        auto result = std::move(sparse_write).take_result();
        assert(result.has_value());
        assert(*result == suffix.size());
    }

    assert(current.sync_all().has_value());
    auto size = current.size();
    assert(size.has_value());
    assert(*size == 6);

    auto suffix_read = current.read_at(5, 1);
    assert(suffix_read.has_value());
    assert(suffix_read->size() == 1);
    assert((*suffix_read)[0] == std::byte{'!'});

    executor.shutdown();
    assert(current.close().has_value());
    std::filesystem::remove(path);
}

void test_async_write_owns_input_buffer_snapshot() {
    using namespace voris::io;

    const auto path = temp_file("write_snapshot");
    std::filesystem::remove(path);

    auto opened = file::open(path, file_open_mode::write);
    assert(opened.has_value());
    auto current = std::move(*opened);

    blocking_executor executor(1, 2);
    shard owner;
    manual_gate blocker;
    block_worker(executor, blocker);

    std::vector<std::byte> data{
        std::byte{'V'}, std::byte{'I'}, std::byte{'O'}, std::byte{'!'}};

    task<std::size_t> written;
    {
        current_scheduler_scope scope(owner.scheduler());
        written = current.async_write_at(executor, 0, data);
    }

    std::ranges::fill(data, std::byte{'x'});
    blocker.release();

    pump_until_ready(owner, written);
    auto write_result = std::move(written).take_result();
    assert(write_result.has_value());
    assert(*write_result == 4);

    assert(current.sync_all().has_value());
    task<std::vector<std::byte>> read_back_task;
    {
        current_scheduler_scope scope(owner.scheduler());
        read_back_task = current.async_read_at(executor, 0, 4);
    }
    pump_until_ready(owner, read_back_task);
    auto read_back = std::move(read_back_task).take_result();
    assert(read_back.has_value());
    assert(read_back->size() == 4);
    assert((*read_back)[0] == std::byte{'V'});
    assert((*read_back)[1] == std::byte{'I'});
    assert((*read_back)[2] == std::byte{'O'});
    assert((*read_back)[3] == std::byte{'!'});

    executor.shutdown();
    assert(current.close().has_value());
    std::filesystem::remove(path);
}

void test_async_close_then_async_read_write_report_closed() {
    using namespace voris::io;

    const auto path = temp_file("async_close");
    std::filesystem::remove(path);

    auto opened = file::open(path, file_open_mode::write);
    assert(opened.has_value());
    auto current = std::move(*opened);

    blocking_executor executor(1, 4);
    shard owner;

    {
        current_scheduler_scope scope(owner.scheduler());
        auto close = current.async_close(executor);
        pump_until_ready(owner, close);
        auto close_result = std::move(close).take_result();
        assert(close_result.has_value());
    }

    std::array<std::byte, 1> one{std::byte{'!'}};

    {
        current_scheduler_scope scope(owner.scheduler());
        auto read = current.async_read_at(executor, 0, 1);
        pump_until_ready(owner, read);
        auto read_result = std::move(read).take_result();
        assert(!read_result.has_value());
        assert(read_result.error().classification == vio_error_code::closed);

        auto write = current.async_write_at(executor, 0, one);
        pump_until_ready(owner, write);
        auto write_result = std::move(write).take_result();
        assert(!write_result.has_value());
        assert(write_result.error().classification == vio_error_code::closed);
    }

    executor.shutdown();
    std::filesystem::remove(path);
}

} // namespace

int main() {
    test_sync_file_operations();
    test_async_open_runs_on_executor_and_completes_on_owner_scheduler();
    test_async_submit_failure_reports_executor_error();
    test_async_file_operation_reports_resource_exhausted_when_queue_is_full();
    test_executor_shutdown_drains_queued_file_work();
    test_async_open_missing_read_reports_disk_backend_failure();
    test_async_read_past_eof_returns_short_buffer();
    test_async_zero_length_and_offset_writes_are_successful();
    test_async_write_owns_input_buffer_snapshot();
    test_async_close_then_async_read_write_report_closed();
    return 0;
}
