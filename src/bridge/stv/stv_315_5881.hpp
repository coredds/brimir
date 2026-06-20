#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace ymir::cart {

class STV3155881 {
public:
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
