## Create the library target (STATIC for internal linking)
add_library(brimir-core STATIC
    include/brimir/brimir.hpp
    include/brimir/version.hpp

    include/brimir/core/configuration.hpp
    include/brimir/core/configuration_defs.hpp
    include/brimir/core/hash.hpp
    include/brimir/core/scheduler.hpp
    include/brimir/core/scheduler_defs.hpp
    include/brimir/core/types.hpp

    include/brimir/db/cdb_rom_db.hpp
    include/brimir/db/game_db.hpp
    include/brimir/db/ipl_db.hpp
    include/brimir/db/rom_cart_db.hpp

    include/brimir/debug/cdblock_tracer_base.hpp
    include/brimir/debug/cd_drive_tracer_base.hpp
    include/brimir/debug/debug_break.hpp
    include/brimir/debug/scsp_tracer_base.hpp
    include/brimir/debug/scu_tracer_base.hpp
    include/brimir/debug/sh2_tracer_base.hpp
    include/brimir/debug/watchpoint_defs.hpp
    include/brimir/debug/ygr_tracer_base.hpp

    include/brimir/hw/hw_defs.hpp

    include/brimir/hw/cart/cart_base.hpp
    include/brimir/hw/cart/cart_impl_bup.hpp
    include/brimir/hw/cart/cart_impl_dram.hpp
    include/brimir/hw/cart/cart_impl_none.hpp
    include/brimir/hw/cart/cart_impl_rom.hpp
    include/brimir/hw/cart/cart_slot.hpp

    include/brimir/hw/cdblock/cdblock.hpp
    include/brimir/hw/cdblock/cdblock_buffer.hpp
    include/brimir/hw/cdblock/cdblock_defs.hpp
    include/brimir/hw/cdblock/cdblock_filter.hpp
    include/brimir/hw/cdblock/cdblock_internal_callbacks.hpp
    include/brimir/hw/cdblock/cd_drive.hpp
    include/brimir/hw/cdblock/cd_drive_internal_callbacks.hpp
    include/brimir/hw/cdblock/ygr.hpp
    include/brimir/hw/cdblock/ygr_internal_callbacks.hpp

    include/brimir/hw/m68k/m68k.hpp
    include/brimir/hw/m68k/m68k_decode.hpp
    include/brimir/hw/m68k/m68k_defs.hpp
    include/brimir/hw/m68k/m68k_disasm.hpp

    include/brimir/hw/scsp/scsp.hpp
    include/brimir/hw/scsp/scsp_callbacks.hpp
    include/brimir/hw/scsp/scsp_defs.hpp
	include/brimir/hw/scsp/scsp_midi_defs.hpp
    include/brimir/hw/scsp/scsp_dsp.hpp
    include/brimir/hw/scsp/scsp_dsp_instr.hpp
    include/brimir/hw/scsp/scsp_internal_callbacks.hpp
    include/brimir/hw/scsp/scsp_slot.hpp
    include/brimir/hw/scsp/scsp_timer.hpp

    include/brimir/hw/scu/scu.hpp
    include/brimir/hw/scu/scu_callbacks.hpp
    include/brimir/hw/scu/scu_defs.hpp
    include/brimir/hw/scu/scu_dma.hpp
    include/brimir/hw/scu/scu_dsp.hpp
    include/brimir/hw/scu/scu_dsp_instr.hpp
    include/brimir/hw/scu/scu_dsp_disasm.hpp
    include/brimir/hw/scu/scu_internal_callbacks.hpp

    include/brimir/hw/sh1/sh1.hpp
    include/brimir/hw/sh1/sh1_ad.hpp
    include/brimir/hw/sh1/sh1_bsc.hpp
    include/brimir/hw/sh1/sh1_decode.hpp
    include/brimir/hw/sh1/sh1_defs.hpp
    #include/brimir/hw/sh1/sh1_disasm.hpp
    include/brimir/hw/sh1/sh1_dmac.hpp
    include/brimir/hw/sh1/sh1_excpt.hpp
    include/brimir/hw/sh1/sh1_intc.hpp
    include/brimir/hw/sh1/sh1_internal_callbacks.hpp
    include/brimir/hw/sh1/sh1_itu.hpp
    include/brimir/hw/sh1/sh1_pfc.hpp
    include/brimir/hw/sh1/sh1_power.hpp
    include/brimir/hw/sh1/sh1_regs.hpp
    include/brimir/hw/sh1/sh1_sci.hpp
    include/brimir/hw/sh1/sh1_tpc.hpp
    include/brimir/hw/sh1/sh1_wdt.hpp

    include/brimir/hw/sh2/sh2.hpp
    include/brimir/hw/sh2/sh2_bsc.hpp
    include/brimir/hw/sh2/sh2_cache.hpp
    include/brimir/hw/sh2/sh2_decode.hpp
    include/brimir/hw/sh2/sh2_disasm.hpp
    include/brimir/hw/sh2/sh2_divu.hpp
    include/brimir/hw/sh2/sh2_dmac.hpp
    include/brimir/hw/sh2/sh2_excpt.hpp
    include/brimir/hw/sh2/sh2_frt.hpp
    include/brimir/hw/sh2/sh2_intc.hpp
    include/brimir/hw/sh2/sh2_internal_callbacks.hpp
    include/brimir/hw/sh2/sh2_power.hpp
    include/brimir/hw/sh2/sh2_regs.hpp
    include/brimir/hw/sh2/sh2_sci.hpp
    include/brimir/hw/sh2/sh2_ubc.hpp
    include/brimir/hw/sh2/sh2_wdt.hpp

    include/brimir/hw/smpc/rtc.hpp
    include/brimir/hw/smpc/smpc.hpp
    include/brimir/hw/smpc/smpc_internal_callbacks.hpp
    
    include/brimir/hw/smpc/peripheral/peripheral_base.hpp
    include/brimir/hw/smpc/peripheral/peripheral_callbacks.hpp
    include/brimir/hw/smpc/peripheral/peripheral_defs.hpp
    include/brimir/hw/smpc/peripheral/peripheral_impl_analog_pad.hpp
    include/brimir/hw/smpc/peripheral/peripheral_impl_arcade_racer.hpp
    include/brimir/hw/smpc/peripheral/peripheral_impl_control_pad.hpp
    include/brimir/hw/smpc/peripheral/peripheral_impl_mission_stick.hpp
    include/brimir/hw/smpc/peripheral/peripheral_impl_null.hpp
    include/brimir/hw/smpc/peripheral/peripheral_impl_shuttle_mouse.hpp
    include/brimir/hw/smpc/peripheral/peripheral_impl_virtua_gun.hpp
    include/brimir/hw/smpc/peripheral/peripheral_port.hpp
    include/brimir/hw/smpc/peripheral/peripheral_report.hpp
    include/brimir/hw/smpc/peripheral/peripheral_state_common.hpp

    include/brimir/hw/vdp/vdp.hpp
    include/brimir/hw/vdp/vdp_callbacks.hpp
    include/brimir/hw/vdp/vdp_configs.hpp
    include/brimir/hw/vdp/vdp_defs.hpp
    include/brimir/hw/vdp/vdp_internal_callbacks.hpp
    include/brimir/hw/vdp/vdp_state.hpp
    include/brimir/hw/vdp/vdp1_defs.hpp
    include/brimir/hw/vdp/vdp1_regs.hpp
    include/brimir/hw/vdp/vdp2_defs.hpp
    include/brimir/hw/vdp/vdp2_regs.hpp
    include/brimir/hw/vdp/renderer/vdp_renderer.hpp
    include/brimir/hw/vdp/renderer/vdp_renderer_base.hpp
    include/brimir/hw/vdp/renderer/vdp_renderer_defs.hpp
    include/brimir/hw/vdp/renderer/vdp_renderer_null.hpp
    include/brimir/hw/vdp/renderer/vdp_renderer_sw.hpp
    include/brimir/hw/vdp/renderer/sw/vdp1_steppers.hpp

    include/brimir/media/cdrom_crc.hpp
    include/brimir/media/disc.hpp
    include/brimir/media/filesystem.hpp
    include/brimir/media/frame_address.hpp
    include/brimir/media/iso9660.hpp
    include/brimir/media/media_defs.hpp
    include/brimir/media/saturn_header.hpp
    include/brimir/media/subheader.hpp
    
    include/brimir/media/loader/loader.hpp
    include/brimir/media/loader/loader_bin_cue.hpp
    include/brimir/media/loader/loader_chd.hpp
    include/brimir/media/loader/loader_img_ccd_sub.hpp
    include/brimir/media/loader/loader_iso.hpp
    include/brimir/media/loader/loader_mdf_mds.hpp
    include/brimir/media/loader/loader_result.hpp

    include/brimir/media/binary_reader/binary_reader.hpp
    include/brimir/media/binary_reader/binary_reader_composite.hpp
    include/brimir/media/binary_reader/binary_reader_file.hpp
    include/brimir/media/binary_reader/binary_reader_impl.hpp
    include/brimir/media/binary_reader/binary_reader_mem.hpp
    include/brimir/media/binary_reader/binary_reader_mmap.hpp
    include/brimir/media/binary_reader/binary_reader_subview.hpp
    include/brimir/media/binary_reader/binary_reader_zero.hpp

    include/brimir/state/state.hpp
    include/brimir/state/state_cdblock.hpp
    include/brimir/state/state_cd_drive.hpp
    include/brimir/state/state_m68k.hpp
    include/brimir/state/state_scheduler.hpp
    include/brimir/state/state_scsp.hpp
    include/brimir/state/state_scsp_dsp.hpp
    include/brimir/state/state_scsp_slot.hpp
    include/brimir/state/state_scsp_timer.hpp
    include/brimir/state/state_scu.hpp
    include/brimir/state/state_sh1.hpp
    include/brimir/state/state_sh2.hpp
    include/brimir/state/state_smpc.hpp
    include/brimir/state/state_system.hpp
    include/brimir/state/state_vdp.hpp
    include/brimir/state/state_ygr.hpp

    include/brimir/sys/backup_ram.hpp
    include/brimir/sys/backup_ram_defs.hpp
    include/brimir/sys/bus.hpp
    include/brimir/sys/clocks.hpp
    include/brimir/sys/memory.hpp
    include/brimir/sys/memory_defs.hpp
    include/brimir/sys/saturn.hpp
    include/brimir/sys/system.hpp
    include/brimir/sys/system_features.hpp
    include/brimir/sys/system_internal_callbacks.hpp
    
    include/brimir/util/arith_ops.hpp
    include/brimir/util/backup_datetime.hpp
    include/brimir/util/benchmark.hpp
    include/brimir/util/bit_ops.hpp
    include/brimir/util/bitmask_enum.hpp
    include/brimir/util/callback.hpp
    include/brimir/util/compiler_info.hpp
    include/brimir/util/constexpr_for.hpp
    include/brimir/util/cpu_features.hpp
    include/brimir/util/data_ops.hpp
    include/brimir/util/date_time.hpp
    include/brimir/util/dev_assert.hpp
    include/brimir/util/dev_log.hpp
    include/brimir/util/event.hpp
    include/brimir/util/function_info.hpp
    include/brimir/util/inline.hpp
    include/brimir/util/lsn_denormals.hpp
    include/brimir/util/process.hpp
    include/brimir/util/scope_guard.hpp
    include/brimir/util/size_ops.hpp
    include/brimir/util/thread_name.hpp
    include/brimir/util/type_traits_ex.hpp
    include/brimir/util/unreachable.hpp
    include/brimir/vdp_profiling.hpp


    src/brimir.cpp
    src/vdp_profiling.cpp

    src/core/configuration.cpp
    src/core/hash.cpp

    src/db/cdb_rom_db.cpp
    src/db/game_db.cpp
    src/db/ipl_db.cpp
    src/db/rom_cart_db.cpp

    src/hw/cart/cart_impl_bup.cpp
    src/hw/cart/cart_slot.cpp

    src/hw/cdblock/cdblock.cpp
    src/hw/cdblock/cdblock_devlog.hpp
    src/hw/cdblock/cdblock_partition_manager.cpp
    src/hw/cdblock/cd_drive.cpp
    src/hw/cdblock/ygr.cpp
    
    src/hw/m68k/m68k.cpp
    src/hw/m68k/m68k_addr_modes.hpp
    src/hw/m68k/m68k_decode.cpp
    src/hw/m68k/m68k_disasm.cpp
    
    src/hw/scsp/scsp.cpp
    src/hw/scsp/scsp_dsp.cpp
    src/hw/scsp/scsp_slot.cpp
    
    src/hw/scu/scu.cpp
    src/hw/scu/scu_devlog.hpp
    src/hw/scu/scu_dsp.cpp
    src/hw/scu/scu_dsp_disasm.cpp

    src/hw/smpc/rtc.cpp
    src/hw/smpc/smpc.cpp

    src/hw/smpc/peripheral/peripheral_impl_analog_pad.cpp
    src/hw/smpc/peripheral/peripheral_impl_arcade_racer.cpp
    src/hw/smpc/peripheral/peripheral_impl_control_pad.cpp
    src/hw/smpc/peripheral/peripheral_impl_mission_stick.cpp
    src/hw/smpc/peripheral/peripheral_impl_shuttle_mouse.cpp
    src/hw/smpc/peripheral/peripheral_impl_virtua_gun.cpp

    src/hw/sh1/sh1.cpp
    src/hw/sh1/sh1_decode.cpp

    src/hw/sh2/sh2.cpp
    src/hw/sh2/sh2_decode.cpp
    src/hw/sh2/sh2_disasm.cpp

    src/hw/vdp/vdp.cpp
    src/hw/vdp/renderer/vdp_renderer_base.cpp
    src/hw/vdp/renderer/vdp_renderer_sw.cpp

    src/media/cdrom_crc.cpp
    src/media/filesystem.cpp
    src/media/saturn_header.cpp

    src/media/loader/loader.cpp
    src/media/loader/loader_bin_cue.cpp
    src/media/loader/loader_chd.cpp
    src/media/loader/loader_img_ccd_sub.cpp
    src/media/loader/loader_iso.cpp
    src/media/loader/loader_mdf_mds.cpp

    src/sys/backup_ram.cpp
    src/sys/memory.cpp
    src/sys/saturn.cpp

    src/util/backup_datetime.cpp
    src/util/cpu_features.cpp
    src/util/date_time.cpp
    src/util/event.cpp
    src/util/process.cpp
    src/util/thread_name.cpp
    src/util/virtual_memory.cpp
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
    xxhash  # xxHash exports 'xxhash' target from build/cmake
    chdr-static
)
if (WIN32)
    ## synchronization.lib required for WaitOnAddress and WakeByAddressAll
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

## Define version macros
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
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION="${Brimir_VERSION}")
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_MAJOR=${Brimir_VERSION_MAJOR})
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_MINOR=${Brimir_VERSION_MINOR})
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_PATCH=${Brimir_VERSION_PATCH})
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_PRERELEASE="${Brimir_VERSION_PRERELEASE}")
target_compile_definitions(brimir-core PUBLIC BRIMIR_VERSION_BUILD="${Brimir_VERSION_BUILD}")
if (DEFINED Brimir_BUILD_TIMESTAMP AND NOT Brimir_BUILD_TIMESTAMP STREQUAL "")
    target_compile_definitions(brimir-core PUBLIC BRIMIR_BUILD_TIMESTAMP="${Brimir_BUILD_TIMESTAMP}")
endif ()

## Define additional macros
target_compile_definitions(brimir-core PUBLIC "BRIMIR_ENABLE_DEVLOG=$<BOOL:${Brimir_ENABLE_DEVLOG}>")
target_compile_definitions(brimir-core PUBLIC "BRIMIR_DEV_ASSERTIONS=$<BOOL:${Brimir_ENABLE_DEV_ASSERTIONS}>")
target_compile_definitions(brimir-core PUBLIC "BRIMIR_DEV_BUILD=$<BOOL:${Brimir_DEV_BUILD}>")
target_compile_definitions(brimir-core PUBLIC "BRIMIR_EXTRA_INLINING=$<BOOL:${Brimir_EXTRA_INLINING}>")
target_compile_definitions(brimir-core PUBLIC "TOML_EXCEPTIONS=0")

## Generate the export header and attach it to the target
include(GenerateExportHeader)
generate_export_header(brimir-core EXPORT_FILE_NAME include/brimir/export.h)
target_compile_definitions(brimir-core PUBLIC "$<$<NOT:$<BOOL:${BUILD_SHARED_LIBS}>>:Brimir_core_STATIC_DEFINE>")
target_include_directories(brimir-core PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>")

if (IPO_SUPPORTED AND Brimir_ENABLE_IPO)
    message(STATUS "Enabling IPO / LTO for brimir-core")
    set_property(TARGET brimir-core PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

## Apply performance options
if (Brimir_AVX2)
    target_compile_definitions(brimir-core PUBLIC ${PROJECT_NAME}_AVX2=1)
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
    # ARM GCC is stricter about force-inline - allow as warning instead of error  
    target_compile_options(brimir-core PRIVATE -Wno-error=attributes)
endif()

## Enable unity build
#set_target_properties(brimir-core PROPERTIES UNITY_BUILD ON)
#set_target_properties(brimir-core PROPERTIES UNITY_BUILD_BATCH_SIZE 0)

## Configure Visual Studio filters
# GPU rendering support (optional)
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

