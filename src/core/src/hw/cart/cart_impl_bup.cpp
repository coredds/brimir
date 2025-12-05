#include <brimir/hw/cart/cart_impl_bup.hpp>

#include <brimir/util/size_ops.hpp>

#include <utility>

namespace brimir::cart {

static constexpr uint8 GetCartID(uint32 bupSize) {
    if (bupSize <= 512_KiB) {
        return 0x21; // 4 Mbit Backup RAM
    } else if (bupSize <= 1_MiB) {
        return 0x22; // 8 Mbit Backup RAM
    } else if (bupSize <= 2_MiB) {
        return 0x23; // 16 Mbit Backup RAM
    } else {
        return 0x24; // 32 Mbit Backup RAM
    }
}

BackupMemoryCartridge::BackupMemoryCartridge(bup::BackupMemory &&backupRAM)
    : BaseCartridge(GetCartID(backupRAM.Size()), CartType::BackupMemory)
    , m_backupRAM(std::move(backupRAM)) {}

void BackupMemoryCartridge::CopyBackupMemoryFrom(const bup::IBackupMemory &backupRAM) {
    ChangeID(GetCartID(backupRAM.Size()));
    m_backupRAM.CopyFrom(backupRAM);
}

} // namespace brimir::cart
