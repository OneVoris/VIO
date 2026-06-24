# Roadmap

Milestones are capability gates rather than calendar promises. A later milestone may be explored in a branch, but it must not become the default until earlier release gates pass.

## M0 — Semantics and a Testable Kernel

Complete every `VIO-M0-*` task in [TODO.md](TODO.md).

**Exit gate:** tests for the milestone pass in Debug and Release; public behavior is documented; sanitizer/fuzz/benchmark requirements relevant to the milestone are recorded.

## M1 — Cancellation, Deadlines, and Structured Concurrency

Complete every `VIO-M1-*` task in [TODO.md](TODO.md).

**Exit gate:** tests for the milestone pass in Debug and Release; public behavior is documented; sanitizer/fuzz/benchmark requirements relevant to the milestone are recorded.

## M2 — Shard Runtime and Scheduling

Complete every `VIO-M2-*` task in [TODO.md](TODO.md).

**Exit gate:** tests for the milestone pass in Debug and Release; public behavior is documented; sanitizer/fuzz/benchmark requirements relevant to the milestone are recorded.

## M3 — Timers and Asynchronous Primitives

Complete every `VIO-M3-*` task in [TODO.md](TODO.md).

**Exit gate:** tests for the milestone pass in Debug and Release; public behavior is documented; sanitizer/fuzz/benchmark requirements relevant to the milestone are recorded.

## M4 — Linux epoll Backend

Complete every `VIO-M4-*` task in [TODO.md](TODO.md).

**Exit gate:** tests for the milestone pass in Debug and Release; public behavior is documented; sanitizer/fuzz/benchmark requirements relevant to the milestone are recorded.

## M5 — Files and Blocking Work

Complete every `VIO-M5-*` task in [TODO.md](TODO.md).

**Exit gate:** tests for the milestone pass in Debug and Release; public behavior is documented; sanitizer/fuzz/benchmark requirements relevant to the milestone are recorded.

## M6 — io_uring Backend

Complete every `VIO-M6-*` task in [TODO.md](TODO.md).

**Exit gate:** tests for the milestone pass in Debug and Release; public behavior is documented; sanitizer/fuzz/benchmark requirements relevant to the milestone are recorded.

## M7 — kqueue and IOCP

Complete every `VIO-M7-*` task in [TODO.md](TODO.md).

**Exit gate:** tests for the milestone pass in Debug and Release; public behavior is documented; sanitizer/fuzz/benchmark requirements relevant to the milestone are recorded.

## M8 — Hardening, Benchmarks, and Release

Complete every `VIO-M8-*` task in [TODO.md](TODO.md).

**Exit gate:** tests for the milestone pass in Debug and Release; public behavior is documented; sanitizer/fuzz/benchmark requirements relevant to the milestone are recorded.

## Cross-Repository Gate

A milestone that depends on a new upstream capability may start only after that capability is released by the owning repository and published in VXrepo. Downstream repositories do not implement private copies of upstream behavior.

## First Release Gate

- Public headers compile independently.
- Required dependency versions are published in VXrepo.
- CI passes on the declared Tier-1 platform/compiler matrix.
- Security-sensitive parsers and state machines have fuzz or adversarial tests.
- The changelog and compatibility notes describe every public behavior change.
- A project license has been selected and added.
