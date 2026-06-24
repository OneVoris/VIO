# VIO — VorisIO

C++23 coroutine runtime, per-core shard scheduler, cancellation, timers, asynchronous primitives, and portable I/O backends.

> Status: architecture and implementation-planning scaffold. The public API and ABI are not stable. A project license must be selected before public distribution.

## Design Priorities

1. Correctness and security boundaries.
2. Diagnosability and deterministic failure behavior.
3. Tail latency and bounded resource usage.
4. Throughput and allocation efficiency.
5. API convenience.

## Goals

- Guarantee exactly-once completion for every asynchronous operation.
- Resume continuations on a defined scheduler rather than an accidental callback thread.
- Provide structured concurrency, cancellation, deadlines, and deterministic shutdown.
- Use bounded queues and explicit backpressure throughout the runtime.
- Present one operation contract across readiness and completion operating-system APIs.

## Non-Goals

- Providing TCP, HTTP, QUIC, cache, or database semantics.
- Using an unbounded global work-stealing pool for network I/O.
- Exposing epoll, io_uring, kqueue, or IOCP types in public APIs.

## Repository Isolation

This is an independent repository. It must not use relative includes into another Voris checkout, Git submodules for Voris libraries, copied upstream source, or private upstream headers. Required Voris packages are resolved through **VXrepo** and XMake package requirements.

Required internal packages: `voris-vmem`.

Optional internal packages: none.

When an upstream defect or missing capability blocks work, contributors must first refresh VXrepo/upstream state and reproduce against the newest compatible upstream release. A downstream workaround is not the default. See [Repository Isolation](docs/REPOSITORY_ISOLATION.md) and [Dependencies](docs/DEPENDENCIES.md).

## Repository Layout

```text
VIO/
├── include/voris/io/
├── src/
├── tests/
├── fuzz/
├── benchmarks/
├── examples/
├── docs/
│   └── adr/
├── tools/
├── xmake.lua
├── voris-package.toml
├── TODO.md
└── AGENTS.md              # local Chinese instructions; ignored by Git
```

## Documentation

- [Architecture](ARCHITECTURE.md)
- [Roadmap](ROADMAP.md)
- [TODO List](TODO.md)
- [API Conventions](docs/API.md)
- [Building](docs/BUILDING.md)
- [Testing](docs/TESTING.md)
- [Dependencies](docs/DEPENDENCIES.md)
- [Repository Isolation](docs/REPOSITORY_ISOLATION.md)
- [Release Process](docs/RELEASES.md)
- [Security Policy](SECURITY.md)
- [Contributing](CONTRIBUTING.md)

## Build the Scaffold

Register VXrepo so XMake can resolve the required released Voris packages:

```bash
xrepo add-repo vxrepo <VXREPO_GIT_URL>
xrepo info voris-vmem
```

```bash
xmake f -m debug --build_tests=y
xmake
xmake test
```

VMem is required from M0 onward. VIO consumes the latest released `voris-vmem` package available through VXrepo with no explicit version selector in this repository. Record the actual resolved version from `xrepo info voris-vmem` in local build evidence and release notes rather than pinning it in source.

## Package Identity

- VXrepo package: `voris-vio`
- C++ namespace: `voris::io`
- Primary XMake target: `voris_vio`
- Language baseline: C++23

## Licensing

VIO is distributed under GPL-3.0-only. Separate commercial licenses are available
from the project owner. See [Licensing](docs/LICENSING.md) before publishing or
accepting external contributions.
