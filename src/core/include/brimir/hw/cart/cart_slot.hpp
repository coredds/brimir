#pragma once

#include "cart_base.hpp"

#include <concepts>
#include <memory>
#include <type_traits>

namespace brimir::cart {

class CartridgeSlot {
public:
    CartridgeSlot();

    void Reset(bool hard) {
        m_cart->Reset(hard);
    }

    // Inserts the cartridge into this slot.
    template <typename T, typename... Args>
        requires std::derived_from<T, cart::BaseCartridge>
    T *InsertCartridge(Args &&...args) {
        m_cart = std::make_unique<T>(std::forward<Args>(args)...);
        return static_cast<T *>(m_cart.get());
    }

    // Removes the cartridge from this slot.
    void RemoveCartridge();

    // Returns a reference to the inserted cartridge.
    [[nodiscard]] BaseCartridge &GetCartridge() {
        return *m_cart;
    }

    // Returns a reference to the inserted cartridge.
    [[nodiscard]] const BaseCartridge &GetCartridge() const {
        return *m_cart;
    }

    // Retrieves the inserted cartridge's ID.
    [[nodiscard]] uint8 GetID() const {
        return m_cart->GetID();
    }

    // Retrieves the inserted cartridge's type.
    [[nodiscard]] CartType GetCartridgeType() const {
        return m_cart->GetType();
    }

    template <bool peek>
    uint8 ReadByte(uint32 address) const {
        if constexpr (peek) {
            return m_cart->PeekByte(address);
        } else {
            return m_cart->ReadByte(address);
        }
    }

    template <bool peek>
    uint16 ReadWord(uint32 address) const {
        if constexpr (peek) {
            return m_cart->PeekWord(address);
        } else {
            return m_cart->ReadWord(address);
        }
    }

    template <bool poke>
    void WriteByte(uint32 address, uint8 value) {
        if constexpr (poke) {
            m_cart->PokeByte(address, value);
        } else {
            m_cart->WriteByte(address, value);
        }
    }

    template <bool poke>
    void WriteWord(uint32 address, uint16 value) {
        if constexpr (poke) {
            m_cart->PokeWord(address, value);
        } else {
            m_cart->WriteWord(address, value);
        }
    }

private:
    std::unique_ptr<BaseCartridge> m_cart;
};

} // namespace brimir::cart
