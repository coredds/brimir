# Vulkan-free front half of the VDP1 GPU renderer (brimir-vdp1-front): the GPU-visible structs (vdp1_hw_params.hpp), the
# shader binding table (vdp1_bindings.hpp), the generated shader layout headers (gpu/shaders/generated/*.layout.h) and
# the front end that turns VDP1 commands into spans and an op stream (vdp1_front.*). Nothing here includes a Vulkan
# header. Added regardless of BRIMIR_VULKAN, so its [vdp1] tests also run in BRIMIR_VULKAN=OFF builds; brimir-gpu links
# it. The Vulkan back half (vdp1_vulkan_backend.*) will be built by brimir-gpu.

add_library(brimir-vdp1-front STATIC
    vdp1_front.cpp
)

# Consumers include "gpu/vdp1/..." and "gpu/shaders/generated/*.layout.h".
target_include_directories(brimir-vdp1-front PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(brimir-vdp1-front PUBLIC cxx_std_20)
# The bindings header builds on the VDP2 binding types (Binding, DescriptorKind, StageUsage).
target_link_libraries(brimir-vdp1-front PUBLIC brimir-vdp2-front brimir::brimir-core)

# Keep it out of the core's dynamic symbol table on ELF/Mach-O, like brimir-gpu.
set_target_properties(brimir-vdp1-front PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(MSVC)
    set_target_properties(brimir-vdp1-front PROPERTIES FOLDER "Brimir")
endif()
