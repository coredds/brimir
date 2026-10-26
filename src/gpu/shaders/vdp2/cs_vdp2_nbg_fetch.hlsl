// Brimir: VDP2 NBG character fetch pass (no Ymir equivalent). For each (NBG, dispatch row) it runs the software
// renderer's character fetcher over the row, windows included, and stores the character each fetching pixel shows in
// the NBG character table (vdp2_nbg_chars.hlsli), which the backgrounds shader reads for the NBGs NBGUsesCharTable
// selects. Only those NBGs run; the back half dispatches the pass only for batches that have one
// (Vdp2Batch::needsFetchPass), between the sprite and backgrounds dispatches (the sprite window is the sprite pass's
// output).
//
// One workgroup per (row, NBG). Its 32 threads first evaluate every pixel of the row in parallel (scroll position,
// character location, pattern name data, windows, mosaic), none of which depends on the fetcher's state, then resolve
// the fetcher's state machine with prefix maxima over the row (see "Entrypoint"). A single thread running the fetcher
// over the row in order is bound by the latency of its dependent instructions: ~45 us per row on an RX 9060 XT (~10 ms
// per frame with one batch per line), ~20 us with the per-pixel work done in parallel first.
//
// Bindings (set 0; tools/shaders/build_shaders.cmake shifts t by 0 and u by 16):
//   0  t0  g_layerRenderParams   StructuredBuffer<LayerRenderParams>  STORAGE_BUFFER_DYNAMIC
//   2  t2  g_vram                ByteAddressBuffer                    STORAGE_BUFFER
//   7  t7  g_nbgLines            StructuredBuffer<NBGLine>            STORAGE_BUFFER
//   9  t9  g_spriteAttrsIn       Texture2DArray<uint>                 SAMPLED_IMAGE
//  21  u5  g_nbgCharsOut         RWStructuredBuffer<uint>             STORAGE_BUFFER
// Push constants: NBGFetchPushConstants (CommonRenderParams, then the batch's row count). Dispatch: (rows, 1, 4);
// group x is the row startY + x, z the NBG.

#include "vdp2_common_params.hlsli"
#include "vdp2_nbg_line.hlsli"
#include "vdp2_render_params_layer.hlsli"

#include "vdp2_defs.hlsli"
#include "vdp2_nbg_chars.hlsli"

#include "util/bit_ops.hlsli"
#include "util/data_ops.hlsli"

// Host mirror: VDP2NBGFetchPushConstants in src/gpu/vdp2/vdp2_bindings.hpp.
struct NBGFetchPushConstants {
    CommonRenderParams common;
    uint rows; // dispatch rows of the batch, from common.startY
};
[[vk::push_constant]] NBGFetchPushConstants g_push;

StructuredBuffer<LayerRenderParams> g_layerRenderParams : register(t0);
ByteAddressBuffer g_vram : register(t2);
StructuredBuffer<NBGLine> g_nbgLines : register(t7);
Texture2DArray<uint> g_spriteAttrsIn : register(t9);

RWStructuredBuffer<uint> g_nbgCharsOut : register(u5);

static const uint kGroupSize = 32;

// ---------------------------------------------------------------------------------------------------------------------
// Parameters (as cs_vdp2_render_bgs.hlsl)

static const uint interlaceMode = BitExtract(g_push.common.displayParams, 2, 2);
static const uint oddField = BitExtract(g_push.common.displayParams, 4, 1);
static const bool exclusiveMonitor = BitTest(g_push.common.displayParams, 5);
static const bool hiResH = BitTest(g_push.common.displayParams, 8);
static const bool deinterlace = BitTest(g_push.common.enhancements, 0);
static const uint2 outputSize = UnpackOutputSize(g_push.common.outputSize);

uint ReadVRAM16(uint address) {
    return Read16(g_vram, address & 0x7FFFE);
}

uint ReadVRAM32(uint address) {
    return Read32(g_vram, address & 0x7FFFC);
}

// The window row of an NBG row (cs_vdp2_render_bgs.hlsl NBGWindowY).
uint NBGWindowY(uint row) {
    const bool interlaced = interlaceMode == kInterlaceModeDoubleDensity;
    uint y = (!deinterlace && interlaced && !exclusiveMonitor) ? ((row << 1) | oddField) : row;
    if (deinterlace && interlaceMode == kInterlaceModeSingleDensity) {
        y >>= 1;
    }
    return y;
}

// ---------------------------------------------------------------------------------------------------------------------
// Windows: cs_vdp2_render_bgs.hlsl InsideWindows with the per-line parts (line window table reads, the vertical test,
// the horizontal range) evaluated once per row.

struct LineWindow {
    bool enabled;
    bool invert;
    bool insideY;
    int startX;
    int endX;
};

LineWindow PrepareLineWindow(GlobalWindowParams window, bool enabled, bool invert, uint y) {
    int2 start = window.start;
    int2 end = window.end;

    if (window.lineWindowTableEnable) {
        const uint address = window.lineWindowTableAddress + y * 4;
        start.x = ReadVRAM16(address + 0);
        end.x = ReadVRAM16(address + 2);
    }

    start.x = SignExtend(start.x, 16);
    end.x = SignExtend(end.x, 16);
    start.y = SignExtend(start.y, 16);
    end.y = SignExtend(end.y, 16);

    if (start.x < 0) {
        start.x = 0;
    }
    if (end.x < 0) {
        if (start.x >= end.x) {
            start.x = 0x3FF;
        }
        end.x = 0;
    }

    if (!hiResH) {
        start.x >>= 1;
        end.x >>= 1;
    }

    LineWindow w;
    w.enabled = enabled;
    w.invert = invert;
    w.insideY = int(y) >= start.y && int(y) <= end.y;
    w.startX = start.x;
    w.endX = end.x;
    return w;
}

bool InsideLineWindow(LineWindow w, uint x) {
    const bool inside = w.insideY && int(x) >= w.startX && int(x) <= w.endX;
    return inside != w.invert;
}

// ---------------------------------------------------------------------------------------------------------------------
// Character fetching (vdp2_front.cpp LocateNBGChar, FetchNBGCharacter: sw:5161-5191, 5230-5303)

struct NBGCharLocation {
    uint pageAddress;
    uint charIndex; // below 64 * 64
    uint cellX;
    uint bank;
};

NBGCharLocation LocateNBGChar(const NBGParams params, uint2 scrollPos) {
    const uint cellSizeShift = params.base.cellSizeShift;
    const uint2 pageShift = params.base.pageShift;

    const uint2 planePos = (scrollPos >> (pageShift + 9)) & 1u;
    const uint plane = planePos.x | (planePos.y << 1);
    const uint pageBaseAddress = params.pageBaseAddresses[plane];

    // HACK: apply data access shift here too (sw:5178-5181)
    const uint bank = BitExtract(pageBaseAddress, 17, 2);
    if (BitTest(params.base.vramDataOffset, bank)) {
        scrollPos.x += 8;
    }

    const uint2 pagePos = (scrollPos >> 9) & pageShift;
    const uint page = pagePos.x + (pagePos.y << 1);
    const uint pageSize = kPageSizes[cellSizeShift][params.base.twoWordChar ? 1 : 0];

    // As cs_vdp2_render_bgs.hlsl LocateScrollBGChar (FXC shift-merge workaround kept)
    const uint2 baseCharPatPos = (scrollPos >> 3) & 0x3F;
    const uint2 charPatPos = cellSizeShift != 0 ? (baseCharPatPos >> 1) : baseCharPatPos;

    NBGCharLocation loc;
    loc.pageAddress = pageBaseAddress + (page << pageSize);
    loc.charIndex = charPatPos.x + (charPatPos.y << (6 - cellSizeShift));
    loc.cellX = (scrollPos.x >> 3) & cellSizeShift;
    loc.bank = bank;
    return loc;
}

// The character at loc as ymir::vdp::Character::u32.
uint FetchNBGCharacter(const BaseBGParams params, NBGCharLocation loc) {
    if (params.twoWordChar) {
        const uint charAddress = loc.pageAddress + loc.charIndex * 4;
        if (!BitTest(params.patNameAccess, BitExtract(charAddress, 17, 2))) {
            return 0;
        }
        return ReadVRAM32(charAddress);
    }

    const uint charAddress = loc.pageAddress + loc.charIndex * 2;
    if (!BitTest(params.patNameAccess, BitExtract(charAddress, 17, 2))) {
        return 0;
    }
    const uint charData = ReadVRAM16(charAddress);

    const uint fourCellChar = params.cellSizeShift != 0 ? 1 : 0;
    const uint extChar = params.extChar ? 1 : 0;

    // Character number bit range from the 1-word character pattern data (charData)
    const uint baseCharNumMask = extChar != 0 ? 0xFFF : 0x3FF;
    const uint baseCharNumPos = 2 * fourCellChar;

    // Upper character number bit range from the supplementary character number (bgParams.supplCharNum)
    const uint supplCharNumStart = 2 * fourCellChar + 2 * extChar;
    const uint supplCharNumMask = 0x1Fu >> supplCharNumStart;
    const uint supplCharNumPos = 10 + supplCharNumStart;

    uint charNum = ((charData & baseCharNumMask) << baseCharNumPos) |
                   (((params.supplScrollCharNum >> supplCharNumStart) & supplCharNumMask) << supplCharNumPos);
    if (fourCellChar != 0) {
        charNum |= BitExtract(params.supplScrollCharNum, 0, 2);
    }
    const uint palNum = params.colorFormat != kColorFormatPalette16 ? BitExtract(charData, 12, 3) << 4
                                                                    : BitExtract(charData, 12, 4) | params.supplPalNum;
    const uint flipH = extChar == 0 && BitTest(charData, 10) ? 1 : 0;
    const uint flipV = extChar == 0 && BitTest(charData, 11) ? 1 : 0;

    return (charNum & 0x7FFF) | ((palNum & 0x7F) << 16) | ((params.supplSpecialColorCalc != 0 ? 1u : 0u) << 28) |
           ((params.supplSpecialPriority != 0 ? 1u : 0u) << 29) | (flipH << 30) | (flipV << 31);
}

// ---------------------------------------------------------------------------------------------------------------------
// Scroll position: cs_vdp2_render_bgs.hlsl NBGVCellScrollY and NBGScrollPos (the closed form of software's per-pixel
// scroll increments and vertical cell scroll reads, sw:4530-4581), so that the positions the table is computed at are
// the positions bgs draws.

uint NBGVCellScrollY(const NBGParams params, NBGLine nbgLine, uint index, uint x) {
    if (index > 1 || !params.vcellScrollEnable) {
        return 0;
    }

    // Table reads after the line's first one: one per cell column change between consecutive pixels (the increment is
    // below 8 pixels, so at most one per pixel)
    uint reads = 0;
    if (!params.base.mosaicEnable) {
        const uint fracScrollX = uint(nbgLine.fracScrollX) + params.scrollAmount.x;
        reads = ((fracScrollX & 0x7FF) + x * uint(nbgLine.scrollIncH)) >> 11;
    }

    int offset = int(reads);
    if (params.vcellScrollRepeat && offset > 0) {
        --offset;
    }
    if (params.vcellScrollDelay) {
        --offset;
    }
    if (offset < 0) {
        return nbgLine.prevLastVCellScroll;
    }

    const uint vcellScrollTableAddress = BitExtract(g_push.common.vcellScroll, 0, 19);
    const uint vcellScrollInc = BitExtract(g_push.common.vcellScroll, 19, 3) << 2u;
    const uint vcellAddress = vcellScrollTableAddress + params.vcellScrollOffset + uint(offset) * vcellScrollInc;
    return BitExtract(ReadVRAM32(vcellAddress), 8, 19);
}

uint2 NBGScrollPos(const NBGParams params, NBGLine nbgLine, uint index, uint x) {
    const uint fracScrollX = uint(nbgLine.fracScrollX) + params.scrollAmount.x + x * uint(nbgLine.scrollIncH);
    const uint fracScrollY = uint(nbgLine.fracScrollY) + params.scrollAmount.y + NBGVCellScrollY(params, nbgLine, index, x);
    return uint2(fracScrollX >> 8, fracScrollY >> 8);
}

// ---------------------------------------------------------------------------------------------------------------------
// Entrypoint
//
// The fetcher (sw:5204-5226) over the row's fetches f in order, with prev(f) the fetch before f:
// - f fetches a character ("new") when it is the first fetch or its character index differs from prev(f)'s;
// - a new f shows currChar = its delayed character (the character of the last new fetch before f, or the line's
//   nextChar) when its bank has the delay, else its own character, and nextChar becomes its own character;
// - a fetch that is not new keeps currChar, except with 2x2 characters, a delay and a cell column change from
//   lastCellX (the cell column of the last such fetch before it, none at the line start) to column 1: then currChar
//   becomes nextChar, the character of its run's new fetch.
// So a fetch shows its run start's currChar, or its run start's own character once a switch happened in the run. The
// pass evaluates that with prefix maxima over the row ("the last fetch / new fetch / ... at or before x").

static const uint kFetchBitCellX = 12;
static const uint kFetchBitDelay = 13;
static const uint kFetchBitFetches = 14;
static const uint kStartBitNew = 16; // gs_start between steps 2 and 3: bits 0-15 prev(x) + 1, bit 16 new

// Per pixel x of the row (shared memory, 5 x 2816 bytes plus the scan totals: kNBGFetchSharedMemorySize in
// src/gpu/vdp2/vdp2_bindings.hpp, checked against the SPIR-V by tests/unit/test_gpu_vdp2_pipelines.cpp):
groupshared uint gs_fetch[kMaxResH]; // bits 0-11 charIndex, 12 cellX, 13 delay, 14 fetches
groupshared uint gs_char[kMaxResH];  // the character at x's location (ymir::vdp::Character::u32)
groupshared uint gs_start[kMaxResH]; // prev(x) and new (step 2), then for a new fetch the currChar it shows
groupshared uint gs_scanA[kMaxResH]; // prefix maxima of positions + 1: the last fetch, then the last switch
groupshared uint gs_scanB[kMaxResH]; // prefix maxima of positions + 1: the last new fetch | the last lastCellX setter
groupshared uint gs_scanTotals[kGroupSize];

// Channel-wise maximum of two pairs of 16-bit values.
uint MaxPair(uint a, uint b) {
    return max(a & 0xFFFF, b & 0xFFFF) | (max(a >> 16, b >> 16) << 16);
}

// In-place inclusive prefix maximum (MaxPair) of gs_scanA (which == 0) or gs_scanB over [0, n): each thread scans a
// segment, then adds the maximum of the segments before it. Ends with a barrier.
void PrefixMax(uint which, uint lane, uint n) {
    const uint seg = (n + kGroupSize - 1) / kGroupSize;
    const uint start = min(lane * seg, n);
    const uint end = min(start + seg, n);
    uint run = 0;
    for (uint x = start; x < end; ++x) {
        run = MaxPair(run, which == 0 ? gs_scanA[x] : gs_scanB[x]);
        if (which == 0) {
            gs_scanA[x] = run;
        } else {
            gs_scanB[x] = run;
        }
    }
    // The segments' maxima, prefix maxima in log2(kGroupSize) steps
    gs_scanTotals[lane] = run;
    GroupMemoryBarrierWithGroupSync();
    [unroll]
    for (uint offset = 1; offset < kGroupSize; offset <<= 1) {
        const uint before = lane >= offset ? gs_scanTotals[lane - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_scanTotals[lane] = MaxPair(gs_scanTotals[lane], before);
        GroupMemoryBarrierWithGroupSync();
    }
    const uint prefix = lane > 0 ? gs_scanTotals[lane - 1] : 0;
    for (uint y = start; y < end; ++y) {
        if (which == 0) {
            gs_scanA[y] = MaxPair(gs_scanA[y], prefix);
        } else {
            gs_scanB[y] = MaxPair(gs_scanB[y], prefix);
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

[numthreads(32, 1, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 threadId : SV_GroupThreadID) {
    // Every exit before the barriers is uniform across the group.
    const uint index = groupId.z;
    const uint lane = threadId.x;
    if (groupId.x >= g_push.rows || index > 3) {
        return;
    }
    const uint row = g_push.common.startY + groupId.x;
    if (row >= kMaxResV) {
        return;
    }
    const NBGParams params = g_layerRenderParams[0].nbg[index];
    if (!NBGUsesCharTable(params, index)) {
        return;
    }
    const NBGLine nbgLine = g_nbgLines[index * kMaxResV + row];
    if (nbgLine.mosaicSrcLine != row) {
        // Vertical mosaic: the row repeats another row's output, which has its own characters
        return;
    }
    const uint hres = min(outputSize.x, kMaxResH); // the groupshared arrays hold kMaxResH pixels

    // Per-line window state (InsideWindows: W0, W1 and the sprite window, combined with AND or OR)
    const LayerWindowParamsS windows = params.base.windowParams;
    const uint windowY = NBGWindowY(row);
    const LineWindow w0 = PrepareLineWindow(g_layerRenderParams[0].windows[0], windows.base.window0Enable,
                                            windows.base.window0Invert, windowY);
    const LineWindow w1 = PrepareLineWindow(g_layerRenderParams[0].windows[1], windows.base.window1Enable,
                                            windows.base.window1Invert, windowY);
    const bool anyWindow = windows.base.window0Enable || windows.base.window1Enable || windows.spriteWindowEnable;
    const bool windowLogicAND = windows.base.windowLogicAnd;
    const uint mosaicH = params.base.mosaicEnable ? BitExtract(g_push.common.layerParams, 14, 4) + 1 : 1;
    const bool fourCellChar = params.base.cellSizeShift != 0;

    // Brimir: matches software (sw:4557-4601, 5204-5226; vdp2_front.cpp DrawNBGFetcher and FetchNBGChar with
    // windows): the pipelined fetcher over the row, from its state at the line start (nextChar from the CPU,
    // lastCharIndex and lastCellX reset: vdp2_front.cpp PrepareLine and StoreNBGLines, sw:2531-2539).

    // 1. Every pixel: whether it fetches (the first pixel of its mosaic block, outside the windows, sw:4557-4586), its
    //    character location and the character there. Then gs_scanA: the last fetch at or before x.
    for (uint x1 = lane; x1 < hres; x1 += kGroupSize) {
        bool fetches = x1 % mosaicH == 0;
        if (fetches && anyWindow) {
            bool windowed = windowLogicAND;
            if (w0.enabled) {
                const bool inside = InsideLineWindow(w0, x1);
                windowed = windowLogicAND ? (windowed && inside) : (windowed || inside);
            }
            if (w1.enabled) {
                const bool inside = InsideLineWindow(w1, x1);
                windowed = windowLogicAND ? (windowed && inside) : (windowed || inside);
            }
            if (windows.spriteWindowEnable) {
                const bool inside = BitTest(g_spriteAttrsIn[uint3(x1, windowY, 0)], kSpriteAttrBitShadowWindow) !=
                                    windows.spriteWindowInvert;
                windowed = windowLogicAND ? (windowed && inside) : (windowed || inside);
            }
            fetches = !windowed;
        }
        uint fetch = 0;
        uint ch = 0;
        if (fetches) {
            const NBGCharLocation loc = LocateNBGChar(params, NBGScrollPos(params, nbgLine, index, x1));
            ch = FetchNBGCharacter(params.base, loc);
            fetch = loc.charIndex | (loc.cellX << kFetchBitCellX) |
                    ((BitTest(params.base.charPatDelay, loc.bank) ? 1u : 0u) << kFetchBitDelay) |
                    (1u << kFetchBitFetches);
        }
        gs_fetch[x1] = fetch;
        gs_char[x1] = ch;
        gs_scanA[x1] = fetches ? x1 + 1 : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    PrefixMax(0, lane, hres);

    // 2. prev(x) and the new fetches. gs_scanB: the last new fetch at or before x (the run start, low half) and the
    //    last fetch at or before x that is not new and has the delay (it sets lastCellX, high half).
    for (uint x2 = lane; x2 < hres; x2 += kGroupSize) {
        const uint fetch = gs_fetch[x2];
        uint channels = 0;
        uint start = 0;
        if (BitTest(fetch, kFetchBitFetches)) {
            const uint prev = x2 > 0 ? gs_scanA[x2 - 1] : 0;
            const bool isNew = prev == 0 || BitExtract(gs_fetch[prev - 1], 0, 12) != BitExtract(fetch, 0, 12);
            if (isNew) {
                channels = x2 + 1;
            } else if (BitTest(fetch, kFetchBitDelay)) {
                channels = (x2 + 1) << 16;
            }
            start = prev | ((isNew ? 1u : 0u) << kStartBitNew);
        }
        gs_start[x2] = start;
        gs_scanB[x2] = channels;
    }
    GroupMemoryBarrierWithGroupSync();
    PrefixMax(1, lane, hres);

    // 3. The currChar of each new fetch; gs_scanA: the last switch at or before x.
    for (uint x3 = lane; x3 < hres; x3 += kGroupSize) {
        const uint fetch = gs_fetch[x3];
        uint switchPos = 0;
        if (BitTest(fetch, kFetchBitFetches)) {
            const bool delay = BitTest(fetch, kFetchBitDelay);
            const uint prevNew = gs_start[x3];
            if (BitTest(prevNew, kStartBitNew)) {
                uint start = gs_char[x3];
                if (delay) {
                    // The character of the last new fetch before x3: the run start of prev(x3)
                    const uint prev = prevNew & 0xFFFF;
                    start = prev != 0 ? gs_char[(gs_scanB[prev - 1] & 0xFFFF) - 1] : nbgLine.nextChar;
                }
                gs_start[x3] = start;
            } else if (fourCellChar && delay && BitExtract(fetch, kFetchBitCellX, 1) == 1) {
                // lastCellX: the cell column of the last non-new delayed fetch before x3, none at the line start
                const uint last = x3 > 0 ? gs_scanB[x3 - 1] >> 16 : 0;
                const uint lastCellX = last != 0 ? BitExtract(gs_fetch[last - 1], kFetchBitCellX, 1) : 0xFF;
                if (lastCellX != 1) {
                    switchPos = x3 + 1;
                }
            }
        }
        gs_scanA[x3] = switchPos;
    }
    GroupMemoryBarrierWithGroupSync();
    if (fourCellChar) {
        PrefixMax(0, lane, hres);
    }

    // 4. Each fetch shows its run start's currChar, or the run start's own character after a switch in the run.
    const uint charBase = NBGCharIndex(index, row, 0);
    for (uint x4 = lane; x4 < hres; x4 += kGroupSize) {
        if (!BitTest(gs_fetch[x4], kFetchBitFetches)) {
            continue;
        }
        const uint runStart = (gs_scanB[x4] & 0xFFFF) - 1;
        const uint lastSwitch = gs_scanA[x4];
        const bool switched = lastSwitch != 0 && lastSwitch - 1 > runStart;
        g_nbgCharsOut[charBase + x4] = switched ? gs_char[runStart] : gs_start[runStart];
    }
}
