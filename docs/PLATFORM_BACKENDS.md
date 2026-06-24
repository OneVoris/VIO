# Platform Backends

VIO backends share the backend contract from ADR 0004, but operating systems do
not expose identical cancellation and file-I/O semantics.

- Linux epoll remains the default Linux readiness backend.
- Linux io_uring is optional until M8 capability, race, and benchmark gates
  justify default enablement.
- kqueue is the macOS/BSD readiness backend.
- IOCP is the Windows completion backend. `OVERLAPPED` operation storage must
  remain alive after cancellation until the completion or cancellation result is
  observed.

Cancellation is always a request. Backends that cannot stop an already-started
file operation must report the real completion result.
