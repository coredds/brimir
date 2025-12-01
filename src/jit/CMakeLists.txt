# ==============================================================================
# Brimir SH-2 JIT Compiler
# ==============================================================================

cmake_minimum_required(VERSION 3.20)

# Project
project(BrimirJIT VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ==============================================================================
# Dependencies
# ==============================================================================

# Include Brimir core headers
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# ==============================================================================
# JIT Library (Future - Phase 3+)
# ==============================================================================

# TODO: Build actual JIT compiler once Phase 2 is complete
# add_library(brimir-jit
#     src/backends/...
# )

# ==============================================================================
# Brimir SH-2 Wrapper for Testing
# ==============================================================================

add_library(brimir-sh2-wrapper
    src/validation/brimir_sh2_wrapper.cpp
    include/brimir_sh2_wrapper.hpp
)

target_include_directories(brimir-sh2-wrapper PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/include
)

# Link against Brimir core
target_link_libraries(brimir-sh2-wrapper PUBLIC
    brimir::brimir-core
)

# ==============================================================================
# JIT Test Framework
# ==============================================================================

add_library(jit-test-framework
    src/validation/jit_test_framework.cpp
    src/validation/instruction_test_generator.cpp
    src/validation/block_test_generator.cpp
    src/validation/control_flow_test_generator.cpp
    src/validation/fuzz_test_generator.cpp
    src/validation/game_regression_test.cpp
    include/jit_test_framework.hpp
)

target_include_directories(jit-test-framework PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(jit-test-framework PUBLIC
    brimir-sh2-wrapper
)

# ==============================================================================
# Test Executable
# ==============================================================================

add_executable(jit-tests
    tests/test_runner.cpp
)

target_link_libraries(jit-tests PRIVATE
    jit-test-framework
    brimir-sh2-wrapper
)

# ==============================================================================
# Installation (Optional)
# ==============================================================================

# install(TARGETS jit-tests DESTINATION bin)

# ==============================================================================
# Test Target
# ==============================================================================

enable_testing()
add_test(NAME JITValidationTests COMMAND jit-tests)
