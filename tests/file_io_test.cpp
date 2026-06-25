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

void test_async_open_failure_is_returned_as_result() {
    using namespace voris::io;

    const auto path = temp_file("missing_read");
    std::filesystem::remove(path);

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
    test_async_open_failure_is_returned_as_result();
    test_async_write_owns_input_buffer_snapshot();
    test_async_close_then_async_read_write_report_closed();
    return 0;
}
