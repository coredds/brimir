# Bridge layer - adapts Ymir emulator to libretro API

add_library(brimir_bridge OBJECT
    core_wrapper.cpp
    lockstep.cpp
    savestate_codec.cpp
)

target_include_directories(brimir_bridge PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_features(brimir_bridge PUBLIC cxx_std_20)

# Link with Brimir core and the SH-2 JIT (PUBLIC so consumers of the object
# library, such as tests and tools, link the JIT too)
target_link_libraries(brimir_bridge PUBLIC brimir-jit brimir::brimir-core)

# CoreWrapper installs the Vulkan VDP renderer (src/gpu/vdp) in BRIMIR_VULKAN builds; brimir-gpu defines BRIMIR_VULKAN.
if(BRIMIR_VULKAN)
    target_link_libraries(brimir_bridge PUBLIC brimir-gpu)
endif()