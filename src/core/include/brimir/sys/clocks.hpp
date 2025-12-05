#pragma once

#include <brimir/core/configuration_defs.hpp>

#include <brimir/core/types.hpp>

#include <array>

namespace brimir::sys {

inline constexpr double kNTSCClock = 39375000.0 * 8.0 / 11.0;
inline constexpr double kPALClock = 28437500.0;

inline constexpr uint64 kNTSCClocksPerFrame = 455 * 525 * 2;
inline constexpr uint64 kPALClocksPerFrame = 455 * 625 * 2;

inline constexpr double kNTSCFrameRate = brimir::sys::kNTSCClock / brimir::sys::kNTSCClocksPerFrame;
inline constexpr double kPALFrameRate = brimir::sys::kPALClock / brimir::sys::kPALClocksPerFrame;

inline constexpr double kNTSCFrameInterval = brimir::sys::kNTSCClocksPerFrame / brimir::sys::kNTSCClock;
inline constexpr double kPALFrameInterval = brimir::sys::kPALClocksPerFrame / brimir::sys::kPALClock;

enum class ClockSpeed { _320, _352 };

// Clock speeds:
// - Master clock - used by both SH-2s, VDP1, VDP2 and SCU:
//   - 320 mode: 26.846591 MHz (NTSC) / 26.660156 MHz (PAL)
//   - 352 mode: 28.636364 MHz (NTSC) / 28.437500 MHz (PAL)
//   - NTSC clock (352 mode) = 39375000 * 8/11
//   - 320 mode clock = 352 mode clock * 15/16
//   - VDP pixel clock is 1/2 on hi-res modes or 1/4 at lo-res modes
//   - SCU DSP runs at 1/2 clock speed
// - SCSP: 22.579200 MHz (= 44100 * 512)
//   - MC68EC000 runs at 1/2 SCSP clock
// - CD Block SH1: 20.000000 MHz
// - SMPC MCU: 4.000000 MHz
// - RTC: 32768 Hz (but this emulator updates it at 1 Hz)
//
// The listed ratios below are all exact and relative to the master clock (SH-2/VDPs/SCU).
// These ratios are used in the scheduler to accurately schedule events relative to each clock.
//
// NTSC system at clock 320 mode:
//   Clock rate         Ratio       Minimized ratio
//   26,846,590.91   39424:39424          1:1
//   22,579,200.00   39424:46875      39424:46875
//   20,000,000.00   39424:52920        704:945
//    4,000,000.00   39424:264600       704:4725
//
// NTSC system at clock 352 mode:
//   Clock rate         Ratio       Minimized ratio
//   28,636,363.64   2464:2464            1:1
//   22,579,200.00   2464:3125         2464:3125
//   20,000,000.00   2464:3528           44:63
//    4,000,000.00   2464:17640          44:315
//
// PAL system at clock 320 mode:
//   Clock rate         Ratio       Minimized ratio
//   26,660,156.25   172032:172032        1:1
//   22,579,200.00   172032:203125   172032:203125
//   20,000,000.00   172032:229320     1024:1365
//    4,000,000.00   172032:1146600    1024:6825
//
// PAL system at clock 352 mode:
//   Clock rate         Ratio       Minimized ratio
//   28,437,500.00   32256:32256          1:1
//   22,579,200.00   32256:40625      32256:40625
//   20,000,000.00   32256:45864         64:91
//    4,000,000.00   32256:229320        64:455

struct ClockRatios {
    uint64 masterClock;
    uint64 masterClockNum;
    uint64 masterClockDen;

    uint64 SCSPNum;
    uint64 SCSPDen;

    uint64 CDBlockNum;
    uint64 CDBlockDen;

    uint64 SMPCNum;
    uint64 SMPCDen;

    uint64 RTCNum;
    uint64 RTCDen;
};

inline constexpr std::array<ClockRatios, 4> kClockRatios = {
    // [0] NTSC, 320 mode
    ClockRatios{
        .masterClock = 39375000ull,
        .masterClockNum = 8 * 15,
        .masterClockDen = 11 * 16,

        .SCSPNum = 39424,
        .SCSPDen = 46875,
        .CDBlockNum = 704,
        .CDBlockDen = 945,
        .SMPCNum = 704,
        .SMPCDen = 4725,
        .RTCNum = 11 * 16,
        .RTCDen = 39375000ull * 8 * 15,
    },

    // [1] NTSC, 352 mode
    ClockRatios{
        .masterClock = 39375000ull,
        .masterClockNum = 8,
        .masterClockDen = 11,

        .SCSPNum = 2464,
        .SCSPDen = 3125,
        .CDBlockNum = 44,
        .CDBlockDen = 63,
        .SMPCNum = 44,
        .SMPCDen = 315,
        .RTCNum = 11,
        .RTCDen = 39375000ull * 8,
    },

    // [2] PAL, 320 mode
    ClockRatios{
        .masterClock = 28437500ull,
        .masterClockNum = 15,
        .masterClockDen = 16,

        .SCSPNum = 172032,
        .SCSPDen = 203125,
        .CDBlockNum = 1024,
        .CDBlockDen = 1365,
        .SMPCNum = 1024,
        .SMPCDen = 6825,
        .RTCNum = 1 * 16,
        .RTCDen = 28437500ull * 15,
    },

    // [3] PAL, 352 mode
    ClockRatios{
        .masterClock = 28437500ull,
        .masterClockNum = 1,
        .masterClockDen = 1,

        .SCSPNum = 32256,
        .SCSPDen = 40625,
        .CDBlockNum = 64,
        .CDBlockDen = 91,
        .SMPCNum = 64,
        .SMPCDen = 455,
        .RTCNum = 1,
        .RTCDen = 28437500ull,
    },
};

} // namespace brimir::sys
