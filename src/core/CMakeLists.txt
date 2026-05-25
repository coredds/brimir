## Create the library target (STATIC for internal linking)
file(GLOB_RECURSE YMIR_HEADERS CONFIGURE_DEPENDS "include/ymir/*.hpp")
file(GLOB_RECURSE YMIR_SOURCES CONFIGURE_DEPENDS "src/ymir/*.cpp")

add_library(brimir-core STATIC
    ${YMIR_HEADERS}
    ${YMIR_SOURCES}

    src/vdp_profiling.cpp
    include/brimir/vdp_profiling.hpp

    src/util/cpu_features.cpp
    include/brimir/util/cpu_features.hpp
    include/brimir/util/benchmark.hpp
)
add_library(brimir::brimir-core ALIAS brimir-core)
set_target_properties(brimir-core PROPERTIES
                      VERSION ${Brimir_core_VERSION}
                      SOVERSION ${Brimir_core_VERSION_MAJOR})
target_include_directories(brimir-core
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_compile_features(brimir-core PUBLIC cxx_std_20)

## Add dependencies
target_link_libraries(brimir-core PUBLIC
    fmt::fmt
    mio
    concurrentqueue
    xxhash
    chdr-static
)
if (WIN32)
    target_link_libraries(brimir-core PUBLIC
        synchronization
    )
endif ()

# Build precompiled header
target_precompile_headers(brimir-core
    PUBLIC
        "$<$<COMPILE_LANGUAGE:CXX>:<algorithm$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<array$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<atomic$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<bit$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<cassert$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<charconv$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<cinttypes$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<climits$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<cmath$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<concepts$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<condition_variable$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<cstddef$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<cstdint$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<cstdlib$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<deque$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<filesystem$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<fstream$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<functional$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<initializer_list$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<iosfwd$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<iostream$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<iterator$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<limits$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<memory$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<mutex$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<new$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<numeric$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<optional$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<queue$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<set$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<span$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<sstream$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<string$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<thread$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<type_traits$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<unordered_map$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<unordered_set$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<utility$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<variant$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<vector$<ANGLE-R>>"

        "$<$<COMPILE_LANGUAGE:CXX>:<fmt/format.h$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<fmt/std.h$<ANGLE-R>>"
        "$<$<COMPILE_LANGUAGE:CXX>:<fmt/xchar.h$<ANGLE-R>>"

        "$<$<COMPILE_LANGUAGE:CXX>:<mio/mmap.hpp$<ANGLE-R>>"
)

## Define version macros (Brimir + Ymir equivalents)
if (DEFINED Brimir_VERSION_PRERELEASE AND NOT Brimir_VERSION_PRERELEASE STREQUAL "")
    set(Brimir_VERSION "${Brimir_VERSION}-${Brimir_VERSION_PRERELEASE}")
endif ()
if (Brimir_DEV_BUILD)
    set(Brimir_VERSION "${Brimir_VERSION}-dev")
endif ()
if (DEFINED Brimir_VERSION_BUILD AND NOT Brimir_VERSION_BUILD STREQUAL "")
    set(Brimir_VERSION "${Brimir_VERSION}+${Brimir_VERSION_BUILD}")
endif ()
message(STATUS "Brimir core version string: ${Brimir_VERSION}")

# Brimir version macros (used by Brimir-specific code)
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION="${Brimir_VERSION}")
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_MAJOR=${Brimir_VERSION_MAJOR})
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_MINOR=${Brimir_VERSION_MINOR})
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_PATCH=${Brimir_VERSION_PATCH})
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_PRERELEASE="${Brimir_VERSION_PRERELEASE}")
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_BUILD="${Brimir_VERSION_BUILD}")
if (DEFINED Brimir_BUILD_TIMESTAMP AND NOT Brimir_BUILD_TIMESTAMP STREQUAL "")
    target_compile_definitions(brimir-core PUBLIC BRIMIR_BUILD_TIMESTAMP="${Brimir_BUILD_TIMESTAMP}")
endif ()

# Ymir version macros (used by Ymir hardware layer)
target_compile_definitions(brimir-core PUBLIC Ymir_VERSION="${Brimir_VERSION}")
target_compile_definitions(brimir-core PUBLIC Ymir_VERSION_MAJOR=${Brimir_VERSION_MAJOR})
target_compile_definitions(brimir-core PUBLIC Ymir_VERSION_MINOR=${Brimir_VERSION_MINOR})
target_compile_definitions(brimir-core PUBLIC Ymir_VERSION_PATCH=${Brimir_VERSION_PATCH})
target_compile_definitions(brimir-core PUBLIC Ymir_VERSION_PRERELEASE="${Brimir_VERSION_PRERELEASE}")
target_compile_definitions(brimir-core PUBLIC Ymir_VERSION_BUILD="${Brimir_VERSION_BUILD}")
if (DEFINED Brimir_BUILD_TIMESTAMP AND NOT Brimir_BUILD_TIMESTAMP STREQUAL "")
    target_compile_definitions(brimir-core PUBLIC Ymir_BUILD_TIMESTAMP="${Brimir_BUILD_TIMESTAMP}")
endif ()

## Define additional macros (Brimir + Ymir equivalents)
target_compile_definitions(brimir-core PUBLIC "BRIMIR_ENABLE_DEVLOG=$<BOOL:${Brimir_ENABLE_DEVLOG}>")
target_compile_definitions(brimir-core PUBLIC "BRIMIR_DEV_ASSERTIONS=$<BOOL:${Brimir_ENABLE_DEV_ASSERTIONS}>")
target_compile_definitions(brimir-core PUBLIC "BRIMIR_DEV_BUILD=$<BOOL:${Brimir_DEV_BUILD}>")
target_compile_definitions(brimir-core PUBLIC "BRIMIR_EXTRA_INLINING=$<BOOL:${Brimir_EXTRA_INLINING}>")

target_compile_definitions(brimir-core PUBLIC "Ymir_ENABLE_DEVLOG=$<BOOL:${Brimir_ENABLE_DEVLOG}>")
target_compile_definitions(brimir-core PUBLIC "Ymir_DEV_ASSERTIONS=$<BOOL:${Brimir_ENABLE_DEV_ASSERTIONS}>")
target_compile_definitions(brimir-core PUBLIC "Ymir_DEV_BUILD=$<BOOL:${Brimir_DEV_BUILD}>")
target_compile_definitions(brimir-core PUBLIC "Ymir_EXTRA_INLINING=$<BOOL:${Brimir_EXTRA_INLINING}>")
target_compile_definitions(brimir-core PUBLIC "Ymir_DISABLE_FORCE_INLINE=$<BOOL:${Brimir_DISABLE_FORCE_INLINE}>")
target_compile_definitions(brimir-core PUBLIC "TOML_EXCEPTIONS=0")

## Generate the export header for Ymir
include(GenerateExportHeader)
generate_export_header(brimir-core EXPORT_FILE_NAME include/ymir/export.h)
target_compile_definitions(brimir-core PUBLIC "$<$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>:Brimir_core_STATIC_DEFINE>")
target_include_directories(brimir-core PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>")

if (IPO_SUPPORTED AND Brimir_ENABLE_IPO)
    message(STATUS "Enabling IPO / LTO for brimir-core")
    set_property(TARGET brimir-core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

## Apply performance options
if (Brimir_AVX2)
    target_compile_definitions(brimir-core PUBLIC ${PROJECT_NAME}_AVX2=1)
    target_compile_definitions(brimir-core PUBLIC Ymir_AVX2=1)
    if (MSVC)
        target_compile_options(brimir-core PUBLIC "/arch:AVX2")
    else ()
        target_compile_options(brimir-core PUBLIC "-mavx2")
        target_compile_options(brimir-core PUBLIC "-mfma")
        target_compile_options(brimir-core PUBLIC "-mbmi")
    endif ()
endif ()

## ARM-specific compiler options
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    target_compile_options(brimir-core PRIVATE -Wno-error=attributes)
endif()

## GPU rendering support (optional)
option(BRIMIR_GPU_VULKAN "Enable Vulkan GPU renderer" OFF)

if(BRIMIR_GPU_VULKAN)
    find_package(Vulkan)

    if(Vulkan_FOUND)
        message(STATUS "Vulkan SDK found - GPU rendering enabled")
        target_compile_definitions(brimir-core PUBLIC BRIMIR_GPU_VULKAN_ENABLED)
        target_link_libraries(brimir-core PUBLIC Vulkan::Vulkan)
    else()
        message(WARNING "Vulkan SDK not found - GPU rendering will be disabled")
        message(WARNING "Install Vulkan SDK from https://vulkan.lunarg.com/")
    endif()
else()
    message(STATUS "GPU rendering disabled (use -DBRIMIR_GPU_VULKAN=ON to enable)")
endif()

if (MSVC)
    vs_set_filters(TARGET brimir-core)
    set_target_properties(brimir-core PROPERTIES FOLDER "Brimir")
endif ()
