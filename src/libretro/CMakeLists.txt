# Libretro API implementation

add_library(brimir_libretro_api OBJECT
    libretro.cpp
    options.cpp
)

target_include_directories(brimir_libretro_api PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_compile_features(brimir_libretro_api PUBLIC cxx_std_20)

# Propagate the project version so retro_get_system_info reports the same
# version as CMake / the .info file.
target_compile_definitions(brimir_libretro_api PRIVATE BRIMIR_VERSION="${Brimir_VERSION}")