#pragma once

#include <tuple>
#include <utility>

#include <voris/io/task.hpp>

namespace voris::io {

template<class... Tasks>
[[nodiscard]] auto when_all(Tasks&&... tasks) {
    return [] (Tasks&&... inner_tasks)
        -> task<std::tuple<typename std::remove_cvref_t<Tasks>::result_type...>> {
        co_return std::tuple<typename std::remove_cvref_t<Tasks>::result_type...>{
            std::move(inner_tasks).take_result()...};
    }(std::forward<Tasks>(tasks)...);
}

} // namespace voris::io
