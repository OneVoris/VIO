# Building VorisIO

## Requirements

- XMake available on `PATH`.
- A C++23 compiler and standard library.
- Python 3.11 or newer for repository validation tools.
- VXrepo registration for released Voris package recipes.

## Basic Builds

Register VXrepo once in the development environment, then inspect the resolved VMem package:

```bash
xrepo add-repo vxrepo <VXREPO_GIT_URL>
xrepo info voris-vmem
```

```bash
xmake f -m debug --build_tests=y
xmake
xmake test

xmake f -m release --build_tests=y
xmake
xmake test
```

`voris-vmem` is a required package from M0 onward. XMake resolves it through VXrepo during normal configuration using the no-version selector `add_requires("voris-vmem")`. VXrepo provides the latest released package; the repository records the actual resolved version from `xrepo info voris-vmem` as validation evidence rather than pinning a selector in source.

The repository never assumes sibling source checkouts. Development overrides must be local, explicit, and uncommitted.

## Debug/Release CI

The `Debug Release` GitHub Actions workflow is the normal repository evidence
path for non-sanitized debug and release builds. It runs the same configure,
build, and test sequence on `ubuntu-latest` and `windows-latest`, registers
VXrepo, refreshes repository metadata, records `xrepo info voris-vmem` before
configuration, and runs `python tools/check_repository.py`.

This workflow is an evidence collection entry point. Evidence commit
`ea0d6568a37c35f5efa63f57c924b1fa8d6a8d66` recorded `Debug Release` run
`28219130885`; future candidates still need recorded runner passes across
Debug, Release, ASan+UBSan, and TSan configurations before claiming the release
Definition of Done.

## Options

| Option | Default | Meaning |
|---|---:|---|
| `build_shared` | `false` | Build the primary library as a shared library. |
| `build_tests` | `false` | Build and register test targets. |
| `build_examples` | `false` | Build examples when implementation examples exist. |
| `build_benchmarks` | `false` | Build benchmark targets. |
| `build_fuzzers` | `false` | Build fuzz targets with the selected toolchain. |
| `sanitize_thread` | `false` | Enable ThreadSanitizer on supported Linux clang/gcc-like toolchains. |
| `sanitize_address_undefined` | `false` | Enable AddressSanitizer and UndefinedBehaviorSanitizer on supported Linux clang/gcc-like toolchains. |

Project-specific component options are documented in `xmake.lua` comments and the architecture document.

## Sanitizers

Use separate build directories or CI workspaces for ASan+UBSan and TSan. The
`sanitize_address_undefined` and `sanitize_thread` options are mutually
exclusive so one sanitizer family cannot be mixed into the same configuration.

ASan+UBSan is a Linux clang/gcc-like configuration. The repository CI entry uses
the runner GCC C++ toolchain on `ubuntu-latest` and covers both debug and
release modes:

```bash
xmake f -m debug --build_tests=y --sanitize_address_undefined=y --cc=gcc --cxx=g++
xmake
xmake test

xmake f -m release --build_tests=y --sanitize_address_undefined=y --cc=gcc --cxx=g++
xmake
xmake test
```

This configuration is an evidence collection entry point. Evidence commit
`ea0d6568a37c35f5efa63f57c924b1fa8d6a8d66` recorded `ASan UBSan` run
`28219130878`; release validation still needs recorded Debug, Release,
ASan+UBSan, and TSan results for each future candidate before claiming the
release Definition of Done.

## Validation

```bash
python tools/check_repository.py
```

The validator checks required files, documentation language, ignored Agent documents, TODO identifiers, and relative Markdown links.
