#include <voris/io/runtime.hpp>

#include <cassert>

int main() {
    voris::io::runtime_options options;
    auto runtime = voris::io::runtime::create(options);
    assert(runtime.has_value());
    runtime->start();
    runtime->request_stop();
    runtime->join();
    return 0;
}
