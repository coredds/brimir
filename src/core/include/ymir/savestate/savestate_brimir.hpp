#pragma once

// Brimir: emulator state the upstream save state omits or cannot restore exactly, so that a save state followed by a
// load continues exactly like the uninterrupted run (rewind, run-ahead and netplay need that). Kept apart from the
// upstream structs so their layout (and Brimir's codec of them) is unchanged. See src/core/BRIMIR_FORK.md.

#include <ymir/core/types.hpp>

#include <array>
#include <cstddef>

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

    // From Brimir save-state version 7 on (version 6 states end before this member; `valid` is then false).
    struct SCSPSlots {
        bool valid;
        // Modulation inputs each slot reads from the sound stack in pipeline steps 2-2 and 2-3 and uses in step 3-2
        // (SCSP::SlotProcessStep*), possibly in a later sample; kept from the last read while MDL < 5.
        std::array<sint16, 32> modXSample;
        std::array<sint16, 32> modYSample;
    } scspSlots;

    // From version 7 on: the internal backup RAM (32 KiB of data bytes). Games write it while running (records,
    // saves); without it, a run-ahead frame's write survived the load and the game read it a frame early.
    struct BackupRAM {
        bool valid; // false when the system has no internal backup RAM image (or in older states)
        std::array<uint8, 32 * 1024> internal;
    } backupRAM;

    // From version 9 on (versions 7 and 8 end before this member): what the peripherals on the two controller ports
    // keep between reads, so a read in progress continues the same after a load. A peripheral of another type than
    // the saved one (the device was changed since) keeps its own state.
    struct Peripherals {
        bool valid;
        struct Port {
            uint8 type;      // peripheral::PeripheralType of the connected peripheral
            bool mode;       // 3D Control Pad analog mode, Mission Stick six-axis mode
            uint8 reportPos; // position in the TH/TR handshake (Saturn ID protocol)
            bool tl;         // TL line of the TH/TR handshake
            bool mouseReset; // Shuttle Mouse reset line
            bool gunReload;  // Virtua Gun reload latched at the external latch
            // Written and read by the bridge (CoreWrapper), not by the core: its input state for the port (RetroPad
            // buttons of the previous frame for the mode switch and throttle latch edges, the 3D Control Pad mode it
            // reports, the Mission Stick throttle latch).
            uint16 prevButtons;
            bool analogMode;
            bool throttleLatched;
            uint8 throttle;
        };
        std::array<Port, 2> ports;
    } peripherals;

    // From version 10 on (version 9 ends before this member): the Virtua Gun jitter of VDP::ExternalLatch (enabled
    // for Death Crimson), whose random numbers come from this xorshift32 state (never zero) and avoid repeating the
    // last value on each axis.
    struct VirtuaGun {
        bool valid;
        uint32 jitterState;
        uint32 lastJitterX;
        uint32 lastJitterY;
    } virtuaGun;

    // From version 11 on (version 10 ends before this member): the accuracy settings of the session that saved the
    // state, which are not emulation state (each session keeps its own). Written and read by the bridge
    // (CoreWrapper), not by the core; the bridge warns when a loaded state's settings differ from the session's (a
    // netplay peer or a state from another configuration may then run differently). `valid` is false in states
    // saved by the core alone.
    struct Settings {
        bool valid;
        bool sh2CacheEmulation; // active, whether from the option or the game database
        uint8 scspStepGranularity; // 0 .. 5
    } settings;
};

// Bytes of the extension in a version 6 payload (before scspSlots), on every supported ABI.
inline constexpr size_t kBrimirSaveStateV6Size = 786480;

// Bytes of the extension in a version 7 or 8 payload (before peripherals), on every supported ABI.
inline constexpr size_t kBrimirSaveStateV8Size = 819376;

// Bytes of the extension in a version 9 payload (before virtuaGun), on every supported ABI.
inline constexpr size_t kBrimirSaveStateV9Size = 819392;

// Bytes of the extension in a version 10 payload (before settings), on every supported ABI.
inline constexpr size_t kBrimirSaveStateV10Size = 819408;

} // namespace ymir::savestate
