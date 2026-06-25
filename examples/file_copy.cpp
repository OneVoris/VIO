#include <voris/io/file.hpp>
#include <voris/io/blocking_executor.hpp>
#include <voris/io/shard.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <exception>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

template<class Task>
void pump_until_ready(voris::io::shard& owner, Task& task) {
    using namespace std::chrono_literals;

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!task.is_ready() && std::chrono::steady_clock::now() < deadline) {
        auto ran = owner.run_one_loop_iteration();
        assert(ran.has_value());
        if (*ran == 0) {
            std::this_thread::yield();
        }
    }
    assert(task.is_ready());
}

class temp_directory {
public:
    temp_directory()
        : path_(create_unique_path()) {}

    temp_directory(const temp_directory&) = delete;
    temp_directory& operator=(const temp_directory&) = delete;

    ~temp_directory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] std::filesystem::path source_path() const {
        return path_ / "source.bin";
    }

    [[nodiscard]] std::filesystem::path destination_path() const {
        return path_ / "destination.bin";
    }

private:
    [[nodiscard]] static std::filesystem::path create_unique_path() {
        const auto base = std::filesystem::temp_directory_path();
        std::random_device seed;
        std::mt19937_64 random(seed());

        for (int attempt = 0; attempt != 16; ++attempt) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
            const auto name = "vio_example_file_copy_" + std::to_string(now) + "_" +
                              std::to_string(random());
            auto candidate = base / name;

            std::error_code ec;
            if (std::filesystem::create_directory(candidate, ec)) {
                return candidate;
            }
        }

        std::terminate();
    }

    std::filesystem::path path_;
};

} // namespace

int main() {
    const temp_directory temp;
    const auto src = temp.source_path();
    const auto dst = temp.destination_path();

    auto source = voris::io::file::open(src, voris::io::file_open_mode::write);
    assert(source.has_value());
    std::array<std::byte, 4> data{
        std::byte{'V'}, std::byte{'I'}, std::byte{'O'}, std::byte{'!'}};
    assert(source->write_at(0, data).has_value());
    assert(source->sync_all().has_value());
    assert(source->close().has_value());

    voris::io::blocking_executor executor(1, 4);
    voris::io::shard owner;

    auto readable = voris::io::file::open(src, voris::io::file_open_mode::read);
    assert(readable.has_value());

    auto writable = voris::io::file::open(dst, voris::io::file_open_mode::write);
    assert(writable.has_value());

    std::vector<std::byte> copied;
    {
        voris::io::current_scheduler_scope scope(owner.scheduler());
        auto read = readable->async_read_at(executor, 0, data.size());
        pump_until_ready(owner, read);
        auto read_result = std::move(read).take_result();
        assert(read_result.has_value());
        copied = std::move(*read_result);
    }

    assert(copied.size() == data.size());
    {
        voris::io::current_scheduler_scope scope(owner.scheduler());
        auto write = writable->async_write_at(executor, 0, copied);
        pump_until_ready(owner, write);
        auto write_result = std::move(write).take_result();
        assert(write_result.has_value());
        assert(*write_result == copied.size());
    }

    assert(writable->sync_all().has_value());

    auto read_back = writable->read_at(0, data.size());
    assert(read_back.has_value());
    assert(read_back->size() == data.size());
    assert(*read_back == std::vector<std::byte>(data.begin(), data.end()));

    assert(readable->close().has_value());
    assert(writable->close().has_value());
    executor.shutdown();

    return 0;
}
