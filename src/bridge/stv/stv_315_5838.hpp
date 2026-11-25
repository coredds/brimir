// Brimir - Sega 315-5838 cartridge compression/encryption chip (Decathlete)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0 (the combined work)
//
// This file is derived from MAME's src/mame/sega/315-5838_317-0229_comp.h and .cpp (MAME 0.289): the chip's state is
// that class's. The original is under the BSD-3-Clause licence, whose notice follows and is retained as it requires:
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
//
// The chip reads compressed words from an 8 MiB window of the cartridge image (MAME's "protbank": bank N starts at
// image offset N * 0x800000), deciphers each (Decathlete's cipher), and decodes the bit stream with a 12-entry
// canonical Huffman tree into bytes of a 256-byte dictionary; the game uploads the tree and the dictionary. Besides
// MAME's state, the chip holds the board glue's bank and libretro-stv's `active` flag (see stv_cartridge.cpp).

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ymir::cart {

class STV3155838 {
public:
    struct TreeNode {
        uint8_t  len;     // in bits
        uint8_t  idx;     // in the dictionary
        uint16_t pattern; // of the first code, left-justified in 12 bits
    };

    // Everything the chip holds except the ROM view, for ST-V save states. No padding: every byte is a member. The
    // reset value (a chip never used) is State{}.
    struct State {
        uint32_t srcOffset;         // MAME m_srcoffset: the next source word, a 23-bit word offset
        uint32_t srcStart;          // m_srcstart
        int32_t  numBitsCompressed; // m_num_bits_compressed
        int32_t  numBits;           // m_num_bits
        int32_t  treeIndex;         // m_compstate.it2
        int32_t  dictIndex;         // m_compstate.id
        uint16_t mode;              // m_compstate.mode
        uint16_t valCompressed;     // m_val_compressed
        uint16_t val;               // m_val
        uint8_t  abort;             // m_abort
        uint8_t  bank;              // stv_state's m_protbank entry (0-2)
        std::array<TreeNode, 13> tree;      // m_compstate.tree (entry 12 is the never-written sentinel)
        std::array<uint8_t, 256> dictionary; // m_compstate.dictionary
        uint8_t  active;            // a source address was written (libretro-stv's gate on the reads)
        uint8_t  reserved[3];
    };

    // Decathlete's cipher of a source word (MAME's decipher)
    static uint16_t Decipher(uint16_t c);

    // MAME's device_reset: the source address and the failsafe; the tables are kept. Also stops the chip answering
    // reads (libretro-stv's reset clears its `active` flag) and selects bank 0.
    void Reset();

    // The cartridge image (big-endian bytes, a power of two in size; empty reads 0xFFFF)
    void SetROM(std::span<const uint8_t> rom);
    // The 8 MiB window the source words come from (stv_state's m_protbank->set_entry)
    void SetBank(uint8_t bank);

    // srcaddr_w (MAME's set_prot_addr): the written lanes of the source address, and the decoder restarts
    void SetSourceAddress(uint32_t data, uint32_t mask);
    // data_w (MAME's write_prot_data with rev_words 0): the high half is the table upload mode, the low half a table
    // word. Another mask (MAME: fatalerror) is ignored.
    void WriteData(uint32_t data, uint32_t mask);
    void SetTableUploadMode(uint16_t value);
    void UploadTableData(uint16_t value);

    // data_r: two decompressed bytes, the first in the high half
    uint16_t ReadData();

    bool IsActive() const { return m_active; }

    void SaveState(State &state) const;
    // Whether LoadState would take the state
    bool ValidateState(const State &state) const;
    // Returns false and leaves the chip untouched when the state holds values the chip cannot be in.
    bool LoadState(const State &state);

private:
    uint16_t SourceWord();
    uint8_t  DecompressedByte();

    std::span<const uint8_t> m_rom;

    uint32_t m_srcOffset = 0;
    uint32_t m_srcStart  = 0;
    bool     m_abort     = false;

    uint16_t m_mode = 0;
    std::array<TreeNode, 13> m_tree{};
    int      m_treeIndex = 0;
    std::array<uint8_t, 256> m_dictionary{};
    int      m_dictIndex = 0;

    int      m_numBitsCompressed = 0;
    uint16_t m_valCompressed     = 0;
    int      m_numBits           = 0;
    uint16_t m_val               = 0;

    uint8_t m_bank   = 0;
    bool    m_active = false;
};

} // namespace ymir::cart
