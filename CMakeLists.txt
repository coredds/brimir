cmake_minimum_required(VERSION 3.28)

# Project information
project(Brimir
    VERSION 0.1.1
    DESCRIPTION "Sega Saturn emulation libretro core based on Ymir"
    LANGUAGES CXX C
)

# C++20 required (inherited from Ymir)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Options
option(BRIMIR_BUILD_TESTS "Build tests" OFF)
option(BRIMIR_STATIC_LINK "Static linking" OFF)
option(BRIMIR_LTO "Enable Link-Time Optimization" OFF)

# Use fmt as header-only to avoid DLL dependency
add_compile_definitions(FMT_HEADER_ONLY=1)

# Build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Platform detection
if(WIN32)
    set(BRIMIR_PLATFORM_WINDOWS TRUE)
elseif(APPLE)
    set(BRIMIR_PLATFORM_MACOS TRUE)
elseif(UNIX)
    set(BRIMIR_PLATFORM_LINUX TRUE)
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Add cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src/ymir/cmake")

# Include Ymir's VS helpers if on MSVC
if (MSVC)
    include(VSHelpers)
endif()

# Compiler warnings
if(MSVC)
    # Default to W4 warnings for our code
    add_compile_options(/W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    
    # Suppress common external library warnings
    # These will be applied to Ymir and vendor libraries
    set(EXTERNAL_WARNINGS
        /wd4244  # Type conversion (int to smaller type)
        /wd4100  # Unreferenced parameter
        /wd4201  # Nameless struct/union
        /wd4324  # Structure padding
        /wd4458  # Declaration hides class member
    )
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Subdirectories - Ymir emulator core dependencies
# Only include what ymir-core actually needs (skip imgui/SDL3)
include(cmake/YmirDeps.cmake)

# Set Ymir version variables (normally set by src/ymir/CMakeLists.txt project() command)
set(Ymir_VERSION_MAJOR 0)
set(Ymir_VERSION_MINOR 2)
set(Ymir_VERSION_PATCH 1)
set(Ymir_VERSION_PRERELEASE "")
set(Ymir_VERSION_BUILD "")
set(Ymir_VERSION "0.2.1")
set(Ymir_DEV_BUILD ON)
set(Ymir_ENABLE_DEVLOG ON)
set(Ymir_ENABLE_DEV_ASSERTIONS OFF)

# Add only Ymir's core library (not full project to avoid vendor conflicts)
add_subdirectory(src/ymir/libs EXCLUDE_FROM_ALL)

# Suppress warnings for Ymir and vendor libraries (external code)
if(MSVC AND DEFINED EXTERNAL_WARNINGS)
    if(TARGET ymir-core)
        target_compile_options(ymir-core PRIVATE ${EXTERNAL_WARNINGS})
    endif()
    if(TARGET xxHash)
        target_compile_options(xxHash PRIVATE ${EXTERNAL_WARNINGS})
    endif()
    if(TARGET lz4)
        target_compile_options(lz4 PRIVATE ${EXTERNAL_WARNINGS})
    endif()
    if(TARGET chdr-static)
        target_compile_options(chdr-static PRIVATE ${EXTERNAL_WARNINGS})
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # GCC/Clang warning suppressions for external code
    set(EXTERNAL_WARNINGS_GCC
        -Wno-maybe-uninitialized
        -Wno-unused-parameter
        -Wno-unused-variable
        -Wno-sign-compare
        -Wno-missing-field-initializers
        -Wno-pedantic
        -Wno-uninitialized
        -Wno-overflow
        -Wno-comment
        -Wno-error=inline  # Don't error on inline failures
    )
    if(TARGET ymir-core)
        target_compile_options(ymir-core PRIVATE ${EXTERNAL_WARNINGS_GCC})
        # Force-include header to override FORCE_INLINE before Ymir headers
        target_compile_options(ymir-core PRIVATE 
            -include ${CMAKE_CURRENT_SOURCE_DIR}/cmake/force_inline_override.h
            -Wno-attributes
        )
    endif()
    if(TARGET xxHash)
        target_compile_options(xxHash PRIVATE ${EXTERNAL_WARNINGS_GCC})
    endif()
    if(TARGET lz4)
        target_compile_options(lz4 PRIVATE ${EXTERNAL_WARNINGS_GCC})
    endif()
    if(TARGET chdr-static)
        target_compile_options(chdr-static PRIVATE ${EXTERNAL_WARNINGS_GCC})
    endif()
endif()

# Bridge layer - adapts Ymir to libretro
add_subdirectory(src/bridge)

# Libretro API implementation
add_subdirectory(src/libretro)

# Main libretro core library
add_library(brimir_libretro SHARED
    $<TARGET_OBJECTS:brimir_bridge>
    $<TARGET_OBJECTS:brimir_libretro_api>
)

# Link with Ymir core
target_link_libraries(brimir_libretro PRIVATE ymir::ymir-core)

# Set output name based on platform
if(BRIMIR_PLATFORM_WINDOWS)
    set_target_properties(brimir_libretro PROPERTIES
        OUTPUT_NAME "brimir_libretro"
        SUFFIX ".dll"
        PREFIX ""
    )
elseif(BRIMIR_PLATFORM_MACOS)
    set_target_properties(brimir_libretro PROPERTIES
        OUTPUT_NAME "brimir_libretro"
        SUFFIX ".dylib"
        PREFIX ""
    )
else()
    set_target_properties(brimir_libretro PROPERTIES
        OUTPUT_NAME "brimir_libretro"
        SUFFIX ".so"
        PREFIX ""
    )
endif()

# Link-Time Optimization
if(BRIMIR_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set_property(TARGET brimir_libretro PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    else()
        message(WARNING "LTO requested but not supported: ${ipo_error}")
    endif()
endif()

# Tests
if(BRIMIR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    message(STATUS "Building with tests enabled")
else()
    message(STATUS "Building without tests (use -DBRIMIR_BUILD_TESTS=ON to enable)")
endif()

# Installation
install(TARGETS brimir_libretro
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES resources/info/brimir_libretro.info
    DESTINATION share/libretro/info
)

# Print configuration summary
message(STATUS "")
message(STATUS "Brimir Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build tests: ${BRIMIR_BUILD_TESTS}")
message(STATUS "  Static linking: ${BRIMIR_STATIC_LINK}")
message(STATUS "  LTO: ${BRIMIR_LTO}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "")

