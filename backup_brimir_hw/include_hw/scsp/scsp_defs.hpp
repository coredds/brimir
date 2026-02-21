#pragma once

#include <brimir/core/types.hpp>

namespace brimir::scsp {

// Number of SCSP slots
inline constexpr uint64 kSlots = 32;

// Audio sampling rate in Hz
inline constexpr uint64 kAudioFreq = 44100;

// Number of SCSP cycles per slot
inline constexpr uint64 kCyclesPerSlot = 16;

// Number of SCSP cycles per sample: 16 cycles per slot * 32 slots = 512 cycles
inline constexpr uint64 kCyclesPerSample = kCyclesPerSlot * kSlots;

// Number of SCSP cycles per MC68EC000 cycle
inline constexpr uint64 kCyclesPerM68KCycle = 2;

// Number of M68K cycles per slot
inline constexpr uint64 kM68KCyclesPerSlot = kCyclesPerSlot / kCyclesPerM68KCycle;

// Number of M68K cycles per sample
inline constexpr uint64 kM68KCyclesPerSample = kCyclesPerSample / kCyclesPerM68KCycle;

// SCSP clock frequency: 22,579,200 Hz = 44,100 Hz * 512 cycles per sample
inline constexpr uint64 kClockFreq = kAudioFreq * kCyclesPerSample;

// Pending interrupt flags
inline constexpr uint16 kIntrINT0N = 0;          // External INT0N line
inline constexpr uint16 kIntrINT1N = 1;          // External INT1N line
inline constexpr uint16 kIntrINT2N = 2;          // External INT2N line
inline constexpr uint16 kIntrMIDIInput = 3;      // MIDI input non-empty
inline constexpr uint16 kIntrDMATransferEnd = 4; // DMA transfer end
inline constexpr uint16 kIntrCPUManual = 5;      // CPU manual interrupt request
inline constexpr uint16 kIntrTimerA = 6;         // Timer A
inline constexpr uint16 kIntrTimerB = 7;         // Timer B
inline constexpr uint16 kIntrTimerC = 8;         // Timer C
inline constexpr uint16 kIntrMIDIOutput = 9;     // MIDI output empty
inline constexpr uint16 kIntrSample = 10;        // Once every sample tick

} // namespace brimir::scsp
