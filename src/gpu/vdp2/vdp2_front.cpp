#include "gpu/vdp2/vdp2_front.hpp"

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
    // Brimir: resolution and framebuffer as SoftwareVDPRenderer::Reset (sw:91-106), which fills its framebuffer black.
    m_HRes = kDefaultResH;
    m_VRes = kDefaultResV;
    m_exclusiveMonitor = false;
    m_pendingClear = true;
    m_pendingClearColor = 0xFF000000u;
    if (hard) {
        m_cramColorCache.fill({});
        // Brimir: matches software (sw:96-98)
        m_cram555Cache.fill({});
    }

    m_vramDirty.SetAll();
    ++m_cramGeneration;

    m_nextLayerRenderLine = 0;
    m_nextComposeLine = 0;

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
    ++m_cramGeneration;
}

// Ported from d3d:2111-2117 (UpdateEnhancements, VDP2 part).
void Vdp2FrontEnd::SetEnhancements(const config::Enhancements &enhancements) {
    m_enhancements = enhancements;
    m_common.enhancements.deinterlace = enhancements.deinterlace;
    m_common.enhancements.transparentMeshes = enhancements.transparentMeshes;
}

void Vdp2FrontEnd::MarkAllDirty() {
    m_vramDirty.SetAll();
    ++m_cramGeneration;
}

// -----------------------------------------------------------------------------
// CRAM colour cache

// Ported from d3d:3447-3486 (VDP2CacheCRAMColor).
void Vdp2FrontEnd::CacheCRAMColor(uint32_t address) {
    CRAMColorCache &colorCache = m_cramColorCache;
    switch (m_state.regs2.vramControl.colorRAMMode) {
    case 0: { // RGB 5:5:5, half CRAM
        const auto value = m_state.mem2.ReadCRAM<uint16>(address & ~1u);
        const Color555 color5{.u16 = value};
        const Color888 color8 = ConvertRGB555to888(color5);
        ColorR8G8B8A8 &color = colorCache[(address >> 1u) & (kVDP2CRAMColorCacheEntries - 1)];
        color.r = color8.r;
        color.g = color8.g;
        color.b = color8.b;
        color.a = color8.msb;
        break;
    }
    case 1: { // RGB 5:5:5, full CRAM
        const auto value = m_state.mem2.ReadCRAM<uint16>(address & ~1u);
        const Color555 color5{.u16 = value};
        const Color888 color8 = ConvertRGB555to888(color5);
        ColorR8G8B8A8 &color = colorCache[(address >> 1u) & (kVDP2CRAMColorCacheEntries - 1)];
        color.r = color8.r;
        color.g = color8.g;
        color.b = color8.b;
        color.a = color8.msb;
        break;
    }
    case 2: [[fallthrough]]; // RGB 8:8:8, full CRAM
    case 3: [[fallthrough]]; // RGB 8:8:8, full CRAM
    default: {
        address = m_state.mem2.UnmapCRAMAddress<uint8>(address);
        const auto value = m_state.mem2.ReadCRAM<uint32>(address & ~3u);
        const Color888 color8{.u32 = value};
        ColorR8G8B8A8 &color = colorCache[(address >> 2u) & (kVDP2CRAMColorCacheEntries - 1)];
        color.r = color8.r;
        color.g = color8.g;
        color.b = color8.b;
        color.a = color8.msb;
        break;
    }
    }
}

// Brimir: matches software (sw:905-914, VDP2UpdateCRAMCache for one 16-bit or 8-bit write).
void Vdp2FrontEnd::Cache555Color(uint32_t address) {
    address &= (kVDP2CRAMSize - 1) & ~1u;
    const Color555 color5{.u16 = util::ReadBE<uint16>(&m_state.mem2.CRAM[address])}; // raw CRAM, as software
    m_cram555Cache[address / sizeof(uint16)] = ConvertRGB555to888(color5);
}

// Ported from d3d:3488-3526 (VDP2CacheAllCRAMColors).
void Vdp2FrontEnd::CacheAllCRAMColors() {
    CRAMColorCache &colorCache = m_cramColorCache;
    switch (m_state.regs2.vramControl.colorRAMMode) {
    case 0: // RGB 5:5:5, half CRAM
        for (uint32 i = 0; i < 1024; ++i) {
            const auto value = m_state.mem2.ReadCRAM<uint16>(i * sizeof(uint16));
            const Color555 color5{.u16 = value};
            const Color888 color8 = ConvertRGB555to888(color5);
            colorCache[i].r = color8.r;
            colorCache[i].g = color8.g;
            colorCache[i].b = color8.b;
            colorCache[i].a = color8.msb;
        }
        break;
    case 1: // RGB 5:5:5, full CRAM
        for (uint32 i = 0; i < 2048; ++i) {
            const auto value = m_state.mem2.ReadCRAM<uint16>(i * sizeof(uint16));
            const Color555 color5{.u16 = value};
            const Color888 color8 = ConvertRGB555to888(color5);
            colorCache[i].r = color8.r;
            colorCache[i].g = color8.g;
            colorCache[i].b = color8.b;
            colorCache[i].a = color8.msb;
        }
        break;
    case 2: [[fallthrough]]; // RGB 8:8:8, full CRAM
    case 3: [[fallthrough]]; // RGB 8:8:8, full CRAM
    default:
        for (uint32 i = 0; i < 1024; ++i) {
            const auto value = m_state.mem2.ReadCRAM<uint32>(i * sizeof(uint32));
            const Color888 color8{.u32 = value};
            colorCache[i].r = color8.r;
            colorCache[i].g = color8.g;
            colorCache[i].b = color8.b;
            colorCache[i].a = color8.msb;
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

        switch (address) {
        case 0x092: // SCYN2
            m_resetNBGScrollY[0] = true;
            break;
        case 0x096: // SCYN3
            m_resetNBGScrollY[1] = true;
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

// Ported from d3d:4100-4116 (VDP2InitNBGs).
void Vdp2FrontEnd::InitNBGs() {
    const VDP2Regs &regs2 = m_state.regs2;

    for (uint32 i = 0; i < 4; ++i) {
        const BGParams &bgParams = regs2.bgParams[i + 1];
        NBGLayerState &nbgState = m_state.state2.nbgLayerStates[i];

        // NOTE: fracScrollX/Y are computed from scratch in the shader
        nbgState.scrollIncH = bgParams.scrollIncH;

        if (i < 2) {
            nbgState.lineScrollTableAddress = bgParams.lineScrollTableAddress;
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

// Ported from d3d:4180-4225 (VDP2UpdateRotationParameterBases). Brimir: kMaxResV rows per parameter; Xst, Yst and KA
// accumulate from the last computed line, as software's rotParamStates do (sw:2156-2184), so lines skipped with the
// display latch off or with no RBG enabled continue where software does.
void Vdp2FrontEnd::UpdateRotationParameterBases(uint32_t y) {
    VDP2Regs &regs2 = m_state.regs2;
    if (!regs2.bgEnabled[4] && !regs2.bgEnabled[5]) {
        // Skip if no RBGs are enabled
        return;
    }

    const bool readAll = y == 0;

    const uint32 baseAddress = regs2.commonRotParams.baseAddress & 0xFFF7C; // mask bit 6 (shifted left by 1)
    for (uint32 i = 0; i < 2; ++i) {
        VDP2RotParamBase &base = m_lastRotParamBases[i];
        RotationParams &src = regs2.rotParams[i];

        const uint32 address = baseAddress + i * 0x80;

        base.tableAddress = address;

        if (readAll || src.readXst) {
            base.Xst = bit::extract_signed<6, 28, sint32>(m_state.mem2.ReadVRAM<uint32>(address + 0x00));
            src.readXst = false;
        } else {
            base.Xst += bit::extract_signed<6, 18, sint32>(m_state.mem2.ReadVRAM<uint32>(address + 0x0C));
        }

        if (readAll || src.readYst) {
            base.Yst = bit::extract_signed<6, 28, sint32>(m_state.mem2.ReadVRAM<uint32>(address + 0x04));
            src.readYst = false;
        } else {
            base.Yst += bit::extract_signed<6, 18, sint32>(m_state.mem2.ReadVRAM<uint32>(address + 0x10));
        }

        if (readAll || src.readKAst) {
            const uint32 KAst = bit::extract<6, 31>(m_state.mem2.ReadVRAM<uint32>(address + 0x54));
            base.KA = src.coeffTableAddressOffset + KAst;
            src.readKAst = false;
        } else {
            base.KA += bit::extract_signed<6, 25>(m_state.mem2.ReadVRAM<uint32>(address + 0x58));
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
    if (!regs1.tvmr.hdtvEnable && !regs1.tvmr.fbRotEnable) {
        if (regs1.tvmr.pixel8Bits) {
            params.spriteParams.inHalfResH = (regs2.TVMD.HRESOn & 0b110) == 0b000;
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
void Vdp2FrontEnd::UpdateLayerRenderParams(uint32_t y) {
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
        renderParams.lineScrollTableAddress = bgState.lineScrollTableAddress;
        renderParams.vcellScrollOffset = bgState.vcellScrollOffset;
        renderParams.vcellScrollDelay = bgState.vcellScrollDelay;
        renderParams.vcellScrollRepeat = bgState.vcellScrollRepeat;
        if (i >= 2 && m_resetNBGScrollY[i - 2]) {
            renderParams.scrollYIncBase = y;
            m_resetNBGScrollY[i - 2] = false;
        }
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
    std::memcpy(arena.data() + offset, data, size);
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

    // Brimir: the mode-dependent cache, then the RGB555 table of every CRAM word for line colours (sw:2653-2654, 2270).
    std::array<ColorR8G8B8A8, 2 * kVDP2CRAMColorCacheEntries> colors;
    std::copy(m_cramColorCache.begin(), m_cramColorCache.end(), colors.begin());
    for (size_t i = 0; i < m_cram555Cache.size(); ++i) {
        colors[kVDP2CRAM555TableBase + i] = ToRGBA8(m_cram555Cache[i]);
    }
    static_assert(sizeof(colors) == kVDP2CRAMColorBufferSize);
    const uint32_t colorsOffset = AppendArena(colors.data(), sizeof(colors));
    m_record.copies.push_back({colorsOffset, 0, kVDP2CRAMColorBufferSize, Vdp2Copy::Target::CRAMColors});

    // Brimir: Ymir uploads the coefficient half only if an RBG and CRAM coefficients are enabled at this moment, and
    // never retries after BGON changes (stage1b-port-inventory.md section 3.4d). Snapshot it on every change.
    const uint32_t coeffsOffset = AppendArena(&m_state.mem2.CRAM[kVDP2CRAMSize / 2], kVDP2CRAMRotCoeffBufferSize);
    m_record.copies.push_back({coeffsOffset, 0, kVDP2CRAMRotCoeffBufferSize, Vdp2Copy::Target::CRAMCoeffs});

    m_record.features.copyBytes += kVDP2CRAMColorBufferSize + kVDP2CRAMRotCoeffBufferSize;
}

void Vdp2FrontEnd::PackParams(uint32_t y) {
    UpdateCommonRenderParams();
    UpdateLayerRenderParams(y);
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
// composite is cleared like the software framebuffer (sw:518-530), with the colour computed now, as software does.
void Vdp2FrontEnd::SetResolution(uint32_t h, uint32_t v, bool exclusive) {
    m_HRes = h;
    m_VRes = v;
    m_exclusiveMonitor = exclusive;

    uint32_t color = 0xFF000000u;
    if (m_state.regs2.TVMD.BDCLMD) {
        color |= m_state.state2.lineBackLayerState.backColor.u32;
    }
    m_pendingClear = true;
    m_pendingClearColor = color;
}

uint32_t Vdp2FrontEnd::YShift() const {
    // Ported from d3d:4309-4310. Brimir: exclusive monitor modes never double rows (480 rows fill the 512-row arrays).
    const bool deinterlace = m_enhancements.deinterlace && m_state.regs2.TVMD.IsInterlaced() && !m_exclusiveMonitor;
    return deinterlace ? 1u : 0u;
}

// Ported from d3d:4270-4297 (VDP2BeginFrame, VDP2 part; the VDP1 FBRAM flush and debug sync stay out: VDP1 stays on
// the CPU and the display framebuffer is added with AddSpriteSlice).
void Vdp2FrontEnd::BeginFrame() {
    // Brimir: a frame that was never consumed (or never ended) is dropped; its snapshots never reached the GPU, so
    // the next frame uploads everything again and keeps any pending composite clear.
    if (m_frameReady || m_inFrame) {
        MarkAllDirty();
        if (m_record.clearComposite && !m_pendingClear) {
            m_pendingClear = true;
            m_pendingClearColor = m_record.clearColor;
        }
        m_frameReady = false;
    }

    m_record.arena.clear();
    m_record.batches.clear();
    m_record.copies.clear();
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

    // Brimir: matches software (sw:2070-2078, 2522): access patterns are computed per line only, in PrepareLine while
    // the display latch is on; Ymir also computes them here.
    InitNBGs();

    // Brimir: Ymir clears scrollYIncBase after UpdateState(0) has uploaded the parameters, so the first batch could
    // see the previous frame's base. Clear it before packing.
    for (auto &nbg : m_layer.nbg) {
        nbg.scrollYIncBase = 0;
    }
    PackParams(0);
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
        BeginFrame();
        m_nextLayerRenderLine = y;
        m_nextComposeLine = y;
    }

    PrepareLine(y);
    PackParams(y);

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
    }
    StoreLineColorBackScreens(y);
    for (uint32 i = 0; i < 2; ++i) {
        m_rotParamBases[i * kMaxResV + y] = m_lastRotParamBases[i];
    }
    CountFeatures();
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
