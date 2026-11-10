// Brimir - save-state payload codec
// Licensed under GPL-3.0

#include <brimir/savestate_codec.hpp>

#include <brimir/peripheral_input.hpp>

#include <ymir/hw/cart/rom_cart_defs.hpp>
#include <ymir/hw/smpc/peripheral/peripheral_defs.hpp>
#include <ymir/hw/vdp/vdp1_defs.hpp>
#include <ymir/hw/vdp/vdp1_regs.hpp>

#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>

namespace brimir {

namespace {

using ymir::savestate::SaveState;
using ymir::savestate::SCUSaveState;
using ymir::savestate::SMPCSaveState;

// The visitors below bind every member of these structs with a structured binding, so a member added
// upstream fails to compile there. The sizes are pinned as well (64-bit ABIs): if one of these fires,
// an upstream field was added or changed -- add it to the serializer and bump
// CoreWrapper::kSaveStateVersion.
static_assert(sizeof(void*) != 8 || sizeof(SCUSaveState) == 2496, "SCUSaveState changed: update the serializer");
static_assert(sizeof(void*) != 8 || sizeof(SMPCSaveState) == 144, "SMPCSaveState changed: update the serializer");
static_assert(sizeof(ymir::savestate::BrimirSaveState) == 819456,
              "BrimirSaveState changed: bump CoreWrapper::kSaveStateVersion and convert older states");
// Version 12's smpcSettings took the version 11 struct's tail padding; its mouse made the struct larger than version
// 11's (so a version 12 payload under a version 11 header is refused): both pinned too.
static_assert(offsetof(ymir::savestate::BrimirSaveState, smpcSettings) == 819411 &&
                  sizeof(ymir::savestate::BrimirSaveState::SMPCSettings) == 6,
              "BrimirSaveState changed: bump CoreWrapper::kSaveStateVersion and convert older states");
static_assert(offsetof(ymir::savestate::BrimirSaveState, mouse) == 819420 &&
                  sizeof(ymir::savestate::BrimirSaveState::Mouse) == 36,
              "BrimirSaveState changed: bump CoreWrapper::kSaveStateVersion and convert older states");
static_assert(sizeof(ymir::savestate::BrimirSaveState) > ymir::savestate::kBrimirSaveStateV11Size,
              "a version 12 payload must not fit a version 11 header");

// Counts bytes (out == nullptr) or writes them.
class Writer {
public:
    explicit Writer(uint8_t* out) : m_out(out) {}

    template <typename T>
    void Raw(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "only trivially copyable members are written as raw bytes");
        Bytes(&value, sizeof(T));
    }

    void Size(const size_t& value) {
        Raw(static_cast<uint64_t>(value));
    }

    void Vector(const std::vector<uint8>& value, size_t maxSize) {
        if (value.size() > maxSize) {
            m_ok = false;
        }
        Raw(static_cast<uint32_t>(value.size()));
        Bytes(value.data(), value.size());
    }

    size_t Position() const { return m_pos; }
    bool Ok() const { return m_ok; }

private:
    void Bytes(const void* data, size_t size) {
        if (m_out != nullptr && size > 0) {
            std::memcpy(m_out + m_pos, data, size);
        }
        m_pos += size;
    }

    uint8_t* m_out;
    size_t m_pos = 0;
    bool m_ok = true;
};

// Reads with a bounds check on every field; any failure sticks.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> in) : m_in(in) {}

    template <typename T>
    void Raw(T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "only trivially copyable members are read as raw bytes");
        if (!Take(sizeof(T))) {
            return;
        }
        if constexpr (std::is_same_v<T, bool>) {
            // Any other byte would be an invalid bool.
            if (m_in[m_pos - 1] > 1) {
                m_ok = false;
                return;
            }
        }
        std::memcpy(&value, m_in.data() + m_pos - sizeof(T), sizeof(T));
    }

    void Size(size_t& value) {
        uint64_t raw = 0;
        Raw(raw);
        if (raw > std::numeric_limits<size_t>::max()) {
            m_ok = false;
            return;
        }
        value = static_cast<size_t>(raw);
    }

    void Vector(std::vector<uint8>& value, size_t maxSize) {
        uint32_t size = 0;
        Raw(size);
        if (!m_ok || size > maxSize || !Take(size)) {
            m_ok = false;
            return;
        }
        value.assign(m_in.begin() + static_cast<std::ptrdiff_t>(m_pos - size),
                     m_in.begin() + static_cast<std::ptrdiff_t>(m_pos));
    }

    // The first `size` bytes of a trivially copyable member (an older, shorter layout of it); the rest is untouched.
    template <typename T>
    void RawPrefix(T& value, size_t size) {
        static_assert(std::is_trivially_copyable_v<T>, "only trivially copyable members are read as raw bytes");
        if (size > sizeof(T) || !Take(size)) {
            m_ok = false;
            return;
        }
        std::memcpy(&value, m_in.data() + m_pos - size, size);
    }

    bool Done() const { return m_ok && m_pos == m_in.size(); }

private:
    bool Take(size_t size) {
        if (!m_ok || m_in.size() - m_pos < size) {
            m_ok = false;
            return false;
        }
        m_pos += size;
        return true;
    }

    std::span<const uint8_t> m_in;
    size_t m_pos = 0;
    bool m_ok = true;
};

// Zeroes every field, padding included, as value-initialization does; vectors keep their capacity.
class Clearer {
public:
    template <typename T>
    void Raw(T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "only trivially copyable members are cleared bytewise");
        std::memset(&value, 0, sizeof(T));
    }

    void Size(size_t& value) { value = 0; }

    void Vector(std::vector<uint8>& value, size_t /*maxSize*/) { value.clear(); }
};

template <typename Archive, typename State>
void VisitSCU(Archive& ar, State& scu) {
    auto& [dma, dsp, cartType, cartData, intrMask, intrStatus, abusIntrsPendingAck, pendingIntrLevel,
           pendingIntrIndex, timer0Counter, timer0Compare, timer1Reload, timer1Mode, timerEnable, wramSizeSelect] = scu;
    ar.Raw(dma);
    ar.Raw(dsp);
    ar.Raw(cartType);
    ar.Vector(cartData, kMaxSaveStateCartDataSize);
    ar.Raw(intrMask);
    ar.Raw(intrStatus);
    ar.Raw(abusIntrsPendingAck);
    ar.Raw(pendingIntrLevel);
    ar.Raw(pendingIntrIndex);
    ar.Raw(timer0Counter);
    ar.Raw(timer0Compare);
    ar.Raw(timer1Reload);
    ar.Raw(timer1Mode);
    ar.Raw(timerEnable);
    ar.Raw(wramSizeSelect);
}

template <typename Archive, typename State>
void VisitSMPC(Archive& ar, State& smpc) {
    auto& [IREG, OREG, COMREG, SR, SF, PDR1, PDR2, DDR1, DDR2, IOSEL, EXLE, intback, busValue, resetDisable,
           commandEventState, rtcTimestamp, rtcSysClockCount] = smpc;
    ar.Raw(IREG);
    ar.Raw(OREG);
    ar.Raw(COMREG);
    ar.Raw(SR);
    ar.Raw(SF);
    ar.Raw(PDR1);
    ar.Raw(PDR2);
    ar.Raw(DDR1);
    ar.Raw(DDR2);
    ar.Raw(IOSEL);
    ar.Raw(EXLE);

    auto& [getPeripheralData, optimize, port1mode, port2mode, report, reportOffset, inProgress] = intback;
    ar.Raw(getPeripheralData);
    ar.Raw(optimize);
    ar.Raw(port1mode);
    ar.Raw(port2mode);
    ar.Vector(report, kMaxSaveStateINTBACKReportSize);
    ar.Size(reportOffset);
    ar.Raw(inProgress);

    ar.Raw(busValue);
    ar.Raw(resetDisable);
    ar.Raw(commandEventState);
    ar.Raw(rtcTimestamp);
    ar.Raw(rtcSysClockCount);
}

// version: payloads from version 6 on end with the Brimir extension (savestate_brimir.hpp); earlier ones lack it,
// version 6 has only its first kBrimirSaveStateV6Size bytes (no scspSlots; read with Reader::RawPrefix), and versions
// 7 and 8 only the first kBrimirSaveStateV8Size bytes (no peripherals), version 9 the first kBrimirSaveStateV9Size
// bytes (no virtuaGun), version 10 the first kBrimirSaveStateV10Size bytes (no settings), version 11 the first
// kBrimirSaveStateV11Size bytes (padding where smpcSettings is, and no mouse).
template <typename Archive, typename State>
void Visit(Archive& ar, State& s, uint32_t version = kSaveStatePayloadVersion) {
    auto& [scheduler, system, msh2, ssh2, scu, smpc, vdp, scsp, cdblockLLE, cdblock, sh1, ygr, cddrive, cdblockDRAM,
           discHash, msh2SpilloverCycles, ssh2SpilloverCycles, sh1SpilloverCycles, sh1FracCycles, brimir] = s;
    ar.Raw(scheduler);
    ar.Raw(system);
    ar.Raw(msh2);
    ar.Raw(ssh2);
    VisitSCU(ar, scu);
    VisitSMPC(ar, smpc);
    ar.Raw(vdp);
    ar.Raw(scsp);
    ar.Raw(cdblockLLE);
    ar.Raw(cdblock);
    ar.Raw(sh1);
    ar.Raw(ygr);
    ar.Raw(cddrive);
    ar.Raw(cdblockDRAM);
    ar.Raw(discHash);
    ar.Raw(msh2SpilloverCycles);
    ar.Raw(ssh2SpilloverCycles);
    ar.Raw(sh1SpilloverCycles);
    ar.Raw(sh1FracCycles);
    if (version >= 12) {
        ar.Raw(brimir);
    } else if (version == 11) {
        if constexpr (std::is_same_v<Archive, Reader>) {
            ar.RawPrefix(brimir, ymir::savestate::kBrimirSaveStateV11Size);
        }
    } else if (version == 10) {
        if constexpr (std::is_same_v<Archive, Reader>) {
            ar.RawPrefix(brimir, ymir::savestate::kBrimirSaveStateV10Size);
        }
    } else if (version == 9) {
        if constexpr (std::is_same_v<Archive, Reader>) {
            ar.RawPrefix(brimir, ymir::savestate::kBrimirSaveStateV9Size);
        }
    } else if (version >= 7) {
        if constexpr (std::is_same_v<Archive, Reader>) {
            ar.RawPrefix(brimir, ymir::savestate::kBrimirSaveStateV8Size);
        }
    } else if (version == 6) {
        if constexpr (std::is_same_v<Archive, Reader>) {
            ar.RawPrefix(brimir, ymir::savestate::kBrimirSaveStateV6Size);
        }
    }
}

// Whether the cartridge image has a size the state's version holds for its cartridge type (false for a value that is
// not a cartridge type). SCU::ValidateState checks only the 8 and 32 Mbit sizes, and SCU::LoadState copies a fixed
// size per type out of cartData, so every size is checked here. From version 8 on the image of a backup memory
// cartridge is its data (one of the four backup cartridge sizes) and a ROM cartridge has none (codec header).
bool ValidCartDataSize(SCUSaveState::CartType type, size_t size, uint32_t version) {
    using CartType = SCUSaveState::CartType;
    switch (type) {
    case CartType::None: return size == 0;
    case CartType::BackupMemory:
        if (version < 8) {
            return size == 0;
        }
        return size == 512 * 1024 || size == 1024 * 1024 || size == 2 * 1024 * 1024 || size == 4 * 1024 * 1024;
    case CartType::DRAM8Mbit: return size == 1024 * 1024;
    case CartType::DRAM32Mbit: return size == 4 * 1024 * 1024;
    case CartType::DRAM48Mbit: return size == 6 * 1024 * 1024;
    case CartType::ROM: return size == (version < 8 ? ymir::cart::kROMCartSize : 0);
    }
    return false;
}

// The extension is read as raw bytes, so its bools are checked here (any byte other than 0 or 1 is an invalid bool),
// with the ranges the loaders rely on: a VDP1 return address is "none" or within VDP1 VRAM (a Call at the last slot
// returns to its end), the timing field is 0 or 1, the DSP program has at most 128 steps.
bool ValidBrimirExtension(const ymir::savestate::BrimirSaveState &ext, uint32_t version) {
    const auto byte = [](const bool &value) {
        uint8_t raw = 0;
        std::memcpy(&raw, &value, 1);
        return raw;
    };
    if (byte(ext.valid) != 1 || byte(ext.vdp.TVMDDirty) > 1 || byte(ext.vdp.vdp1InInfiniteLoop) > 1) {
        return false;
    }
    if (ext.vdp.VTimingField > 1) {
        return false;
    }
    if (ext.vdp.vdp1ReturnAddress != ymir::vdp::kVDP1NoReturn && ext.vdp.vdp1ReturnAddress > ymir::vdp::kVDP1VRAMSize) {
        return false;
    }
    if (version >= 7 && byte(ext.backupRAM.valid) > 1) {
        return false;
    }
    if (version >= 7 && byte(ext.scspSlots.valid) != 1) {
        return false;
    }
    if (version >= 9) {
        // The peripheral type is a PeripheralType (None .. ShuttleMouse); the longest TH/TR handshake (six-axis
        // Mission Stick) has 22 positions.
        if (byte(ext.peripherals.valid) != 1) {
            return false;
        }
        for (const auto& port : ext.peripherals.ports) {
            if (port.type > static_cast<uint8_t>(ymir::peripheral::PeripheralType::ShuttleMouse) ||
                port.reportPos >= 22 || byte(port.mode) > 1 || byte(port.tl) > 1 || byte(port.mouseReset) > 1 ||
                byte(port.gunReload) > 1 || byte(port.analogMode) > 1 || byte(port.throttleLatched) > 1) {
                return false;
            }
        }
    }
    if (version >= 10) {
        // xorshift32 never reaches zero; the jitter values are 0 .. 3
        if (byte(ext.virtuaGun.valid) != 1 || ext.virtuaGun.jitterState == 0 || ext.virtuaGun.lastJitterX > 3 ||
            ext.virtuaGun.lastJitterY > 3) {
            return false;
        }
    }
    if (version >= 11) {
        // Bridge-written: false in states saved by the core alone
        if (byte(ext.settings.valid) > 1 || byte(ext.settings.sh2CacheEmulation) > 1 ||
            ext.settings.scspStepGranularity > 5) {
            return false;
        }
    }
    if (version >= 12) {
        // Saved by the core (SMPC::SaveBrimirState) in every state; SMEM is any four bytes
        if (byte(ext.smpcSettings.valid) != 1 || byte(ext.smpcSettings.STE) > 1) {
            return false;
        }
        // Saved by the core in every state too; the movement built up is bounded (AdvancePortInput), the buttons are
        // four bits. The report's movement is any value (ShuttleMouse clamps it when read).
        if (byte(ext.mouse.valid) != 1) {
            return false;
        }
        for (const auto& port : ext.mouse.ports) {
            if (port.accumX < -kMouseMovementLimit || port.accumX > kMouseMovementLimit ||
                port.accumY < -kMouseMovementLimit || port.accumY > kMouseMovementLimit || port.reportButtons > 0xF) {
                return false;
            }
        }
    }
    return ext.scsp.dspProgramLength <= 128;
}

} // namespace

size_t EncodedSaveStateSize(const SaveState& state) {
    Writer counter(nullptr);
    Visit(counter, state);
    return counter.Position();
}

size_t MaxEncodedSaveStateSize(size_t cartDataSize) {
    // Every field but the two vectors' contents.
    static const size_t fixedSize = [] {
        const auto empty = std::make_unique<SaveState>();
        return EncodedSaveStateSize(*empty);
    }();
    return fixedSize + cartDataSize + kMaxSaveStateINTBACKReportSize;
}

bool EncodeSaveState(const SaveState& state, std::vector<uint8_t>& out) {
    out.resize(EncodedSaveStateSize(state));
    Writer writer(out.data());
    Visit(writer, state);
    return writer.Ok();
}

void ResetSaveState(SaveState& state) {
    Clearer clearer;
    Visit(clearer, state);
}

bool DecodeSaveState(std::span<const uint8_t> payload, SaveState& state, uint32_t version) {
    Reader reader(payload);
    Visit(reader, state, version);
    if (version < 6) {
        std::memset(&state.brimir, 0, sizeof(state.brimir)); // valid = false: the state loads as before
    } else if (version == 6) {
        // Everything from scspSlots on (its first bytes may hold the version 6 padding): every `valid` false, and no
        // earlier state's internal backup RAM is left in the extension
        using ymir::savestate::BrimirSaveState;
        constexpr size_t from = offsetof(BrimirSaveState, scspSlots);
        static_assert(from <= ymir::savestate::kBrimirSaveStateV6Size);
        std::memset(reinterpret_cast<uint8_t*>(&state.brimir) + from, 0, sizeof(BrimirSaveState) - from);
    } else if (version < 9) {
        // Everything from peripherals on (its first bytes may hold the version 7/8 padding): valid false, so the
        // peripherals and the bridge keep their state, as these versions loaded
        using ymir::savestate::BrimirSaveState;
        constexpr size_t from = offsetof(BrimirSaveState, peripherals);
        static_assert(from <= ymir::savestate::kBrimirSaveStateV8Size);
        std::memset(reinterpret_cast<uint8_t*>(&state.brimir) + from, 0, sizeof(BrimirSaveState) - from);
    } else if (version == 9) {
        // virtuaGun: valid false, the jitter starts over from its seed (VDP::LoadState)
        using ymir::savestate::BrimirSaveState;
        constexpr size_t from = offsetof(BrimirSaveState, virtuaGun);
        static_assert(from <= ymir::savestate::kBrimirSaveStateV9Size);
        std::memset(reinterpret_cast<uint8_t*>(&state.brimir) + from, 0, sizeof(BrimirSaveState) - from);
    } else if (version == 10) {
        // settings: valid false (unknown, nothing to compare)
        using ymir::savestate::BrimirSaveState;
        constexpr size_t from = offsetof(BrimirSaveState, settings);
        static_assert(from == ymir::savestate::kBrimirSaveStateV10Size);
        std::memset(reinterpret_cast<uint8_t*>(&state.brimir) + from, 0, sizeof(BrimirSaveState) - from);
    } else if (version == 11) {
        // smpcSettings (read from the version 11 padding): valid false, the session keeps its BIOS settings; mouse
        // (after version 11's end): valid false, no movement built up
        using ymir::savestate::BrimirSaveState;
        constexpr size_t from = offsetof(BrimirSaveState, smpcSettings);
        static_assert(from <= ymir::savestate::kBrimirSaveStateV11Size);
        std::memset(reinterpret_cast<uint8_t*>(&state.brimir) + from, 0, sizeof(BrimirSaveState) - from);
    }
    if (!reader.Done()) {
        return false;
    }
    // Read as a raw byte: any value other than 0 or 1 is an invalid bool (Saturn::LoadState branches on it)
    uint8_t cdblockLLE = 0;
    std::memcpy(&cdblockLLE, &state.cdblockLLE, 1);
    if (cdblockLLE > 1) {
        return false;
    }
    if (!ValidCartDataSize(state.scu.cartType, state.scu.cartData.size(), version)) {
        return false;
    }
    // SMPC reads the report from this offset on (SMPC::INTBACKPeripheral).
    if (state.smpc.intback.reportOffset > state.smpc.intback.report.size()) {
        return false;
    }
    if (version >= 6 && !ValidBrimirExtension(state.brimir, version)) {
        return false;
    }
    return true;
}

void UpgradeVersion4SaveState(SaveState& state) {
    // Character::palNum is 7 bits wide: the old load path shifted the stored value left into it.
    for (auto& field : state.vdp.renderer.vramFetchers) {
        for (auto& fetcher : field) {
            fetcher.currChar.palNum = static_cast<uint8>((fetcher.currChar.palNum << 4u) & 0x7Fu);
            fetcher.nextChar.palNum = static_cast<uint8>((fetcher.nextChar.palNum << 4u) & 0x7Fu);
        }
    }
}

} // namespace brimir
