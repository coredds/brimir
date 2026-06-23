// Brimir ST-V Boot Test
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "catch_amalgamated.hpp"
#include <brimir/core_wrapper.hpp>
#include <ymir/sys/saturn.hpp>

#include <cstdio>

using namespace brimir;

static void DumpCartCS0(ymir::Saturn* saturn) {
    printf("\n--- Cartridge CS0 at 0x02000000 ---\n");
    for (uint32 off = 0; off < 0x100; off += 16) {
        uint32 addr = 0x02000000u + off;
        printf("  +%04X: %04X %04X %04X %04X %04X %04X %04X %04X\n", off,
               saturn->mainBus.Peek<uint16>(addr), saturn->mainBus.Peek<uint16>(addr+2),
               saturn->mainBus.Peek<uint16>(addr+4), saturn->mainBus.Peek<uint16>(addr+6),
               saturn->mainBus.Peek<uint16>(addr+8), saturn->mainBus.Peek<uint16>(addr+10),
               saturn->mainBus.Peek<uint16>(addr+12), saturn->mainBus.Peek<uint16>(addr+14));
    }
}

static void DumpBus(ymir::Saturn* saturn, uint32 addr, uint32 count) {
    printf("--- Bus peek at 0x%08X ---\n", addr);
    for (uint32 off = 0; off < count; off += 16) {
        uint32 a = addr + off;
        printf("  +%04X: %04X %04X %04X %04X %04X %04X %04X %04X\n", off,
               saturn->mainBus.Peek<uint16>(a), saturn->mainBus.Peek<uint16>(a+2),
               saturn->mainBus.Peek<uint16>(a+4), saturn->mainBus.Peek<uint16>(a+6),
               saturn->mainBus.Peek<uint16>(a+8), saturn->mainBus.Peek<uint16>(a+10),
               saturn->mainBus.Peek<uint16>(a+12), saturn->mainBus.Peek<uint16>(a+14));
    }
}

TEST_CASE("ST-V Baku Baku boots past CD block area after CKCHG352 NMI", "[stv][integration]") {
    CoreWrapper core;
    REQUIRE(core.Initialize());

    const char* biosDir = "F:/OneDrive/Roms/BIOS";
    const char* romPath = "C:/Users/david/Downloads/bakubaku.zip";

    bool loaded = core.LoadSTVGame(romPath, biosDir);
    if (!loaded) {
        const auto& err = core.GetLastError();
        WARN("LoadSTVGame failed: " << (err.empty() ? "(no message)" : err));
    }
    REQUIRE(loaded);

    ymir::Saturn* saturn = core.GetSaturn();
    REQUIRE(saturn != nullptr);

    // Dump before running any frames
    printf("\n=== Before frame 1 ===\n");
    DumpCartCS0(saturn);
    DumpBus(saturn, 0x00000000u, 0x40);  // BIOS vectors
    DumpBus(saturn, 0x06000000u, 0x40);  // CD block area
    DumpBus(saturn, 0x00000F40u, 0x20);  // Cartridge header area (EEPROM init reads here)
    printf("--- Cartridge at 0x02020000 (program code area) ---\n");
    DumpBus(saturn, 0x02020000u, 0x40);
    printf("--- Cartridge at 0x02200000 (word ROM, first 16LE file) ---\n");
    DumpBus(saturn, 0x02200000u, 0x60);
    printf("--- Cartridge at 0x02400000 (4MB offset, word ROM mpr17970.2) ---\n");
    DumpBus(saturn, 0x02400000u, 0x60);
    printf("--- Cartridge at 0x02000200 (reset vector target) ---\n");
    DumpBus(saturn, 0x02000200u, 0x40);

    bool pcLeftCDBlock = false;
    uint32 lastMPC = 0;
    uint32 lastSPC = 0;

    for (int frame = 1; frame <= 180; frame++) {
        core.RunFrame();

        {
            ymir::sh2::SH2::Probe mProbe(saturn->masterSH2);
            ymir::sh2::SH2::Probe sProbe(saturn->slaveSH2);
            lastMPC = mProbe.PC();
            lastSPC = sProbe.PC();
        }

        bool inCDBlock = (lastMPC >= 0x06000000u && lastMPC < 0x06100000u);
        if (!inCDBlock && lastMPC > 0x00010000u) {
            pcLeftCDBlock = true;
            printf("Frame %d: Master PC left CD block! mPC=0x%08X sPC=0x%08X\n", frame, lastMPC, lastSPC);
            break;
        }

        if (frame == 60) {
            printf("--- CD block area at frame 60 ---\n");
            DumpBus(saturn, 0x06000000u, 0x80);
            DumpBus(saturn, 0x06002000u, 0x80);
            DumpBus(saturn, 0x06015000u, 0x80);
        }
        if (frame < 10 || frame % 30 == 0) {
            printf("Frame %d: mPC=0x%08X sPC=0x%08X\n", frame, lastMPC, lastSPC);
        }
    }

    printf("Final state: mPC=0x%08X sPC=0x%08X\n", lastMPC, lastSPC);

    if (!pcLeftCDBlock) {
        WARN("Master PC still in CD block area after 180 frames.");
    }

    CHECK(pcLeftCDBlock);
}
