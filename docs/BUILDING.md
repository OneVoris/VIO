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

Project-specific component options are documented in `xmake.lua` comments and the architecture document.

## Sanitizers

Use separate build directories or CI workspaces for ASan+UBSan and TSan. Do not combine TSan with ASan.

## Validation

```bash
python tools/check_repository.py
```

The validator checks required files, documentation language, ignored Agent documents, TODO identifiers, and relative Markdown links.
