# io_uring Backend

The io_uring backend is optional and is not the default Linux backend in M6.
Linux continues to default to epoll until io_uring passes capability detection,
exactly-once cancellation race tests, differential behavior tests, and benchmark
criteria.

Registered buffers and files are optional optimizations. They do not change
default VIO ownership semantics: VIO does not own caller storage, does not take
raw file-descriptor ownership, and does not close caller-owned descriptors.
Explicit Linux registration is still a kernel-side lifetime event. Successful
`IORING_REGISTER_BUFFERS` may pin or map memory, and successful
`IORING_REGISTER_FILES` may install a registered file set that holds kernel file
references until unregister or ring teardown.
They are never used by ordinary `submit()` calls in M6: socket and file reads
and writes continue to submit `IORING_OP_READ`/`IORING_OP_WRITE` with the
ordinary file descriptor. VIO does not silently switch operations to
`READ_FIXED`, `WRITE_FIXED`, or fixed-file-table lookup when resources are
registered.

Registered buffer lifecycle is explicit. Callers register a non-empty span of
`io_uring_registered_buffer` views. The caller retains ownership of the backing
storage and must keep it valid until `unregister_buffers()`, `shutdown()`, or
backend destruction releases the registration; on Linux the kernel may hold
pins or mappings for that same interval. A second buffer registration while one
is active returns `invalid_state`; unregister first to replace it. Empty lists
and `count == 0` return `invalid_state`. The legacy count-only overload remains
source-compatible but cannot register real fixed buffers; when the capability
exists it reports `invalid_state` and callers must use the buffer-view
overload.

Registered file lifecycle is also explicit. Callers register a non-empty span
of current `backend_handle_token` values. VIO keeps only handle-token state and
never takes raw-fd ownership; `close_handle()` never closes user-owned
descriptors. On Linux, the kernel registered-file table may hold file
references until `unregister_files()`, `shutdown()`, backend destruction, or
ring teardown releases them. A second file registration while one is active
returns `invalid_state`; unregister first to replace it. Closing a handle that
is part of the registered-file set invalidates and unregisters the whole
registered-file state before the handle token is closed, so stale fixed-file
references cannot affect later ordinary operations. Empty lists and `count == 0`
return `invalid_state`. The legacy count-only overload remains source-compatible
but cannot register real files; when the capability exists it reports
`invalid_state` and callers must use the token-list overload.

Explicit unregister on an inactive resource kind returns `invalid_state`.
`shutdown()` and backend destruction attempt to unregister active buffers and
files. If a real Linux registration syscall fails during explicit register, VIO
rolls back without publishing local registered state. If an explicit unregister
syscall fails, VIO reports `backend_failure` and keeps the local state active so
a later cleanup attempt still knows what must be released.

`supports_registered_files` means the kernel probe reports the files-update
opcode as a candidate for registered-file support. M6-006 defines the lifecycle
and default-off semantics, but M6 still does not use fixed files by default.

Capability detection is conservative. VIO first attempts a minimal
`io_uring_setup` syscall and reports the backend unavailable when the syscall is
missing, denied, or fails. When setup succeeds, VIO queries
`IORING_REGISTER_PROBE` and only marks operation opcodes and optional registered
buffer/file support that the probe reports as supported. A kernel that permits
setup but does not support probing is treated as available but not default
eligible.
