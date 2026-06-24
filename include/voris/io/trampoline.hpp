#pragma once

#include <voris/io/scheduler.hpp>

namespace voris::io {

class trampoline {
public:
    [[nodiscard]] static void_result schedule(scheduler_ref scheduler, continuation next);
    [[nodiscard]] static void_result schedule_system(scheduler_ref scheduler, continuation next);
};

} // namespace voris::io
