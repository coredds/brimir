# Brimir Tests

# Use Catch2 amalgamated version (single-header)
# Downloaded from https://github.com/catchorg/Catch2/releases
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Test executable
add_executable(brimir_tests
    catch_amalgamated.cpp
    main.cpp
    # Options metadata (compiled directly from libretro layer)
    ../src/libretro/options.cpp
    unit/test_options.cpp
    # Basic smoke tests
    unit/test_basic.cpp
    unit/test_bitmask_enum.cpp
    unit/test_cdblock_hle.cpp
    unit/test_game_db.cpp
    # Ymir hardware-layer backport regressions (SMPC, VDP1, VDP2)
    unit/test_hw_backports.cpp
    # VDP color calculation tests
    unit/test_vdp_color_calc.cpp
    unit/test_vdp_composite.cpp
    # Core wrapper tests (audio volume, rotation, overscan crop)
    unit/test_core_wrapper.cpp
    unit/test_profiler.cpp
    # Audio ring buffer tests
    unit/test_audio_ring_buffer.cpp
    # Media loader tests (CUE/CCD compressed audio, sparse tracks, sector alignment)
    unit/test_media_loader.cpp
    # SRAM persistence tests
    unit/test_sram_persistence.cpp
    # Saturn hardware state via GetSaturn() (memory, CD tray, config/savestate round trips)
    unit/test_memory_components.cpp
    unit/test_cd_operations.cpp
    unit/test_system_integration.cpp
    # BIOS tests (integration cases SKIP when no real BIOS is in tests/fixtures/)
    unit/test_bios.cpp
    unit/test_bios_integration.cpp
    # SH-2 JIT (isolated SH-2 rig and differential tests)
    unit/sh2_test_rig.cpp
    unit/test_sh2_jit_iface.cpp
    unit/test_jit_ir.cpp
)

target_link_libraries(brimir_tests PRIVATE
    brimir_bridge
    brimir-jit
    brimir-core
)

target_include_directories(brimir_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/libretro
)

target_compile_definitions(brimir_tests PRIVATE BRIMIR_BUILD_TESTS)

# Register with CTest (enable_testing() is called from the top-level CMakeLists).
# The amalgamated Catch2 build has no catch_discover_tests, so the whole suite
# runs as a single CTest entry. Run with: ctest --test-dir build --output-on-failure
add_test(NAME brimir_tests
    COMMAND brimir_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# Copy test fixtures if they exist
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
         DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()


