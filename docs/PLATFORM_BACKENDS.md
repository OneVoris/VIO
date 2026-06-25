# Platform Backends

VIO backends share the backend contract from ADR 0004, but operating systems do
not expose identical cancellation and file-I/O semantics.

- Linux epoll remains the default Linux backend in M6 release evidence.
- Linux io_uring is optional. `select_default_linux_backend()` chooses io_uring
  only when the core capability set, exactly-once cancellation race gate,
  behavioral differential gate, benchmark evidence gate, and Linux real-provider
  validation gate all pass. Otherwise Linux selection falls back to epoll.
- kqueue is the macOS/BSD readiness backend. It owns a kqueue descriptor,
  uses `EVFILT_USER` for shard wake-up, registers socket read/write filters
  with generation-aware cookies, and treats stale events after close or native
  descriptor reuse as cleanup-only observations. Non-kqueue platforms keep
  returning `unsupported` for backend operations.
- IOCP is the Windows completion backend. `OVERLAPPED` operation storage must
  remain alive after cancellation until the completion or cancellation result is
  observed.

Cancellation is always a request. Backends that cannot stop an already-started
file operation must report the real completion result.
