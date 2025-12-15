# Brimir Tests

# Use Catch2 amalgamated version (single-header)
# Downloaded from https://github.com/catchorg/Catch2/releases
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Test executable
add_executable(brimir_tests
    catch_amalgamated.cpp
    main.cpp
    # Basic smoke tests
    unit/test_basic.cpp
    # VDP priority and compositing tests
    unit/test_vdp_priority.cpp
    # VDP color calculation tests
    unit/test_vdp_color_calc.cpp
    # GPU infrastructure tests
    unit/test_gpu_infrastructure.cpp
    # Core wrapper tests (disabled until fixed)
    # unit/test_core_wrapper.cpp
    # unit/test_hardware_access.cpp
    # Other tests (disabled until we need them)
    # unit/test_memory_components.cpp
    # unit/test_scu_components.cpp
    # unit/test_smpc_operations.cpp
    # unit/test_sh2_cpu.cpp
    # unit/test_vdp_components.cpp
    # unit/test_cd_operations.cpp
    # unit/test_scsp_audio.cpp
    # unit/test_system_integration.cpp
    # unit/test_video_output.cpp
    # unit/test_audio_output.cpp
    # unit/test_input_handling.cpp
    # # BIOS tests
    # unit/test_bios.cpp
    # unit/test_bios_integration.cpp
    # unit/test_bios_boot_sequence.cpp
    # # Game loading tests
    # unit/test_game_loading.cpp
    # unit/test_save_states.cpp
    # unit/test_cartridge_support.cpp
    # unit/test_sram_persistence.cpp
    # # Hardware-accurate tests (documentation-based)
    # unit/test_sh2_cache.cpp
    # unit/test_vdp_registers.cpp
    # unit/test_memory_map.cpp
    # unit/test_scu_dma.cpp
    # # Regression tests
    # unit/test_regressions.cpp
)

target_link_libraries(brimir_tests PRIVATE
    brimir_bridge
    brimir-core
)

target_include_directories(brimir_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Enable CTest support
include(CTest)

# Note: Using amalgamated Catch2, so we don't need catch_discover_tests
# Run tests manually with: ./build/bin/Debug/brimir_tests.exe or ctest

# Copy test fixtures if they exist
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
         DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()


