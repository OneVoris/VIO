#include "backend_contract_suite.hpp"

int main() {
    vio_backend_contract_tests::run_virtual_backend_contract_suite();
    vio_backend_contract_tests::run_epoll_backend_contract_suite();
    vio_backend_contract_tests::run_kqueue_backend_contract_suite();
    vio_backend_contract_tests::run_io_uring_backend_contract_suite();
    vio_backend_contract_tests::run_iocp_backend_contract_suite();

    return 0;
}
