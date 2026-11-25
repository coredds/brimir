// Brimir - Sega 315-5838 cartridge compression/encryption chip (Decathlete)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0 (the combined work)
//
// This file is derived from MAME's src/mame/sega/315-5838_317-0229_comp.cpp (MAME 0.289): the cipher, the table
// uploads and the decompression are a translation of that code (its debug dumps and its hack modes for the other games
// are left out). The original is under the BSD-3-Clause licence, whose notice follows and is retained as it requires:
//
// Copyright (c) David Haywood, Samuel Neves, Peter Wilhelmsen, Morten Shearman Kirkegaard
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
// following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
//    disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
//    following disclaimer in the documentation and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
//    products derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include "stv_315_5838.hpp"

#include <cassert>
#include <type_traits>

namespace ymir::cart {

static_assert(std::has_unique_object_representations_v<STV3155838::State>);

namespace {

constexpr uint32_t kSourceMask = 0x007FFFFFu; // MAME masks m_srcoffset to 23 bits
constexpr uint32_t kBankSize   = 0x800000u;   // device_rom_interface<23>: an 8 MiB window
constexpr uint8_t  kBankCount  = 3;           // init_decathlt configures entries 0-2

// The most bits a transfer can read before the failsafe stops it (the whole window, 8M words)
constexpr int32_t kMaxBits = 16 * 0x800000;

} // namespace

uint16_t STV3155838::Decipher(uint16_t c) {
    uint16_t x[16];
    for (int b = 0; b < 16; ++b) {
        x[b] = (c >> b) & 1;
    }

    uint16_t p = 0;
    p |= (x[7] ^ x[9] ^ x[14] ? 0 : x[5] ^ x[12]) ^ x[14];
    p |= (((x[7] ^ x[9]) & (x[12] ^ x[14] ^ x[5])) ^ x[14] ^ 1) << 1;
    p |= ((x[6] & x[8]) ^ (x[6] & x[15]) ^ (x[8] & x[15]) ^ 1) << 2;
    p |= (x[11] ^ x[14] ^ 1) << 3;
    p |= ((x[7] & (x[1] ^ x[8] ^ x[12])) ^ x[12]) << 4;
    p |= ((x[6] | x[8]) ^ (x[8] & x[15])) << 5;
    p |= (x[4] ^ (x[3] | x[10])) << 6;
    p |= ((x[14] & (x[5] ^ x[12])) ^ x[7] ^ x[9] ^ 1) << 7;
    p |= (x[4] ^ x[13] ^ 1) << 8;
    p |= (x[6] ^ (x[8] | (x[15] ^ 1))) << 9;
    p |= (x[7] ^ (x[12] | (x[1] ^ x[8] ^ x[7] ^ 1))) << 10;
    p |= (x[3] ^ x[10] ^ 1) << 11;
    p |= (x[0] ^ x[2]) << 12;
    p |= (x[8] ^ x[1] ? x[12] : x[7]) << 13;
    p |= (x[0] ^ x[11] ^ x[14] ^ 1) << 14;
    p |= (x[10] ^ 1) << 15;
    return p;
}

void STV3155838::Reset() {
    m_srcOffset = 0;
    m_srcStart = 0;
    m_abort = false;
    // Not in MAME's device_reset: the decoder (set_prot_addr restarts it anyway), the glue's bank, and libretro-stv's
    // `active` flag, which its reset clears so the cartridge reads the ROM until the game starts the chip again
    m_numBitsCompressed = 0;
    m_valCompressed = 0;
    m_numBits = 0;
    m_val = 0;
    m_bank = 0;
    m_active = false;
}

void STV3155838::SetROM(std::span<const uint8_t> rom) {
    assert((rom.size() & (rom.size() - 1)) == 0 && "the cartridge image is a power of two in size");
    m_rom = rom;
}

void STV3155838::SetBank(uint8_t bank) {
    if (bank < kBankCount) {
        m_bank = bank;
    }
}

// MAME's source_word_r: read_word((m_srcoffset * 2) ^ 2) on a little-endian 8-bit space over the big-endian cartridge
// region is the big-endian word at byte offset m_srcoffset * 2 of the window (the ^ 2 undoes the host's byte order).
// The window's offsets are 23 bits. The image is mirrored as the CPU sees it at 0x02000000 on.
uint16_t STV3155838::SourceWord() {
    uint16_t word = 0xFFFFu;
    if (!m_rom.empty()) {
        const size_t offset = (static_cast<size_t>(m_bank) * kBankSize + ((m_srcOffset * 2u) & (kBankSize - 2u))) &
                              (m_rom.size() - 1);
        word = static_cast<uint16_t>(m_rom[offset] << 8 | m_rom[offset + 1]);
    }
    m_srcOffset = (m_srcOffset + 1) & kSourceMask;

    // if we've wrapped around to where we started something has gone wrong with the transfer, abandon (MAME)
    if (m_srcOffset == m_srcStart) {
        m_abort = true;
    }
    return word;
}

// MAME's get_decompressed_byte (HACK_MODE_NONE)
uint8_t STV3155838::DecompressedByte() {
    for (;;) {
        if (m_abort) {
            return 0xFF;
        }

        if (m_numBitsCompressed == 0) {
            m_valCompressed = Decipher(SourceWord());
            m_numBitsCompressed = 16;
        }

        m_numBitsCompressed--;
        m_val <<= 1;
        m_val |= 1 & (m_valCompressed >> m_numBitsCompressed);
        m_numBits++;

        // Codes are at most 12 bits: MAME would shift by a negative count for a longer entry (undefined), so none of
        // those matches here
        if (m_numBits > 12) {
            continue;
        }
        const int shift = 12 - m_numBits;
        for (int i = 0; i < 12; i++) {
            if (m_numBits != m_tree[i].len) continue;
            if (m_val < (m_tree[i].pattern >> shift)) continue;
            if (m_numBits < 12 && m_val >= (m_tree[i + 1].pattern >> shift)) continue;

            const int j = m_tree[i].idx + m_val - (m_tree[i].pattern >> shift);

            m_val = 0;
            m_numBits = 0;

            // MAME reads past the dictionary for an index over 255 (tables the game never uploads); kept inside here
            return m_dictionary[j & 0xFF];
        }
    }
}

uint16_t STV3155838::ReadData() {
    const uint8_t hi = DecompressedByte();
    const uint8_t lo = DecompressedByte();
    return static_cast<uint16_t>(hi << 8 | lo);
}

void STV3155838::SetSourceAddress(uint32_t data, uint32_t mask) {
    m_srcOffset = (m_srcOffset & ~mask) | (data & mask);
    m_srcOffset &= kSourceMask;
    m_srcStart = m_srcOffset;
    m_abort = false;

    m_numBitsCompressed = 0;
    m_valCompressed = 0;
    m_numBits = 0;
    m_val = 0;

    // libretro-stv: the cartridge answers reads from the chip only once a source address was written
    m_active = true;
}

void STV3155838::SetTableUploadMode(uint16_t value) {
    m_mode = value;
    if (!(m_mode & 0x80)) { // 0x8000 and 0x0000
        m_treeIndex = 0;
    } else { // 0x8080 and 0x0080
        m_dictIndex = 0;
    }
}

// MAME asserts that the indices stay inside the tables; words past them are dropped here (as libretro-stv does)
void STV3155838::UploadTableData(uint16_t value) {
    if (!(m_mode & 0x80)) {
        if (m_treeIndex / 2 >= 12) {
            return;
        }
        if ((m_treeIndex & 1) == 0) {
            m_tree[m_treeIndex / 2].len = static_cast<uint8_t>((0xFF00 & value) >> 8);
            m_tree[m_treeIndex / 2].idx = static_cast<uint8_t>(0x00FF & value);
        } else {
            m_tree[m_treeIndex / 2].pattern = value;
        }
        m_treeIndex++;
    } else {
        if (m_dictIndex >= 255) {
            return;
        }
        m_dictionary[m_dictIndex++] = static_cast<uint8_t>((0xFF00 & value) >> 8);
        m_dictionary[m_dictIndex++] = static_cast<uint8_t>(0x00FF & value);
    }
}

void STV3155838::WriteData(uint32_t data, uint32_t mask) {
    if (mask == 0xFFFF0000u) {
        SetTableUploadMode(static_cast<uint16_t>(data >> 16));
    } else if (mask == 0x0000FFFFu) {
        UploadTableData(static_cast<uint16_t>(data & 0xFFFFu));
    }
}

void STV3155838::SaveState(State &state) const {
    state = {};
    state.srcOffset = m_srcOffset;
    state.srcStart = m_srcStart;
    state.numBitsCompressed = m_numBitsCompressed;
    state.numBits = m_numBits;
    state.treeIndex = m_treeIndex;
    state.dictIndex = m_dictIndex;
    state.mode = m_mode;
    state.valCompressed = m_valCompressed;
    state.val = m_val;
    state.abort = m_abort ? 1 : 0;
    state.bank = m_bank;
    state.tree = m_tree;
    state.dictionary = m_dictionary;
    state.active = m_active ? 1 : 0;
}

bool STV3155838::ValidateState(const State &state) const {
    if (state.srcOffset > kSourceMask || state.srcStart > kSourceMask) return false;
    if (state.abort > 1 || state.active > 1 || state.bank >= kBankCount) return false;
    if (state.reserved[0] != 0 || state.reserved[1] != 0 || state.reserved[2] != 0) return false;
    if (state.numBitsCompressed < 0 || state.numBitsCompressed > 16) return false;
    if (state.treeIndex < 0 || state.treeIndex > 24) return false;
    if (state.dictIndex < 0 || state.dictIndex > 256 || (state.dictIndex & 1) != 0) return false;
    if (state.tree[12].len != 0 || state.tree[12].idx != 0 || state.tree[12].pattern != 0) return false;
    // A read ends on a code (the code bits cleared) or on the failsafe, and a state is saved between reads
    if (state.abort == 0) {
        if (state.numBits != 0 || state.val != 0) return false;
    } else if (state.numBits < 0 || state.numBits > kMaxBits) {
        return false;
    }
    return true;
}

bool STV3155838::LoadState(const State &state) {
    if (!ValidateState(state)) {
        return false;
    }
    m_srcOffset = state.srcOffset;
    m_srcStart = state.srcStart;
    m_numBitsCompressed = state.numBitsCompressed;
    m_numBits = state.numBits;
    m_treeIndex = state.treeIndex;
    m_dictIndex = state.dictIndex;
    m_mode = state.mode;
    m_valCompressed = state.valCompressed;
    m_val = state.val;
    m_abort = state.abort != 0;
    m_bank = state.bank;
    m_tree = state.tree;
    m_dictionary = state.dictionary;
    m_active = state.active != 0;
    return true;
}

} // namespace ymir::cart
