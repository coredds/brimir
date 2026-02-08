# CMake script to embed shader bytecode as C++ arrays
# Usage: cmake -P embed_shaders.cmake

set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(OUTPUT_FILE "${CMAKE_CURRENT_SOURCE_DIR}/embedded_shaders.hpp")

# Read VDP1 basic shaders
file(READ "${SHADER_DIR}/vdp1_vertex.spv" VERT_SHADER_HEX HEX)
string(LENGTH "${VERT_SHADER_HEX}" VERT_SHADER_SIZE_HEX)
math(EXPR VERT_SHADER_SIZE "${VERT_SHADER_SIZE_HEX} / 2")

file(READ "${SHADER_DIR}/vdp1_fragment.spv" FRAG_SHADER_HEX HEX)
string(LENGTH "${FRAG_SHADER_HEX}" FRAG_SHADER_SIZE_HEX)
math(EXPR FRAG_SHADER_SIZE "${FRAG_SHADER_SIZE_HEX} / 2")

# Read VDP1 textured shaders
file(READ "${SHADER_DIR}/vdp1_textured_vertex.spv" TEXTURED_VERT_SHADER_HEX HEX)
string(LENGTH "${TEXTURED_VERT_SHADER_HEX}" TEXTURED_VERT_SHADER_SIZE_HEX)
math(EXPR TEXTURED_VERT_SHADER_SIZE "${TEXTURED_VERT_SHADER_SIZE_HEX} / 2")

file(READ "${SHADER_DIR}/vdp1_textured_fragment.spv" TEXTURED_FRAG_SHADER_HEX HEX)
string(LENGTH "${TEXTURED_FRAG_SHADER_HEX}" TEXTURED_FRAG_SHADER_SIZE_HEX)
math(EXPR TEXTURED_FRAG_SHADER_SIZE "${TEXTURED_FRAG_SHADER_SIZE_HEX} / 2")

# Read compositor shaders
file(READ "${SHADER_DIR}/compositor.vert.spv" COMPOSITOR_VERT_HEX HEX)
string(LENGTH "${COMPOSITOR_VERT_HEX}" COMPOSITOR_VERT_SIZE_HEX)
math(EXPR COMPOSITOR_VERT_SIZE "${COMPOSITOR_VERT_SIZE_HEX} / 2")

file(READ "${SHADER_DIR}/compositor.frag.spv" COMPOSITOR_FRAG_HEX HEX)
string(LENGTH "${COMPOSITOR_FRAG_HEX}" COMPOSITOR_FRAG_SIZE_HEX)
math(EXPR COMPOSITOR_FRAG_SIZE "${COMPOSITOR_FRAG_SIZE_HEX} / 2")

# Read NBG layer shaders
file(READ "${SHADER_DIR}/nbg_layer.vert.spv" NBG_VERT_HEX HEX)
string(LENGTH "${NBG_VERT_HEX}" NBG_VERT_SIZE_HEX)
math(EXPR NBG_VERT_SIZE "${NBG_VERT_SIZE_HEX} / 2")

file(READ "${SHADER_DIR}/nbg_layer.frag.spv" NBG_FRAG_HEX HEX)
string(LENGTH "${NBG_FRAG_HEX}" NBG_FRAG_SIZE_HEX)
math(EXPR NBG_FRAG_SIZE "${NBG_FRAG_SIZE_HEX} / 2")

# Read upscale shaders
file(READ "${SHADER_DIR}/upscale.vert.spv" UPSCALE_VERT_HEX HEX)
string(LENGTH "${UPSCALE_VERT_HEX}" UPSCALE_VERT_SIZE_HEX)
math(EXPR UPSCALE_VERT_SIZE "${UPSCALE_VERT_SIZE_HEX} / 2")

file(READ "${SHADER_DIR}/upscale.frag.spv" UPSCALE_FRAG_HEX HEX)
string(LENGTH "${UPSCALE_FRAG_HEX}" UPSCALE_FRAG_SIZE_HEX)
math(EXPR UPSCALE_FRAG_SIZE "${UPSCALE_FRAG_SIZE_HEX} / 2")

# Read VDP1 sprite shaders
file(READ "${SHADER_DIR}/vdp1_sprite.vert.spv" VDP1_SPRITE_VERT_HEX HEX)
string(LENGTH "${VDP1_SPRITE_VERT_HEX}" VDP1_SPRITE_VERT_SIZE_HEX)
math(EXPR VDP1_SPRITE_VERT_SIZE "${VDP1_SPRITE_VERT_SIZE_HEX} / 2")

file(READ "${SHADER_DIR}/vdp1_sprite.frag.spv" VDP1_SPRITE_FRAG_HEX HEX)
string(LENGTH "${VDP1_SPRITE_FRAG_HEX}" VDP1_SPRITE_FRAG_SIZE_HEX)
math(EXPR VDP1_SPRITE_FRAG_SIZE "${VDP1_SPRITE_FRAG_SIZE_HEX} / 2")

# Read FXAA shader
file(READ "${SHADER_DIR}/fxaa.frag.spv" FXAA_FRAG_HEX HEX)
string(LENGTH "${FXAA_FRAG_HEX}" FXAA_FRAG_SIZE_HEX)
math(EXPR FXAA_FRAG_SIZE "${FXAA_FRAG_SIZE_HEX} / 2")

# Read FSR RCAS shader
file(READ "${SHADER_DIR}/fsr_rcas.frag.spv" FSR_RCAS_FRAG_HEX HEX)
string(LENGTH "${FSR_RCAS_FRAG_HEX}" FSR_RCAS_FRAG_SIZE_HEX)
math(EXPR FSR_RCAS_FRAG_SIZE "${FSR_RCAS_FRAG_SIZE_HEX} / 2")

# Convert hex to C++ array format
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," VERT_SHADER_DATA "${VERT_SHADER_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FRAG_SHADER_DATA "${FRAG_SHADER_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," TEXTURED_VERT_SHADER_DATA "${TEXTURED_VERT_SHADER_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," TEXTURED_FRAG_SHADER_DATA "${TEXTURED_FRAG_SHADER_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," COMPOSITOR_VERT_DATA "${COMPOSITOR_VERT_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," COMPOSITOR_FRAG_DATA "${COMPOSITOR_FRAG_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," NBG_VERT_DATA "${NBG_VERT_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," NBG_FRAG_DATA "${NBG_FRAG_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," UPSCALE_VERT_DATA "${UPSCALE_VERT_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," UPSCALE_FRAG_DATA "${UPSCALE_FRAG_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," VDP1_SPRITE_VERT_DATA "${VDP1_SPRITE_VERT_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," VDP1_SPRITE_FRAG_DATA "${VDP1_SPRITE_FRAG_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FXAA_FRAG_DATA "${FXAA_FRAG_HEX}")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FSR_RCAS_FRAG_DATA "${FSR_RCAS_FRAG_HEX}")

# Remove trailing comma
string(REGEX REPLACE ",$" "" VERT_SHADER_DATA "${VERT_SHADER_DATA}")
string(REGEX REPLACE ",$" "" FRAG_SHADER_DATA "${FRAG_SHADER_DATA}")
string(REGEX REPLACE ",$" "" TEXTURED_VERT_SHADER_DATA "${TEXTURED_VERT_SHADER_DATA}")
string(REGEX REPLACE ",$" "" TEXTURED_FRAG_SHADER_DATA "${TEXTURED_FRAG_SHADER_DATA}")
string(REGEX REPLACE ",$" "" COMPOSITOR_VERT_DATA "${COMPOSITOR_VERT_DATA}")
string(REGEX REPLACE ",$" "" COMPOSITOR_FRAG_DATA "${COMPOSITOR_FRAG_DATA}")
string(REGEX REPLACE ",$" "" NBG_VERT_DATA "${NBG_VERT_DATA}")
string(REGEX REPLACE ",$" "" NBG_FRAG_DATA "${NBG_FRAG_DATA}")
string(REGEX REPLACE ",$" "" UPSCALE_VERT_DATA "${UPSCALE_VERT_DATA}")
string(REGEX REPLACE ",$" "" UPSCALE_FRAG_DATA "${UPSCALE_FRAG_DATA}")
string(REGEX REPLACE ",$" "" VDP1_SPRITE_VERT_DATA "${VDP1_SPRITE_VERT_DATA}")
string(REGEX REPLACE ",$" "" VDP1_SPRITE_FRAG_DATA "${VDP1_SPRITE_FRAG_DATA}")
string(REGEX REPLACE ",$" "" FXAA_FRAG_DATA "${FXAA_FRAG_DATA}")
string(REGEX REPLACE ",$" "" FSR_RCAS_FRAG_DATA "${FSR_RCAS_FRAG_DATA}")

# Generate header file
file(WRITE "${OUTPUT_FILE}" "// Auto-generated - DO NOT EDIT\n")
file(APPEND "${OUTPUT_FILE}" "// Embedded Vulkan shader bytecode (SPIR-V)\n\n")
file(APPEND "${OUTPUT_FILE}" "#pragma once\n\n")
file(APPEND "${OUTPUT_FILE}" "#include <cstdint>\n")
file(APPEND "${OUTPUT_FILE}" "#include <cstddef>\n\n")
file(APPEND "${OUTPUT_FILE}" "namespace brimir::vdp::shaders {\n\n")

file(APPEND "${OUTPUT_FILE}" "// ===== VDP1 Basic Shaders =====\n\n")
file(APPEND "${OUTPUT_FILE}" "// VDP1 Vertex Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t vdp1_vert_size = ${VERT_SHADER_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t vdp1_vert_data[] = {\n    ${VERT_SHADER_DATA}\n};\n\n")
file(APPEND "${OUTPUT_FILE}" "// VDP1 Fragment Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t vdp1_frag_size = ${FRAG_SHADER_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t vdp1_frag_data[] = {\n    ${FRAG_SHADER_DATA}\n};\n\n")

file(APPEND "${OUTPUT_FILE}" "// ===== VDP1 Textured Shaders =====\n\n")
file(APPEND "${OUTPUT_FILE}" "// VDP1 Textured Vertex Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t vdp1_textured_vert_size = ${TEXTURED_VERT_SHADER_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t vdp1_textured_vert_data[] = {\n    ${TEXTURED_VERT_SHADER_DATA}\n};\n\n")
file(APPEND "${OUTPUT_FILE}" "// VDP1 Textured Fragment Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t vdp1_textured_frag_size = ${TEXTURED_FRAG_SHADER_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t vdp1_textured_frag_data[] = {\n    ${TEXTURED_FRAG_SHADER_DATA}\n};\n\n")

file(APPEND "${OUTPUT_FILE}" "// ===== Compositor Shaders =====\n\n")
file(APPEND "${OUTPUT_FILE}" "// Layer Compositor Vertex Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t compositor_vert_size = ${COMPOSITOR_VERT_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t compositor_vert_data[] = {\n    ${COMPOSITOR_VERT_DATA}\n};\n\n")
file(APPEND "${OUTPUT_FILE}" "// Layer Compositor Fragment Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t compositor_frag_size = ${COMPOSITOR_FRAG_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t compositor_frag_data[] = {\n    ${COMPOSITOR_FRAG_DATA}\n};\n\n")

file(APPEND "${OUTPUT_FILE}" "// ===== NBG Layer Shaders =====\n\n")
file(APPEND "${OUTPUT_FILE}" "// NBG Layer Vertex Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t nbg_vert_size = ${NBG_VERT_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t nbg_vert_data[] = {\n    ${NBG_VERT_DATA}\n};\n\n")
file(APPEND "${OUTPUT_FILE}" "// NBG Layer Fragment Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t nbg_frag_size = ${NBG_FRAG_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t nbg_frag_data[] = {\n    ${NBG_FRAG_DATA}\n};\n\n")

file(APPEND "${OUTPUT_FILE}" "// ===== Upscale Shaders =====\n\n")
file(APPEND "${OUTPUT_FILE}" "// Upscale Vertex Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t upscale_vert_size = ${UPSCALE_VERT_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t upscale_vert_data[] = {\n    ${UPSCALE_VERT_DATA}\n};\n\n")
file(APPEND "${OUTPUT_FILE}" "// Upscale Fragment Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t upscale_frag_size = ${UPSCALE_FRAG_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t upscale_frag_data[] = {\n    ${UPSCALE_FRAG_DATA}\n};\n\n")

file(APPEND "${OUTPUT_FILE}" "// ===== VDP1 Sprite Shaders =====\n\n")
file(APPEND "${OUTPUT_FILE}" "// VDP1 Sprite Vertex Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t vdp1_sprite_vert_size = ${VDP1_SPRITE_VERT_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t vdp1_sprite_vert_data[] = {\n    ${VDP1_SPRITE_VERT_DATA}\n};\n\n")
file(APPEND "${OUTPUT_FILE}" "// VDP1 Sprite Fragment Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t vdp1_sprite_frag_size = ${VDP1_SPRITE_FRAG_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t vdp1_sprite_frag_data[] = {\n    ${VDP1_SPRITE_FRAG_DATA}\n};\n\n")

file(APPEND "${OUTPUT_FILE}" "// ===== FXAA Shader =====\n\n")
file(APPEND "${OUTPUT_FILE}" "// FXAA Fragment Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t fxaa_frag_size = ${FXAA_FRAG_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t fxaa_frag_data[] = {\n    ${FXAA_FRAG_DATA}\n};\n\n")

file(APPEND "${OUTPUT_FILE}" "// ===== FSR 1.0 RCAS Shader =====\n\n")
file(APPEND "${OUTPUT_FILE}" "// FSR RCAS Fragment Shader (SPIR-V bytecode)\n")
file(APPEND "${OUTPUT_FILE}" "constexpr size_t fsr_rcas_frag_size = ${FSR_RCAS_FRAG_SIZE};\n")
file(APPEND "${OUTPUT_FILE}" "constexpr uint8_t fsr_rcas_frag_data[] = {\n    ${FSR_RCAS_FRAG_DATA}\n};\n\n")

file(APPEND "${OUTPUT_FILE}" "} // namespace brimir::vdp::shaders\n")

message(STATUS "Embedded shaders:")
message(STATUS "  vdp1_vertex.spv (${VERT_SHADER_SIZE} bytes)")
message(STATUS "  vdp1_fragment.spv (${FRAG_SHADER_SIZE} bytes)")
message(STATUS "  vdp1_textured_vertex.spv (${TEXTURED_VERT_SHADER_SIZE} bytes)")
message(STATUS "  vdp1_textured_fragment.spv (${TEXTURED_FRAG_SHADER_SIZE} bytes)")
message(STATUS "  compositor.vert.spv (${COMPOSITOR_VERT_SIZE} bytes)")
message(STATUS "  compositor.frag.spv (${COMPOSITOR_FRAG_SIZE} bytes)")
message(STATUS "  nbg_layer.vert.spv (${NBG_VERT_SIZE} bytes)")
message(STATUS "  nbg_layer.frag.spv (${NBG_FRAG_SIZE} bytes)")
message(STATUS "  upscale.vert.spv (${UPSCALE_VERT_SIZE} bytes)")
message(STATUS "  upscale.frag.spv (${UPSCALE_FRAG_SIZE} bytes)")
message(STATUS "  vdp1_sprite.vert.spv (${VDP1_SPRITE_VERT_SIZE} bytes)")
message(STATUS "  vdp1_sprite.frag.spv (${VDP1_SPRITE_FRAG_SIZE} bytes)")
message(STATUS "  fxaa.frag.spv (${FXAA_FRAG_SIZE} bytes)")
message(STATUS "  fsr_rcas.frag.spv (${FSR_RCAS_FRAG_SIZE} bytes)")
