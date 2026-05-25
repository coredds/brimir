#include <ymir/db/rom_cart_db.hpp>

#include <unordered_map>

namespace ymir::db {

const ROMCartInfo *GetROMCartInfo(XXH128Hash hash) {
    if (hash == kKOF95ROMInfo.hash) {
        return &kKOF95ROMInfo;
    } else if (hash == kUltramanROMInfo.hash) {
        return &kUltramanROMInfo;
    } else {
        return nullptr;
    }
}

} // namespace ymir::db
