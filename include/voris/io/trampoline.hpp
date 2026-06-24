#pragma once

#include <voris/io/scheduler.hpp>

namespace voris::io {

class trampoline {
public:
    static void schedule(scheduler_ref scheduler, continuation next);
};

} // namespace voris::io
