# Building VorisIO

## Requirements

- XMake 3.0.0 or newer.
- A C++23 compiler and standard library.
- Python 3.11 or newer for repository validation tools.
- VXrepo registration when `--with_voris_dependencies=y` is enabled.

## Basic Builds

```bash
xmake f -m debug --build_tests=y
xmake
xmake test

xmake f -m release --build_tests=y
xmake
xmake test
```

## Resolve Voris Dependencies

```bash
xrepo add-repo vxrepo <VXREPO_GIT_URL>
xmake f -m debug --with_voris_dependencies=y --build_tests=y
xmake
```

The repository never assumes sibling source checkouts. Development overrides must be local, explicit, and uncommitted.

## Options

| Option | Default | Meaning |
|---|---:|---|
| `build_shared` | `false` | Build the primary library as a shared library. |
| `build_tests` | `false` | Build and register test targets. |
| `build_examples` | `false` | Build examples when implementation examples exist. |
| `build_benchmarks` | `false` | Build benchmark targets. |
| `build_fuzzers` | `false` | Build fuzz targets with the selected toolchain. |
| `with_voris_dependencies` | `false` | Resolve internal packages through VXrepo. |

Project-specific component options are documented in `xmake.lua` comments and the architecture document.

## Sanitizers

Use separate build directories or CI workspaces for ASan+UBSan and TSan. Do not combine TSan with ASan.

## Validation

```bash
python tools/check_repository.py
```

The validator checks required files, documentation language, ignored Agent documents, TODO identifiers, and relative Markdown links.
