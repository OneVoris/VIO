#include <voris/io/file.hpp>

#include <array>
#include <cassert>
#include <filesystem>

int main() {
    const auto src = std::filesystem::temp_directory_path() / "vio_example_src.bin";
    const auto dst = std::filesystem::temp_directory_path() / "vio_example_dst.bin";
    std::filesystem::remove(src);
    std::filesystem::remove(dst);

    auto source = voris::io::file::open(src, voris::io::file_open_mode::write);
    assert(source.has_value());
    std::array<std::byte, 1> data{std::byte{'x'}};
    assert(source->write_at(0, data).has_value());
    assert(source->close().has_value());

    std::filesystem::copy_file(src, dst);
    std::filesystem::remove(src);
    std::filesystem::remove(dst);
    return 0;
}
