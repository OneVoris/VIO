# Contributing

## Scope

Contributions must fit the boundary in [ARCHITECTURE.md](ARCHITECTURE.md). Cross-repository changes are split into independent upstream and downstream changes and released in dependency order.

## Workflow

1. Select one task identifier from [TODO.md](TODO.md) or open a focused issue.
2. Write or update an ADR before changing public API, persistent format, wire behavior, scheduling/lifetime semantics, or security defaults.
3. Add a failing test before the implementation change when practical.
4. Implement the smallest complete change.
5. Run `python tools/check_repository.py`, builds, tests, and applicable sanitizers/fuzzers/benchmarks.
6. Update public documentation in English.
7. Update the changelog when observable behavior changes.

## Commit and Pull Request Rules

- Keep one primary task or inseparable task set per change.
- Do not commit `AGENTS.md` or `.agent/`; they are local Chinese operating documents and are intentionally ignored.
- Do not vendor another Voris repository or include its private headers.
- Describe ownership, cancellation, limits, compatibility, and performance impact where relevant.

## Public Documentation Language

All tracked public Markdown documentation is English. Local Agent instructions are Chinese and ignored by Git. The repository validator enforces this split.

## Definition of Done

A change is complete only when behavior, failures, tests, documentation, and required measurements agree.
