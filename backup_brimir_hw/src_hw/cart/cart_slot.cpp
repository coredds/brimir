#include <brimir/hw/cart/cart_slot.hpp>

#include <brimir/hw/cart/cart_impl_none.hpp>

namespace brimir::cart {

CartridgeSlot::CartridgeSlot() {
    RemoveCartridge();
}

void CartridgeSlot::RemoveCartridge() {
    m_cart = std::make_unique<NoCartridge>();
}

} // namespace brimir::cart
