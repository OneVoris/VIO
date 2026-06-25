#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

#include <voris/io/error.hpp>
#include <voris/io/task.hpp>

namespace voris::io {

class blocking_executor;

namespace detail {

struct file_state;

} // namespace detail

enum class file_open_mode {
    read,
    write,
    read_write,
};

struct file_close_result {};

class file {
public:
    file() = default;
    ~file();

    file(const file&) = delete;
    file& operator=(const file&) = delete;

    file(file&& other) noexcept;
    file& operator=(file&& other) noexcept;

    [[nodiscard]] static io_result<file> open(const std::filesystem::path& path,
                                              file_open_mode mode);
    [[nodiscard]] static task<file> async_open(blocking_executor& executor,
                                               std::filesystem::path path,
                                               file_open_mode mode);

    [[nodiscard]] bool is_open() const noexcept;
    void_result close();
    [[nodiscard]] task<file_close_result> async_close(blocking_executor& executor);

    [[nodiscard]] io_result<std::vector<std::byte>> read_at(std::uint64_t offset,
                                                            std::size_t size);
    [[nodiscard]] io_result<std::size_t> write_at(std::uint64_t offset,
                                                  std::span<const std::byte> data);
    [[nodiscard]] task<std::vector<std::byte>> async_read_at(blocking_executor& executor,
                                                             std::uint64_t offset,
                                                             std::size_t size);
    [[nodiscard]] task<std::size_t> async_write_at(blocking_executor& executor,
                                                   std::uint64_t offset,
                                                   std::span<const std::byte> data);
    [[nodiscard]] io_result<std::uint64_t> size() const;
    [[nodiscard]] void_result truncate(std::uint64_t size);
    [[nodiscard]] void_result allocate(std::uint64_t size);
    [[nodiscard]] void_result sync_data();
    [[nodiscard]] void_result sync_all();

    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    explicit file(std::shared_ptr<detail::file_state> state) noexcept;

    std::shared_ptr<detail::file_state> state_;
};

struct sendfile_view {
    const file* source{};
    std::uint64_t offset{};
    std::uint64_t length{};
};

[[nodiscard]] io_result<sendfile_view> make_sendfile_view(const file& source,
                                                          std::uint64_t offset,
                                                          std::uint64_t length);

} // namespace voris::io
