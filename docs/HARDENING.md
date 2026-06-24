# Hardening and Test Gates

Required release gates:

- Long cancellation, close, and shutdown stress tests.
- TSan jobs for tasks, scopes, channels, mailboxes, and backends.
- ASan+UBSan jobs for Debug and Release builds.
- Backend contract suite for virtual, epoll, io_uring, kqueue, and IOCP where
  the platform supports them.
- End-to-end overload and memory ceiling tests.

Benchmark records include commit, compiler, standard library, flags, CPU,
operating system/kernel, workload, throughput, latency percentiles, peak RSS,
allocations per operation, and timeout/error counts.
