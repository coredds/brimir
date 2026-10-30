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
    # GPU stage 2: no submit or wait outside the allowed list (sync submits and their waits in retro_run, fence waits
    # in retro_serialize, the context callbacks)
    require_log(${name} "submits: sync [0-9]+, context [0-9]+, refused [0-9]+, ops-only after refused [0-9]+, outside 0;")
    require_log(${name} "outside the allowed list 0")
    require_log(${name} "core device memory: [0-9.]+ MiB after the last frame, peak [0-9.]+ MiB, [0-9]+ allocations")
endfunction()

# VDP1 on the GPU: the sync submits vkhost saw (the core's logged sync fence) are the ones the core counted (reads,
# saves, detaches, dumps; the detach in context_destroy included), and none was for a read or a save.
function(check_sync_counts name)
    string(REGEX MATCH "VDP1 on the GPU: [0-9]+ ops-only submits, ([0-9]+) sync submits \\(reads 0, saves 0," core "${${name}_err}")
    if(core STREQUAL "")
        fail(${name} "no 'VDP1 on the GPU: ... sync submits (reads 0, saves 0, ...' line")
    endif()
    set(core_syncs "${CMAKE_MATCH_1}")
    string(REGEX MATCH "submits: sync ([0-9]+)," host "${${name}_err}")
    if(NOT host OR NOT CMAKE_MATCH_1 STREQUAL core_syncs)
        fail(${name} "vkhost saw '${host}', the core counted ${core_syncs} sync submits")
    endif()
endfunction()
# VDP2 ran on the GPU for the whole run (the Vulkan VDP2 renderer attached at every context, never reverted).
function(check_vdp2_renderer name)
    require_log(${name} "Vulkan VDP2 renderer active")
    require_log(${name} "peak [0-9.]*[1-9][0-9.]* MiB, [1-9][0-9]* allocations")
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

# An operational exception: recorded frame `index` repeats frame index - 1 exactly (a dupe: the frame was lost with
# its device or output) and every other frame is identical to software; 299 frames come from set_image.
function(check_one_repeat name index)
    check_run(${name} 299 ALLOWED 1)
    math(EXPR previous "${index} - 1")
    list(GET ${name}_hashes ${index} got)
    list(GET ${name}_hashes ${previous} before)
    string(REGEX REPLACE "^frame [0-9]+ " "" got_pixels "${got}")
    string(REGEX REPLACE "^frame [0-9]+ " "" before_pixels "${before}")
    if(NOT got_pixels STREQUAL before_pixels)
        fail(${name} "recorded frame ${index} is not a repeat of frame ${previous} ('${got}' vs '${before}')")
    endif()
    foreach(i RANGE 299)
        if(NOT i EQUAL index)
            list(GET ${name}_hashes ${i} got)
            list(GET software_hashes ${i} want)
            if(NOT got STREQUAL want)
                fail(${name} "recorded frame ${i} differs from software ('${got}' vs '${want}'); only ${index} may")
            endif()
        endif()
    endforeach()
    message(STATUS "${name}: frame ${index} repeats frame ${previous}, the other 299 frames identical to software")
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
# The two states' renderer sections may differ only in the line-local fetcher caches on brimir_state_diff's allowlist
# (tools/state_diff.cpp); anything carried from line to line or frame to frame must match, and so must the VDP1
# framebuffers (vdp.FBRAM, displayFB: compared strictly, the runs are at the same frame of the same content).
if(NOT STATE_DIFF OR NOT EXISTS "${STATE_DIFF}")
    fail(vulkan "STATE_DIFF (brimir_state_diff) not found: ${STATE_DIFF}")
endif()
execute_process(COMMAND "${STATE_DIFF}" "${sw_state}" "${vk_state}"
                OUTPUT_VARIABLE state_diff_out ERROR_VARIABLE state_diff_err RESULT_VARIABLE state_diff_code)
if(NOT state_diff_code EQUAL 0)
    fail(vulkan "the software- and Vulkan-saved states differ outside the allowlist (exit ${state_diff_code}):\n"
                "${state_diff_out}${state_diff_err}")
endif()
string(REGEX MATCH "[0-9]+ allowed and 0 unexpected" state_diff_summary "${state_diff_out}")
message(STATUS "vulkan: the state saved after frame 100 matches software's renderer section (${state_diff_summary})")

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

# ... after the frame was emulated (VDP2EndFrame) and before it is recorded. Operational exception: context destroyed
# during present. The frame is lost with the device, so recorded frame 90 (frame 150) repeats frame 89 exactly (a dupe);
# every other frame matches software.
run_vkhost(reset_in_present --vulkan --reset-in-present 150)
check_one_repeat(reset_in_present 90)

# ... and the new context only allows the copy-only output: the renderer reverts to software (with its state). The
# frame the renderer emulated is lost, so frame 90 repeats frame 89 rather than uploading the bridge's frame from before
# the install; the software frames after it match.
run_vkhost(reset_to_copy_only --vulkan --reset-in-present 150 --fail-compute-on-reset)
check_one_repeat(reset_to_copy_only 90)
check_vulkan_counters(reset_to_copy_only)
require_log(reset_to_copy_only "using copy-only frame output")
require_log(reset_to_copy_only "Vulkan VDP2 renderer off")
check_vulkan_counters(reset_in_present)
check_vdp2_renderer(reset_in_present)
# With VDP1 on the GPU (the default) the frame's VDP1 ops were pending at context_destroy: the detach's sync submit, the
# one vkhost tells by the fence the core logs for its sync context.
require_log(reset_in_present "submits: sync 1,")
require_log(reset_in_present "sync submits \\(reads 0, saves 0, detaches 1,")
require_log(reset_in_present "context resets 2,")

# The sync index mask grows mid-run (0b1 -> 0b111): the next shown frame re-creates the slots and the renderer gains
# frame contexts in place (its composite, the other field when weaving, is kept; scene "weave keeps the other field
# when the frame contexts grow" checks that on interlaced modes).
run_vkhost(grow --vulkan --grow-mask 100)
check_run(grow 300)
check_vulkan_counters(grow)
check_vdp2_renderer(grow)

# ... with the mask growing before a hidden frame (run 100 is hidden with --hide-every 2): the growth waits for the next
# shown frame (no presentation calls in hidden frames).
run_vkhost(software_hidden_grow --hide-every 2)
check_software_reference(software_hidden_grow)
run_vkhost(grow_hidden --vulkan --hide-every 2 --grow-mask 100)
check_run(grow_hidden 300 REFERENCE software_hidden_grow)
check_vulkan_counters(grow_hidden)
check_vdp2_renderer(grow_hidden)

# ... and re-creating the compute output fails: copy-only from then on and the renderer reverted to software (with
# its state). The frame of the growth was rendered for the lost output, so recorded frame 40 (frame 100) repeats
# frame 39 (a dupe); the copy-only output never uploads the bridge's stale pre-install frame.
run_vkhost(grow_fail --vulkan --grow-mask 100 --fail-compute-on-grow)
check_one_repeat(grow_fail 40)
check_vulkan_counters(grow_fail)
require_log(grow_fail "Vulkan frame output changed to copy-only")
require_log(grow_fail "Vulkan VDP2 renderer off")

# Hidden frames (video disabled: run-ahead, fast-forward): the renderer still submits their VDP2 work (no presenter,
# no set_image). Deinterlacing on (the default) and off (brimir_deinterlace_mode=none: brimir_deinterlacing=disabled
# alone is overridden by the mode option). The menu is progressive; weave across hidden frames on interlaced modes is
# the scene "weave across hidden frames recorded into hidden contexts".
foreach(deinterlacing IN ITEMS bob none)
    set(hide_args --hide-every 2 --option brimir_deinterlace_mode=${deinterlacing})
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

# Internal resolution (brimir_internal_resolution). The Vulkan runs hash the sample points (--hash-sample: the native
# image of the N x frame's sample points, the scale followed from the core's "frames at Nx" log line); the menu is
# low-res and progressive, so no point is excluded and --hash-sample 1 of a software run is its plain hash.
run_vkhost(software_sampled --hash-sample 1)
check_software_reference(software_sampled)
if(NOT "${software_sampled_hashes}" STREQUAL "${software_hashes}")
    fail(software_sampled "--hash-sample 1 of the menu is not the plain hash")
endif()

# 2x from the start: N x the native size reported with SET_GEOMETRY (the maximum, 2816 square, declared at load). The
# menu has no sub-pixel content (no zoom, rotation or fine scroll), so --sharpness finds every 2 x 2 block constant.
run_vkhost(scale2 --vulkan --internal-resolution 2 --hash-sample 2 --sharpness 2)
check_run(scale2 300)
check_vulkan_counters(scale2)
check_vdp2_renderer(scale2)
require_log(scale2 "Internal resolution: frames at 2x")
require_log(scale2 "SET_GEOMETRY 640x448 \\(max 2816x2816, aspect 1.3333\\)")
forbid_log(scale2 "SET_GEOMETRY 320x224")
require_log(scale2 "SET_SYSTEM_AV_INFO calls 0,")
require_log(scale2 "hash-sample: sampled 21504000 of 21504000 points over 300 frames \\(100.0%\\)")
require_log(scale2 "sharpness: 300 frames \\(300 at more than 1x\\), mean non-constant blocks 0.00%, mean abs diff 0.0000")
# ... rotated and cropped: the presenter's crop (decided natively, offsets N * (crop / 2)) and rotation at 2x, the
# sample points mapped through the rotation by --hash-sample.
run_vkhost(scale2_transformed --vulkan --internal-resolution 2 --hash-sample 2 ${transform_options})
check_run(scale2_transformed 300 REFERENCE software_transformed)
check_vulkan_counters(scale2_transformed)
check_vdp2_renderer(scale2_transformed)
require_log(scale2_transformed "SET_GEOMETRY 384x576")

# Live changes 1x -> 2x -> 4x -> 1x (as from RetroArch's Quick Menu), with synchronization validation: each frame at its
# scale, only SET_GEOMETRY (never SET_SYSTEM_AV_INFO, which would reinit the frontend's video driver), one submit per
# retro_run. Deinterlacing on (the default) and off (weave: the composite keeps the other field through the rescale; the
# menu is progressive, the interlaced case is the scene "a live internal resolution change keeps the woven field").
run_vkhost(software_weave --option brimir_deinterlace_mode=none)
check_software_reference(software_weave)
set(ENV{BRIMIR_VK_SYNC_VALIDATION} 1)
foreach(deinterlacing IN ITEMS bob none)
    set(name live_${deinterlacing})
    set(reference software)
    if(deinterlacing STREQUAL "none")
        set(reference software_weave)
    endif()
    run_vkhost(${name} --vulkan --hash-sample 1 --option brimir_deinterlace_mode=${deinterlacing}
               --set-option-at 100 brimir_internal_resolution=2 --set-option-at 160 brimir_internal_resolution=4
               --set-option-at 220 brimir_internal_resolution=1)
    check_run(${name} 300 REFERENCE ${reference})
    check_vulkan_counters(${name})
    check_vdp2_renderer(${name})
    require_log(${name} "SET_SYSTEM_AV_INFO calls 0, SET_GEOMETRY calls 4")
    string(REGEX MATCHALL "SET_GEOMETRY [0-9]+x[0-9]+" geometries "${${name}_err}")
    if(NOT "${geometries}" STREQUAL "SET_GEOMETRY 320x224;SET_GEOMETRY 640x448;SET_GEOMETRY 1280x896;SET_GEOMETRY 320x224")
        fail(${name} "SET_GEOMETRY sequence '${geometries}', expected 320x224, 640x448, 1280x896, 320x224")
    endif()
    string(REGEX MATCHALL "frames at [0-9]x" scales "${${name}_err}")
    if(NOT "${scales}" STREQUAL "frames at 1x;frames at 2x;frames at 4x;frames at 1x")
        fail(${name} "presented scales '${scales}', expected 1x, 2x, 4x, 1x")
    endif()
    message(STATUS "${name}: live 1x -> 2x -> 4x -> 1x, SET_GEOMETRY N x native, no SET_SYSTEM_AV_INFO")
endforeach()
unset(ENV{BRIMIR_VK_SYNC_VALIDATION})

# Hidden frames (video disabled) at 2x: recorded at the current scale (the next shown frame weaves against them).
run_vkhost(hidden_scale2 --vulkan --internal-resolution 2 --hash-sample 2 --hide-every 2
           --option brimir_deinterlace_mode=none)
check_run(hidden_scale2 300 REFERENCE software_hidden_none)
check_vulkan_counters(hidden_scale2)
check_vdp2_renderer(hidden_scale2)
require_log(hidden_scale2 "180 hidden frames")
require_log(hidden_scale2 "Internal resolution: frames at 2x")

# A driver reinit from inside retro_run at 2x: the requested scale is allocated again on the new device.
run_vkhost(reset_in_run_scale2 --vulkan --internal-resolution 2 --hash-sample 2 --reset-in-run 150)
check_run(reset_in_run_scale2 300)
check_vulkan_counters(reset_in_run_scale2)
check_vdp2_renderer(reset_in_run_scale2)
require_log(reset_in_run_scale2 "context resets 2,")
forbid_log(reset_in_run_scale2 "frames at 1x")

# The copy-only output ignores the option: native frames, identical to software.
run_vkhost(copy_only_scale4 --vulkan --fake-api 1.1 --internal-resolution 4 --hash-sample 4)
check_run(copy_only_scale4 300)
check_vulkan_counters(copy_only_scale4)
check_software_renderer(copy_only_scale4)
require_log(copy_only_scale4 "using copy-only frame output")
require_log(copy_only_scale4 "Internal resolution 4x needs the Vulkan VDP2 renderer; frames stay native")
require_log(copy_only_scale4 "Internal resolution: frames at 1x")
forbid_log(copy_only_scale4 "frames at [2-4]x")

# A GPU short of memory (the core's allocations above 64 MiB fail: 4x needs 154 MiB of layers and composite, 2x 39):
# 4x at load falls back to 1x, a live change to 2x works, a live change back to 4x keeps 2x; the frames stay correct.
run_vkhost(alloc_fail --vulkan --internal-resolution 4 --hash-sample 4 --fail-alloc-above 64
           --set-option-at 150 brimir_internal_resolution=2 --set-option-at 200 brimir_internal_resolution=4)
check_run(alloc_fail 300)
check_vulkan_counters(alloc_fail)
check_vdp2_renderer(alloc_fail)
require_log(alloc_fail "VDP2 internal resolution: requested 4x, using 1x")
require_log(alloc_fail "Internal resolution 4x could not be allocated; staying at 2x")
require_log(alloc_fail "SET_SYSTEM_AV_INFO calls 0,")
string(REGEX MATCHALL "frames at [0-9]x" alloc_scales "${alloc_fail_err}")
if(NOT "${alloc_scales}" STREQUAL "frames at 1x;frames at 2x")
    fail(alloc_fail "presented scales '${alloc_scales}', expected 1x then 2x")
endif()

# The output images (the ones handed to set_image) cannot be created above 3x: a live 2x -> 4x keeps the renderer and the
# output at 2x (the images are created before the renderer changes, so the two never differ); a live 3x then works.
run_vkhost(output_images_fail --vulkan --internal-resolution 2 --hash-sample 2 --fail-output-images-above 3
           --set-option-at 150 brimir_internal_resolution=4 --set-option-at 200 brimir_internal_resolution=3)
check_run(output_images_fail 300)
check_vulkan_counters(output_images_fail)
check_vdp2_renderer(output_images_fail)
require_log(output_images_fail "vkCreateImage of a 2816x2816 output image refused")
require_log(output_images_fail
    "Internal resolution 4x: creating the 2816 px output images failed: VK_ERROR_OUT_OF_DEVICE_MEMORY; staying at 2x")
string(REGEX MATCHALL "frames at [0-9]x" output_scales "${output_images_fail_err}")
if(NOT "${output_scales}" STREQUAL "frames at 2x;frames at 3x")
    fail(output_images_fail "presented scales '${output_scales}', expected 2x then 3x")
endif()
# ... and at load (4x requested, output images at most 2x wide): the renderer is attached at the output's scale.
run_vkhost(output_images_fail_load --vulkan --internal-resolution 4 --hash-sample 4 --fail-output-images-above 2)
check_run(output_images_fail_load 300)
check_vulkan_counters(output_images_fail_load)
check_vdp2_renderer(output_images_fail_load)
require_log(output_images_fail_load "VDP2 internal resolution: requested 4x, using 1x")
string(REGEX MATCHALL "frames at [0-9]x" load_scales "${output_images_fail_load_err}")
if(NOT "${load_scales}" STREQUAL "frames at 1x")
    fail(output_images_fail_load "presented scales '${load_scales}', expected 1x")
endif()
# ... and when the sync index mask grows at 4x (the output images are re-created for the larger ring, now at most 2x
# wide): the output steps down to 2x (3x is refused too) and the renderer falls back to it, instead of the copy-only
# output; the sample points stay identical to software, under synchronization validation.
set(ENV{BRIMIR_VK_SYNC_VALIDATION} 1)
run_vkhost(output_images_fail_grow --vulkan --internal-resolution 4 --hash-sample 4 --grow-mask 100
           --fail-output-images-above 2 --fail-output-images-on-grow)
unset(ENV{BRIMIR_VK_SYNC_VALIDATION})
check_run(output_images_fail_grow 300)
check_vulkan_counters(output_images_fail_grow)
check_vdp2_renderer(output_images_fail_grow)
require_log(output_images_fail_grow "vkCreateImage of a 2112x2112 output image refused")
require_log(output_images_fail_grow
    "Internal resolution 4x: recreating the output images for the new sync index mask failed; using 2x")
forbid_log(output_images_fail_grow "copy-only")
string(REGEX MATCHALL "frames at [0-9]x" grow_scales "${output_images_fail_grow_err}")
if(NOT "${grow_scales}" STREQUAL "frames at 4x;frames at 2x")
    fail(output_images_fail_grow "presented scales '${grow_scales}', expected 4x then 2x")
endif()

# The renderer cannot be attached at any scale (allocations above 8 MiB fail: the 2x output images, 7.6 MiB each, are
# created, the renderer's resources at 2x and at 1x are not): software renderer with the GPU output, and the output
# images shrink back to 1x for its native frames (the memory left equals a 1x request's under the same limit).
run_vkhost(attach_fail --vulkan --internal-resolution 2 --fail-alloc-above 8)
check_run(attach_fail 300)
check_vulkan_counters(attach_fail)
check_software_renderer(attach_fail)
require_log(attach_fail "creating the VDP2 GPU resources failed")
forbid_log(attach_fail "creating the 1408 px output images failed")
run_vkhost(attach_fail_native --vulkan --internal-resolution 1 --fail-alloc-above 8)
check_run(attach_fail_native 300)
require_log(attach_fail_native "creating the VDP2 GPU resources failed")
string(REGEX MATCH "core device memory: [0-9.]+ MiB after the last frame" attach_memory "${attach_fail_err}")
string(REGEX MATCH "core device memory: [0-9.]+ MiB after the last frame" native_memory "${attach_fail_native_err}")
if(attach_memory STREQUAL "" OR NOT attach_memory STREQUAL native_memory)
    fail(attach_fail "'${attach_memory}' after a failed attach at 2x, '${native_memory}' at 1x: the 2x output images were kept")
endif()

# A frontend that reads each image two displayed frames after set_image (RetroArch's threaded video path keeps the
# published images; the latest legal read is before the core's next wait_sync_index for that sync index): live changes
# 1x -> 4x -> 2x under synchronization validation. The replaced output images must stay alive until then (the run fails
# if the core destroys an image or view before its read), and every frame read late is still the right one. Also with
# weave and hidden frames, and across a reinit from inside retro_run.
set(ENV{BRIMIR_VK_SYNC_VALIDATION} 1)
set(deferred_changes --set-option-at 100 brimir_internal_resolution=4 --set-option-at 160 brimir_internal_resolution=2)
run_vkhost(deferred_live --vulkan --defer-image-read 2 --hash-sample 1 ${deferred_changes})
check_run(deferred_live 300)
run_vkhost(deferred_hidden --vulkan --defer-image-read 2 --hash-sample 1 ${deferred_changes} --hide-every 2
           --option brimir_deinterlace_mode=none)
check_run(deferred_hidden 300 REFERENCE software_hidden_none)
unset(ENV{BRIMIR_VK_SYNC_VALIDATION})
run_vkhost(deferred_reset --vulkan --defer-image-read 1 --internal-resolution 2 --hash-sample 2 --reset-in-run 150)
check_run(deferred_reset 300)
require_log(deferred_reset "context resets 2,")
foreach(name IN ITEMS deferred_live deferred_hidden deferred_reset)
    check_vulkan_counters(${name})
    check_vdp2_renderer(${name})
endforeach()
foreach(name IN ITEMS deferred_live deferred_hidden)
    string(REGEX MATCHALL "frames at [0-9]x" deferred_scales "${${name}_err}")
    if(NOT "${deferred_scales}" STREQUAL "frames at 1x;frames at 4x;frames at 2x")
        fail(${name} "presented scales '${deferred_scales}', expected 1x, 4x, 2x")
    endif()
endforeach()

# The core's frame submit fails in the two runs of a live 1x -> 4x change (recorded frames 40 and 41): the frontend
# repeats the image it holds, which belongs to the replaced output images, and reads it again late (--defer-image-read 2:
# the second repeat after the next frame, i.e. after a retired image that stopped being current too early would be
# freed). The replaced images must stay alive until a full ring after they stop being current: the run fails if the core
# destroys one before its read. Frames 40 and 41 repeat frame 39, the other 298 frames match software, at 4x from 42 on.
set(ENV{BRIMIR_VK_SYNC_VALIDATION} 1)
run_vkhost(repeat_after_change --vulkan --defer-image-read 2 --hash-sample 1 --set-option-at 100
           brimir_internal_resolution=4 --fail-submit-at 100 --fail-submits 2)
unset(ENV{BRIMIR_VK_SYNC_VALIDATION})
check_vulkan_counters(repeat_after_change)
require_log(repeat_after_change "recorded 300 frames \\(298 hardware\\)")
require_log(repeat_after_change "Vulkan frame output failed: vkQueueSubmit failed")
require_log(repeat_after_change "vkQueueSubmit refused")
forbid_log(repeat_after_change "Vulkan VDP2 renderer (unavailable|off)")
if(NOT "${repeat_after_change_code}" STREQUAL "0")
    fail(repeat_after_change "brimir_vkhost exited with ${repeat_after_change_code}")
endif()
list(LENGTH repeat_after_change_hashes repeat_count)
if(NOT repeat_count EQUAL 300)
    fail(repeat_after_change "${repeat_count} frames recorded, expected 300")
endif()
list(GET repeat_after_change_hashes 39 repeat_before)
string(REGEX REPLACE "^frame [0-9]+ " "" repeat_before "${repeat_before}")
foreach(i RANGE 299)
    list(GET repeat_after_change_hashes ${i} got)
    list(GET software_hashes ${i} want)
    if(i EQUAL 40 OR i EQUAL 41)
        string(REGEX REPLACE "^frame [0-9]+ " "" got_pixels "${got}")
        if(NOT got_pixels STREQUAL repeat_before)
            fail(repeat_after_change "recorded frame ${i} is not a repeat of frame 39 ('${got}')")
        endif()
    elseif(NOT got STREQUAL want)
        fail(repeat_after_change "recorded frame ${i} differs from software ('${got}' vs '${want}')")
    endif()
endforeach()
message(STATUS "repeat_after_change: frames 40 and 41 repeat frame 39 (failed submits right after a live change, "
               "late reads), the other 298 identical to software")

# VDP1 on the GPU (GPU stage 2): the default with the Vulkan VDP renderer (--gpu-vdp1 sets BRIMIR_VDP1_GPU=1 explicitly;
# --software-vdp1 sets the debug override BRIMIR_VDP1_GPU=0). The VDP1 ops go in the frame's submit, around its VDP2
# work, so the menu is identical to software with one submit per retro_run (with synchronization validation). Every
# submit ends with the readbacks of the framebuffer slices the GPU changed (the CPU shadow), so the save state of
# retro_serialize only waits for fences: no sync submit for reads or saves (the menu never reads the framebuffer).
require_log(vulkan "Vulkan VDP1 renderer active")
set(gpu_vdp1_state "${state_dir}/gpu_vdp1.state")
set(ENV{BRIMIR_VK_SYNC_VALIDATION} 1)
run_vkhost(gpu_vdp1 --vulkan --gpu-vdp1 --save-state-at 100 "${gpu_vdp1_state}")
unset(ENV{BRIMIR_VK_SYNC_VALIDATION})
check_run(gpu_vdp1 300)
check_vulkan_counters(gpu_vdp1)
check_vdp2_renderer(gpu_vdp1)
require_log(gpu_vdp1 "Vulkan VDP1 renderer active")
require_log(gpu_vdp1 "VDP1 on the GPU: [0-9]+ ops-only submits, [0-9]+ sync submits \\(reads 0, saves 0,")
check_sync_counts(gpu_vdp1)
# The state saved with VDP1 on the GPU matches software's: the VDP1 framebuffers (from the shadow; compared strictly by
# brimir_state_diff) and the renderer section (mesh and doubleV from the GPU path).
execute_process(COMMAND "${STATE_DIFF}" "${sw_state}" "${gpu_vdp1_state}"
                OUTPUT_VARIABLE state_diff_out ERROR_VARIABLE state_diff_err RESULT_VARIABLE state_diff_code)
if(NOT state_diff_code EQUAL 0)
    fail(gpu_vdp1 "the software- and GPU-VDP1-saved states differ outside the allowlist (exit ${state_diff_code}):\n"
                  "${state_diff_out}${state_diff_err}")
endif()
# ... and the debug override: VDP1 in software inside the Vulkan VDP renderer (stage 1).
run_vkhost(software_vdp1 --vulkan --software-vdp1)
check_run(software_vdp1 300)
check_vulkan_counters(software_vdp1)
check_vdp2_renderer(software_vdp1)
forbid_log(software_vdp1 "Vulkan VDP1 renderer active")
# ... with hidden frames (their submits carry the VDP1 ops too).
run_vkhost(gpu_vdp1_hidden --vulkan --gpu-vdp1 --hide-every 2 --option brimir_deinterlace_mode=bob)
check_run(gpu_vdp1_hidden 300 REFERENCE software_hidden_bob)
check_vulkan_counters(gpu_vdp1_hidden)
check_vdp2_renderer(gpu_vdp1_hidden)
require_log(gpu_vdp1_hidden "180 hidden frames")
# ... and the ops-only submit: the present submit of run 100 is refused, its VDP1 ops are pending again and go alone in
# a second submit of that run (the one allowed ops-only submit, logged by the core); recorded frame 40 repeats frame 39
# (operational exception, as for any failed submit) and every later frame is identical to software, so no VDP1 op was
# lost.
run_vkhost(gpu_vdp1_ops_only --vulkan --gpu-vdp1 --fail-submit-at 100)
check_one_repeat(gpu_vdp1_ops_only 40)
check_vulkan_counters(gpu_vdp1_ops_only) # the refused submit is counted apart: one accepted submit per retro_run
require_log(gpu_vdp1_ops_only "vkQueueSubmit refused")
require_log(gpu_vdp1_ops_only "VDP1 ops-only submit after a refused frame submit")
require_log(gpu_vdp1_ops_only "refused 1, ops-only after refused 1,")
require_log(gpu_vdp1_ops_only "VDP1 on the GPU: [1-9][0-9]* ops-only submits, [0-9]+ sync submits \\(reads 0, saves 0,")
# ... save states: a serialize and unserialize after every frame (single-instance run-ahead's pattern) continues
# exactly; retro_serialize waits for the last submit's readbacks (fence waits only).
run_vkhost(gpu_vdp1_roundtrip --vulkan --gpu-vdp1 --state-roundtrip 1)
check_run(gpu_vdp1_roundtrip 300)
check_vulkan_counters(gpu_vdp1_roundtrip)
check_vdp2_renderer(gpu_vdp1_roundtrip)
require_log(gpu_vdp1_roundtrip "360 state round trips")
check_sync_counts(gpu_vdp1_roundtrip)
# ... run-ahead (hidden frames, a second instance on software).
run_vkhost(gpu_vdp1_runahead --vulkan --gpu-vdp1 --runahead)
check_run(gpu_vdp1_runahead 300 REFERENCE software_runahead)
check_vulkan_counters(gpu_vdp1_runahead)
check_vdp2_renderer(gpu_vdp1_runahead)
require_log(gpu_vdp1_runahead "second instance ran 360 frames, 0 hardware frames, context none")
# ... a driver reinit inside retro_run: the detach syncs the GPU framebuffers into software, the attach uploads them.
run_vkhost(gpu_vdp1_reset_in_run --vulkan --gpu-vdp1 --reset-in-run 150)
check_run(gpu_vdp1_reset_in_run 300)
check_vulkan_counters(gpu_vdp1_reset_in_run)
check_vdp2_renderer(gpu_vdp1_reset_in_run)
require_log(gpu_vdp1_reset_in_run "context resets 2,")
# ... and --compare of the two renderers, also at 2x through the sample points.
foreach(compare_case IN ITEMS "--gpu-vdp1" "--gpu-vdp1|--internal-resolution|2|--hash-sample|2")
    string(REPLACE "|" ";" compare_args "${compare_case}")
    execute_process(
        COMMAND "${VKHOST}" ${common_args} --compare ${compare_args}
        OUTPUT_VARIABLE compare_out ERROR_VARIABLE compare_err RESULT_VARIABLE compare_code)
    if(NOT compare_code EQUAL 0 OR NOT compare_out MATCHES "compare: OK, 300 frames identical")
        message(FATAL_ERROR "--compare ${compare_args}: exit ${compare_code}\n${compare_out}\n${compare_err}")
    endif()
endforeach()
message(STATUS "gpu_vdp1: VDP1 on the GPU identical to software (shown, hidden, after a refused submit, state round "
               "trips every frame, run-ahead, a reinit in retro_run, --compare at 1x and 2x); --software-vdp1 too")
# ... and the probe (diagnostics, synchronization validation on): the VDP1 times of the VDP2 probe line, with the GPU
# time from timestamp queries (the menu draws and erases every frame), or the core's line saying the queue has no
# timestamps.
set(ENV{BRIMIR_VK_SYNC_VALIDATION} 1)
run_vkhost(gpu_vdp1_probe --vulkan --probe)
unset(ENV{BRIMIR_VK_SYNC_VALIDATION})
check_run(gpu_vdp1_probe 300)
check_vulkan_counters(gpu_vdp1_probe)
require_log(gpu_vdp1_probe "probe: vdp1_front_us +median [0-9.]+ max [0-9.]+ mean [0-9.]+")
if(NOT "${gpu_vdp1_probe_err}" MATCHES "VDP1 probe: no GPU timestamps on this queue")
    string(REGEX MATCH "probe: vdp1_gpu_us +median [0-9.]+ max [0-9.]+ mean ([0-9.]+)" ignored "${gpu_vdp1_probe_err}")
    if(NOT CMAKE_MATCH_1 OR CMAKE_MATCH_1 STREQUAL "0.0")
        fail(gpu_vdp1_probe "vdp1_gpu_us is not above 0 ('${CMAKE_MATCH_1}') and the core did not log missing timestamps")
    endif()
    message(STATUS "gpu_vdp1_probe: VDP1 GPU time mean ${CMAKE_MATCH_1} us per frame (timestamps)")
endif()

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
# ... with state compares: both runs save a state every 60 frames, each pair diffed by brimir_state_diff (VDP1
# framebuffers strictly, the renderer section with doubleV and mesh); a Vulkan run that emulates other frames (hidden
# frames in the Vulkan child only) must fail on the states too.
execute_process(
    COMMAND "${VKHOST}" ${common_args} --compare --compare-state 60
    OUTPUT_VARIABLE compare_out ERROR_VARIABLE compare_err RESULT_VARIABLE compare_code)
if(NOT compare_code EQUAL 0 OR NOT compare_out MATCHES "compare: OK, 5 state pairs identical \\(every 60 frames\\)")
    message(FATAL_ERROR "--compare --compare-state 60: exit ${compare_code}\n${compare_out}\n${compare_err}")
endif()
set(ENV{BRIMIR_VKHOST_COMPARE_VULKAN_ARGS} "--hide-every 2")
execute_process(
    COMMAND "${VKHOST}" --bios "${BIOS}" --warmup 60 --frames 120 --compare --compare-state 60
    OUTPUT_VARIABLE compare_out ERROR_VARIABLE compare_err RESULT_VARIABLE compare_code)
unset(ENV{BRIMIR_VKHOST_COMPARE_VULKAN_ARGS})
string(REGEX MATCH "work directory kept: ([^\r\n]*)\\)" ignored "${compare_out}")
if(CMAKE_MATCH_1)
    file(REMOVE_RECURSE "${CMAKE_MATCH_1}")
endif()
if(NOT compare_code EQUAL 3 OR NOT compare_out MATCHES "the states after recorded frame 59 differ" OR
   NOT compare_out MATCHES "2 of 2 state pairs differ, first after frame 59")
    message(FATAL_ERROR "--compare-state with differing runs: exit ${compare_code}\n${compare_out}\n${compare_err}")
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
# ... at internal resolution 2 against software at 1x, through the sample points.
execute_process(
    COMMAND "${VKHOST}" ${common_args} --compare --internal-resolution 2 --hash-sample 2
    OUTPUT_VARIABLE compare_out ERROR_VARIABLE compare_err RESULT_VARIABLE compare_code)
if(NOT compare_code EQUAL 0 OR NOT compare_out MATCHES "compare: OK, 300 frames identical" OR
   NOT compare_out MATCHES "vulkan: geometry: SET_GEOMETRY 640x448")
    message(FATAL_ERROR "--compare --internal-resolution 2 --hash-sample 2: exit ${compare_code}\n${compare_out}\n${compare_err}")
endif()
message(STATUS "--compare: identical runs pass (also with hidden frames and state round trips, with state compares, and "
               "at 2x through the sample points), a difference is reported (exit 3, first pixel; differing states)")

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
foreach(rejected IN ITEMS "--refuse-hw" "--fake-api;1.1" "--option;brimir_renderer=software"
                          "--compare-state;2" "--dump-dir;${CMAKE_CURRENT_BINARY_DIR}/vkhost_rejected_dump;--dump-every;2;--dump-frame;0")
    execute_process(
        COMMAND "${VKHOST}" --bios "${BIOS}" --warmup 1 --frames 1 --compare ${rejected}
        OUTPUT_QUIET ERROR_VARIABLE rejected_err RESULT_VARIABLE rejected_code)
    if(NOT rejected_code EQUAL 1)
        message(FATAL_ERROR "--compare ${rejected} must be a usage error (exit 1), got ${rejected_code}\n${rejected_err}")
    endif()
endforeach()
message(STATUS "--compare: vacuous Vulkan runs and conflicting options are rejected")

message(STATUS "brimir_vkhost Vulkan checks passed")
