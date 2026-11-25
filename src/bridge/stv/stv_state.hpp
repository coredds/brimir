// Brimir - ST-V save state block
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// What an ST-V save state holds besides the Saturn payload: the cabinet's I/O board with its 93C46 and the coin and
// input state, the cartridge's protection chips, and the time base of the EEPROM's write cycles. The payload of an ST-V
// state is
//
//   [Saturn payload (EncodeSaveState)][block][uint32 block size]['S','T','V','1']
//
// inside the usual LZ4 frame and header (same magic and version as a Saturn state). The session decides which layout to
// expect, never the bytes: a Saturn session decodes the whole payload as a Saturn state (an ST-V state then has bytes
// left over and is refused), an ST-V session requires the trailer and its own game's hash.
//
// Block versions: 1 (Brimir 0.10.0) is magic, version, game hash, I/O board, 315-5881 protection, scheduler count,
// remainder, flags. 2 inserts the 315-5838's state before the flags. A version 1 block still decodes, with the 315-5838
// at its reset value; a block is always saved in the current version.

#pragma once

#include "stv_cartridge.hpp"
#include "stv_io.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace brimir::stv {

inline constexpr uint32_t kSTVStateMagic = 0x42'56'54'53;   // 'S','T','V','B' in memory
inline constexpr uint32_t kSTVStateVersion = 2;
inline constexpr uint32_t kSTVStateOldestVersion = 1;
inline constexpr uint32_t kSTVStateFlagsKnown = 0;          // versions 1 and 2 define no flags

// The four bytes that end the payload of an ST-V state
inline constexpr uint8_t kSTVTrailerMagic[4] = {'S', 'T', 'V', '1'};
inline constexpr size_t kSTVTrailerSize = 8;                // uint32 block size, then the magic

struct STVStateBlock {
    uint32_t version = kSTVStateVersion;                    // as decoded; EncodeSTVState writes the current one
    uint64_t gameHash = 0;                                  // STVGameHash of the game's short name
    STVIOBoardState io{};
    ymir::cart::STVGameROMCartridge::ProtectionState protection{};
    uint64_t schedulerCount = 0;                            // STVSystem's time base: the count of the last advance
    uint64_t nsRemainderBits = 0;                           // and the fraction of a nanosecond not passed on (a double)
    ymir::cart::STV3155838::State comp5838{};               // version 2; the reset value from a version 1 block
    uint32_t flags = 0;
};

// xxHash64 of a game's MAME short name: what ties a state to its game
uint64_t STVGameHash(std::string_view shortName);

// The size of an encoded block of the current version or of the given one (0 for a version this core does not read),
// and of the whole tail (block and trailer)
size_t STVStateBlockSize();
size_t STVStateBlockSize(uint32_t version);
inline size_t STVStateTailSize() {
    return STVStateBlockSize() + kSTVTrailerSize;
}
inline size_t STVStateTailSize(uint32_t version) {
    const size_t block = STVStateBlockSize(version);
    return block != 0 ? block + kSTVTrailerSize : 0;
}

// Appends the block, in the current version, and the trailer to out
void EncodeSTVState(const STVStateBlock &block, std::vector<uint8_t> &out);

// The size of the tail a payload's trailer announces (block and trailer; any block size that fits), or 0 when the
// payload does not end with an ST-V trailer. DecodeSTVState checks the block.
size_t STVStateTailSizeIn(std::span<const uint8_t> payload);

// Whether a payload ends with an ST-V trailer. Only for telling a user why a Saturn session refused a state: what a
// session accepts never depends on it.
bool HasSTVTrailer(std::span<const uint8_t> payload);

// Reads the tail of an ST-V payload (the last STVStateTailSizeIn(payload) bytes). False unless the trailer and the
// block's magic are right, the version is one this core reads and the size is that version's, and the flags are known;
// then why (if given) says what was wrong. The block's contents (values the I/O board and the chips can be in, the
// game) are checked by whoever loads it.
bool DecodeSTVState(std::span<const uint8_t> in, STVStateBlock &out, std::string *why = nullptr);

} // namespace brimir::stv
