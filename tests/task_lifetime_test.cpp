#include <voris/io/task.hpp>

#include "test_assert.hpp"
#include <type_traits>

int main() {
    using namespace voris::io;

    static_assert(!std::is_copy_constructible_v<task<int>>);
    static_assert(!std::is_copy_assignable_v<task<int>>);
    static_assert(std::is_move_constructible_v<task<int>>);
    static_assert(std::is_move_assignable_v<task<int>>);

    static_assert(!std::is_copy_constructible_v<task<void>>);
    static_assert(!std::is_copy_assignable_v<task<void>>);
    static_assert(std::is_move_constructible_v<task<void>>);
    static_assert(std::is_move_assignable_v<task<void>>);

    return 0;
}
