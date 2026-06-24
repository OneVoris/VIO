#include <voris/io/file.hpp>

#include <array>
#include <cassert>
#include <filesystem>

int main() {
    using namespace voris::io;

    const auto path = std::filesystem::temp_directory_path() / "vio_file_io_test.bin";
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
    return 0;
}
