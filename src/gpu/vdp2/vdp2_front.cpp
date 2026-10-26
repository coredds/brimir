#include "gpu/vdp2/vdp2_front.hpp"

#include "gpu/vdp2/vdp2_bindings.hpp"

#include <ymir/hw/vdp/vdp_common_defs.hpp>
#include <ymir/util/bit_ops.hpp>
#include <ymir/util/data_ops.hpp>

#include <algorithm>
#include <bit>
#include <cstring>

namespace brimir::gpu::vdp2 {

using namespace ymir;
using namespace ymir::vdp;

namespace {

// Ported from d3d:56-70 (PackBools).
template <std::unsigned_integral T, size_t N>
uint32_t PackBools(const std::array<bool, N> &bools) {
    T value = 0;
    const size_t count = std::min(N, sizeof(T) * 8);
    for (size_t i = 0; i < count; ++i) {
        if (bools[i]) {
            value |= static_cast<T>(1u) << static_cast<T>(i);
        }
    }
    return value;
}

template <typename T>
bool Differs(const T &lhs, const T &rhs) {
    return std::memcmp(&lhs, &rhs, sizeof(T)) != 0;
}

ColorR8G8B8A8 ToRGBA8(Color888 c) {
    return {static_cast<uint8_t>(c.r), static_cast<uint8_t>(c.g), static_cast<uint8_t>(c.b),
            static_cast<uint8_t>(c.msb)};
}

} // namespace

Vdp2FrontEnd::Vdp2FrontEnd(VDPState &state, const config::VDP2AccessPatternsConfig &accessPatternsConfig)
    : m_state(state)
    , m_accessPatternsConfig(accessPatternsConfig) {
    m_record.spriteSliceOffset.fill(UINT32_MAX);
    m_record.rotBases.fill({});
    m_record.lnclBack.fill(0);
    m_record.nbgLines.fill({});
    Reset(true);
}

// Ported from d3d:2057-2076 (Reset, VDP2 part).
void Vdp2FrontEnd::Reset(bool hard) {
    // Brimir: resolution and framebuffer as SoftwareVDPRenderer::Reset (sw:91-106), which fills its whole framebuffer
    // black at once. Inside a frame (VDP::Reset from an SMPC clock change), the lines rendered so far are recorded with
    // the old resolution before the clear; the lines the VDP renders again follow it (RenderLine).
    CloseRenderedLines();
    m_HRes = kDefaultResH;
    m_VRes = kDefaultResV;
    m_exclusiveMonitor = false;
    ClearComposite(0xFF000000u, 0);
    if (hard) {
        m_cramColorCache.fill({});
        // Brimir: matches software (sw:96-98)
        m_cram555Cache.fill({});
    }

    m_vramDirty.SetAll();
    MarkCRAMDirty();

    // Brimir: matches software (sw:109-112, VRAMFetcher::Reset), which resets the first two fetchers of each field.
    for (auto &fetchers : m_nbgFetchers) {
        for (uint32 i = 0; i < 2; ++i) {
            fetchers[i].currChar = {};
            fetchers[i].nextChar = {};
            fetchers[i].lastCharIndex = 0xFFFFFFFF;
            fetchers[i].lastCellX = 0xFF;
            fetchers[i].lastVCellScroll = 0xFFFFFFFF;
        }
    }

    CacheAllCRAMColors();
    UpdateEnabledLayers();
}

// Ported from d3d:4612-4625 (PostLoadStateSync, VDP2 part).
void Vdp2FrontEnd::PostLoadStateSync() {
    // Brimir: matches software (sw:234-236)
    for (uint32 address = 0; address < kVDP2CRAMSize; address += 2) {
        Cache555Color(address);
    }
    CacheAllCRAMColors();
    UpdateEnabledLayers();
    m_vramDirty.SetAll();
    MarkCRAMDirty();

    // Brimir: the loaded state2.rotParamStates are the last computed line's Xst, Yst and KA (the next line continues
    // from them, as software does); lines prepared with the display latch off before the next computed line use them
    // with the table address of the loaded registers.
    for (uint32 i = 0; i < 2; ++i) {
        m_rotTableAddress[i] = (m_state.regs2.commonRotParams.baseAddress & 0xFFF7C) + i * 0x80;
    }
    // Brimir: the layer images of the record before the load are gone; vertical mosaic restarts from drawn lines.
    for (auto &rows : m_nbgLastDrawnRow) {
        rows.fill(UINT32_MAX);
    }
}

// Brimir: matches software (sw:256-282, 288-314): the NBG fetchers in the renderer section of save states, in
// software's representation (save-state version 5: palette numbers unshifted, as software, f92da96).
void Vdp2FrontEnd::SaveFetchers(savestate::VDPSaveState::VDPRendererSaveState &state) const {
    auto copyChar = [](savestate::VDPSaveState::VDPRendererSaveState::CharacterSaveState &dst, const Character &src) {
        dst.charNum = src.charNum;
        dst.palNum = src.palNum;
        dst.specColorCalc = src.specColorCalc;
        dst.specPriority = src.specPriority;
        dst.flipH = src.flipH;
        dst.flipV = src.flipV;
    };
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < 4; j++) {
            const NBGFetcher &fetcher = m_nbgFetchers[i][j];
            auto &dst = state.vramFetchers[i][j];
            copyChar(dst.currChar, fetcher.currChar);
            copyChar(dst.nextChar, fetcher.nextChar);
            dst.lastCharIndex = fetcher.lastCharIndex;
            dst.lastCellX = fetcher.lastCellX;
            dst.lastVCellScroll = fetcher.lastVCellScroll;
        }
    }
}

void Vdp2FrontEnd::LoadFetchers(const savestate::VDPSaveState::VDPRendererSaveState &state) {
    auto copyChar = [](Character &dst, const savestate::VDPSaveState::VDPRendererSaveState::CharacterSaveState &src) {
        dst.charNum = src.charNum;
        dst.palNum = src.palNum;
        dst.specColorCalc = src.specColorCalc;
        dst.specPriority = src.specPriority;
        dst.flipH = src.flipH;
        dst.flipV = src.flipV;
    };
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j < 4; j++) {
            NBGFetcher &fetcher = m_nbgFetchers[i][j];
            const auto &src = state.vramFetchers[i][j];
            copyChar(fetcher.currChar, src.currChar);
            copyChar(fetcher.nextChar, src.nextChar);
            fetcher.lastCharIndex = src.lastCharIndex;
            fetcher.lastCellX = src.lastCellX;
            fetcher.lastVCellScroll = src.lastVCellScroll;
        }
    }
}

// Ported from d3d:2111-2117 (UpdateEnhancements, VDP2 part).
void Vdp2FrontEnd::SetEnhancements(const config::Enhancements &enhancements) {
    m_enhancements = enhancements;
    m_common.enhancements.deinterlace = enhancements.deinterlace;
    m_common.enhancements.transparentMeshes = enhancements.transparentMeshes;
}

void Vdp2FrontEnd::MarkAllDirty() {
    m_vramDirty.SetAll();
    MarkCRAMDirty();
}

// Brimir: every CRAM word is uploaded again at the next flush.
void Vdp2FrontEnd::MarkCRAMDirty() {
    ++m_cramGeneration;
    m_cramColorsDirty.SetAll();
    m_cramCoeffsDirty.SetAll();
}

// -----------------------------------------------------------------------------
// CRAM colour cache

// Brimir: stores colour cache entry `index` and marks it for upload when it changed.
void Vdp2FrontEnd::SetCachedColor(uint32_t index, const Color888 &color8) {
    const ColorR8G8B8A8 color = ToRGBA8(color8);
    ColorR8G8B8A8 &entry = m_cramColorCache[index & (kVDP2CRAMColorCacheEntries - 1)];
    if (Differs(entry, color)) {
        entry = color;
        m_cramColorsDirty.Set(index & (kVDP2CRAMColorCacheEntries - 1));
    }
}

// Ported from d3d:3447-3486 (VDP2CacheCRAMColor).
void Vdp2FrontEnd::CacheCRAMColor(uint32_t address) {
    switch (m_state.regs2.vramControl.colorRAMMode) {
    case 0: // RGB 5:5:5, half CRAM
    case 1: // RGB 5:5:5, full CRAM
    {
        const auto value = m_state.mem2.ReadCRAM<uint16>(address & ~1u);
        const Color555 color5{.u16 = value};
        SetCachedColor(address >> 1u, ConvertRGB555to888(color5));
        break;
    }
    case 2: [[fallthrough]]; // RGB 8:8:8, full CRAM
    case 3: [[fallthrough]]; // RGB 8:8:8, full CRAM
    default: {
        address = m_state.mem2.UnmapCRAMAddress<uint8>(address);
        const auto value = m_state.mem2.ReadCRAM<uint32>(address & ~3u);
        SetCachedColor(address >> 2u, Color888{.u32 = value});
        break;
    }
    }
}

// Brimir: matches software (sw:905-914, VDP2UpdateCRAMCache for one 16-bit or 8-bit write).
void Vdp2FrontEnd::Cache555Color(uint32_t address) {
    address &= (kVDP2CRAMSize - 1) & ~1u;
    const Color555 color5{.u16 = util::ReadBE<uint16>(&m_state.mem2.CRAM[address])}; // raw CRAM, as software
    const Color888 color8 = ConvertRGB555to888(color5);
    Color888 &entry = m_cram555Cache[address / sizeof(uint16)];
    if (entry.u32 != color8.u32) {
        entry = color8;
        m_cramColorsDirty.Set(kVDP2CRAM555TableBase + address / sizeof(uint16));
    }
}

// Ported from d3d:3488-3526 (VDP2CacheAllCRAMColors).
void Vdp2FrontEnd::CacheAllCRAMColors() {
    switch (m_state.regs2.vramControl.colorRAMMode) {
    case 0: // RGB 5:5:5, half CRAM
    case 1: // RGB 5:5:5, full CRAM
    {
        const uint32 count = m_state.regs2.vramControl.colorRAMMode == 0 ? 1024 : 2048;
        for (uint32 i = 0; i < count; ++i) {
            const auto value = m_state.mem2.ReadCRAM<uint16>(i * sizeof(uint16));
            const Color555 color5{.u16 = value};
            SetCachedColor(i, ConvertRGB555to888(color5));
        }
        break;
    }
    case 2: [[fallthrough]]; // RGB 8:8:8, full CRAM
    case 3: [[fallthrough]]; // RGB 8:8:8, full CRAM
    default:
        for (uint32 i = 0; i < 1024; ++i) {
            const auto value = m_state.mem2.ReadCRAM<uint32>(i * sizeof(uint32));
            SetCachedColor(i, Color888{.u32 = value});
        }
        break;
    }
}

// -----------------------------------------------------------------------------
// Memory and register writes

// Ported from d3d:3528-3530 (VDP2WriteVRAM).
void Vdp2FrontEnd::WriteVRAM(uint32_t address) {
    m_vramDirty.Set((address & (kVDP2VRAMSize - 1)) >> kVRAMChunkShift);
}

// Ported from d3d:3532-3535 (VDP2WriteCRAM).
void Vdp2FrontEnd::WriteCRAM(uint32_t address) {
    ++m_cramGeneration;
    CacheCRAMColor(address);
    Cache555Color(address); // Brimir: matches software (sw:407-408)
    // Brimir: the raw word in the coefficient half (the upper 2 KiB)
    address &= kVDP2CRAMSize - 1;
    if (address >= kVDP2CRAMSize / 2) {
        m_cramCoeffsDirty.Set((address - kVDP2CRAMSize / 2) / sizeof(uint32_t));
    }
}

// Ported from d3d:3537-3623 (VDP2WriteReg).
void Vdp2FrontEnd::WriteReg(uint32_t address, uint16_t value) {
    // Brimir: Ymir's table also flags the render- and compose-dirty registers to bump parameter generations. Here the
    // parameters are rebuilt and compared every line, which also catches changes that come without a register write
    // (TVSTAT.ODD, VDP1 TVMR/FBCR, the display framebuffer, derived state), so only these columns are kept.
    struct DirtyFlags {
        bool enabledLayers = false;
        bool cram = false;
    };
    static constexpr auto kDirtyFlags = [] {
        std::array<DirtyFlags, 0x11E / sizeof(uint16) + 1> arr{};
        for (uint32 addr : {0x020 /*BGON*/, 0x028 /*CHCTLA*/, 0x02A /*CHCTLB*/}) {
            arr[addr / sizeof(uint16)].enabledLayers = true;
        }
        arr[0x00E / sizeof(uint16) /*RAMCTL*/].cram = true;
        return arr;
    }();
    (void)value;

    if (address <= 0x11E) {
        const auto &dirtyFlags = kDirtyFlags[address / sizeof(uint16)];
        if (dirtyFlags.enabledLayers) {
            UpdateEnabledLayers();
        }
        if (dirtyFlags.cram) {
            ++m_cramGeneration;
            CacheAllCRAMColors();
        }

        // Brimir: matches software (sw:430-438): the vertical scroll counter restarts at the written scroll value.
        switch (address) {
        case 0x092: // SCYN2
            m_state.state2.nbgLayerStates[2].fracScrollY = 0;
            break;
        case 0x096: // SCYN3
            m_state.state2.nbgLayerStates[3].fracScrollY = 0;
            break;
        }
    }
}

// -----------------------------------------------------------------------------
// Derived state

// Ported from d3d:4118-4120 (VDP2UpdateEnabledLayers).
void Vdp2FrontEnd::UpdateEnabledLayers() {
    m_state.state2.UpdateEnabledBGs(m_state.regs2, m_debugRenderOptions);
}

// Ported from d3d:4093-4098 (VDP2CalcAccessPatterns); the generation bump is replaced by the parameter memcmp.
void Vdp2FrontEnd::CalcAccessPatterns() {
    m_state.state2.CalcAccessPatterns(m_state.regs2, m_accessPatternsConfig);
}

// Ported from d3d:4122-4127 (VDP2CalcVCellScrollDelay); the generation bump is replaced by the parameter memcmp.
void Vdp2FrontEnd::CalcVCellScrollDelay() {
    m_state.state2.CalcVCellScrollDelay(m_state.regs2);
}

// Brimir: matches software (sw:2070-2078, VDP2InitFrame), replacing Ymir's VDP2InitNBGs (d3d:4100-4116).
void Vdp2FrontEnd::InitFrame() {
    const VDP2Regs &regs2 = m_state.regs2;
    if (!regs2.bgEnabled[5]) {
        InitNormalBG(0);
    }
    InitNormalBG(1);
    InitNormalBG(2);
    InitNormalBG(3);
}

// Brimir: matches software (sw:2081-2097, VDP2InitNormalBG).
void Vdp2FrontEnd::InitNormalBG(uint32_t index) {
    const VDP2Regs &regs2 = m_state.regs2;
    const BGParams &bgParams = regs2.bgParams[index + 1];
    NBGLayerState &bgState = m_state.state2.nbgLayerStates[index];
    bgState.fracScrollX = 0;
    bgState.fracScrollY = 0;
    if (!m_enhancements.deinterlace && regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity && regs2.TVSTAT.ODD) {
        bgState.fracScrollY += bgParams.scrollIncV;
    }

    bgState.mosaicCounterY = 0;
    if (index < 2) {
        bgState.lineScrollTableAddress = bgParams.lineScrollTableAddress;
    }
}

// Brimir: matches software (sw:2111-2143, VDP2UpdateLineScreenScroll).
void Vdp2FrontEnd::UpdateLineScreenScroll(uint32_t y, const BGParams &bgParams, NBGLayerState &bgState) {
    const VDP2Regs &regs2 = m_state.regs2;
    bgState.scrollIncH = bgParams.scrollIncH;

    if ((y & ((1u << bgParams.lineScrollInterval) - 1)) != 0) {
        return;
    }

    uint32 address = bgState.lineScrollTableAddress;
    auto read = [&] {
        const uint32 value = m_state.mem2.ReadVRAM<uint32>(address);
        address += sizeof(uint32);
        return value;
    };

    size_t count = 1;
    if (regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity &&
        (y > 0 || (!m_enhancements.deinterlace && regs2.TVSTAT.ODD))) {
        ++count;
    }
    for (size_t i = 0; i < count; ++i) {
        if (bgParams.lineScrollXEnable) {
            bgState.fracScrollX = bit::extract<8, 26>(read());
        }
        if (bgParams.lineScrollYEnable) {
            bgState.fracScrollY = bit::extract<8, 26>(read());
        }
        if (bgParams.lineZoomEnable) {
            bgState.scrollIncH = bit::extract<8, 18>(read());
        }
    }
    bgState.lineScrollTableAddress = address;
}

// Brimir: matches software (sw:2542-2565, VDP2FinishLine).
void Vdp2FrontEnd::FinishLine() {
    const VDP2Regs &regs2 = m_state.regs2;
    const bool doubleDensity = regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity;

    // Update NBG coordinates
    for (uint32 i = 0; i < 4; ++i) {
        const BGParams &bgParams = regs2.bgParams[i + 1];
        NBGLayerState &bgState = m_state.state2.nbgLayerStates[i];
        bgState.fracScrollY += bgParams.scrollIncV;
        // Update the vertical scroll coordinate twice in double-density interlaced mode.
        // If deinterlacing, the second increment is done after rendering the alternate scanline.
        if (doubleDensity) {
            bgState.fracScrollY += bgParams.scrollIncV;
        }

        // Increment mosaic counter
        if (bgParams.mosaicEnable) {
            ++bgState.mosaicCounterY;
            if (bgState.mosaicCounterY >= regs2.mosaicV) {
                bgState.mosaicCounterY = 0;
            }
        }
    }
}

// Ported from d3d:4129-4154 (VDP2DrawLineColorBackScreens).
// Brimir: matches software (sw:2645-2664): the colours are computed into state2.lineBackLayerState, LNCL from the
// RGB555 colour cache of CRAM word (value & 0x7FF) in every CRAM mode. PrepareLine calls this only while the display
// latch is on (sw:2518-2520); every line stores the state as it is then, so lines prepared with the latch off repeat
// the last computed colours, as software composes them.
void Vdp2FrontEnd::DrawLineColorBackScreens(uint32_t y) {
    const VDP2Regs &regs = m_state.regs2;
    LineBackLayerState &state = m_state.state2.lineBackLayerState;

    // Read line color screen color
    const LineBackScreenParams &lineParams = regs.lineScreenParams;
    uint32 lineAddress = lineParams.baseAddress;
    if (lineParams.perLine) {
        lineAddress += y * sizeof(uint16);
    }
    const uint32 cramAddress = m_state.mem2.ReadVRAM<uint16>(lineAddress) * sizeof(uint16);
    state.lineColor = m_cram555Cache[(cramAddress / sizeof(uint16)) & 0x7FF];

    // Read back screen color
    const LineBackScreenParams &backParams = regs.backScreenParams;
    uint32 backAddress = backParams.baseAddress;
    if (backParams.perLine) {
        backAddress += y * sizeof(Color555);
    }
    const Color555 color555{.u16 = m_state.mem2.ReadVRAM<uint16>(backAddress)};
    state.backColor = ConvertRGB555to888(color555);
}

void Vdp2FrontEnd::StoreLineColorBackScreens(uint32_t y) {
    const LineBackLayerState &state = m_state.state2.lineBackLayerState;
    m_lnclBack[y] = ToRGBA8(state.lineColor);
    m_lnclBack[kMaxResV + y] = ToRGBA8(state.backColor);
}

// Ported from d3d:4180-4225 (VDP2UpdateRotationParameterBases). Brimir: matches software (sw:2145-2184,
// VDP2CalcRotationParameterTables): Xst, Yst and KA accumulate in state2.rotParamStates, as software maintains them, so
// lines skipped with the display latch off or with no RBG enabled continue where software does, and save states carry
// them; kMaxResV rows per parameter.
void Vdp2FrontEnd::UpdateRotationParameterBases(uint32_t y) {
    VDP2Regs &regs2 = m_state.regs2;
    if (!regs2.bgEnabled[4] && !regs2.bgEnabled[5]) {
        // Skip if no RBGs are enabled
        return;
    }

    const bool readAll = y == 0;

    const uint32 baseAddress = regs2.commonRotParams.baseAddress & 0xFFF7C; // mask bit 6 (shifted left by 1)
    for (uint32 i = 0; i < 2; ++i) {
        RotationParamState &state = m_state.state2.rotParamStates[i];
        RotationParams &src = regs2.rotParams[i];

        // Brimir: software reads the table from (address & 0x7FFFF) (sw:2163); ReadVRAM masks the same way.
        const uint32 address = baseAddress + i * 0x80;

        m_rotTableAddress[i] = address;

        if (readAll || src.readXst) {
            state.Xst = bit::extract_signed<6, 28, sint32>(m_state.mem2.ReadVRAM<uint32>(address + 0x00));
            src.readXst = false;
        } else {
            state.Xst += bit::extract_signed<6, 18, sint32>(m_state.mem2.ReadVRAM<uint32>(address + 0x0C));
        }

        if (readAll || src.readYst) {
            state.Yst = bit::extract_signed<6, 28, sint32>(m_state.mem2.ReadVRAM<uint32>(address + 0x04));
            src.readYst = false;
        } else {
            state.Yst += bit::extract_signed<6, 18, sint32>(m_state.mem2.ReadVRAM<uint32>(address + 0x10));
        }

        if (readAll || src.readKAst) {
            const uint32 KAst = bit::extract<6, 31>(m_state.mem2.ReadVRAM<uint32>(address + 0x54));
            state.KA = src.coeffTableAddressOffset + KAst;
            src.readKAst = false;
        } else {
            state.KA += bit::extract_signed<6, 25>(m_state.mem2.ReadVRAM<uint32>(address + 0x58));
        }
    }
}

// -----------------------------------------------------------------------------
// Parameter packers

// Ported from d3d:3715-3833 (VDP2UpdateCommonRenderParams).
void Vdp2FrontEnd::UpdateCommonRenderParams() {
    // m_common.startY and outputSize stay 0: the back half sets them per dispatch.

    const VDP1Regs &regs1 = m_state.regs1;
    const VDP2Regs &regs2 = m_state.regs2;
    VDP2CommonRenderParams &params = m_common;

    params.displayParams.displayEnable = regs2.TVMD.DISP;
    params.displayParams.borderColorMode = regs2.TVMD.BDCLMD;
    params.displayParams.interlaceMode = static_cast<HLSLuint>(regs2.TVMD.LSMDn);
    params.displayParams.oddField = regs2.TVSTAT.ODD;
    params.displayParams.exclusiveMonitor = m_exclusiveMonitor;
    params.displayParams.colorRAMMode = regs2.vramControl.colorRAMMode;
    params.displayParams.hiResH = bit::test<1>(regs2.TVMD.HRESOn);
    params.displayParams.palMode = regs2.TVSTAT.PAL;
    params.displayParams.hresMode = regs2.TVMD.HRESOn;
    params.displayParams.vresMode = regs2.TVMD.VRESOn;
    params.displayParams.dblInterlaceEnable = regs1.dblInterlaceEnable;
    params.displayParams.dblInterlaceDrawLine = regs1.dblInterlaceDrawLine;
    // Brimir: firstInterlacedFrame is not ported (resolution changes clear the composite instead); it stays 0.

    params.layerParams.layerEnabled = PackBools<HLSLuint>(m_state.state2.layerEnabled);
    params.layerParams.bgEnabled = PackBools<HLSLuint>(regs2.bgEnabled);
    params.layerParams.lineColorEnableRBG0 = regs2.bgParams[0].lineColorScreenEnable;
    params.layerParams.lineColorEnableRBG1 = regs2.bgParams[1].lineColorScreenEnable;
    params.layerParams.mosaicH = regs2.mosaicH - 1;
    params.layerParams.mosaicV = regs2.mosaicV - 1;
    params.layerParams.rotParamMode = static_cast<HLSLuint>(regs2.commonRotParams.rotParamMode);
    params.layerParams.restrictedColorCalc = regs2.restrictedColorCalc;
    params.layerParams.extendedColorCalc = regs2.colorCalcParams.extendedColorCalcEnable && regs2.TVMD.HRESOn < 2;
    params.layerParams.useAdditiveBlend = regs2.colorCalcParams.useAdditiveBlend;
    params.layerParams.useSecondScreenRatio = regs2.colorCalcParams.useSecondScreenRatio;
    params.layerParams.colorGradEnable = regs2.colorCalcParams.colorGradEnable;
    params.layerParams.colorGradScreen = static_cast<HLSLuint>(regs2.colorCalcParams.colorGradScreen);

    auto isCoeff = [](RotDataBankSel sel) { return sel == RotDataBankSel::Coefficients; };

    const VRAMControl &vramCtl = regs2.vramControl;
    params.rotParams.coeffTableCRAM = vramCtl.colorRAMCoeffTableEnable;
    params.rotParams.coeffDataAccess = 0;
    if (isCoeff(vramCtl.rotDataBankSelA0)) {
        params.rotParams.coeffDataAccess |= 1u << 0u;
    }
    if (isCoeff(vramCtl.partitionVRAMA ? vramCtl.rotDataBankSelA1 : vramCtl.rotDataBankSelA0)) {
        params.rotParams.coeffDataAccess |= 1u << 1u;
    }
    if (isCoeff(vramCtl.rotDataBankSelB0)) {
        params.rotParams.coeffDataAccess |= 1u << 2u;
    }
    if (isCoeff(vramCtl.partitionVRAMB ? vramCtl.rotDataBankSelB1 : vramCtl.rotDataBankSelB0)) {
        params.rotParams.coeffDataAccess |= 1u << 3u;
    }
    params.rotParams.coeffDataPerDot = vramCtl.perDotRotationCoeffs;

    const RotationParams &rotParamsA = regs2.rotParams[0];
    params.rotParams.coeffATableEnable = rotParamsA.coeffTableEnable;
    params.rotParams.coeffAUseLineColorData = rotParamsA.coeffUseLineColorData;
    params.rotParams.coeffADataSize = rotParamsA.coeffDataSize;
    params.rotParams.coeffADataMode = static_cast<HLSLuint>(rotParamsA.coeffDataMode);

    const RotationParams &rotParamsB = regs2.rotParams[1];
    params.rotParams.coeffBTableEnable = rotParamsB.coeffTableEnable;
    params.rotParams.coeffBUseLineColorData = rotParamsB.coeffUseLineColorData;
    params.rotParams.coeffBDataSize = rotParamsB.coeffDataSize;
    params.rotParams.coeffBDataMode = static_cast<HLSLuint>(rotParamsB.coeffDataMode);

    params.spriteParams.rotate = regs1.tvmr.fbRotEnable;
    params.spriteParams.pixel8Bits = regs1.tvmr.pixel8Bits;
    params.spriteParams.type = regs2.spriteParams.type;
    params.spriteParams.fbSizeH = std::countr_zero(regs1.tvmr.fbSizeH) - 9;
    params.spriteParams.fbSizeV = std::countr_zero(regs1.tvmr.fbSizeV) - 8;
    params.spriteParams.inHalfResH = false;
    params.spriteParams.outHalfResH = false;
    // Brimir: matches software (sw:2672-2677): the output is doubled in hi-res modes with rotation too (Ymir clears
    // both with rotation); the sprite shader uses inHalfResH only without rotation.
    if (!regs1.tvmr.hdtvEnable) {
        if (regs1.tvmr.pixel8Bits) {
            params.spriteParams.inHalfResH = !regs1.tvmr.fbRotEnable && (regs2.TVMD.HRESOn & 0b110) == 0b000;
        } else {
            params.spriteParams.outHalfResH = (regs2.TVMD.HRESOn & 0b110) == 0b010;
        }
    }
    params.spriteParams.mixedFormat = regs2.spriteParams.mixedFormat;
    params.spriteParams.colorCalcEnable = regs2.spriteParams.colorCalcEnable;
    params.spriteParams.colorCalcValue = regs2.spriteParams.colorCalcValue;
    params.spriteParams.colorCalcCond = static_cast<HLSLuint>(regs2.spriteParams.colorCalcCond);
    params.spriteParams.colorDataOffset = regs2.spriteParams.colorDataOffset >> 8u;
    params.spriteParams.useSpriteWindow = regs2.spriteParams.useSpriteWindow;
    params.spriteParams.windowEnabled = regs2.spriteParams.spriteWindowEnabled;
    params.spriteParams.windowInverted = regs2.spriteParams.spriteWindowInverted;
    params.spriteParams.displayFB = m_state.fbIndex.display;

    // Brimir: the latched border colour mode, for display-off lines.
    params.borderColorModeLatch = regs2.borderColorModeLatch;

    params.spritePriosRatios.x = 0;
    params.spritePriosRatios.y = 0;
    for (uint32 i = 0; i < 4; i++) {
        params.spritePriosRatios.x |= regs2.spriteParams.priorities[i] << (8 * i);
        params.spritePriosRatios.x |= regs2.spriteParams.colorCalcRatios[i] << (8 * i + 3);

        params.spritePriosRatios.y |= regs2.spriteParams.priorities[i + 4] << (8 * i);
        params.spritePriosRatios.y |= regs2.spriteParams.colorCalcRatios[i + 4] << (8 * i + 3);
    }

    params.vcellScroll.tableAddress = regs2.vcellScrollTableAddress;
    params.vcellScroll.inc = regs2.vcellScrollInc >> 2u;

    params.windows.spriteWindowLogic = regs2.spriteParams.windowSet.logic == WindowLogic::And;
    params.windows.spriteW0Enable = regs2.spriteParams.windowSet.enabled[0];
    params.windows.spriteW0Invert = regs2.spriteParams.windowSet.inverted[0];
    params.windows.spriteW1Enable = regs2.spriteParams.windowSet.enabled[1];
    params.windows.spriteW1Invert = regs2.spriteParams.windowSet.inverted[1];

    params.windows.colorCalcWindowLogic = regs2.colorCalcParams.windowSet.logic == WindowLogic::And;
    params.windows.colorCalcW0Enable = regs2.colorCalcParams.windowSet.enabled[0];
    params.windows.colorCalcW0Invert = regs2.colorCalcParams.windowSet.inverted[0];
    params.windows.colorCalcW1Enable = regs2.colorCalcParams.windowSet.enabled[1];
    params.windows.colorCalcW1Invert = regs2.colorCalcParams.windowSet.inverted[1];
    params.windows.colorCalcSWEnable = regs2.colorCalcParams.windowSet.enabled[2];
    params.windows.colorCalcSWInvert = regs2.colorCalcParams.windowSet.inverted[2];
}

// Ported from d3d:3835-3993 (VDP2UpdateLayerRenderParams, body without the generation gate and the upload tail).
void Vdp2FrontEnd::UpdateLayerRenderParams() {
    const VDP2Regs &regs2 = m_state.regs2;

    // These can be either 0 or 8, but we'll condense them to single bits
    auto packVRAMDataOffsets = [](const std::array<uint32, 4> values) {
        uint32 value = 0;
        for (size_t i = 0; i < values.size(); ++i) {
            if (values[i] != 0u) {
                value |= 1u << i;
            }
        }
        return value;
    };

    // NBG0-3
    for (int i = 0; i < 4; ++i) {
        const BGParams &bgParams = regs2.bgParams[i + 1];
        const NBGLayerState &bgState = m_state.state2.nbgLayerStates[i];

        const bool bitmap = bgParams.bitmap;

        NBGParams &renderParams = m_layer.nbg[i];
        renderParams.base.enabled = regs2.bgEnabled[i];
        renderParams.base.enableTransparency = bgParams.enableTransparency;
        renderParams.base.bitmap = bgParams.bitmap;
        renderParams.base.priorityNumber = bgParams.priorityNumber;
        renderParams.base.priorityMode = static_cast<HLSLuint>(bgParams.priorityMode);
        renderParams.base.specialFunctionSelect = bgParams.specialFunctionSelect;
        renderParams.base.cellSizeShift = bgParams.cellSizeShift;
        renderParams.base.colorFormat = static_cast<HLSLuint>(bgParams.colorFormat);
        renderParams.base.cramOffset = bgParams.cramOffset;
        renderParams.base.supplScrollCharNum = bgParams.supplScrollCharNum;
        renderParams.base.supplPalNum = bitmap ? bgParams.supplBitmapPalNum : bgParams.supplScrollPalNum;
        renderParams.base.supplSpecialColorCalc =
            bitmap ? bgParams.supplBitmapSpecialColorCalc : bgParams.supplScrollSpecialColorCalc;
        renderParams.base.supplSpecialPriority =
            bitmap ? bgParams.supplBitmapSpecialPriority : bgParams.supplScrollSpecialPriority;
        renderParams.base.mosaicEnable = bgParams.mosaicEnable;
        renderParams.base.colorCalcEnable = bgParams.colorCalcEnable;
        renderParams.base.extChar = bgParams.extChar;
        renderParams.base.twoWordChar = bgParams.twoWordChar;
        renderParams.base.patNameAccess = PackBools<HLSLuint>(bgParams.patNameAccess);
        renderParams.base.charPatAccess = PackBools<HLSLuint>(bgParams.charPatAccess);
        renderParams.base.charPatDelay = PackBools<HLSLuint>(bgParams.charPatDelay);
        renderParams.base.vramDataOffset = packVRAMDataOffsets(bgParams.vramDataOffset);
        renderParams.base.specialColorCalcMode = static_cast<HLSLuint>(bgParams.specialColorCalcMode);
        renderParams.base.pageShift = {bgParams.pageShiftH, bgParams.pageShiftV};
        renderParams.base.bitmapSize = {bgParams.bitmapSizeH, bgParams.bitmapSizeV};
        renderParams.base.bitmapBaseAddress = bgParams.bitmapBaseAddress;
        renderParams.base.windowParams.base.windowLogicAnd = bgParams.windowSet.logic == WindowLogic::And;
        renderParams.base.windowParams.base.window0Enable = bgParams.windowSet.enabled[0];
        renderParams.base.windowParams.base.window0Invert = bgParams.windowSet.inverted[0];
        renderParams.base.windowParams.base.window1Enable = bgParams.windowSet.enabled[1];
        renderParams.base.windowParams.base.window1Invert = bgParams.windowSet.inverted[1];
        renderParams.base.windowParams.spriteWindowEnable = bgParams.windowSet.enabled[2];
        renderParams.base.windowParams.spriteWindowInvert = bgParams.windowSet.inverted[2];

        renderParams.scrollAmount = {bgParams.scrollAmountH, bgParams.scrollAmountV};
        renderParams.scrollInc = {bgParams.scrollIncH, bgParams.scrollIncV};
        renderParams.pageBaseAddresses = bgParams.pageBaseAddresses;
        renderParams.vcellScrollEnable = bgParams.vcellScrollEnable;
        renderParams.lineScrollXEnable = bgParams.lineScrollXEnable;
        renderParams.lineScrollYEnable = bgParams.lineScrollYEnable;
        renderParams.lineZoomEnable = bgParams.lineZoomEnable;
        renderParams.lineScrollInterval = bgParams.lineScrollInterval;
        // Brimir: the line scroll table address and the vertical scroll base are per-line state (VDP2NBGLine, as
        // software's NBGLayerState); left 0 here so that they do not split batches.
        renderParams.lineScrollTableAddress = 0;
        renderParams.vcellScrollOffset = bgState.vcellScrollOffset;
        renderParams.vcellScrollDelay = bgState.vcellScrollDelay;
        renderParams.vcellScrollRepeat = bgState.vcellScrollRepeat;
        renderParams.scrollYIncBase = 0;
    }

    // RBG0-1 / RotParam A-B
    for (int i = 0; i < 2; ++i) {
        const BGParams &bgParams = regs2.bgParams[i];
        const RotationParams &rotParams = regs2.rotParams[i];

        const bool bitmap = bgParams.bitmap;

        RBGParams &renderParams = m_layer.rbg[i];
        renderParams.base.enabled = regs2.bgEnabled[i + 4];
        renderParams.base.enableTransparency = bgParams.enableTransparency;
        renderParams.base.bitmap = bgParams.bitmap;
        renderParams.base.priorityNumber = bgParams.priorityNumber;
        renderParams.base.priorityMode = static_cast<HLSLuint>(bgParams.priorityMode);
        renderParams.base.specialFunctionSelect = bgParams.specialFunctionSelect;
        renderParams.base.cellSizeShift = bgParams.cellSizeShift;
        renderParams.base.colorFormat = static_cast<HLSLuint>(bgParams.colorFormat);
        renderParams.base.cramOffset = bgParams.cramOffset;
        renderParams.base.supplScrollCharNum = bgParams.supplScrollCharNum;
        renderParams.base.supplPalNum = bitmap ? bgParams.supplBitmapPalNum : bgParams.supplScrollPalNum;
        renderParams.base.supplSpecialColorCalc =
            bitmap ? bgParams.supplBitmapSpecialColorCalc : bgParams.supplScrollSpecialColorCalc;
        renderParams.base.supplSpecialPriority =
            bitmap ? bgParams.supplBitmapSpecialPriority : bgParams.supplScrollSpecialPriority;
        renderParams.base.mosaicEnable = bgParams.mosaicEnable;
        renderParams.base.colorCalcEnable = bgParams.colorCalcEnable;
        renderParams.base.extChar = bgParams.extChar;
        renderParams.base.twoWordChar = bgParams.twoWordChar;
        renderParams.base.patNameAccess = PackBools<HLSLuint>(bgParams.patNameAccess);
        renderParams.base.charPatAccess = PackBools<HLSLuint>(bgParams.charPatAccess);
        renderParams.base.charPatDelay = PackBools<HLSLuint>(bgParams.charPatDelay);
        renderParams.base.vramDataOffset = packVRAMDataOffsets(bgParams.vramDataOffset);
        renderParams.base.specialColorCalcMode = static_cast<HLSLuint>(bgParams.specialColorCalcMode);
        renderParams.base.pageShift = {rotParams.pageShiftH, rotParams.pageShiftV};
        renderParams.base.bitmapSize = {bgParams.bitmapSizeH, bgParams.bitmapSizeV};
        renderParams.base.bitmapBaseAddress = rotParams.bitmapBaseAddress;
        renderParams.base.windowParams.base.windowLogicAnd = bgParams.windowSet.logic == WindowLogic::And;
        renderParams.base.windowParams.base.window0Enable = bgParams.windowSet.enabled[0];
        renderParams.base.windowParams.base.window0Invert = bgParams.windowSet.inverted[0];
        renderParams.base.windowParams.base.window1Enable = bgParams.windowSet.enabled[1];
        renderParams.base.windowParams.base.window1Invert = bgParams.windowSet.inverted[1];
        renderParams.base.windowParams.spriteWindowEnable = bgParams.windowSet.enabled[2];
        renderParams.base.windowParams.spriteWindowInvert = bgParams.windowSet.inverted[2];

        renderParams.screenOverProcess = static_cast<HLSLuint>(rotParams.screenOverProcess);
        renderParams.screenOverPatternName = rotParams.screenOverPatternName;
        renderParams.pageBaseAddresses[0] = m_state.state2.rbgPageBaseAddresses[0][i];
        renderParams.pageBaseAddresses[1] = m_state.state2.rbgPageBaseAddresses[1][i];
    }

    // Windows 0 and 1
    for (int i = 0; i < 2; ++i) {
        const WindowParams &windowParams = regs2.windowParams[i];
        VDP2GlobalWindowParams &renderParams = m_layer.windows[i];
        renderParams.start = {windowParams.startX, windowParams.startY};
        renderParams.end = {windowParams.endX, windowParams.endY};
        renderParams.lineWindowTableAddress = windowParams.lineWindowTableAddress;
        renderParams.lineWindowTableEnable = windowParams.lineWindowTableEnable;
    }

    VDP2LayerWindowParams &rotWindows = m_layer.rotWindows;
    rotWindows.windowLogicAnd = regs2.commonRotParams.windowSet.logic == WindowLogic::And;
    rotWindows.window0Enable = regs2.commonRotParams.windowSet.enabled[0];
    rotWindows.window0Invert = regs2.commonRotParams.windowSet.inverted[0];
    rotWindows.window1Enable = regs2.commonRotParams.windowSet.enabled[1];
    rotWindows.window1Invert = regs2.commonRotParams.windowSet.inverted[1];

    VDP2LineBackScreenParams &lnclParams = m_layer.lineScreenParams;
    lnclParams.baseAddress = regs2.lineScreenParams.baseAddress;
    lnclParams.perLine = regs2.lineScreenParams.perLine;

    VDP2LineBackScreenParams &backParams = m_layer.backScreenParams;
    backParams.baseAddress = regs2.backScreenParams.baseAddress;
    backParams.perLine = regs2.backScreenParams.perLine;

    // Special function codes
    m_layer.specialFunctionCodes = PackBools<uint32>(regs2.specialFunctionCodes[0].colorMatches) |
                                   (PackBools<uint32>(regs2.specialFunctionCodes[1].colorMatches) << 8u);
}

// Ported from d3d:4021-4064 (VDP2UpdateComposeParams, body without the generation gate and the upload tail).
void Vdp2FrontEnd::UpdateComposeParams() {
    const VDP2Regs &regs2 = m_state.regs2;

    auto &params = m_compose;
    params.colorCalcEnable = 0                                               //
                             | (regs2.spriteParams.colorCalcEnable << 0)     //
                             | (regs2.bgParams[0].colorCalcEnable << 1)      //
                             | (regs2.bgParams[1].colorCalcEnable << 2)      //
                             | (regs2.bgParams[2].colorCalcEnable << 3)      //
                             | (regs2.bgParams[3].colorCalcEnable << 4)      //
                             | (regs2.bgParams[4].colorCalcEnable << 5)      //
                             | (regs2.backScreenParams.colorCalcEnable << 6) //
                             | (regs2.lineScreenParams.colorCalcEnable << 7) //
        ;
    params.colorOffsetEnable = PackBools<HLSLuint>(regs2.colorOffsetEnable);
    params.colorOffsetSelect = PackBools<HLSLuint>(regs2.colorOffsetSelect);
    params.lineColorEnable = 0                                                 //
                             | (regs2.spriteParams.lineColorScreenEnable << 0) //
                             | (regs2.bgParams[0].lineColorScreenEnable << 1)  //
                             | (regs2.bgParams[1].lineColorScreenEnable << 2)  //
                             | (regs2.bgParams[2].lineColorScreenEnable << 3)  //
                             | (regs2.bgParams[3].lineColorScreenEnable << 4)  //
                             | (regs2.bgParams[4].lineColorScreenEnable << 5)  //
        ;

    params.colorOffsetA.r = bit::sign_extend<9>(regs2.colorOffset[0].r);
    params.colorOffsetA.g = bit::sign_extend<9>(regs2.colorOffset[0].g);
    params.colorOffsetA.b = bit::sign_extend<9>(regs2.colorOffset[0].b);

    params.colorOffsetB.r = bit::sign_extend<9>(regs2.colorOffset[1].r);
    params.colorOffsetB.g = bit::sign_extend<9>(regs2.colorOffset[1].g);
    params.colorOffsetB.b = bit::sign_extend<9>(regs2.colorOffset[1].b);

    for (int i = 0; i < 5; ++i) {
        params.bgColorCalcRatios[i] = regs2.bgParams[i].colorCalcRatio;
    }
    params.backLineColorCalcRatios[0] = regs2.backScreenParams.colorCalcRatio;
    params.backLineColorCalcRatios[1] = regs2.lineScreenParams.colorCalcRatio;

    // Brimir: per-layer shadow enable from SDCTL, indexed like colorCalcEnable (bit 0, the sprite, uses its own
    // shadow bit; bit 6 is the back screen).
    params.shadowEnable = 0                                            //
                          | (regs2.bgParams[0].shadowEnable << 1)      //
                          | (regs2.bgParams[1].shadowEnable << 2)      //
                          | (regs2.bgParams[2].shadowEnable << 3)      //
                          | (regs2.bgParams[3].shadowEnable << 4)      //
                          | (regs2.bgParams[4].shadowEnable << 5)      //
                          | (regs2.backScreenParams.shadowEnable << 6) //
        ;
}

// -----------------------------------------------------------------------------
// Uploads (snapshots into the frame record)

uint32_t Vdp2FrontEnd::AppendArena(const void *data, size_t size, uint32_t alignment) {
    std::vector<uint8_t> &arena = m_record.arena;
    const size_t offset = (arena.size() + alignment - 1) & ~static_cast<size_t>(alignment - 1);
    arena.resize(offset + size);
    if (data != nullptr) {
        std::memcpy(arena.data() + offset, data, size);
    }
    return static_cast<uint32_t>(offset);
}

// Ported from d3d:3625-3659 (VDP2FlushVRAM): the dirty-run loop; the upload is a snapshot into the arena.
void Vdp2FrontEnd::FlushVRAM() {
    if (!m_vramDirty) {
        return;
    }

    size_t pos, count = 0;
    for (pos = m_vramDirty.FindNext(count); pos < m_vramDirty.Size(); pos = m_vramDirty.FindNext(count, pos + count)) {
        const uint32 vramOffset = static_cast<uint32>(pos) << kVRAMChunkShift;
        const uint32 size = static_cast<uint32>(count) << kVRAMChunkShift;

        const uint32_t arenaOffset = AppendArena(&m_state.mem2.VRAM[vramOffset], size);
        m_record.copies.push_back({arenaOffset, vramOffset, size, Vdp2Copy::Target::VRAM});
        m_record.features.copyBytes += size;
    }
    m_vramDirty.ClearAll();
}

// Ported from d3d:3661-3713 (VDP2FlushCRAM): the generation check; the uploads are snapshots into the arena.
void Vdp2FrontEnd::FlushCRAM() {
    if (m_flushedCRAMGeneration == m_cramGeneration) {
        return;
    }
    m_flushedCRAMGeneration = m_cramGeneration;

    // Brimir: Ymir uploads the whole CRAM buffers; here only the words changed since the last flush are snapshotted:
    // the mode-dependent cache, then the RGB555 table of every CRAM word for line colours (sw:2653-2654, 2270) ...
    static_assert(sizeof(ColorR8G8B8A8) == sizeof(uint32_t) &&
                  2 * sizeof(CRAMColorCache) == kVDP2CRAMColorBufferSize);
    size_t pos, count = 0;
    for (pos = m_cramColorsDirty.FindNext(count); pos < m_cramColorsDirty.Size();
         pos = m_cramColorsDirty.FindNext(count, pos + count)) {
        const uint32_t size = static_cast<uint32_t>(count * sizeof(uint32_t));
        const uint32_t arenaOffset = AppendArena(nullptr, size);
        auto *dst = reinterpret_cast<ColorR8G8B8A8 *>(m_record.arena.data() + arenaOffset);
        for (size_t i = pos; i < pos + count; ++i) {
            dst[i - pos] = i < kVDP2CRAM555TableBase ? m_cramColorCache[i]
                                                     : ToRGBA8(m_cram555Cache[i - kVDP2CRAM555TableBase]);
        }
        m_record.copies.push_back(
            {arenaOffset, static_cast<uint32_t>(pos * sizeof(uint32_t)), size, Vdp2Copy::Target::CRAMColors});
        m_record.features.copyBytes += size;
    }
    m_cramColorsDirty.ClearAll();

    // ... and the coefficient half. Brimir: Ymir uploads it only if an RBG and CRAM coefficients are enabled at this
    // moment, and never retries after BGON changes (stage1b-port-inventory.md section 3.4d); here every change is.
    for (pos = m_cramCoeffsDirty.FindNext(count); pos < m_cramCoeffsDirty.Size();
         pos = m_cramCoeffsDirty.FindNext(count, pos + count)) {
        const uint32_t offset = static_cast<uint32_t>(pos * sizeof(uint32_t));
        const uint32_t size = static_cast<uint32_t>(count * sizeof(uint32_t));
        const uint32_t arenaOffset = AppendArena(&m_state.mem2.CRAM[kVDP2CRAMSize / 2 + offset], size);
        m_record.copies.push_back({arenaOffset, offset, size, Vdp2Copy::Target::CRAMCoeffs});
        m_record.features.copyBytes += size;
    }
    m_cramCoeffsDirty.ClearAll();
}

void Vdp2FrontEnd::PackParams() {
    UpdateCommonRenderParams();
    UpdateLayerRenderParams();
    UpdateComposeParams();
}

// Ported from d3d:4251-4261 (VDP2UpdateState). Brimir: the parameters are packed before (PackParams); flushing makes
// them current, and the batch that renders with them snapshots them when it is recorded.
void Vdp2FrontEnd::UpdateState() {
    FlushVRAM();
    FlushCRAM();
    m_flushedCommon = m_common;
    m_flushedLayer = m_layer;
    m_flushedCompose = m_compose;
}

uint32_t Vdp2FrontEnd::SnapshotLayerParams() {
    const std::vector<uint8_t> &arena = m_record.arena;
    if (m_layerSnapshotOffset == UINT32_MAX ||
        std::memcmp(arena.data() + m_layerSnapshotOffset, &m_flushedLayer, sizeof(m_flushedLayer)) != 0) {
        m_layerSnapshotOffset = AppendArena(&m_flushedLayer, sizeof(m_flushedLayer), kParamSnapshotAlignment);
    }
    return m_layerSnapshotOffset;
}

uint32_t Vdp2FrontEnd::SnapshotComposeParams() {
    const std::vector<uint8_t> &arena = m_record.arena;
    if (m_composeSnapshotOffset == UINT32_MAX ||
        std::memcmp(arena.data() + m_composeSnapshotOffset, &m_flushedCompose, sizeof(m_flushedCompose)) != 0) {
        m_composeSnapshotOffset = AppendArena(&m_flushedCompose, sizeof(m_flushedCompose), kParamSnapshotAlignment);
    }
    return m_composeSnapshotOffset;
}

void Vdp2FrontEnd::AddSpriteSlice(uint32_t index, std::span<const uint8_t, kSpriteSliceSize> data) {
    if (index >= m_record.spriteSliceOffset.size()) {
        return;
    }
    m_record.spriteSliceOffset[index] = AppendArena(data.data(), data.size());
    m_record.spriteDisplayFB = m_state.fbIndex.display & 1u;
    if (index >= kSpriteSliceMesh0) {
        ++m_record.features.meshSlices;
    }
}

// -----------------------------------------------------------------------------
// Resolution and frame flow

// Ported from d3d:4263-4268 (VDP2SetResolution). Brimir: resChangedToInterlaced/firstInterlacedFrame are dropped; the
// composite is cleared like the software framebuffer (sw:520-532): at once, HRes x VRes words of the new resolution,
// with the colour computed now. Inside a frame, the lines rendered so far are recorded first (with the old
// resolution), so the clear covers them, and the later lines are rendered after it with the new resolution.
void Vdp2FrontEnd::SetResolution(uint32_t h, uint32_t v, bool exclusive) {
    CloseRenderedLines();
    m_HRes = h;
    m_VRes = v;
    m_exclusiveMonitor = exclusive;

    uint32_t color = 0xFF000000u;
    if (m_state.regs2.TVMD.BDCLMD) {
        color |= m_state.state2.lineBackLayerState.backColor.u32;
    }
    ClearComposite(color, h * v);
}

// Brimir: records every line rendered in this record so far (the batching rule's split, for a change that is not a
// parameter: a composite clear, a resolution change, a restarted frame).
void Vdp2FrontEnd::CloseRenderedLines() {
    if (m_inFrame && m_lastLine != UINT32_MAX) {
        CloseCompose(m_lastLine);
        RenderLayerLines(m_lastLine);
    }
}

// Brimir: a clear inside a record goes between its batches; between records, the next record starts with it (a newer
// clear replaces an older one, which it covers).
void Vdp2FrontEnd::ClearComposite(uint32_t color, uint32_t words) {
    if (m_inFrame) {
        m_record.clears.push_back({static_cast<uint32_t>(m_record.batches.size()), color, words});
    } else {
        m_pendingClear = true;
        m_pendingClearColor = color;
    }
}

uint32_t Vdp2FrontEnd::YShift() const {
    // Ported from d3d:4309-4310. Brimir: exclusive monitor modes never double rows (480 rows fill the 512-row arrays).
    const bool deinterlace = m_enhancements.deinterlace && m_state.regs2.TVMD.IsInterlaced() && !m_exclusiveMonitor;
    return deinterlace ? 1u : 0u;
}

// Ported from d3d:4270-4297 (VDP2BeginFrame, VDP2 part; the VDP1 FBRAM flush and debug sync stay out: VDP1 stays on
// the CPU and the display framebuffer is added with AddSpriteSlice).
void Vdp2FrontEnd::BeginFrame() {
    // Brimir: matches software (sw:544-550): the NBG counters restart at every frame (not at a record started by a
    // line without BeginFrame).
    InitFrame();
    StartRecord();
}

void Vdp2FrontEnd::StartRecord() {
    // Brimir: a frame that was never consumed (or never ended) is dropped; its snapshots never reached the GPU, so
    // the next frame uploads everything again and keeps any pending composite clear (the last one of the frame).
    if (m_frameReady || m_inFrame) {
        MarkAllDirty();
        if ((m_record.clearComposite || !m_record.clears.empty()) && !m_pendingClear) {
            m_pendingClear = true;
            m_pendingClearColor = m_record.clears.empty() ? m_record.clearColor : m_record.clears.back().color;
        }
        m_frameReady = false;
    }

    m_record.arena.clear();
    m_record.batches.clear();
    m_record.copies.clear();
    m_record.clears.clear();
    m_record.spriteSliceOffset.fill(UINT32_MAX);
    m_record.features = {};
    m_record.clearComposite = m_pendingClear;
    m_record.clearColor = m_pendingClearColor;
    m_pendingClear = false;
    m_inFrame = true;

    m_nextLayerRenderLine = 0;
    m_nextComposeLine = 0;
    m_pendingComposes.clear();
    m_pendingCopiesStart = 0;
    m_layerSnapshotOffset = UINT32_MAX;
    m_composeSnapshotOffset = UINT32_MAX;
    m_lastLine = UINT32_MAX;
    for (auto &rows : m_nbgLastDrawnRow) {
        rows.fill(UINT32_MAX);
    }

    // Brimir: matches software (sw:2522): access patterns are computed per line only, in PrepareLine while the display
    // latch is on; Ymir also computes them here.
    PackParams();
    UpdateState();
}

// Ported from d3d:4445-4473 (VDP2RenderLine): the batching rule, with parameter dirtiness by memcmp.
void Vdp2FrontEnd::RenderLine(uint32_t y) {
    if (y >= kMaxResV) {
        return;
    }
    if (!m_inFrame) {
        // Brimir: a line without BeginFrame (power-on, or a state loaded mid-frame) starts a record at this line; the
        // rows above it keep the composite's previous contents, as the software framebuffer does.
        StartRecord();
        m_nextLayerRenderLine = y;
        m_nextComposeLine = y;
    } else if (m_lastLine != UINT32_MAX && y <= m_lastLine) {
        // Brimir: the frame restarted (VDP::Reset inside a frame renders line 0 again, without VBlank): the lines
        // rendered so far are recorded, and the batching starts over at this line, as software draws the lines again.
        CloseRenderedLines();
        m_nextLayerRenderLine = y;
        m_nextComposeLine = y;
        // Vertical mosaic restarts from lines drawn after the restart, as at the start of a record.
        for (auto &rows : m_nbgLastDrawnRow) {
            rows.fill(UINT32_MAX);
        }
    }
    m_lastLine = y;

    PrepareLine(y);
    // Brimir: matches software (sw:552-566): the NBGs are drawn with the prepared state, then their counters advance.
    StoreNBGLines(y);
    FinishLine();
    PackParams();

    // When Y=0, the changes happened during vblank (or, more precisely, between the last Y of the previous frame
    // and the first line of this frame). Otherwise, the changes happened between Y-1 and Y. Therefore, we need to
    // render lines up to Y-1 then sync the state, unless Y=0, in which case we just sync the state.

    // Brimir: "y > 0" as the first line of the record (a record can start mid-frame).
    if (y > m_nextLayerRenderLine || y > m_nextComposeLine) {
        // Brimir: the common parameters are push constants of every dispatch, so they split both.
        const bool commonDirty = Differs(m_common, m_flushedCommon);
        const bool cramDirty = m_cramGeneration != m_flushedCRAMGeneration;
        const bool layerRenderParamsDirty = Differs(m_layer, m_flushedLayer);
        const bool composeParamsDirty = Differs(m_compose, m_flushedCompose);
        // Brimir: Ymir also renders layers when only the compose parameters changed, so that the compose dispatch
        // finds its layer rows. Here the compose range waits for them instead (CloseCompose), so it splits only
        // compose.
        const bool renderLayers = m_vramDirty || cramDirty || layerRenderParamsDirty || commonDirty;
        const bool compose = composeParamsDirty || commonDirty;
        if (compose) {
            CloseCompose(y - 1);
        }
        if (renderLayers) {
            RenderLayerLines(y - 1);
        }
    }

    UpdateState();
}

// Ported from d3d:4475-4479 (VDP2EndFrame, the neutral part); the frame copy and submission are the back half's.
void Vdp2FrontEnd::EndFrame() {
    if (!m_inFrame) {
        return;
    }
    m_inFrame = false;

    // Brimir: layers end at the last field line; compose ends at the same line in dispatch rows, which is
    // VRes >> (interlaced && !deinterlace) rows (Ymir composes VRes rows even when weaving, section 3.4a).
    const bool interlaced = !m_exclusiveMonitor && m_VRes > kMaxNormalResV;
    const uint32_t fieldLines = std::min(interlaced ? m_VRes >> 1u : m_VRes, kMaxResV);
    if (fieldLines > 0) {
        CloseCompose(fieldLines - 1);
        RenderLayerLines(fieldLines - 1);
    }
    if (m_pendingCopiesStart < m_record.copies.size()) {
        // Copies with no lines left to render still have to reach the GPU.
        EmitBatch(0, 0, nullptr, SnapshotLayerParams());
    }

    m_record.hres = m_HRes;
    m_record.vres = m_VRes;
    m_record.fieldLines = fieldLines;
    m_record.interlaced = interlaced;
    m_record.deinterlace = m_enhancements.deinterlace && interlaced;
    m_record.exclusive = m_exclusiveMonitor;

    m_record.rotBases = m_rotParamBases;
    m_record.nbgLines = m_nbgLines;
    for (size_t i = 0; i < m_lnclBack.size(); ++i) {
        m_record.lnclBack[i] = std::bit_cast<uint32_t>(m_lnclBack[i]);
    }

    m_record.features.batches = static_cast<uint32_t>(m_record.batches.size());
    m_record.features.arenaBytes = static_cast<uint32_t>(m_record.arena.size());

    m_frameReady = true;
}

// -----------------------------------------------------------------------------
// Batching (Brimir)

// The per-line CPU work of d3d:4446-4450, one function per per-line computation so that the software-exact versions
// can replace them.
void Vdp2FrontEnd::PrepareLine(uint32_t y) {
    // Brimir: matches software (sw:2513-2528): nothing is updated while the display latch is off.
    if (m_state.regs2.displayEnabledLatch) {
        CalcAccessPatterns();
        CalcVCellScrollDelay();
        UpdateRotationParameterBases(y);
        m_state.state2.UpdateRotationPageBaseAddresses(m_state.regs2);
        DrawLineColorBackScreens(y);
        // Brimir: matches software (sw:2103-2109, VDP2UpdateLineScreenScrollParams).
        for (uint32 i = 0; i < 2; ++i) {
            UpdateLineScreenScroll(y, m_state.regs2.bgParams[i + 1], m_state.state2.nbgLayerStates[i]);
        }
        // Brimir: matches software (sw:2531-2539): the alt-field fetchers are only reset while deinterlacing.
        const uint32 maxField = m_enhancements.deinterlace ? 2u : 1u;
        for (uint32 field = 0; field < maxField; ++field) {
            for (NBGFetcher &fetcher : m_nbgFetchers[field]) {
                fetcher.lastCharIndex = 0xFFFFFFFF; // force-fetch first character
                fetcher.lastCellX = 0xFF;           // align 2x2 char fetcher
            }
        }
    }
    StoreLineColorBackScreens(y);
    for (uint32 i = 0; i < 2; ++i) {
        const RotationParamState &state = m_state.state2.rotParamStates[i];
        m_rotParamBases[i * kMaxResV + y] = {m_rotTableAddress[i], state.Xst, state.Yst, state.KA};
    }
    CountFeatures();
}

// Brimir: matches software (sw:2568-2643, 2829-2911): which NBGs software draws on line y and with what state, for
// each field it draws (the alternate field only while deinterlacing). Each dispatch row gets the NBG state software
// draws it with; drawing advances the field's fetcher as software's does.
void Vdp2FrontEnd::StoreNBGLines(uint32_t y) {
    const VDP2Regs &regs2 = m_state.regs2;
    const VDP2State &state2 = m_state.state2;
    const uint32_t yShift = YShift();
    const bool drawAltField = m_enhancements.deinterlace && regs2.TVMD.IsInterlaced(); // sw:561-563
    const bool doubleDensity = regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity;
    const bool bothRBGs = regs2.bgEnabled[4] && regs2.bgEnabled[5]; // sw:2622-2624

    for (uint32 i = 0; i < 4; ++i) {
        const BGParams &bgParams = regs2.bgParams[i + 1];
        const NBGLayerState &bgState = state2.nbgLayerStates[i];
        // sw:2875-2892
        const bool drawn = !bothRBGs && state2.layerEnabled[i + 2] && !(i == 0 && regs2.bgEnabled[5]);
        const bool mosaicRepeat = drawn && bgParams.mosaicEnable && bgState.mosaicCounterY > 0;

        for (uint32 field = 0; field < (drawAltField ? 2u : 1u); ++field) {
            // sw:4522-4524, 4625-4628: the alternate line of double density is one vertical increment further
            const bool altLine = field == 1 && doubleDensity && (!bgParams.bitmap || !bgParams.lineScrollYEnable);

            VDP2NBGLine line{};
            line.fracScrollX = static_cast<int32_t>(bgState.fracScrollX);
            line.fracScrollY = static_cast<int32_t>(bgState.fracScrollY + (altLine ? bgParams.scrollIncV : 0u));
            line.scrollIncH = static_cast<int32_t>(bgState.scrollIncH);

            NBGFetcher &fetcher = m_nbgFetchers[field][i];
            line.prevLastVCellScroll = fetcher.lastVCellScroll;
            line.nextChar = fetcher.nextChar.u32;

            // Exclusive monitor modes never double rows: the alternate field has no row of its own.
            const bool hasRow = field == 0 || yShift != 0;
            const uint32_t row = (y << yShift) | field;
            uint32_t &lastDrawnRow = m_nbgLastDrawnRow[field][i];
            line.mosaicSrcLine = row;
            if (mosaicRepeat) {
                // sw:2889-2892: the layer output of the last line drawn is reused. Before any line of this record,
                // it is the previous frame's, which the GPU no longer has; the row is drawn instead.
                if (lastDrawnRow != UINT32_MAX) {
                    line.mosaicSrcLine = lastDrawnRow;
                }
            } else if (drawn) {
                DrawNBGFetcher(i, line, fetcher);
                if (hasRow) {
                    lastDrawnRow = row;
                }
            }

            if (hasRow && row < kMaxResV) {
                m_nbgLines[i * kMaxResV + row] = line;
            }
        }
    }
}

// Brimir: matches software (sw:5161-5191): where the character of scroll position (scrollX, scrollY) is.
Vdp2FrontEnd::NBGCharLocation Vdp2FrontEnd::LocateNBGChar(const BGParams &bgParams, uint32_t scrollX,
                                                          uint32_t scrollY) const {
    const uint32 fourCellCharValue = bgParams.cellSizeShift ? 1 : 0;

    // Determine plane index from the scroll coordinates
    const uint32 planeX = (scrollX >> (9 + bgParams.pageShiftH)) & 1u;
    const uint32 planeY = (scrollY >> (9 + bgParams.pageShiftV)) & 1u;
    const uint32 plane = planeX + (planeY << 1u);
    const uint32 pageBaseAddress = bgParams.pageBaseAddresses[plane];

    // HACK: apply data access shift here too
    const uint32 bank = (pageBaseAddress >> 17u) & 3u;
    scrollX += bgParams.vramDataOffset[bank];

    // Determine page index from the scroll coordinates
    const uint32 pageX = bit::extract<9>(scrollX) & bgParams.pageShiftH;
    const uint32 pageY = bit::extract<9>(scrollY) & bgParams.pageShiftV;
    const uint32 page = pageX + (pageY << 1u);
    const uint32 pageOffset = page << kPageSizes[fourCellCharValue][bgParams.twoWordChar ? 1 : 0];

    // Determine character pattern from the scroll coordinates
    const uint32 charPatX = bit::extract<3, 8>(scrollX) >> fourCellCharValue;
    const uint32 charPatY = bit::extract<3, 8>(scrollY) >> fourCellCharValue;

    NBGCharLocation loc;
    loc.pageAddress = pageBaseAddress + pageOffset;
    loc.charIndex = charPatX + (charPatY << (6u - fourCellCharValue));
    loc.cellX = static_cast<uint8_t>(bit::extract<3>(scrollX) & fourCellCharValue);
    loc.bank = bank;
    return loc;
}

// Brimir: matches software (sw:5230-5303, VDP2FetchTwoWordCharacter, VDP2FetchOneWordCharacter and
// VDP2ExtractOneWordCharacter with the template parameters as run-time values).
Character Vdp2FrontEnd::FetchNBGCharacter(const BGParams &bgParams, const NBGCharLocation &loc) const {
    if (bgParams.twoWordChar) {
        const uint32 charAddress = loc.pageAddress + loc.charIndex * sizeof(uint32);
        const uint32 charBank = (charAddress >> 17u) & 3u;
        if (!bgParams.patNameAccess[charBank]) {
            return {};
        }
        return Character{.u32 = m_state.mem2.ReadVRAM<uint32>(charAddress)};
    }

    const uint32 charAddress = loc.pageAddress + loc.charIndex * sizeof(uint16);
    const uint32 charBank = (charAddress >> 17u) & 3u;
    if (!bgParams.patNameAccess[charBank]) {
        return {};
    }
    const uint16 charData = m_state.mem2.ReadVRAM<uint16>(charAddress);

    const uint32 fourCellChar = bgParams.cellSizeShift ? 1 : 0;
    const bool largePalette = bgParams.colorFormat != ColorFormat::Palette16;
    const uint32 extChar = bgParams.extChar ? 1 : 0;

    // Character number bit range from the 1-word character pattern data (charData)
    const uint32 baseCharNumEnd = 9 + 2 * extChar;
    const uint32 baseCharNumPos = 2 * fourCellChar;

    // Upper character number bit range from the supplementary character number (bgParams.supplCharNum)
    const uint32 supplCharNumStart = 2 * fourCellChar + 2 * extChar;
    const uint32 supplCharNumEnd = 4;
    const uint32 supplCharNumPos = 10 + supplCharNumStart;

    const uint32 baseCharNum = charData & ((2u << baseCharNumEnd) - 1u);
    const uint32 supplCharNum =
        (bgParams.supplScrollCharNum >> supplCharNumStart) & ((2u << (supplCharNumEnd - supplCharNumStart)) - 1u);

    Character ch;
    ch.charNum = (baseCharNum << baseCharNumPos) | (supplCharNum << supplCharNumPos);
    if (fourCellChar) {
        ch.charNum |= bit::extract<0, 1>(bgParams.supplScrollCharNum);
    }
    if (largePalette) {
        ch.palNum = bit::extract<12, 14>(charData) << 4u;
    } else {
        ch.palNum = (bit::extract<12, 15>(charData) | bgParams.supplScrollPalNum);
    }
    ch.specColorCalc = bgParams.supplScrollSpecialColorCalc;
    ch.specPriority = bgParams.supplScrollSpecialPriority;
    ch.flipH = !extChar && bit::test<10>(charData);
    ch.flipV = !extChar && bit::test<11>(charData);
    return ch;
}

// Brimir: matches software (sw:5193-5215): one character fetch of the pipelined fetcher.
void Vdp2FrontEnd::FetchNBGChar(const BGParams &bgParams, NBGFetcher &fetcher, uint32_t scrollX, uint32_t scrollY) {
    const NBGCharLocation loc = LocateNBGChar(bgParams, scrollX, scrollY);
    const bool charPatDelay = bgParams.charPatDelay[loc.bank];
    if (fetcher.lastCharIndex != loc.charIndex) {
        fetcher.lastCharIndex = loc.charIndex;
        const Character ch = FetchNBGCharacter(bgParams, loc);
        fetcher.currChar = charPatDelay ? fetcher.nextChar : ch;
        fetcher.nextChar = ch;
    } else if (bgParams.cellSizeShift) {
        if (charPatDelay && fetcher.lastCellX != loc.cellX) {
            fetcher.lastCellX = loc.cellX;
            if (loc.cellX == 1) {
                fetcher.currChar = fetcher.nextChar;
            }
        }
    }
}

// Brimir: matches software (sw:4518-4618, 4621-4698): the fetcher state a drawn NBG line leaves (VRAMFetcher's
// lastVCellScroll, and for scroll BGs currChar, nextChar, lastCharIndex and lastCellX). The scroll increment is below
// 8 pixels (11.8 with a 3-bit integer part), so the cell column advances at most once per pixel.
// Windows are not known here: fetches are simulated as if no pixel were windowed. The vertical cell scroll reads and
// lastCharIndex do not depend on windows (the extra tile past the right edge is always fetched). nextChar does not
// either, except when a window hides 512 dots or more up to the right edge (the extra tile's index can then equal an
// earlier fetch's on another page, exception W5); with a character pattern delay, currChar and lastCellX can.
void Vdp2FrontEnd::DrawNBGFetcher(uint32_t index, const VDP2NBGLine &line, NBGFetcher &fetcher) {
    const VDP2Regs &regs2 = m_state.regs2;
    const BGParams &bgParams = regs2.bgParams[index + 1];
    const NBGLayerState &bgState = m_state.state2.nbgLayerStates[index];

    uint32 fracScrollX = static_cast<uint32>(line.fracScrollX) + bgParams.scrollAmountH;
    const uint32 fracScrollY = static_cast<uint32>(line.fracScrollY) + bgParams.scrollAmountV;
    const uint32 scrollIncH = bgState.scrollIncH;

    uint32 cellScrollTableAddress = regs2.vcellScrollTableAddress + bgState.vcellScrollOffset;
    const bool vcellScrollEnable = index <= 1 && bgParams.vcellScrollEnable;

    auto readCellScrollY = [&](bool checkRepeat = false) {
        if (checkRepeat && bgState.vcellScrollRepeat && bgState.vcellScrollDelay) {
            return fetcher.lastVCellScroll;
        }
        const uint32 value = m_state.mem2.ReadVRAM<uint32>(cellScrollTableAddress);
        if (!checkRepeat || !bgState.vcellScrollRepeat) {
            cellScrollTableAddress += regs2.vcellScrollInc;
        }
        const uint32 prevValue = fetcher.lastVCellScroll;
        fetcher.lastVCellScroll = bit::extract<8, 26>(value);
        return bgState.vcellScrollDelay ? prevValue : fetcher.lastVCellScroll;
    };

    uint32 vcellScrollY = 0;
    uint32 vcellScrollX = fracScrollX >> (8u + 3u);
    if (vcellScrollEnable) {
        vcellScrollY = readCellScrollY(true);
    }

    const bool charPatDelay = std::ranges::any_of(bgParams.charPatDelay, [](bool b) { return b; });
    if (!bgParams.bitmap && charPatDelay) {
        // The delayed pipeline depends on every fetch: run the line as software does.
        uint32 mosaicCounterX = 0;
        for (uint32 x = 0; x < m_HRes; x++) {
            if (bgParams.mosaicEnable) {
                const uint8 currMosaicCounterX = mosaicCounterX;
                mosaicCounterX++;
                if (mosaicCounterX >= m_state.regs2.mosaicH) {
                    mosaicCounterX = 0;
                }
                if (currMosaicCounterX > 0) {
                    fracScrollX += scrollIncH;
                    continue;
                }
            } else if (vcellScrollEnable) {
                if ((fracScrollX >> (8u + 3u)) != vcellScrollX) {
                    vcellScrollX = fracScrollX >> (8u + 3u);
                    vcellScrollY = readCellScrollY();
                }
            }
            FetchNBGChar(bgParams, fetcher, fracScrollX >> 8u, (fracScrollY + vcellScrollY) >> 8u);
            fracScrollX += scrollIncH;
        }
    } else {
        // Pixels 0 to HRes - 1, plus the extra tile for scroll BGs: one read per cell column change between
        // consecutive pixels (sw:4568-4574, 4600-4606). Without a delay, the fetcher's characters are those of the
        // last fetch whose index differed from the one before: the line's first fetch, its last pixel's fetch when
        // the line crossed characters in between (in the same character as the last pixel's fetch from its run's
        // start on), and the extra tile stand for the line.
        if (bgParams.bitmap) {
            if (vcellScrollEnable && !bgParams.mosaicEnable) {
                const uint32 crossings = ((fracScrollX & 0x7FF) + (m_HRes - 1) * scrollIncH) >> 11u;
                for (uint32 k = 0; k < crossings; ++k) {
                    vcellScrollY = readCellScrollY();
                }
            }
            return;
        }
        const uint32 firstX = fracScrollX;
        FetchNBGChar(bgParams, fetcher, firstX >> 8u, (fracScrollY + vcellScrollY) >> 8u);

        // The last fetching pixel (the last mosaic block's first pixel with mosaic)
        const uint32 mosaicH = bgParams.mosaicEnable ? m_state.regs2.mosaicH : 1u;
        const uint32 lastX = (m_HRes - 1) - (m_HRes - 1) % mosaicH;
        const auto readsUpTo = [&](uint32 x) { return ((firstX & 0x7FF) + x * scrollIncH) >> 11u; };
        uint32 reads = 0;
        if (vcellScrollEnable && !bgParams.mosaicEnable) {
            for (; reads < readsUpTo(lastX); ++reads) {
                vcellScrollY = readCellScrollY();
            }
        }
        const uint32 lastFracScrollX = firstX + lastX * scrollIncH;
        const uint32 charShift = 8u + 3u + (bgParams.cellSizeShift ? 1u : 0u);
        const uint64_t firstColumn = firstX >> charShift;
        const uint64_t lastColumn = (uint64_t{firstX} + uint64_t{lastX} * scrollIncH) >> charShift;
        const uint32 lastScrollY = (fracScrollY + vcellScrollY) >> 8u;
        const NBGCharLocation lastLoc = LocateNBGChar(bgParams, lastFracScrollX >> 8u, lastScrollY);
        if (firstColumn != lastColumn || lastLoc.charIndex != fetcher.lastCharIndex) {
            // The line crossed characters: the last pixel's run fetched its own character
            fetcher.lastCharIndex = 0xFFFFFFFF;
            FetchNBGChar(bgParams, fetcher, lastFracScrollX >> 8u, lastScrollY);
        }

        if (vcellScrollEnable && !bgParams.mosaicEnable) {
            for (; reads < readsUpTo(m_HRes); ++reads) {
                vcellScrollY = readCellScrollY();
            }
        }
        fracScrollX = firstX + m_HRes * scrollIncH;
        FetchNBGChar(bgParams, fetcher, fracScrollX >> 8u, (fracScrollY + vcellScrollY) >> 8u);
        return;
    }
    // The extra tile fetched past the right edge (sw:4596-4617)
    if (!bgParams.mosaicEnable && vcellScrollEnable) {
        if ((fracScrollX >> (8u + 3u)) != vcellScrollX) {
            vcellScrollX = fracScrollX >> (8u + 3u);
            vcellScrollY = readCellScrollY();
        }
    }
    FetchNBGChar(bgParams, fetcher, fracScrollX >> 8u, (fracScrollY + vcellScrollY) >> 8u);
}
void Vdp2FrontEnd::CountFeatures() {
    const VDP2Regs &regs2 = m_state.regs2;
    Vdp2FeatureCounters &f = m_record.features;

    // bgParams index of each layer in bgEnabled order: NBG0-3, RBG0, RBG1
    static constexpr std::array<uint32_t, 6> kParamIndex{1, 2, 3, 4, 0, 1};
    bool mosaic = false;
    bool charPatDelay = false;
    for (uint32_t i = 0; i < 6; ++i) {
        if (!regs2.bgEnabled[i]) {
            continue;
        }
        const BGParams &bgParams = regs2.bgParams[kParamIndex[i]];
        mosaic |= bgParams.mosaicEnable;
        if (i < 4) {
            charPatDelay |= std::ranges::any_of(bgParams.charPatDelay, [](bool b) { return b; });
        }
    }

    bool vcell = false;
    bool ddLineScroll = false;
    const bool doubleDensity = regs2.TVMD.LSMDn == InterlaceMode::DoubleDensity;
    for (uint32_t i = 0; i < 2; ++i) {
        if (!regs2.bgEnabled[i]) {
            continue;
        }
        const BGParams &bgParams = regs2.bgParams[i + 1];
        vcell |= bgParams.vcellScrollEnable;
        ddLineScroll |= doubleDensity && bgParams.lineScrollInterval > 0 &&
                        (bgParams.lineScrollXEnable || bgParams.lineScrollYEnable || bgParams.lineZoomEnable);
    }

    bool shadow = regs2.backScreenParams.shadowEnable || regs2.transparentShadowEnable;
    for (const BGParams &bgParams : regs2.bgParams) {
        shadow |= bgParams.shadowEnable;
    }

    f.mosaicLines += mosaic;
    f.vcellLines += vcell;
    f.charPatDelayLines += charPatDelay;
    f.ddLineScrollLines += ddLineScroll;
    f.extendedCCLines += regs2.colorCalcParams.extendedColorCalcEnable;
    f.shadowLines += shadow;
    f.cramModeMask |= 1u << regs2.vramControl.colorRAMMode;
}

void Vdp2FrontEnd::EmitBatch(uint32_t layerStartY, uint32_t layerLines, const PendingCompose *compose,
                             uint32_t layerOffset) {
    auto clampRows = [](uint32_t &start, uint32_t &lines) {
        if (start >= kMaxResV) {
            start = 0;
            lines = 0;
        } else {
            lines = std::min(lines, kMaxResV - start);
        }
    };

    Vdp2Batch batch{};
    batch.layerStartY = layerStartY;
    batch.layerLines = layerLines;
    clampRows(batch.layerStartY, batch.layerLines);
    if (compose != nullptr) {
        batch.composeStartY = compose->startY;
        batch.composeLines = compose->lines;
        clampRows(batch.composeStartY, batch.composeLines);
        batch.composeParamsOffset = compose->paramsOffset;
    } else {
        batch.composeParamsOffset = SnapshotComposeParams();
    }
    batch.common = m_flushedCommon;
    batch.common.startY = 0;
    batch.layerParamsOffset = layerOffset;
    batch.firstCopy = m_pendingCopiesStart;
    batch.copyCount = static_cast<uint32_t>(m_record.copies.size()) - m_pendingCopiesStart;
    batch.hres = m_HRes;
    batch.vres = m_VRes;
    batch.needsFetchPass = false;
    if (batch.layerLines > 0) {
        // The parameters the batch's shaders read
        VDP2LayerRenderParams params;
        std::memcpy(&params, m_record.arena.data() + layerOffset, sizeof(params));
        for (uint32_t i = 0; i < 4; ++i) {
            batch.needsFetchPass |= NBGUsesCharTable(params.nbg[i], i);
        }
    }
    m_pendingCopiesStart = static_cast<uint32_t>(m_record.copies.size());
    m_record.batches.push_back(batch);
}

// Ported from d3d:4398-4411 (VDP2ComposeLines, the line range). Brimir: the range is shifted to dispatch rows like
// the layer range (section 3.4b), and it is recorded once the layer rows it reads are.
void Vdp2FrontEnd::CloseCompose(uint32_t lastY) {
    if (lastY < m_nextComposeLine) {
        return;
    }
    const uint32_t yShift = YShift();
    const uint32_t startY = m_nextComposeLine;
    m_nextComposeLine = lastY + 1;
    m_pendingComposes.push_back({startY << yShift, (lastY - startY + 1) << yShift, SnapshotComposeParams()});

    if (lastY < m_nextLayerRenderLine) {
        // Its layer rows are already recorded.
        RenderLayerLines(lastY);
    }
}

// Ported from d3d:4299-4319 (VDP2RenderLayerLines, the line range and deinterlace shift); the dispatches and
// barriers are the back half's. Brimir: the compose ranges that waited for these rows follow in order.
void Vdp2FrontEnd::RenderLayerLines(uint32_t lastY) {
    size_t composeIndex = 0;
    if (lastY >= m_nextLayerRenderLine) {
        const uint32_t yShift = YShift();
        const uint32_t startY = m_nextLayerRenderLine;
        m_nextLayerRenderLine = lastY + 1;
        const PendingCompose *compose = m_pendingComposes.empty() ? nullptr : &m_pendingComposes[0];
        EmitBatch(startY << yShift, (lastY - startY + 1) << yShift, compose, SnapshotLayerParams());
        composeIndex = compose != nullptr ? 1 : 0;
    }
    for (; composeIndex < m_pendingComposes.size(); ++composeIndex) {
        EmitBatch(0, 0, &m_pendingComposes[composeIndex], SnapshotLayerParams());
    }
    m_pendingComposes.clear();
}

} // namespace brimir::gpu::vdp2
