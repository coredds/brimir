# Brimir GPU renderer (docs/superpowers/specs/2026-10-24-vulkan-gpu-renderer-design.md); added when BRIMIR_VULKAN is on.
# Vulkan is loaded at runtime through volk: never link the Vulkan loader (vulkan-1, libvulkan, MoltenVK) here.

# Khronos headers only (vendor/Vulkan-Headers/BRIMIR_VENDOR.md); no prototypes, so nothing can call the loader directly.
add_library(vulkan_headers INTERFACE)
target_include_directories(vulkan_headers SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/vendor/Vulkan-Headers/include)
target_compile_definitions(vulkan_headers INTERFACE VK_NO_PROTOTYPES)

add_library(brimir-gpu STATIC
    volk_impl.cpp
    vk_device.cpp
    vk_util.cpp
    frame_presenter.cpp
    gpu_pass_profile.cpp
    # VDP2 and VDP1 on the GPU: the Vulkan back halves and the renderer (VDP1 in software or on the GPU)
    vdp2/vdp2_vulkan_backend.cpp
    vdp1/vdp1_vulkan_backend.cpp
    vdp/vulkan_vdp_renderer.cpp
    # Tests and tools only (volkInitialize); the core wraps the frontend's device instead.
    headless_device.cpp
)

# Consumers include "gpu/..." (and "gpu/shaders/generated/*.spv.h"), plus <volk.h> and <libretro_vulkan.h>.
target_include_directories(brimir-gpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(brimir-gpu SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/vendor/volk)
target_compile_features(brimir-gpu PUBLIC cxx_std_20)
target_link_libraries(brimir-gpu PUBLIC vulkan_headers brimir-vdp2-front brimir-vdp1-front)

# volk symbols live in namespace volk (no clash with a frontend's vk* symbols), device functions only through a
# VolkDeviceTable (vendor/volk/BRIMIR_VENDOR.md).
target_compile_definitions(brimir-gpu PUBLIC
    BRIMIR_VULKAN=1
    VOLK_NAMESPACE
    VOLK_NO_DEVICE_PROTOTYPES
)

# Keep everything in brimir-gpu out of the core's dynamic symbol table on ELF/Mach-O.
set_target_properties(brimir-gpu PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

set_source_files_properties(volk_impl.cpp PROPERTIES COMPILE_OPTIONS "${EXTERNAL_WARNINGS}")

# volkInitialize() dlopens the system loader (tests and tools only; the core uses the frontend's GIPA).
if(UNIX)
    target_link_libraries(brimir-gpu PRIVATE ${CMAKE_DL_LIBS})
endif()

if(MSVC)
    set_target_properties(brimir-gpu PROPERTIES FOLDER "Brimir")
endif()
