cmake_minimum_required(VERSION 3.28)

# Project information
project(Brimir
    VERSION 0.1.0
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
    add_compile_options(/W4 /WX-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Include directories
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Subdirectories - Ymir emulator core dependencies
# Only include what ymir-core actually needs (skip imgui/SDL3)
include(cmake/YmirDeps.cmake)

# Add only Ymir's core library (not apps or tests)
add_subdirectory(src/ymir/libs EXCLUDE_FROM_ALL)

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

