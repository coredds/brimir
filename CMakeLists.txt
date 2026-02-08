cmake_minimum_required(VERSION 3.28)

# Project information
project(Brimir
    VERSION 0.2.2
    DESCRIPTION "Sega Saturn emulation libretro core"
    LANGUAGES CXX C
)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Required for linking static libraries into shared libraries on Linux
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Add cmake modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Include Visual Studio helpers (MSVC only)
if(MSVC)
    include(VSHelpers)
endif()

# Compiler configuration
if(MSVC)
    # MSVC warnings
    add_compile_options(/W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_definitions(NOMINMAX)

    # Suppress common external library warnings
    set(EXTERNAL_WARNINGS
        /wd4244  # Type conversion (int to smaller type)
        /wd4100  # Unreferenced parameter
        /wd4201  # Nameless struct/union
        /wd4324  # Structure padding
        /wd4458  # Declaration hides class member
    )
else()
    # GCC/Clang warnings
    add_compile_options(
        -Wall -Wextra
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -Wno-sign-compare
        -Wno-unused-function
    )

    set(EXTERNAL_WARNINGS
        -Wno-conversion
        -Wno-unused-parameter
        -Wno-missing-field-initializers
        -Wno-sign-compare
    )
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Subdirectories - Core emulator library dependencies
include(cmake/YmirDeps.cmake)

# Set Brimir core version variables
set(Brimir_VERSION_MAJOR 0)
set(Brimir_VERSION_MINOR 2)
set(Brimir_VERSION_PATCH 2)
set(Brimir_VERSION_PRERELEASE "")
set(Brimir_VERSION_BUILD "")
set(Brimir_VERSION "0.2.2")
set(Brimir_DEV_BUILD ON)
set(Brimir_ENABLE_DEVLOG ON)
set(Brimir_ENABLE_DEV_ASSERTIONS OFF)

# Add core emulator library
add_subdirectory(src/core)

# Suppress warnings for core and vendor libraries
if(TARGET brimir-core)
    target_compile_options(brimir-core PRIVATE ${EXTERNAL_WARNINGS})
endif()
if(TARGET xxhash)
    # xxHash creates 'xxhash' target (lowercase)
    target_compile_options(xxhash PRIVATE ${EXTERNAL_WARNINGS})
endif()
if(TARGET lz4_static)
    # lz4 creates 'lz4_static' target
    target_compile_options(lz4_static PRIVATE ${EXTERNAL_WARNINGS})
endif()
if(TARGET chdr-static)
    target_compile_options(chdr-static PRIVATE ${EXTERNAL_WARNINGS})
endif()

# Bridge layer - adapts core to libretro
add_subdirectory(src/bridge)

# Libretro API implementation
add_subdirectory(src/libretro)

# Tools (benchmarking, testing utilities)
add_subdirectory(tools)

# JIT (optional - enable with -DBUILD_JIT_TESTS=ON)
option(BUILD_JIT_TESTS "Build JIT test framework and tests" OFF)
option(BRIMIR_ENABLE_JIT_TESTING "Enable JIT testing interface (adds test-only methods to SH2)" OFF)

if(BRIMIR_ENABLE_JIT_TESTING)
    add_compile_definitions(BRIMIR_ENABLE_JIT_TESTING)
    message(STATUS "JIT testing interface enabled")
endif()

if(BUILD_JIT_TESTS)
    message(STATUS "Building JIT test framework")
    add_subdirectory(src/jit)
endif()

# Main libretro core library
add_library(brimir_libretro SHARED
    $<TARGET_OBJECTS:brimir_bridge>
    $<TARGET_OBJECTS:brimir_libretro_api>
)

# Link with Brimir core
target_link_libraries(brimir_libretro PRIVATE brimir::brimir-core)

# Platform-specific link dependencies
if(UNIX)
    target_link_libraries(brimir_libretro PRIVATE pthread dl)
endif()

# Set output name
if(WIN32)
    set_target_properties(brimir_libretro PROPERTIES
        OUTPUT_NAME "brimir_libretro"
        SUFFIX ".dll"
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
    RUNTIME DESTINATION bin
)

install(FILES resources/info/brimir_libretro.info
    DESTINATION share/libretro/info
)

# Print configuration summary
message(STATUS "")
message(STATUS "Brimir Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Platform: ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "  Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build tests: ${BRIMIR_BUILD_TESTS}")
message(STATUS "  Static linking: ${BRIMIR_STATIC_LINK}")
message(STATUS "  LTO: ${BRIMIR_LTO}")
message(STATUS "")
