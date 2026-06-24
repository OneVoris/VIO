#include <voris/io/version.hpp>

#include "test_assert.hpp"

int main() {
    assert(!voris::io::version().empty());
    return 0;
}
