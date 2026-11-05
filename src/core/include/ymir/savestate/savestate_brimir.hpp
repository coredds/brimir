#pragma once

// Brimir: emulator state the upstream save state omits or cannot restore exactly, so that a save state followed by a
// load continues exactly like the uninterrupted run (rewind, run-ahead and netplay need that). Kept apart from the
// upstream structs so their layout (and Brimir's codec of them) is unchanged. See src/core/BRIMIR_FORK.md.

#include <ymir/core/types.hpp>

#include <array>

namespace ymir::savestate {

struct BrimirSaveState {
    // False in states from Brimir versions before this extension: the loader then keeps the upstream behaviour.
    bool valid;

    struct VDP {
        // TVMD the current display timings were derived from. A TVMD write takes effect at the next top border
        // (VDP::UpdateResolution), so it can differ from the register while a change is pending.
        uint16 appliedTVMD;
        bool TVMDDirty;
        // Derived by UpdateResolution from the field at the time, then kept until the next top border while the
        // field toggles at VBlank IN.
        uint32 VTimingField;
        uint16 VCounterSkip;
        // VDP1 infinite loop detection (VDP::VDP1ProcessCommand)
        uint32 vdp1LastJumpAddress;
        uint32 vdp1LoopCount;
        bool vdp1InInfiniteLoop;
        // VDP1 command table return address of a Call command
        uint32 vdp1ReturnAddress;
    } vdp;

    struct VDPRenderer {
        // VDP1 framebuffers of the other field in double-density interlace (the software renderer's altFBRAM; the
        // Vulkan renderer's alt slices). The frame shown after a load combines them with the loaded fields.
        alignas(16) std::array<std::array<uint8, 256 * 1024>, 2> altFBRAM;
        // The VDP1 draw framebuffer as drawn so far. A threaded software VDP1 draws into its own copy and copies it
        // into VDPState (the state's FBRAM) only at the end of the draw, swap or erase, so in the middle of a draw the
        // state's FBRAM lacks what was drawn; unthreaded (and the Vulkan renderer) it equals the state's FBRAM.
        alignas(16) std::array<uint8, 256 * 1024> vdp1DrawFB;
    } vdpRenderer;

    struct SCSP {
        // Number of DSP steps run per sample; maintained incrementally by MPRO writes, so it cannot be derived from
        // the program alone.
        uint8 dspProgramLength;
    } scsp;
};

} // namespace ymir::savestate
