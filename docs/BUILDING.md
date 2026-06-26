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
clang on `ubuntu-latest` and covers both debug and release modes:

```bash
xmake f -m debug --build_tests=y --sanitize_address_undefined=y --cc=clang --cxx=clang++
xmake
xmake test

xmake f -m release --build_tests=y --sanitize_address_undefined=y --cc=clang --cxx=clang++
xmake
xmake test
```

This configuration is an evidence collection entry point. It does not by itself
complete the release Definition of Done; release validation still needs recorded
Debug, Release, ASan+UBSan, and TSan results for the candidate being approved.

## Validation

```bash
python tools/check_repository.py
```

The validator checks required files, documentation language, ignored Agent documents, TODO identifiers, and relative Markdown links.
