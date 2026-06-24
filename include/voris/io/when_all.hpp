#pragma once

#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

#include <voris/io/task.hpp>

namespace voris::io {

namespace detail {

template<class TaskTuple, std::size_t... Indices>
[[nodiscard]] auto when_all_impl(TaskTuple tasks, std::index_sequence<Indices...>)
    -> task<std::tuple<typename std::tuple_element_t<Indices, TaskTuple>::result_type...>> {
    co_return std::tuple<typename std::tuple_element_t<Indices, TaskTuple>::result_type...>{
        (co_await std::move(std::get<Indices>(tasks)))...};
}

} // namespace detail

template<class... Tasks>
[[nodiscard]] auto when_all(Tasks&&... tasks) {
    using task_tuple = std::tuple<std::remove_cvref_t<Tasks>...>;
    return detail::when_all_impl(task_tuple{std::forward<Tasks>(tasks)...},
                                 std::index_sequence_for<Tasks...>{});
}

} // namespace voris::io
