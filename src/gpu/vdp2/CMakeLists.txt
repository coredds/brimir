# Vulkan-free front half of the VDP2 GPU renderer: the GPU-visible parameter structs (vdp2_hw_params.hpp), the 32-bit
# rotation math (int64_emu.hpp), the VDP2 shader binding table (vdp2_bindings.hpp), the generated shader layout headers
# (gpu/shaders/generated/*.layout.h) and the front end that records VDP2 frames (vdp2_front.*). Nothing here includes a Vulkan header. Added regardless of
# BRIMIR_VULKAN, so its [vdp2] tests also run in BRIMIR_VULKAN=OFF builds; brimir-gpu links it.

add_library(brimir-vdp2-front STATIC
    vdp2_front.cpp
)

# Consumers include "gpu/vdp2/..." and "gpu/shaders/generated/*.layout.h".
target_include_directories(brimir-vdp2-front PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(brimir-vdp2-front PUBLIC cxx_std_20)
target_link_libraries(brimir-vdp2-front PUBLIC brimir::brimir-core)

# Keep it out of the core's dynamic symbol table on ELF/Mach-O, like brimir-gpu.
set_target_properties(brimir-vdp2-front PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(MSVC)
    set_target_properties(brimir-vdp2-front PROPERTIES FOLDER "Brimir")
endif()
