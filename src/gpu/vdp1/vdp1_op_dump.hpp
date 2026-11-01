#pragma once

// A capture of one VDP2 frame's GPU VDP1 work (GPU stage 3, Task 1; brimir_vkhost --dump-vdp1-ops): the op lists the
// front half handed the back half during the frame, the state they start from (all eight FBRAM slices and VDP1 VRAM)
// and the slices they end with, plus the registers and colours a sprite-layer decode needs. The Vulkan VDP renderer
// writes it (VulkanVDPRenderer::SetVdp1OpDump); the N x VDP1 reference tools read it (tests/unit/vdp1_nx_reference.*).
// Replaying the lists from the start state with the CPU reference (tests/unit/vdp1_raster_reference.hpp) must give the
// end slices.
//
// File: the magic "BRV1OPS1", then little-endian fields in the order of Vdp1OpDump; every vector is a uint32 element
// count and the elements' raw bytes (the structs are the GPU structs, fixed layout). Vulkan-free.

#include "gpu/vdp1/vdp1_front.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace brimir::gpu::vdp1 {

struct Vdp1OpDump {
    uint32_t frame = 0;          // the VDP2 frame captured (VDP2EndFrame calls since SetVdp1OpDump)
    std::vector<uint8_t> fbramStart; // kFbramBufferSize: slices in Vdp1MainSlice/AltSlice/MeshSlice order, raw bytes
    std::vector<uint8_t> vramStart;  // VDP1 VRAM (512 KiB) as of the start
    std::vector<Vdp1OpList> lists;   // in order
    std::vector<uint8_t> fbramEnd;   // kFbramBufferSize after the lists

    // At the end of the frame
    uint32_t drawFB = 0, displayFB = 0;
    uint32_t hres = 320, vres = 224;
    uint32_t exclusiveMonitor = 0;
    uint32_t deinterlace = 0, transparentMeshes = 0; // enhancements
    uint16_t tvmr = 0, fbcr = 0;                       // VDP1 TVMR, FBCR
    std::array<uint16_t, 0x90> vdp2Regs{};             // VDP2 registers 0x000-0x11E (peeked)
    std::array<uint32_t, 2048> palette{};              // CRAM colours as RGB888 (0x00BBGGRR) by colour index, for
                                                       // the CRAM mode at the end of the frame
};

bool WriteVdp1OpDump(const std::string &path, const Vdp1OpDump &dump, std::string *error = nullptr);
bool ReadVdp1OpDump(const std::string &path, Vdp1OpDump &dump, std::string *error = nullptr);

} // namespace brimir::gpu::vdp1
