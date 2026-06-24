#include <voris/io/version.hpp>

#include <cassert>

int main() {
    assert(!voris::io::version().empty());
    return 0;
}
