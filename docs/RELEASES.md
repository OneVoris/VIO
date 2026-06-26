# Release Process

## Versioning

VIO uses semantic versions. During `0.x`, minor versions may contain source-incompatible public API changes; patch versions must remain compatible within the minor line.

## Release Sequence

1. Close the milestone's required TODO items and ADRs.
2. Run repository validation, tests, sanitizers, fuzz regressions, and required benchmarks.
3. Update `CHANGELOG.md`, compatibility notes, and `voris-package.toml`.
4. Create and sign the source tag `vX.Y.Z`.
5. Publish an immutable source archive and SHA-256.
6. Submit the new version to VXrepo.
7. Verify a clean VXrepo install in Debug/Release and supported feature combinations.
8. Notify downstream repositories only after the VXrepo recipe is available.

## Release Evidence Checklist

Release notes must record the exact dependency and backend evidence used for
the tag:

- VXrepo registration state and the actual `xrepo info voris-vmem` output,
  including resolved version, repository, and source URL when available.
- Confirmation that the repository still uses the no-version selector
  `add_requires("voris-vmem")`; release preparation must not pin VMem in
  `xmake.lua`.
- Debug, Release, ASan+UBSan, and TSan command lines and results for the
  release candidate.
- Backend contract results for virtual and every operating-system backend on
  hosts that support them, plus explicit pass/skip/fail records for unsupported
  provider paths.
- io_uring default-enable gate evidence before any release changes the Linux
  default away from epoll.

## Upstream/Downstream Order

An upstream release must be available through VXrepo before a downstream release can depend on it. Downstream repositories must not release against an unreleased upstream commit.

## Compatibility

- Public error identifiers are not reused.
- Disk or wire formats require explicit version/compatibility policy.
- Removed APIs require migration notes.
- Provider interfaces document minimum and maximum supported versions.

## Licensing Gate

No public release occurs until the repository contains an approved license and package metadata reflects it.
