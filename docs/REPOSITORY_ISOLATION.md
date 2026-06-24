# Repository Isolation

VIO is maintained as an independent source repository. Its internal upstream set is: voris-vmem.

## Prohibited Coupling

- Relative includes or source paths outside this repository.
- Git submodules or subtree copies of another Voris library.
- Inclusion of another repository's private headers.
- Downstream reimplementation of an upstream subsystem to avoid requesting an API.
- Stable package recipes that point to moving branches.

## Supported Coupling

- Released public headers and link artifacts resolved from VXrepo.
- Versioned provider interfaces and adapters.
- Public conformance, integration, and interoperability tests.
- A separate integration workspace that consumes packages exactly as an external project would.

## Upstream Escalation

When blocked by an upstream defect or missing public capability:

1. Refresh upstream package metadata.
2. Record the resolved version and commit/tag.
3. Verify the issue still exists in the newest compatible release.
4. Produce a minimal reproducer that uses public APIs only.
5. Submit a structured requirement to the owning upstream repository.
6. Track the request locally without committing Agent-private notes.

A temporary adapter is permitted only when it uses the existing public API, has bounded scope, includes removal criteria, and is recorded in an ADR. Copying or patching upstream internals is not a temporary adapter.

## Release Coordination

The owning upstream repository releases first. VXrepo publishes the immutable version and checksum second. The downstream repository updates and tests its version constraint third.
