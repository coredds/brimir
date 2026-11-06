// Brimir - save-state payload codec
// Licensed under GPL-3.0

#include <brimir/savestate_codec.hpp>

#include <ymir/hw/cart/rom_cart_defs.hpp>
#include <ymir/hw/vdp/vdp1_defs.hpp>
#include <ymir/hw/vdp/vdp1_regs.hpp>

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
static_assert(sizeof(ymir::savestate::BrimirSaveState) == 819376,
              "BrimirSaveState changed: bump CoreWrapper::kSaveStateVersion and convert older states");

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

// version: payloads from version 6 on end with the Brimir extension (savestate_brimir.hpp); earlier ones lack it, and
// version 6 has only its first kBrimirSaveStateV6Size bytes (no scspSlots; read with Reader::RawPrefix).
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
    if (version >= 7) {
        ar.Raw(brimir);
    } else if (version == 6) {
        if constexpr (std::is_same_v<Archive, Reader>) {
            ar.RawPrefix(brimir, ymir::savestate::kBrimirSaveStateV6Size);
        }
    }
}

// The cartridge image size SCU::SaveState writes for each cartridge type, or SIZE_MAX for a value that
// is not a cartridge type. SCU::ValidateState checks only the 8 and 32 Mbit sizes, and SCU::LoadState
// copies a fixed size per type out of cartData, so every size is checked here.
size_t ExpectedCartDataSize(SCUSaveState::CartType type) {
    using CartType = SCUSaveState::CartType;
    switch (type) {
    case CartType::None: return 0;
    case CartType::BackupMemory: return 0;
    case CartType::DRAM8Mbit: return 1024 * 1024;
    case CartType::DRAM32Mbit: return 4 * 1024 * 1024;
    case CartType::DRAM48Mbit: return 6 * 1024 * 1024;
    case CartType::ROM: return ymir::cart::kROMCartSize;
    }
    return SIZE_MAX;
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
        // Before scspSlots (its first bytes may hold the version 6 padding): valid = false
        std::memset(&state.brimir.scspSlots, 0, sizeof(state.brimir.scspSlots));
    }
    if (!reader.Done()) {
        return false;
    }
    if (state.scu.cartData.size() != ExpectedCartDataSize(state.scu.cartType)) {
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
