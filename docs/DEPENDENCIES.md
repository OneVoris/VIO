# Dependency Policy

## Internal Dependencies

Required:

- `voris-vmem`

Optional:

- None.

All internal dependencies are consumed through VXrepo package recipes. This repository does not rely on a monorepo, a sibling checkout, Git submodules, copied public headers, or private upstream implementation files.

## Version Rules

- VIO consumes the latest released `voris-vmem` package exposed by VXrepo with no explicit version selector in this repository.
- The actual resolved version is recorded from `xrepo info voris-vmem` in build evidence, upstream-sync notes, and release validation records.
- Stable releases never depend on a moving branch.
- Applications should commit their resolved dependency lock; libraries record the resolved package version used for validation.
- A changed resolved upstream version requires public API review, full tests, and changelog notes when it affects behavior or compatibility.

## Third-Party Dependencies

A mandatory third-party dependency requires answers to these questions:

1. Why are the standard library and current internal interfaces insufficient?
2. Does the dependency enter a hot path or public ABI?
3. Is the license compatible with the chosen project license?
4. Is it maintained across Tier-1 platforms?
5. Can it be replaced behind a narrow provider or adapter?
6. Who owns security update monitoring?

Development-only test and benchmark tools must not leak into installed headers or link interfaces.

## Upstream Failure Process

1. Refresh VXrepo metadata and inspect the resolved package version.
2. Reproduce against the latest released `voris-vmem` package resolved by VXrepo.
3. Reproduce against the newest upstream development state in an isolated workspace when necessary.
4. Reduce the failure to a minimal public-API example.
5. Open an upstream issue/request with compatibility and test requirements.
6. Do not vendor or privately patch upstream source in this repository.

The local Chinese Agent guide contains the required handoff template.
