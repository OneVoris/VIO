# io_uring Backend

The io_uring backend is optional and is not the default Linux backend in M6.
Linux continues to default to epoll until io_uring passes capability detection,
exactly-once cancellation race tests, differential behavior tests, and benchmark
criteria.

Registered buffers and files are optional optimizations. They do not change
default VIO ownership semantics; public file and buffer ownership remains with
the VIO object or caller-provided view that created the operation.

Capability detection is conservative. VIO first attempts a minimal
`io_uring_setup` syscall and reports the backend unavailable when the syscall is
missing, denied, or fails. When setup succeeds, VIO queries
`IORING_REGISTER_PROBE` and only marks operation opcodes and optional registered
buffer/file support that the probe reports as supported. A kernel that permits
setup but does not support probing is treated as available but not default
eligible.
