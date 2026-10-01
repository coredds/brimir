# asmjit (vendored)

- Upstream: https://github.com/asmjit/asmjit
- Commit: `0d7f01052f2d3f6d6b9bb6714ef32bd7b80177eb` (2026-09-22)
- License: zlib (see `LICENSE.md`)
- Used by: the SH-2 JIT x64 backend (`src/jit/src/x64/`), only in x86-64 builds (`BRIMIR_JIT_X64`).

## Contents

Kept from the upstream tree, unmodified: `asmjit/` (library sources), `CMakeLists.txt`, `LICENSE.md`.
Left out: `asmjit-testing/`, `db/`, `tools/`, `.github/`, docs, presets and configure scripts.
The upstream `CMakeLists.txt` only references `asmjit-testing/` when `ASMJIT_TEST=ON`, which Brimir never sets.

## CMake options set by Brimir (root `CMakeLists.txt`)

| Option | Value | Why |
|---|---|---|
| `ASMJIT_STATIC` | `ON` | static library linked into `brimir-jit` |
| `ASMJIT_NO_AARCH64` | `ON` | only the x86 backend is used |
| `ASMJIT_NO_FOREIGN` | `ON` | no foreign-architecture backends |
| `ASMJIT_NO_INSTALL` | `ON` | keep asmjit out of Brimir's install rules |

Added with `add_subdirectory(vendor/asmjit EXCLUDE_FROM_ALL)`; its warnings are suppressed like the other vendors.

## Updating

Download the new commit's tarball, replace `asmjit/`, `CMakeLists.txt` and `LICENSE.md`, update the commit and date above,
and re-check the option names in `CMakeLists.txt`.
