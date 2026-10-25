# brimir_vkhost checks of the libretro Vulkan context (ctest entry brimir_vkhost_vulkan, tests/CMakeLists.txt).
#
#   cmake -DVKHOST=<brimir_vkhost> -DBIOS=<US BIOS image> -P tests/vkhost_checks.cmake
#
# Runs the BIOS menu (no content) through the core loaded as a libretro core and compares the frame hashes of the
# software path with the Vulkan path in several frontend situations. Prints VKHOST_CHECKS_SKIPPED (ctest reports
# the test as skipped) when the BIOS or a Vulkan device is missing.

cmake_minimum_required(VERSION 3.28)

if(NOT VKHOST OR NOT EXISTS "${VKHOST}")
    message(FATAL_ERROR "VKHOST (brimir_vkhost) not found: ${VKHOST}")
endif()
if(NOT BIOS OR NOT EXISTS "${BIOS}")
    message("VKHOST_CHECKS_SKIPPED: no BIOS image (${BIOS}); see tests/fixtures/README.md")
    return()
endif()

set(common_args --bios "${BIOS}" --warmup 60 --frames 300 --hash)

# Runs brimir_vkhost with the common arguments plus ARGN. Sets <prefix>_code, <prefix>_hashes (the "frame ..."
# lines; Ymir also logs to stdout) and <prefix>_err in the caller.
function(run_vkhost prefix)
    execute_process(
        COMMAND "${VKHOST}" ${common_args} ${ARGN}
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
        RESULT_VARIABLE code
    )
    string(REGEX MATCHALL "frame [0-9]+ [0-9]+x[0-9]+ [0-9a-f]+" hashes "${out}")
    set(${prefix}_code "${code}" PARENT_SCOPE)
    set(${prefix}_hashes "${hashes}" PARENT_SCOPE)
    set(${prefix}_err "${err}" PARENT_SCOPE)
endfunction()

function(fail name message_text)
    message(FATAL_ERROR "${name}: ${message_text}\n--- vkhost stderr ---\n${${name}_err}")
endfunction()

# Exit code 0, 300 frames equal to the software run (software_hashes, or <reference>_hashes when given), `hardware`
# of them read back from set_image.
function(check_run name hardware)
    set(reference software)
    if(ARGC GREATER 2)
        set(reference ${ARGV2})
    endif()
    set(software_hashes "${${reference}_hashes}")
    if(NOT "${${name}_code}" STREQUAL "0")
        fail(${name} "brimir_vkhost exited with ${${name}_code}")
    endif()
    list(LENGTH ${name}_hashes count)
    if(NOT count EQUAL 300)
        fail(${name} "${count} frames recorded, expected 300")
    endif()
    if(NOT "${${name}_hashes}" STREQUAL "${software_hashes}")
        foreach(i RANGE 299)
            list(GET ${name}_hashes ${i} got)
            list(GET software_hashes ${i} want)
            if(NOT got STREQUAL want)
                fail(${name} "differs from the software run: '${got}' vs '${want}'")
            endif()
        endforeach()
    endif()
    if(NOT "${${name}_err}" MATCHES "recorded 300 frames \\(${hardware} hardware\\)")
        fail(${name} "expected ${hardware} hardware frames")
    endif()
    message(STATUS "${name}: 300 frames identical to software (${hardware} hardware)")
endfunction()

run_vkhost(software)
if(NOT software_code EQUAL 0)
    fail(software "brimir_vkhost exited with ${software_code}")
endif()
list(LENGTH software_hashes software_count)
if(NOT software_count EQUAL 300)
    fail(software "${software_count} frames recorded, expected 300")
endif()

run_vkhost(vulkan --vulkan)
if(vulkan_code EQUAL 4)
    message("VKHOST_CHECKS_SKIPPED: no usable Vulkan device\n${vulkan_err}")
    return()
endif()
check_run(vulkan 300)
if(NOT vulkan_err MATCHES "device created through create_device2")
    fail(vulkan "the device was not created through the negotiation interface")
endif()
if(NOT vulkan_err MATCHES "queue-lock violations 0,")
    fail(vulkan "queue-lock violations")
endif()

# A frontend on another video driver refuses SET_HW_RENDER: software, with the fallback logged.
run_vkhost(refused --vulkan --refuse-hw)
check_run(refused 0)
if(NOT refused_err MATCHES "Vulkan renderer unavailable, falling back to software")
    fail(refused "no fallback log line")
endif()

# A driver reinit mid-run: context_destroy, a new device, context_reset.
run_vkhost(reset --vulkan --reset-at 120)
check_run(reset 300)
if(NOT reset_err MATCHES "context resets 2,")
    fail(reset "expected two context resets")
endif()
if(NOT reset_err MATCHES "validation errors 0")
    fail(reset "validation errors")
endif()

# The same from inside retro_run (RetroArch reinits inside SET_SYSTEM_AV_INFO, which PAL content sends): the core
# gets context_destroy and context_reset while it holds its own lock.
run_vkhost(reset_in_run --vulkan --reset-in-run 150)
check_run(reset_in_run 300)
if(NOT reset_in_run_err MATCHES "context resets 2,")
    fail(reset_in_run "expected two context resets")
endif()

# Run-ahead: hidden frames (video disabled) and a second instance without a context. The hidden frames are not
# rolled back (see tools/vkhost/run_core.cpp), so the reference is a software run with the same pattern.
run_vkhost(software_runahead --runahead)
if(NOT software_runahead_code EQUAL 0)
    fail(software_runahead "brimir_vkhost exited with ${software_runahead_code}")
endif()
if("${software_runahead_hashes}" STREQUAL "${software_hashes}")
    fail(software_runahead "the hidden frames did not advance the emulation")
endif()
run_vkhost(runahead --vulkan --runahead)
check_run(runahead 300 software_runahead)
if(NOT runahead_err MATCHES "hidden-frame GPU calls 0,")
    fail(runahead "GPU calls in hidden frames")
endif()
if(NOT runahead_err MATCHES "second instance ran 360 frames, 0 hardware frames, context none")
    fail(runahead "the second instance did not run as expected")
endif()

message(STATUS "brimir_vkhost Vulkan checks passed")
