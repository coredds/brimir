// Brimir - ST-V system wiring
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_system.hpp"

#include "stv_cartridge.hpp"

#include <ymir/sys/saturn.hpp>

#include <utility>

namespace brimir::stv {

namespace {

// The IOGA sits at 0x00400000 (MAME stv_mem: map(0x00400000, 0x0040001f).mirror(0x20)), page 0x40 of the 64 KiB-page
// main bus. On a Saturn that page holds the bus's catch-all (unmapped) handlers, which ignore writes and read 0.
constexpr uint32_t kIOGAPageStart = 0x0040'0000;
constexpr uint32_t kIOGAPageEnd = 0x0040'FFFF;

// The page as Saturn's constructor leaves it (the access cycles were never touched): the catch-all normal handlers
// with no context, and the default side-effect-free handlers (read 0, ignore writes). Saturn's own handlers only
// add a debug log line.
void RestoreSaturnIOGAPage(ymir::sys::SH2Bus &bus) {
    bus.MapNormal(
        kIOGAPageStart, kIOGAPageEnd, nullptr, [](uint32_t, void *) -> uint8_t { return 0; },
        [](uint32_t, void *) -> uint16_t { return 0; }, [](uint32_t, void *) -> uint32_t { return 0; },
        [](uint32_t, uint8_t, void *) {}, [](uint32_t, uint16_t, void *) {}, [](uint32_t, uint32_t, void *) {});
    bus.MapSideEffectFree(
        kIOGAPageStart, kIOGAPageEnd, nullptr, [](uint32_t, void *) -> uint8_t { return 0; },
        [](uint32_t, void *) -> uint16_t { return 0; }, [](uint32_t, void *) -> uint32_t { return 0; },
        [](uint32_t, uint8_t, void *) {}, [](uint32_t, uint16_t, void *) {}, [](uint32_t, uint32_t, void *) {});
}

// ROM header fields the I/O board derives the factory EEPROM from (offsets in the cartridge image)
constexpr uint32_t kHeaderOffset = 0xF40;
constexpr uint32_t kCabinetTypeOffset = 0xF46;
constexpr uint32_t kGameSettingsOffset = 0xF48;

} // namespace

STVSystem::~STVSystem() {
    Detach();
}

bool STVSystem::Attach(ymir::Saturn &saturn, const GameInfo &game, std::array<uint8_t, kSTVBiosSize> &bios,
                       std::span<const uint8_t> cart, uint8_t areaCode, std::string &error) {
    if (m_saturn != nullptr) {
        error = "the ST-V system is attached already";
        return false;
    }
    if (cart.empty()) {
        error = "the ST-V cartridge image is empty";
        return false;
    }

    m_saturn = &saturn;
    m_game = &game;
    m_prevAreaCode = saturn.SMPC.GetAreaCode();
    saturn.SMPC.SavePersistentData(m_prevSMPC);

    saturn.LoadIPL(std::span<uint8_t, kSTVBiosSize>(bios));

    using ProtectionMode = ymir::cart::STVGameROMCartridge::ProtectionMode;
    m_cart = saturn.InsertCartridge<ymir::cart::STVGameROMCartridge>();
    m_cart->LoadROM(cart);
    m_cart->ConfigureProtection(game.chip == Chip::Crypt315_5881 ? ProtectionMode::Chip3155881 : ProtectionMode::None,
                                game.chip == Chip::Crypt315_5881 ? game.cryptKey : 0);

    m_io.Reset(true);
    m_io.SetIPLPointer(saturn.mem.IPL.data());
    m_io.SetSTVMode(true);
    uint8_t header[2];
    uint8_t settings[8];
    for (uint32_t i = 0; i < sizeof(header); i++) {
        header[i] = m_cart->PeekROMByte(kHeaderOffset + i);
    }
    for (uint32_t i = 0; i < sizeof(settings); i++) {
        settings[i] = m_cart->PeekROMByte(kGameSettingsOffset + i);
    }
    m_io.InitEEPROM(header, settings, m_cart->PeekROMByte(kCabinetTypeOffset));
    m_io.MapMemory(saturn.mainBus);

    // The PDR pins carry the cabinet EEPROM and, on PDR2, the 68K's reset line (MAME stv.cpp pdr1_output_w and
    // pdr2_output_w: bit 4 set holds the 68K in reset). Each access first passes the I/O board the emulated time that
    // went by, which ends the EEPROM's write cycles.
    saturn.SMPC.SetSTVMode(true);
    saturn.SMPC.SetSTVPDRHandlers(
        [this]() {
            AdvanceIOTime();
            return m_io.ReadPDR1();
        },
        [this](uint8_t data) {
            AdvanceIOTime();
            m_io.WritePDR1(data);
        },
        [this]() {
            AdvanceIOTime();
            return m_io.ReadPDR2();
        },
        [this](uint8_t data) {
            AdvanceIOTime();
            m_io.WritePDR2(data);
            m_saturn->SCSP.SetCPUEnabled((data & 0x10) == 0);
        });

    saturn.Reset(true);
    saturn.SMPC.SetAreaCode(areaCode);
    ResyncTime();
    return true;
}

void STVSystem::Detach() {
    if (m_saturn == nullptr) {
        return;
    }
    ymir::Saturn &saturn = *m_saturn;

    saturn.SMPC.SetSTVPDRHandlers({}, {}, {}, {});
    saturn.SMPC.SetSTVMode(false);
    saturn.RemoveCartridge();
    RestoreSaturnIOGAPage(saturn.mainBus);
    saturn.SMPC.SetAreaCode(m_prevAreaCode);

    // The BIOS settings are the console's, not the board's; the clock keeps the time it reached
    ymir::smpc::PersistentSMPCData now;
    saturn.SMPC.SavePersistentData(now);
    now.SMEM = m_prevSMPC.SMEM;
    now.STE = m_prevSMPC.STE;
    saturn.SMPC.LoadPersistentData(now);

    m_cart = nullptr;
    m_game = nullptr;
    m_saturn = nullptr;
}

void STVSystem::ResyncTime() {
    m_lastCount = m_saturn != nullptr ? m_saturn->GetSchedulerCount() : 0;
    m_nsRemainder = 0.0;
}

void STVSystem::AdvanceIOTime() {
    const uint64_t now = m_saturn->GetSchedulerCount();
    if (now < m_lastCount) {
        // a hard reset (the scheduler restarted)
        m_lastCount = now;
        return;
    }
    const uint64_t cycles = now - m_lastCount;
    m_lastCount = now;
    if (cycles == 0) {
        return;
    }

    // The scheduler counts master clock cycles: 26.85 MHz in 320 mode, 28.64 MHz in 352 mode (NTSC)
    const ymir::sys::ClockRatios &ratios = m_saturn->GetClockRatios();
    const double hz = static_cast<double>(ratios.masterClock) * static_cast<double>(ratios.masterClockNum) /
                      static_cast<double>(ratios.masterClockDen);
    m_nsRemainder += static_cast<double>(cycles) * 1e9 / hz;
    const uint64_t ns = static_cast<uint64_t>(m_nsRemainder);
    m_nsRemainder -= static_cast<double>(ns);
    if (ns != 0) {
        m_io.AdvanceTime(ns);
    }
}

} // namespace brimir::stv
