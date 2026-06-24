#pragma once

#include <cstdio>
#include <cstdlib>

#undef assert
#define assert(expression)                                                                  \
    do {                                                                                    \
        if (!(expression)) {                                                                \
            std::fprintf(stderr, "%s:%d: assertion failed: %s\n", __FILE__, __LINE__,      \
                         #expression);                                                     \
            std::abort();                                                                   \
        }                                                                                   \
    } while (false)
