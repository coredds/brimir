#pragma once

#include "cart_base.hpp"

namespace brimir::cart {

class NoCartridge final : public BaseCartridge {
public:
    NoCartridge()
        : BaseCartridge(0xFFu, CartType::None) {}

    uint8 ReadByte(uint32 address) const override {
        return 0xFFu;
    }
    uint16 ReadWord(uint32 address) const override {
        return 0xFFFFu;
    }

    void WriteByte(uint32 address, uint8 value) override {}
    void WriteWord(uint32 address, uint16 value) override {}

    uint8 PeekByte(uint32 address) const override {
        return 0xFFu;
    }
    uint16 PeekWord(uint32 address) const override {
        return 0xFFFFu;
    }

    void PokeByte(uint32 address, uint8 value) override {}
    void PokeWord(uint32 address, uint16 value) override {}
};

} // namespace brimir::cart
