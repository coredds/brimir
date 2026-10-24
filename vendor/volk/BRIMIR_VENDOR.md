# volk (vendored)

- Upstream: https://github.com/zeux/volk
- Tag: `vulkan-sdk-1.4.363.0` (commit `54fc0d7a72887cbfc5b030d26897b2b40ac53fb8`, matching Vulkan SDK 1.4.363.0;
  `VOLK_HEADER_VERSION 363`)
- License: MIT (see `LICENSE.md`)
- Used by: `brimir-gpu` (`src/gpu/`), only when `BRIMIR_VULKAN=ON`.

## Contents

Kept from the upstream tree, unmodified: `volk.c`, `volk.h`, `LICENSE.md`.
Left out: `CMakeLists.txt`, `cmake/`, `generate.py`, `test/`, `.github/`, `README.md`.

## How Brimir builds it

Not through upstream's CMake. `src/gpu/volk_impl.cpp` compiles `volk.c` in C++ mode inside `brimir-gpu` with these
definitions (PUBLIC on `brimir-gpu`, so every consumer sees the same declarations):

| Define | Why |
|---|---|
| `VK_NO_PROTOTYPES` | no Vulkan prototypes anywhere; the core never links the Vulkan loader |
| `VOLK_NAMESPACE` | volk's `vk*` function-pointer variables live in C++ namespace `volk`, so they are mangled and never clash with a frontend's (or the loader's) C symbols `vkCreateInstance` etc. |
| `VOLK_NO_DEVICE_PROTOTYPES` | device-level functions are only reachable through a per-device `VolkDeviceTable` |

On Linux and macOS `brimir-gpu` is also compiled with hidden visibility, so none of its symbols are exported from
`brimir_libretro.so`. Windows DLLs export only `__declspec(dllexport)` symbols.

## Updating

Download the new tag's tarball, replace `volk.c`, `volk.h` and `LICENSE.md`, update the tag and commit above, and keep it
in step with `vendor/Vulkan-Headers` (volk only loads entry points the headers declare).
