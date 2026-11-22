// Brimir - Sega 315-5881 cartridge decryption/decompression chip (Radiant Silvergun)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0 (the combined work)
//
// This file is derived from MAME's src/mame/sega/315-5881_crypt.h and .cpp (MAME 0.289): the chip's state is that
// class's. The original is under the BSD-3-Clause licence, whose notice follows and is retained as it requires:
//
// Copyright (c) Andreas Naive, Olivier Galibert, David Haywood
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
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ymir::cart {

class STV3155881 {
public:
    // Everything the chip holds except the ROM view, for ST-V save states. No padding: every byte is a member.
    struct State {
        uint32_t key;
        uint32_t subKey;
        uint32_t addr;
        uint32_t decHeader;
        uint32_t bufferPos;
        uint32_t lineBufferPos;
        uint32_t lineBufferSize;
        uint32_t blockPos;
        uint32_t blockSize;
        uint32_t blockNumLines;
        int32_t  bufferBit;
        int32_t  bufferBit2;
        uint16_t decHist;
        uint16_t buffer2a;
        std::array<uint8_t, 2>   buffer2;
        std::array<uint8_t, 2>   buffer;
        std::array<uint8_t, 512> lineBuffer;
        std::array<uint8_t, 512> lineBufferPrev;
        uint8_t  enabled;
        uint8_t  ready;
        uint8_t  doneCompression;
        uint8_t  reserved;
    };

    void SaveState(State &state) const;
    // Whether LoadState would take the state
    bool ValidateState(const State &state) const;
    // Returns false and leaves the chip untouched when the state holds values the chip cannot be in.
    bool LoadState(const State &state);

    void Reset();

    void SetROM(std::span<const uint8_t> rom);
    void SetEnabled(bool enabled);
    void SetKey(uint32_t key);
    void SetLowAddress(uint16_t value);
    void SetHighAddress(uint16_t value);
    void SetSubKey(uint16_t value);

    uint16_t ReadDecryptedWord();

private:
    uint16_t ReadEncryptedWord(uint32_t addr) const;
    uint16_t GetDecrypted16();
    void     CryptoStart();
    uint32_t GetCompressedBit();
    void     LineFill();
    void     EncFill();

    std::span<const uint8_t> m_rom;

    bool     m_enabled = false;
    uint32_t m_key     = 0;
    uint32_t m_subKey  = 0;
    uint32_t m_addr    = 0;
    bool     m_ready   = false;

    int m_bufferBit  = 0;
    int m_bufferBit2 = 0;

    uint16_t m_decHist   = 0;
    uint32_t m_decHeader = 0;

    uint32_t m_bufferPos     = 0;
    uint32_t m_lineBufferPos = 0;
    uint32_t m_lineBufferSize = 0;
    bool     m_doneCompression = false;

    uint32_t m_blockPos      = 0;
    uint32_t m_blockSize     = 0;
    uint32_t m_blockNumLines = 0;

    uint16_t                m_buffer2a = 0;
    std::array<uint8_t, 2>  m_buffer2{};
    std::array<uint8_t, 2>  m_buffer{};
    std::array<uint8_t, 512> m_lineBuffer{};
    std::array<uint8_t, 512> m_lineBufferPrev{};
};

} // namespace ymir::cart
