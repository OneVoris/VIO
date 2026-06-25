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
- IOCP is the Windows completion backend. It owns one completion port, associates
  caller-owned native handles with bounded association-id/generation completion
  keys, and drains wake/native packets in capped `GetQueuedCompletionStatusEx`
  batches. Submitted operations own heap-stable internal `OVERLAPPED` storage;
  native packets map by the original `OVERLAPPED*` plus the submitted
  association generation to exactly one backend completion. Unknown or stale
  `OVERLAPPED*` packets are cleanup-only. Cancellation records the first reason,
  requests `CancelIoEx` for active storage when possible, and keeps the storage
  alive until the original native completion is observed. Close and shutdown
  request cancellation but report active operations as `closed` only when their
  original native completion arrives; the port is kept open during shutdown
  while active native storage can still be referenced.

Cancellation is always a request. Backends that cannot stop an already-started
file operation must report the real completion result.
