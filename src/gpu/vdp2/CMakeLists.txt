# Vulkan-free front half of the VDP2 GPU renderer: the GPU-visible parameter structs (vdp2_hw_params.hpp), the 32-bit
# rotation math (int64_emu.hpp) and the generated shader layout headers (gpu/shaders/generated/*.layout.h). Nothing here
# includes a Vulkan header. Added regardless of BRIMIR_VULKAN, so its [vdp2] tests also run in BRIMIR_VULKAN=OFF builds;
# brimir-gpu links it. Header-only for now (INTERFACE).

add_library(brimir-vdp2-front INTERFACE)

# Consumers include "gpu/vdp2/..." and "gpu/shaders/generated/*.layout.h".
target_include_directories(brimir-vdp2-front INTERFACE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(brimir-vdp2-front INTERFACE cxx_std_20)
target_link_libraries(brimir-vdp2-front INTERFACE brimir::brimir-core)
