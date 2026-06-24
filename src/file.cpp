#include <voris/io/file.hpp>

#include <algorithm>
#include <system_error>
#include <utility>

namespace voris::io {

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

} // namespace

file::file(std::filesystem::path path, std::fstream stream)
    : path_(std::move(path)),
      stream_(std::move(stream)) {}

file::~file() {
    (void)close();
}

file::file(file&& other) noexcept
    : path_(std::move(other.path_)),
      stream_(std::move(other.stream_)) {}

file& file::operator=(file&& other) noexcept {
    if (this != &other) {
        (void)close();
        path_ = std::move(other.path_);
        stream_ = std::move(other.stream_);
    }
    return *this;
}

io_result<file> file::open(const std::filesystem::path& path, file_open_mode mode) {
    std::fstream stream(path, to_openmode(mode));
    if (!stream.is_open()) {
        return std::unexpected(make_error(vio_error_code::backend_failure,
                                          "file open failed"));
    }
    return file(path, std::move(stream));
}

bool file::is_open() const noexcept {
    return stream_.is_open();
}

void_result file::close() {
    if (stream_.is_open()) {
        stream_.close();
    }
    return {};
}

io_result<std::vector<std::byte>> file::read_at(std::uint64_t offset, std::size_t read_size) {
    if (!stream_.is_open()) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    std::vector<std::byte> buffer(read_size);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(read_size));
    buffer.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, stream_.gcount())));
    return buffer;
}

io_result<std::size_t> file::write_at(std::uint64_t offset, std::span<const std::byte> data) {
    if (!stream_.is_open()) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    stream_.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    if (!stream_) {
        return std::unexpected(make_error(vio_error_code::backend_failure,
                                          "file write failed"));
    }
    return data.size();
}

io_result<std::uint64_t> file::size() const {
    std::error_code ec;
    const auto value = std::filesystem::file_size(path_, ec);
    if (ec) {
        return std::unexpected(make_error(vio_error_code::backend_failure,
                                          static_cast<std::int64_t>(ec.value()),
                                          ec.message()));
    }
    return static_cast<std::uint64_t>(value);
}

void_result file::truncate(std::uint64_t new_size) {
    std::error_code ec;
    std::filesystem::resize_file(path_, new_size, ec);
    if (ec) {
        return std::unexpected(make_error(vio_error_code::backend_failure,
                                          static_cast<std::int64_t>(ec.value()),
                                          ec.message()));
    }
    return {};
}

void_result file::allocate(std::uint64_t allocation_size) {
    auto current_size = size();
    if (!current_size.has_value()) {
        return std::unexpected(current_size.error());
    }
    if (*current_size >= allocation_size) {
        return {};
    }
    return truncate(allocation_size);
}

void_result file::sync_data() {
    if (!stream_.is_open()) {
        return std::unexpected(make_error(vio_error_code::closed));
    }
    stream_.flush();
    return {};
}

void_result file::sync_all() {
    return sync_data();
}

const std::filesystem::path& file::path() const noexcept {
    return path_;
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
