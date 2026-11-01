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
    # devlog must never throw (issue #7: crash on Close Content in GUI frontends)
    unit/test_dev_log.cpp
    unit/test_cdblock_hle.cpp
    unit/test_game_db.cpp
    # Ymir hardware-layer backport regressions (SMPC, VDP1, VDP2)
    unit/test_hw_backports.cpp
    # VDP1 line stepper: clip termination and explicit wrapping
    unit/test_vdp1_line_stepper.cpp
    unit/test_dirty_bitmap.cpp
    # VDP color calculation tests
    unit/test_vdp_color_calc.cpp
    unit/test_vdp_composite.cpp
    unit/test_vdp_savestate.cpp
    unit/test_vdp_uninit_state.cpp
    unit/test_vdp_external_renderer.cpp
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
    unit/test_jit_ir_opt.cpp
    unit/test_jit_backend.cpp
    unit/test_jit_helpers.cpp
    unit/test_jit_diff.cpp
    unit/jit_opcode_specs.cpp
    unit/test_jit_opcodes.cpp
    unit/test_jit_executor.cpp
    unit/test_sh2_state_diff.cpp
    unit/test_jit_integration.cpp
    unit/test_jit_lockstep.cpp
    unit/jit_test_backend.cpp
    unit/jit_random_ir.cpp
    unit/jit_fuzz_programs.cpp
    unit/test_jit_x64.cpp
    unit/test_jit_worker.cpp
    # VDP2 GPU parameter structs vs the reflected shader layouts, 32-bit rotation math ([vdp2] runs without Vulkan;
    # the [gpu] case is compiled with BRIMIR_VULKAN only)
    unit/test_gpu_vdp2_params.cpp
    # VDP2 shaders on the Vulkan 1.2 baseline: SPIR-V checks ([vdp2] runs without Vulkan) and pipeline creation
    # ([gpu], compiled with BRIMIR_VULKAN only)
    unit/test_gpu_vdp2_pipelines.cpp
    # VDP1 GPU structs, accessors and bindings vs the reflected layouts of the layout probe ([vdp1] runs without
    # Vulkan; the [gpu] case is compiled with BRIMIR_VULKAN only)
    unit/test_gpu_vdp1_params.cpp
    # VDP2 front half (parameter packing, CRAM cache, batching, frame records) through the CPU scene harness
    unit/vdp2_scene.cpp
    # CPU reference of the VDP2 sub-pixel rules at internal resolution N (the coordinate-bitmap scenes)
    unit/vdp2_subpixel_reference.cpp
    unit/test_vdp2_subpixel_reference.cpp
    unit/test_vdp2_front.cpp
    # CPU reference of the GPU VDP1 back half (keyed fragments, per-word ordered resolve, erase, CPU writes) fed by the
    # VDP1 front half, against software
    unit/vdp1_raster_reference.cpp
    unit/vdp1_scenario.cpp
    unit/test_vdp1_raster_reference.cpp
    # CPU reference of the N x VDP1 rasteriser (GPU stage 3: rect / quad / replicate spans, N x resolve) on the same
    # op lists, against the native reference; plus the render mode for captured frames ([.][nx-render])
    unit/vdp1_nx_reference.cpp
    unit/test_vdp1_nx_reference.cpp
    unit/test_vdp1_nx_render.cpp
    # VDP1 shaders on the Vulkan 1.2 baseline: SPIR-V checks ([vdp1] runs without Vulkan) and pipeline creation
    # ([gpu], compiled with BRIMIR_VULKAN only)
    unit/test_gpu_vdp1_pipelines.cpp
    # VDP1 front half (commands, spans, op stream, batching, VRAM uploads)
    unit/test_vdp1_front.cpp
    # brimir_vkhost's sample-point mapping and sharpness metric (tools/vkhost/sample_points.hpp, header-only)
    unit/test_vkhost_sample_points.cpp
)

target_link_libraries(brimir_tests PRIVATE
    brimir_bridge
    brimir-jit
    brimir-core
    brimir-vdp2-front
    brimir-vdp1-front
)

# Vulkan GPU renderer (src/gpu)
if(BRIMIR_VULKAN)
    target_sources(brimir_tests PRIVATE
        unit/test_gpu_build.cpp
        # Headless device, owning/non-owning VulkanDevice, vk_util helpers (SKIP without a Vulkan device)
        unit/test_gpu_device.cpp
        # Frame output pass (convert, overscan crop, rotation) against the software bridge
        unit/test_gpu_frame_output.cpp
        # VulkanVDPRenderer (GPU VDP2 back half, software VDP1) against the software renderer, scene by scene
        unit/test_gpu_vdp2_scenes.cpp
        # VDP1 passes (raster, bucket, scatter, resolve, erase, CPU writes) on the device against the CPU reference
        unit/test_gpu_vdp1_passes.cpp
        # VulkanVDPRenderer with VDP1 on the GPU against the software renderer: FBRAM and frames, scene by scene
        unit/test_gpu_vdp1_scenes.cpp
        # The VDP1 parity fuzzer: seeded random programs, FBRAM after every op, GPU against software
        unit/test_gpu_vdp1_fuzz.cpp
        # CoreWrapper with the Vulkan VDP renderer: install, revert, VDP1 threading, device frames (no device needed)
        unit/test_core_wrapper_vulkan.cpp
    )
    target_link_libraries(brimir_tests PRIVATE brimir-gpu)
endif()

target_include_directories(brimir_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/libretro
)

target_compile_definitions(brimir_tests PRIVATE BRIMIR_BUILD_TESTS)
# The VDP1 fuzzer's long-run log header names the source tree's commit (tests/unit/test_gpu_vdp1_fuzz.cpp): the one at
# configure time here (stale after later commits until the next configure), the one at run time from git when it can run.
execute_process(COMMAND git -C "${PROJECT_SOURCE_DIR}" describe --always --dirty --abbrev=12
                OUTPUT_VARIABLE brimir_git_describe OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT brimir_git_describe)
    set(brimir_git_describe "unknown")
endif()
target_compile_definitions(brimir_tests PRIVATE BRIMIR_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                                                BRIMIR_GIT_DESCRIBE="${brimir_git_describe}")

# Register with CTest (enable_testing() is called from the top-level CMakeLists).
# The amalgamated Catch2 build has no catch_discover_tests, so the whole suite
# runs as a single CTest entry. Run with: ctest --test-dir build --output-on-failure
add_test(NAME brimir_tests
    COMMAND brimir_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

# With the x64 backend built, the default entry runs the JIT tests on x64; this one keeps the
# IR backend (RunBlock) covered too.
if(BRIMIR_JIT_X64_EFFECTIVE)
    add_test(NAME brimir_tests_jit_ir
        COMMAND brimir_tests "[jit]"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    set_tests_properties(brimir_tests_jit_ir PROPERTIES ENVIRONMENT "BRIMIR_JIT_BACKEND=ir")
endif()

# The stage 1 GPU scenes (VDP2 on the GPU) again with VDP1 on the GPU (vdp2_scene.hpp BRIMIR_VDP1_MODE): both VDP1
# modes must keep them passing.
if(BRIMIR_VULKAN)
    add_test(NAME brimir_tests_gpu_vdp1
        COMMAND brimir_tests "[gpu][vdp2][scene]"
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
    # Every case in this entry is [gpu]: without a Vulkan device they all SKIP and Catch2 exits with 4 ("all test cases
    # skipped"), which counts as skipped here. With BRIMIR_REQUIRE_VULKAN=1 (the lavapipe job) the cases fail instead.
    set_tests_properties(brimir_tests_gpu_vdp1 PROPERTIES ENVIRONMENT "BRIMIR_VDP1_MODE=gpu" SKIP_RETURN_CODE 4)
endif()

# The libretro Vulkan context through brimir_vkhost (tests/vkhost_checks.cmake): BIOS menu frames, software vs
# Vulkan, refused context, driver reinit, run-ahead. Skipped without the US BIOS fixture or a Vulkan device.
if(TARGET brimir_vkhost)
    add_test(NAME brimir_vkhost_vulkan
        COMMAND ${CMAKE_COMMAND}
            -DVKHOST=$<TARGET_FILE:brimir_vkhost>
            -DSTATE_DIFF=$<TARGET_FILE:brimir_state_diff>
            -DBIOS=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mpr-17933.bin
            -P ${CMAKE_CURRENT_SOURCE_DIR}/vkhost_checks.cmake
    )
    set_tests_properties(brimir_vkhost_vulkan PROPERTIES SKIP_REGULAR_EXPRESSION "VKHOST_CHECKS_SKIPPED")
endif()

# Copy test fixtures if they exist
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
         DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()


