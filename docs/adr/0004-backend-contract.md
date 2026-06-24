# ADR 0004 - Backend Contract

- Status: Proposed
- Date: 2026-06-25
- Owners: repository maintainers
- Related tasks: VIO-M4-001, VIO-M4-010

## Context

VIO must expose one operation contract across deterministic, readiness, and
completion backends. M4 starts with Linux epoll, but the public operation layer
must not expose epoll, io_uring, kqueue, or IOCP details.

## Decision

Backends implement register, submit, cancel, poll, wake, and shutdown. Submit
publishes backend references only through the exactly-once operation state
machine from ADR 0002. Cancel requests backend cancellation but does not destroy
operation storage. Poll reports completions or readiness without resuming user
coroutines inline.

Native handles are tracked through generation-aware registry tokens. Close
invalidates the current generation, cancels pending operations, and treats later
events for older generations as stale cleanup-only events.

Same-direction socket operations are queued FIFO with explicit limits. A read
queue and write queue may each have at most the configured number of pending
operations. Queue overflow reports `resource_exhausted`.

## Alternatives Considered

Rejecting same-direction socket operations was simpler but would not match the
chosen public contract. Exposing backend-specific handles was rejected because
it would leak provider APIs into public VIO headers.

## Consequences

All real backends must pass the same contract suite as the virtual backend.
Linux epoll is the default Linux backend until io_uring passes later capability,
race, and benchmark gates.

## Verification

- Backend contract tests run against `virtual_backend` and epoll when Linux is
  available.
- Registry tests cover close, stale generations, and native handle reuse.
- Socket queue tests cover FIFO same-direction read/write queues and bounded
  overflow.
