# Libretro API implementation

add_library(brimir_libretro_api OBJECT
    libretro.cpp
    options.cpp
)

target_include_directories(brimir_libretro_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_features(brimir_libretro_api PUBLIC cxx_std_20)
# The core's headers (include/brimir/lockstep.hpp includes ymir/savestate/...); Vulkan builds also got them through
# brimir-gpu. Usage requirements only: brimir_libretro links the core itself.
target_link_libraries(brimir_libretro_api PRIVATE brimir::brimir-core)

# Propagate the project version so retro_get_system_info reports the same
# version as CMake / the .info file.
target_compile_definitions(brimir_libretro_api PRIVATE BRIMIR_VERSION="${Brimir_VERSION}")

# Vulkan hardware context (brimir_renderer = vulkan); brimir-gpu also brings the vendored Vulkan headers that
# include/libretro_vulkan.h needs, and defines BRIMIR_VULKAN (the option is only offered in such builds).
if(BRIMIR_VULKAN)
    target_sources(brimir_libretro_api PRIVATE libretro_vulkan.cpp)
    target_link_libraries(brimir_libretro_api PRIVATE brimir-gpu)
endif()
