// Brimir - ST-V save state block
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// What an ST-V save state holds besides the Saturn payload: the cabinet's I/O board with its 93C46 and the coin and
// input state, the cartridge's protection chip, and the time base of the EEPROM's write cycles. The payload of an ST-V
// state is
//
//   [Saturn payload (EncodeSaveState)][block][uint32 block size]['S','T','V','1']
//
// inside the usual LZ4 frame and header (same magic and version as a Saturn state). The session decides which layout to
// expect, never the bytes: a Saturn session decodes the whole payload as a Saturn state (an ST-V state then has bytes
// left over and is refused), an ST-V session requires the trailer and its own game's hash.

#pragma once

#include "stv_cartridge.hpp"
#include "stv_io.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace brimir::stv {

inline constexpr uint32_t kSTVStateMagic = 0x42'56'54'53;   // 'S','T','V','B' in memory
inline constexpr uint32_t kSTVStateVersion = 1;
inline constexpr uint32_t kSTVStateFlagsKnown = 0;          // version 1 defines no flags

// The four bytes that end the payload of an ST-V state
inline constexpr uint8_t kSTVTrailerMagic[4] = {'S', 'T', 'V', '1'};
inline constexpr size_t kSTVTrailerSize = 8;                // uint32 block size, then the magic

struct STVStateBlock {
    uint32_t version = kSTVStateVersion;
    uint64_t gameHash = 0;                                  // STVGameHash of the game's short name
    STVIOBoardState io{};
    ymir::cart::STVGameROMCartridge::ProtectionState protection{};
    uint64_t schedulerCount = 0;                            // STVSystem's time base: the count of the last advance
    uint64_t nsRemainderBits = 0;                           // and the fraction of a nanosecond not passed on (a double)
    uint32_t flags = 0;
};

// xxHash64 of a game's MAME short name: what ties a state to its game
uint64_t STVGameHash(std::string_view shortName);

// The size of an encoded block, and of the whole tail (block and trailer) EncodeSTVState appends
size_t STVStateBlockSize();
inline size_t STVStateTailSize() {
    return STVStateBlockSize() + kSTVTrailerSize;
}

// Appends the block and the trailer to out
void EncodeSTVState(const STVStateBlock &block, std::vector<uint8_t> &out);

// Whether a payload ends with an ST-V trailer. Only for telling a user why a Saturn session refused a state: what a
// session accepts never depends on it.
bool HasSTVTrailer(std::span<const uint8_t> payload);

// Reads the tail of an ST-V payload (the last STVStateTailSize() bytes). False unless the size is exact, the trailer
// and the block's magic and version are right, and the flags are known; the block's contents (values the I/O board and
// the chip can be in, the game) are checked by whoever loads it.
bool DecodeSTVState(std::span<const uint8_t> in, STVStateBlock &out);

} // namespace brimir::stv
