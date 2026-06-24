#pragma once

#include <cstddef>
#include <utility>
#include <variant>

#include <voris/io/cancellation.hpp>
#include <voris/io/task.hpp>

namespace voris::io {

template<class... Results>
struct when_any_result {
    std::size_t index{};
    std::variant<Results...> result;
};

template<class FirstTask, class... RestTasks>
[[nodiscard]] auto when_any(cancellation_source& losers_source,
                            FirstTask&& first,
                            RestTasks&&... rest) {
    return [] (cancellation_source& source, FirstTask&& first_task, RestTasks&&... rest_tasks)
        -> task<when_any_result<typename std::remove_cvref_t<FirstTask>::result_type,
                                typename std::remove_cvref_t<RestTasks>::result_type...>> {
        auto first_result = std::move(first_task).take_result();
        (void)source.request_cancellation(cancellation_reason::manual);
        (void)std::initializer_list<int>{((void)std::move(rest_tasks).take_result(), 0)...};
        co_return when_any_result<typename std::remove_cvref_t<FirstTask>::result_type,
                                  typename std::remove_cvref_t<RestTasks>::result_type...>{
            0, std::variant<typename std::remove_cvref_t<FirstTask>::result_type,
                            typename std::remove_cvref_t<RestTasks>::result_type...>{
                std::in_place_index<0>, std::move(first_result)}};
    }(losers_source, std::forward<FirstTask>(first), std::forward<RestTasks>(rest)...);
}

} // namespace voris::io
