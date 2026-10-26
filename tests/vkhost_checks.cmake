# brimir_vkhost checks of the libretro Vulkan context (ctest entry brimir_vkhost_vulkan, tests/CMakeLists.txt).
#
#   cmake -DVKHOST=<brimir_vkhost> -DBIOS=<US BIOS image> -P tests/vkhost_checks.cmake
#
# Runs the BIOS menu (no content) through the core loaded as a libretro core and compares the frame hashes of the
# software path with the Vulkan path (the Vulkan VDP2 renderer on the compute output, or the software renderer where
# the context does not allow it) in several frontend situations. Prints VKHOST_CHECKS_SKIPPED (ctest reports the test
# as skipped) when the BIOS or a Vulkan device is missing; with BRIMIR_REQUIRE_VULKAN=1 a missing device fails
# instead. Every run with a Vulkan device must have the validation layer on (install it, or set
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

# Save states written and loaded by the runs below.
string(RANDOM LENGTH 8 run_id)
set(state_dir "${CMAKE_CURRENT_BINARY_DIR}/vkhost_checks_${run_id}")
file(MAKE_DIRECTORY "${state_dir}")

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
    file(REMOVE_RECURSE "${state_dir}")
    message(FATAL_ERROR "${name}: ${message_text}\n--- vkhost stderr ---\n${${name}_err}")
endfunction()

function(require_log name regex)
    if(NOT "${${name}_err}" MATCHES "${regex}")
        fail(${name} "expected '${regex}' in the log")
    endif()
endfunction()

function(forbid_log name regex)
    if("${${name}_err}" MATCHES "${regex}")
        fail(${name} "unexpected '${regex}' in the log")
    endif()
endfunction()

# A run with the host's Vulkan device: validation on, no validation errors, no queue-lock violations, no presentation
# in hidden frames, at most one submit per retro_run.
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
    require_log(${name} "validation errors 0,")
    require_log(${name} "max submits per retro_run [01]([^0-9]|$)")
endfunction()

# VDP2 ran on the GPU for the whole run (the Vulkan VDP2 renderer attached at every context, never reverted).
function(check_vdp2_renderer name)
    require_log(${name} "Vulkan VDP2 renderer active")
    forbid_log(${name} "Vulkan VDP2 renderer (unavailable|off)")
    forbid_log(${name} "Vulkan frame output failed")
endfunction()

# ... and the software renderer: the Vulkan VDP2 renderer was never installed.
function(check_software_renderer name)
    forbid_log(${name} "Vulkan VDP2 renderer active")
endfunction()

# check_run(name hardware [REFERENCE ref] [ALLOWED n] [FROM i]): exit code 0 and 300 frames, `hardware` of them read
# back from set_image. Recorded frames FROM..299 (default 0) are compared with <ref>_hashes (default software); at most
# ALLOWED (default 0) of them may differ.
function(check_run name hardware)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "REFERENCE;ALLOWED;FROM" "")
    set(reference software)
    if(DEFINED arg_REFERENCE)
        set(reference ${arg_REFERENCE})
    endif()
    set(allowed 0)
    if(DEFINED arg_ALLOWED)
        set(allowed ${arg_ALLOWED})
    endif()
    set(from 0)
    if(DEFINED arg_FROM)
        set(from ${arg_FROM})
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
    foreach(i RANGE ${from} 299)
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
    message(STATUS "${name}: 300 frames, ${differences} of frames ${from}..299 different from ${reference} "
                   "(allowed ${allowed}), ${hardware} hardware")
endfunction()

function(check_software_reference name)
    if(NOT "${${name}_code}" STREQUAL "0")
        fail(${name} "brimir_vkhost exited with ${${name}_code}")
    endif()
    list(LENGTH ${name}_hashes count)
    if(NOT count EQUAL 300)
        fail(${name} "${count} frames recorded, expected 300")
    endif()
endfunction()

set(sw_state "${state_dir}/software.state")
set(vk_state "${state_dir}/vulkan.state")

run_vkhost(software --save-state-at 100 "${sw_state}")
check_software_reference(software)
if(NOT EXISTS "${sw_state}")
    fail(software "no state saved")
endif()

run_vkhost(vulkan --vulkan --save-state-at 100 "${vk_state}")
if(vulkan_code EQUAL 4)
    file(REMOVE_RECURSE "${state_dir}")
    if("$ENV{BRIMIR_REQUIRE_VULKAN}" STREQUAL "1")
        message(FATAL_ERROR "vulkan: no usable Vulkan device and BRIMIR_REQUIRE_VULKAN=1\n${vulkan_err}")
    endif()
    message("VKHOST_CHECKS_SKIPPED: no usable Vulkan device\n${vulkan_err}")
    return()
endif()
check_run(vulkan 300)
check_vulkan_counters(vulkan)
check_vdp2_renderer(vulkan)
require_log(vulkan "device created through create_device2")
require_log(vulkan "Vulkan frame output ready")
file(SHA256 "${sw_state}" sw_state_hash)
file(SHA256 "${vk_state}" vk_state_hash)
if(sw_state_hash STREQUAL vk_state_hash)
    message(STATUS "vulkan: the state saved after frame 100 is byte-identical to software's")
else()
    message(STATUS "vulkan: the state saved after frame 100 differs from software's (loads are checked below)")
endif()

# A frontend on another video driver refuses SET_HW_RENDER: software, with the fallback logged.
run_vkhost(refused --vulkan --refuse-hw)
check_run(refused 0)
check_software_renderer(refused)
require_log(refused "Vulkan renderer unavailable, falling back to software")

# A frontend on another video driver that would switch drivers: the core does not even ask.
run_vkhost(preferred_gl --vulkan --preferred-hw gl)
check_run(preferred_gl 0)
check_vulkan_counters(preferred_gl)
check_software_renderer(preferred_gl)
require_log(preferred_gl
    "Vulkan renderer selected but the frontend video driver is not Vulkan; using software")
if(preferred_gl_err MATCHES "SET_HW_RENDER")
    fail(preferred_gl "the core called SET_HW_RENDER")
endif()

# A GPU without Vulkan 1.2 (as far as the core can tell): create_device2 still creates a device (refusing could leave
# RetroArch without a GPU), and the core uses the software renderer with the copy-only output, still through Vulkan.
run_vkhost(copy_only --vulkan --fake-api 1.1)
check_run(copy_only 300)
check_vulkan_counters(copy_only)
check_software_renderer(copy_only)
require_log(copy_only "device created through create_device2")
require_log(copy_only "no GPU meets the GPU renderer's baseline")
require_log(copy_only "using copy-only frame output")

# ... with rotation (90 swaps the axes) and the medium overscan crop: the bridge's transformed frame, as in software.
set(transform_options --option brimir_rotation=90 --option brimir_overscan=2)
run_vkhost(software_transformed ${transform_options})
check_software_reference(software_transformed)
if("${software_transformed_hashes}" STREQUAL "${software_hashes}")
    fail(software_transformed "the rotation and overscan options did not change the frames")
endif()
run_vkhost(copy_only_transformed --vulkan --fake-api 1.1 ${transform_options})
check_run(copy_only_transformed 300 REFERENCE software_transformed)
check_vulkan_counters(copy_only_transformed)
require_log(copy_only_transformed "using copy-only frame output")

# ... and the Vulkan VDP2 renderer with the same transforms (the presenter crops and rotates the composite).
run_vkhost(vulkan_transformed --vulkan ${transform_options})
check_run(vulkan_transformed 300 REFERENCE software_transformed)
check_vulkan_counters(vulkan_transformed)
check_vdp2_renderer(vulkan_transformed)

# A driver reinit mid-run: context_destroy, a new device, context_reset; the renderer stays installed and is attached
# to the new device (everything uploaded again, the composite cleared).
run_vkhost(reset --vulkan --reset-at 120)
check_run(reset 300)
check_vulkan_counters(reset)
check_vdp2_renderer(reset)
require_log(reset "context resets 2,")

# The same from inside retro_run (RetroArch reinits inside SET_SYSTEM_AV_INFO, which PAL content sends): the core
# gets context_destroy and context_reset while it holds its own lock.
run_vkhost(reset_in_run --vulkan --reset-in-run 150)
check_run(reset_in_run 300)
check_vulkan_counters(reset_in_run)
check_vdp2_renderer(reset_in_run)
require_log(reset_in_run "context resets 2,")

# ... after the frame was emulated (VDP2EndFrame) and before it is recorded: the frame is lost with the device, so
# that one frame repeats the previous one (a dupe); everything after it matches.
run_vkhost(reset_in_present --vulkan --reset-in-present 150)
check_run(reset_in_present 299 ALLOWED 1)
check_vulkan_counters(reset_in_present)
check_vdp2_renderer(reset_in_present)
require_log(reset_in_present "context resets 2,")

# The sync index mask grows mid-run (0b1 -> 0b111): the slots are re-created before the frame runs and the renderer
# re-attached with the new count of frame contexts.
run_vkhost(grow --vulkan --grow-mask 100)
check_run(grow 300)
check_vulkan_counters(grow)
check_vdp2_renderer(grow)

# ... and re-creating the compute output fails: copy-only from then on, the renderer reverted to software (with its
# state) before that frame runs, so no frame is lost.
run_vkhost(grow_fail --vulkan --grow-mask 100 --fail-compute-on-grow)
check_run(grow_fail 300)
check_vulkan_counters(grow_fail)
require_log(grow_fail "Vulkan frame output changed to copy-only")
require_log(grow_fail "Vulkan VDP2 renderer off")

# Hidden frames (video disabled: run-ahead, fast-forward): the renderer still submits their VDP2 work (no presenter,
# no set_image). Deinterlacing on (the default) and off (weave keeps the other field from the hidden frame).
foreach(deinterlacing IN ITEMS enabled disabled)
    set(hide_args --hide-every 2 --option brimir_deinterlacing=${deinterlacing})
    run_vkhost(software_hidden_${deinterlacing} ${hide_args})
    check_software_reference(software_hidden_${deinterlacing})
    run_vkhost(hidden_${deinterlacing} --vulkan ${hide_args})
    check_run(hidden_${deinterlacing} 300 REFERENCE software_hidden_${deinterlacing})
    check_vulkan_counters(hidden_${deinterlacing})
    check_vdp2_renderer(hidden_${deinterlacing})
    require_log(hidden_${deinterlacing} "180 hidden frames")
    if("${hidden_${deinterlacing}_err}" MATCHES "hidden-frame submits 0,")
        fail(hidden_${deinterlacing} "the hidden frames submitted no VDP2 work")
    endif()
endforeach()

# Save states: a Vulkan run serializing and unserializing every 37 frames continues exactly.
run_vkhost(roundtrip --vulkan --state-roundtrip 37)
check_run(roundtrip 300)
check_vulkan_counters(roundtrip)
check_vdp2_renderer(roundtrip)
require_log(roundtrip "9 state round trips")

# Cross-renderer loads (the renderer section is the software renderer's in both): a software state loaded by the
# Vulkan renderer and a Vulkan state loaded by software, each against software's continuation of the software state;
# one frame of glitch is allowed (frames from 2 on are compared).
set(load_args --warmup 0)
run_vkhost(software_loaded ${load_args} --load-state "${sw_state}")
check_software_reference(software_loaded)
run_vkhost(vulkan_loads_software ${load_args} --vulkan --load-state "${sw_state}")
check_run(vulkan_loads_software 300 REFERENCE software_loaded FROM 2)
check_vulkan_counters(vulkan_loads_software)
check_vdp2_renderer(vulkan_loads_software)
run_vkhost(software_loads_vulkan ${load_args} --load-state "${vk_state}")
check_run(software_loads_vulkan 0 REFERENCE software_loaded FROM 2)

# Run-ahead: hidden frames (video disabled) and a second instance without a context. The hidden frames are not
# rolled back (see tools/vkhost/run_core.cpp), so the reference is a software run with the same pattern.
run_vkhost(software_runahead --runahead)
check_software_reference(software_runahead)
if("${software_runahead_hashes}" STREQUAL "${software_hashes}")
    fail(software_runahead "the hidden frames did not advance the emulation")
endif()
run_vkhost(runahead --vulkan --runahead)
check_run(runahead 300 REFERENCE software_runahead)
check_vulkan_counters(runahead)
check_vdp2_renderer(runahead)
require_log(runahead "second instance ran 360 frames, 0 hardware frames, context none")

file(REMOVE_RECURSE "${state_dir}")

# --compare: software and Vulkan in two child processes. Identical content passes; a Vulkan run that is rotated 180
# degrees (the menu has content after 400 frames) must exit 3 and report the first differing pixel.
execute_process(
    COMMAND "${VKHOST}" ${common_args} --compare
    OUTPUT_VARIABLE compare_out ERROR_VARIABLE compare_err RESULT_VARIABLE compare_code)
if(NOT compare_code EQUAL 0 OR
   NOT compare_out MATCHES "compare: OK, 300 frames identical \\(software vs vulkan VDP2 renderer: 300 hardware frames, [1-9][0-9]* core submits, validation on, 0 validation errors\\)")
    message(FATAL_ERROR "--compare of identical runs: exit ${compare_code}\n${compare_out}\n${compare_err}")
endif()
# ... with hidden frames and state round trips in both runs.
execute_process(
    COMMAND "${VKHOST}" ${common_args} --compare --hide-every 3 --state-roundtrip 50
    OUTPUT_VARIABLE compare_out ERROR_VARIABLE compare_err RESULT_VARIABLE compare_code)
if(NOT compare_code EQUAL 0 OR NOT compare_out MATCHES "compare: OK, 300 frames identical")
    message(FATAL_ERROR "--compare --hide-every 3 --state-roundtrip 50: exit ${compare_code}\n${compare_out}\n${compare_err}")
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
message(STATUS "--compare: identical runs pass (also with hidden frames and state round trips), a difference is "
               "reported (exit 3, first pixel)")

# --compare must not pass when the Vulkan run silently stayed on software or the copy-only output (the comparison
# would be vacuous). The environment hook adds arguments to the Vulkan child only.
foreach(case IN ITEMS "--refuse-hw|no \"Vulkan frame output ready\"" "--fake-api 1.1|using copy-only frame output"
                      "--fake-api 1.1|no \"Vulkan VDP2 renderer active\""
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
