# Backend Capabilities, Limits, and Failure Modes

| Backend | Default | Platforms | Notes |
|---|---:|---|---|
| virtual | no | all | Deterministic contract backend for tests. |
| epoll | Linux default | Linux | Readiness backend; real epoll/eventfd validation requires Linux CI. |
| io_uring | no | Linux | Optional completion backend; default selection falls back to epoll unless core capabilities, exactly-once cancellation races, behavioral differential tests, benchmark evidence, and Linux real-provider validation are all satisfied. |
| kqueue | platform default | macOS/BSD | Readiness backend; requires macOS/BSD CI. |
| IOCP | platform default | Windows | Completion backend; owns an IOCP port, associates handles with generation-aware keys, and drains bounded native batches. Native overlapped operation storage/completion mapping is scheduled for M7-003. |

Common failure classifications:

- `invalid_state`: bad handle token, missing scheduler, invalid options.
- `resource_exhausted`: bounded queue or operation queue full.
- `closed`: operation submitted after shutdown or close.
- `unsupported`: backend unavailable on the current platform.
- `backend_failure`: provider or filesystem failure.

All backends must satisfy ADR 0002 and ADR 0004 before release.

For Linux, `io_uring_backend::default_eligible()` means the detected core
io_uring operation set is present. It is not by itself a default selection
decision. `select_default_linux_backend(capabilities, evidence)` returns epoll
when io_uring is unavailable, when any core opcode is missing, or when any M6
default-enable evidence gate is absent. Registered buffers and registered files
remain optional optimizations and are not required for default eligibility.
