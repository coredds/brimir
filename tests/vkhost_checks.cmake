# brimir_vkhost checks of the libretro Vulkan context (ctest entry brimir_vkhost_vulkan, tests/CMakeLists.txt).
#
#   cmake -DVKHOST=<brimir_vkhost> -DBIOS=<US BIOS image> -P tests/vkhost_checks.cmake
#
# Runs the BIOS menu (no content) through the core loaded as a libretro core and compares the frame hashes of the
# software path with the Vulkan path in several frontend situations. Prints VKHOST_CHECKS_SKIPPED (ctest reports
# the test as skipped) when the BIOS or a Vulkan device is missing; with BRIMIR_REQUIRE_VULKAN=1 a missing device
# fails instead. Every run with a Vulkan device must have the validation layer on (install it, or set
# BRIMIR_VKHOST_ALLOW_NO_VALIDATION=1 to only warn).

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

function(require_log name regex)
    if(NOT "${${name}_err}" MATCHES "${regex}")
        fail(${name} "expected '${regex}' in the log")
    endif()
endfunction()

# A run with the host's Vulkan device: validation on, no validation errors, no queue-lock violations, no GPU calls in
# hidden frames.
function(check_vulkan_counters name)
    if("${${name}_err}" MATCHES "validation off")
        if("$ENV{BRIMIR_VKHOST_ALLOW_NO_VALIDATION}" STREQUAL "1")
            message(WARNING "${name}: the Vulkan validation layer is not installed: 'validation errors 0' proves nothing")
        else()
            fail(${name} "the Vulkan validation layer is not installed (install it, or set BRIMIR_VKHOST_ALLOW_NO_VALIDATION=1)")
        endif()
    endif()
    require_log(${name} "queue-lock violations 0,")
    require_log(${name} "hidden-frame GPU calls 0,")
    require_log(${name} "validation errors 0")
endfunction()

# Exit code 0 and 300 frames, `hardware` of them read back from set_image. Compared with software_hashes (or
# <reference>_hashes as the third argument); at most `allowed` frames (fourth argument, default 0) may differ.
function(check_run name hardware)
    set(reference software)
    if(ARGC GREATER 2)
        set(reference ${ARGV2})
    endif()
    set(allowed 0)
    if(ARGC GREATER 3)
        set(allowed ${ARGV3})
    endif()
    if(NOT "${${name}_code}" STREQUAL "0")
        fail(${name} "brimir_vkhost exited with ${${name}_code}")
    endif()
    list(LENGTH ${name}_hashes count)
    if(NOT count EQUAL 300)
        fail(${name} "${count} frames recorded, expected 300")
    endif()
    set(differences 0)
    set(first_difference "")
    foreach(i RANGE 299)
        list(GET ${name}_hashes ${i} got)
        list(GET ${reference}_hashes ${i} want)
        if(NOT got STREQUAL want)
            math(EXPR differences "${differences} + 1")
            if(first_difference STREQUAL "")
                set(first_difference "'${got}' vs '${want}'")
            endif()
        endif()
    endforeach()
    if(differences GREATER allowed)
        fail(${name} "${differences} frames differ from the ${reference} run (first: ${first_difference})")
    endif()
    require_log(${name} "recorded 300 frames \\(${hardware} hardware\\)")
    message(STATUS "${name}: 300 frames, ${differences} different from ${reference} (allowed ${allowed}), "
                   "${hardware} hardware")
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
    if("$ENV{BRIMIR_REQUIRE_VULKAN}" STREQUAL "1")
        fail(vulkan "no usable Vulkan device and BRIMIR_REQUIRE_VULKAN=1")
    endif()
    message("VKHOST_CHECKS_SKIPPED: no usable Vulkan device\n${vulkan_err}")
    return()
endif()
check_run(vulkan 300)
check_vulkan_counters(vulkan)
require_log(vulkan "device created through create_device2")
require_log(vulkan "Vulkan frame output ready")

# A frontend on another video driver refuses SET_HW_RENDER: software, with the fallback logged.
run_vkhost(refused --vulkan --refuse-hw)
check_run(refused 0)
require_log(refused "Vulkan renderer unavailable, falling back to software")

# A frontend on another video driver that would switch drivers: the core does not even ask.
run_vkhost(preferred_gl --vulkan --preferred-hw gl)
check_run(preferred_gl 0)
check_vulkan_counters(preferred_gl)
require_log(preferred_gl
    "Vulkan renderer selected but the frontend video driver is not Vulkan; using software")
if(preferred_gl_err MATCHES "SET_HW_RENDER")
    fail(preferred_gl "the core called SET_HW_RENDER")
endif()

# A GPU without Vulkan 1.2 (as far as the core can tell): the copy-only output, still through Vulkan.
run_vkhost(copy_only --vulkan --fake-api 1.1)
check_run(copy_only 300)
check_vulkan_counters(copy_only)
require_log(copy_only "using copy-only frame output")

# A driver reinit mid-run: context_destroy, a new device, context_reset.
run_vkhost(reset --vulkan --reset-at 120)
check_run(reset 300)
check_vulkan_counters(reset)
require_log(reset "context resets 2,")

# The same from inside retro_run (RetroArch reinits inside SET_SYSTEM_AV_INFO, which PAL content sends): the core
# gets context_destroy and context_reset while it holds its own lock.
run_vkhost(reset_in_run --vulkan --reset-in-run 150)
check_run(reset_in_run 300)
check_vulkan_counters(reset_in_run)
require_log(reset_in_run "context resets 2,")

# The sync index mask grows mid-run (0b1 -> 0b111): the slots are re-created.
run_vkhost(grow --vulkan --grow-mask 100)
check_run(grow 300)
check_vulkan_counters(grow)

# ... and re-creating the compute output fails: copy-only from then on, one repeated frame at the switch (frame 100
# = recorded frame 40).
run_vkhost(grow_fail --vulkan --grow-mask 100 --fail-compute-on-grow)
check_run(grow_fail 299 software 1)
check_vulkan_counters(grow_fail)
require_log(grow_fail "Vulkan frame output changed to copy-only")

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
check_vulkan_counters(runahead)
require_log(runahead "second instance ran 360 frames, 0 hardware frames, context none")

# --compare: software and Vulkan in two child processes. Identical content passes; a Vulkan run that is rotated 180
# degrees (the menu has content after 400 frames) must exit 3 and report the first differing pixel.
execute_process(
    COMMAND "${VKHOST}" ${common_args} --compare
    OUTPUT_VARIABLE compare_out ERROR_VARIABLE compare_err RESULT_VARIABLE compare_code)
if(NOT compare_code EQUAL 0 OR
   NOT compare_out MATCHES "compare: OK, 300 frames identical \\(software vs vulkan compute output: 300 hardware frames, [1-9][0-9]* core submits, validation on, 0 validation errors\\)")
    message(FATAL_ERROR "--compare of identical runs: exit ${compare_code}\n${compare_out}\n${compare_err}")
endif()
execute_process(
    COMMAND "${VKHOST}" --bios "${BIOS}" --warmup 400 --frames 3 --compare --compare-vulkan-option brimir_rotation=180
    OUTPUT_VARIABLE compare_out ERROR_VARIABLE compare_err RESULT_VARIABLE compare_code)
string(REGEX MATCH "work directory kept: ([^\r\n]*)\\)" ignored "${compare_out}")
if(CMAKE_MATCH_1)
    file(REMOVE_RECURSE "${CMAKE_MATCH_1}")
endif()
if(NOT compare_code EQUAL 3 OR NOT compare_out MATCHES "first at frame 0" OR
   NOT compare_out MATCHES "first differing pixel \\([0-9]+,[0-9]+\\): software 0x[0-9A-F]+, vulkan 0x[0-9A-F]+")
    message(FATAL_ERROR "--compare of different runs: exit ${compare_code}\n${compare_out}\n${compare_err}")
endif()
message(STATUS "--compare: identical runs pass, a difference is reported (exit 3, first pixel)")

# --compare must not pass when the Vulkan run silently stayed on software or the copy-only output (the comparison
# would be vacuous). The environment hook adds arguments to the Vulkan child only.
foreach(case IN ITEMS "--refuse-hw|no \"Vulkan frame output ready\"" "--fake-api 1.1|using copy-only frame output"
                      "--preferred-hw gl|read back 0 hardware frames")
    string(REPLACE "|" ";" case_parts "${case}")
    list(GET case_parts 0 case_args)
    list(GET case_parts 1 case_expect)
    set(ENV{BRIMIR_VKHOST_COMPARE_VULKAN_ARGS} "${case_args}")
    execute_process(
        COMMAND "${VKHOST}" --bios "${BIOS}" --warmup 30 --frames 30 --compare
        OUTPUT_VARIABLE vacuous_out ERROR_VARIABLE vacuous_err RESULT_VARIABLE vacuous_code)
    unset(ENV{BRIMIR_VKHOST_COMPARE_VULKAN_ARGS})
    string(REGEX MATCH "work directory kept: ([^\r\n]*)\\)" ignored "${vacuous_out}")
    if(CMAKE_MATCH_1)
        file(REMOVE_RECURSE "${CMAKE_MATCH_1}")
    endif()
    if(NOT vacuous_code EQUAL 3 OR NOT vacuous_out MATCHES "compare: FAILED" OR NOT vacuous_out MATCHES "${case_expect}")
        message(FATAL_ERROR "--compare with Vulkan child '${case_args}' must fail with '${case_expect}': exit "
                            "${vacuous_code}\n${vacuous_out}\n${vacuous_err}")
    endif()
endforeach()
# ... and the options that would make the Vulkan run leave the compute output are refused up front.
foreach(rejected IN ITEMS "--refuse-hw" "--fake-api;1.1" "--option;brimir_renderer=software")
    execute_process(
        COMMAND "${VKHOST}" --bios "${BIOS}" --warmup 1 --frames 1 --compare ${rejected}
        OUTPUT_QUIET ERROR_VARIABLE rejected_err RESULT_VARIABLE rejected_code)
    if(NOT rejected_code EQUAL 1)
        message(FATAL_ERROR "--compare ${rejected} must be a usage error (exit 1), got ${rejected_code}\n${rejected_err}")
    endif()
endforeach()
message(STATUS "--compare: vacuous Vulkan runs and conflicting options are rejected")

message(STATUS "brimir_vkhost Vulkan checks passed")
