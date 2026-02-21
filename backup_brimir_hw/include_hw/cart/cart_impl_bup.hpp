#pragma once

#include "cart_base.hpp"

#include <brimir/sys/backup_ram.hpp>

#include <filesystem>
#include <memory>

namespace brimir::cart {

class BackupMemoryCartridge final : public BaseCartridge {
public:
    // Creates a backup memory cartridge from the specified backup memory.
    // The size of the cartridge is determined by the size of the given backup memory.
    // Backup memory cartridges come in four sizes: 512 KiB, 1 MiB, 2 MiB or 4 MiB.
    // If the backup memory is smaller than 512 KiB, it will be mirrored across the 512 KiB range.
    // If it is larger than 4 MiB, only the lower 4 MiB will be used.
    BackupMemoryCartridge(bup::BackupMemory &&backupRAM);

    uint8 ReadByte(uint32 address) const override {
        return m_backupRAM.ReadByte(address);
    }

    uint16 ReadWord(uint32 address) const override {
        return m_backupRAM.ReadWord(address);
    }

    void WriteByte(uint32 address, uint8 value) override {
        m_backupRAM.WriteByte(address, value);
    }

    void WriteWord(uint32 address, uint16 value) override {
        m_backupRAM.WriteWord(address, value);
    }

    uint8 PeekByte(uint32 address) const override {
        return ReadByte(address);
    }
    uint16 PeekWord(uint32 address) const override {
        return ReadWord(address);
    }

    void PokeByte(uint32 address, uint8 value) override {
        WriteByte(address, value);
    }
    void PokeWord(uint32 address, uint16 value) override {
        WriteWord(address, value);
    }

    bup::IBackupMemory &GetBackupMemory() {
        return m_backupRAM;
    }

    void CopyBackupMemoryFrom(const bup::IBackupMemory &backupRAM);

private:
    bup::BackupMemory m_backupRAM;
};

} // namespace brimir::cart
