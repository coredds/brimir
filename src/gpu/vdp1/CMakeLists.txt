# Vulkan-free front half of the VDP1 GPU renderer (brimir-vdp1-front): the GPU-visible structs (vdp1_hw_params.hpp), the
# shader binding table (vdp1_bindings.hpp) and the generated shader layout headers (gpu/shaders/generated/*.layout.h).
# Nothing here includes a Vulkan header. Added regardless of BRIMIR_VULKAN, so its [vdp1] tests also run in
# BRIMIR_VULKAN=OFF builds; brimir-gpu links it. An INTERFACE library for now (headers only): the front end
# (vdp1_front.*) makes it a STATIC library. The Vulkan back half (vdp1_vulkan_backend.*) will be built by brimir-gpu.

add_library(brimir-vdp1-front INTERFACE)

# Consumers include "gpu/vdp1/..." and "gpu/shaders/generated/*.layout.h".
target_include_directories(brimir-vdp1-front INTERFACE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(brimir-vdp1-front INTERFACE cxx_std_20)
# The bindings header builds on the VDP2 binding types (Binding, DescriptorKind, StageUsage).
target_link_libraries(brimir-vdp1-front INTERFACE brimir-vdp2-front)
