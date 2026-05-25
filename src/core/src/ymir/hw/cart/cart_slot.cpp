#include <ymir/hw/cart/cart_slot.hpp>

#include <ymir/hw/cart/cart_impl_none.hpp>

namespace ymir::cart {

CartridgeSlot::CartridgeSlot() {
    RemoveCartridge();
}

void CartridgeSlot::RemoveCartridge() {
    m_cart = std::make_unique<NoCartridge>();
}

} // namespace ymir::cart
