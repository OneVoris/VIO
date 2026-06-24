# Backend Capabilities, Limits, and Failure Modes

| Backend | Default | Platforms | Notes |
|---|---:|---|---|
| virtual | no | all | Deterministic contract backend for tests. |
| epoll | Linux default | Linux | Readiness backend; real epoll/eventfd validation requires Linux CI. |
| io_uring | no | Linux | Optional until M8 benchmark and race gates justify default enablement. |
| kqueue | platform default | macOS/BSD | Readiness backend; requires macOS/BSD CI. |
| IOCP | platform default | Windows | Completion backend; `OVERLAPPED` storage remains live until completion observation. |

Common failure classifications:

- `invalid_state`: bad handle token, missing scheduler, invalid options.
- `resource_exhausted`: bounded queue or operation queue full.
- `closed`: operation submitted after shutdown or close.
- `unsupported`: backend unavailable on the current platform.
- `backend_failure`: provider or filesystem failure.

All backends must satisfy ADR 0002 and ADR 0004 before release.
