#pragma once

#include <cstddef>
#include <unordered_map>

#include <voris/io/error.hpp>

namespace voris::io::detail {

struct native_handle_token {
    std::size_t id{};
    std::size_t generation{};
};

class native_handle_registry {
public:
    [[nodiscard]] native_handle_token register_handle(std::size_t native_handle);
    [[nodiscard]] void_result close(native_handle_token token);
    [[nodiscard]] bool is_current(native_handle_token token) const noexcept;
    [[nodiscard]] std::size_t generation(std::size_t native_handle) const noexcept;

private:
    struct entry {
        std::size_t generation{0};
        bool open{false};
    };

    std::unordered_map<std::size_t, entry> entries_;
};

} // namespace voris::io::detail
