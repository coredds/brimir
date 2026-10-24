# Vulkan-Headers (vendored)

- Upstream: https://github.com/KhronosGroup/Vulkan-Headers
- Tag: `v1.4.363` (commit `6802bb4733b63ed5efd3adb308a6c885ef180ea1`, the header version of Vulkan SDK 1.4.363.0)
- License: Apache-2.0 or MIT (see `LICENSE.md` and `LICENSES/`)
- Used by: `brimir-gpu` (`src/gpu/`) and `include/libretro_vulkan.h`, only when `BRIMIR_VULKAN=ON`.

## Contents

Kept from the upstream tree, unmodified: the C headers `include/vulkan/*.h` and `include/vk_video/*.h` (the video codec
headers are included by `vulkan_core.h`), `LICENSE.md`, `LICENSES/`.
Left out: the C++ bindings (`include/vulkan/*.hpp`, `*.cppm`), `registry/`, `tests/`, the build files and `.github/`.

Exposed as the INTERFACE target `vulkan_headers` (`src/gpu/CMakeLists.txt`). There is no loader library: Vulkan is
loaded at runtime through volk.

## Updating

Download the new tag's tarball, replace `include/vulkan/*.h`, `include/vk_video/*.h`, `LICENSE.md` and `LICENSES/`, and
update the tag and commit above. Update `vendor/volk` to the matching `vulkan-sdk-*` tag at the same time.
