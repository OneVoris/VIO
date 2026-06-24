#pragma once

#include <cstddef>

namespace voris::io {

struct loop_budget {
    std::size_t task_budget{64};
    std::size_t completion_budget{64};
    std::size_t timer_budget{64};
};

} // namespace voris::io
