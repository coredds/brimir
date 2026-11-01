// Brimir: the per-unit ordered resolve, shared by the native resolve (cs_vdp1_resolve.hlsl, GPU stage 2) and the N x
// resolve (cs_vdp1_resolve_nx.hlsl, GPU stage 3). Written from the CPU reference's Resolve
// (tests/unit/vdp1_raster_reference.cpp; vdp1_nx_reference.cpp per N x word). Brimir only.
//
// Dispatched indirectly (g_batchState kBatchStateResolveArgs) over the batch's unit range [unitLo, unitHi): with G =
// min(unitHi - unitLo, 4096) groups, thread t of group g owns units u = unitLo + G (t + 64 r) + g, in rounds r = 0, 1, ..
// while u < unitHi (one round for a native range of at most 64 G units; up to N^2 for an N x range). The words of a group
// lie G apart, so the deep stacks around one apex, on neighbouring words, are resolved by different groups in parallel
// instead of one after another by one group. Units outside the range have no fragment. A unit's fragments are the slots
// in its bucket, g_bucket[begin, begin + n) with n = g_unitCounts[u] and begin + n = g_scanSums[u / 64] +
// g_unitOffsets[u] (after the scatter pass). The slots are the order keys, distinct, in arbitrary order; applied in
// ascending order with the write half of VDP1PlotPixel (ApplyFragment) to the unit's (main, mesh) word pair they give
// software's result. The pass then clears g_unitCounts[u] for the next batch.
// - n <= kResolveSmallMax (32): the owning thread loads the slots into registers, insertion-sorts and applies them.
// - Larger units (apex stacks reach thousands) are queued in groupshared memory and resolved by the whole group, one
//   unit at a time, in windows of at most kResolveSortCapacity (2048) keys, in key order. The next window [lo, hi) is:
//   all remaining keys when they fit; otherwise the longest run of bins, from lo, of a kResolveHistogramBins-bin
//   histogram of the remaining keys whose total fits. A histogram serves windows until its bins are used up; when its
//   next bin alone holds too many keys, a histogram of just that bin replaces it (a bin of width 1 holds at most one
//   key, so this ends). The window's keys are gathered into groupshared memory, bitonic-sorted and applied in order by
//   thread 0, kResolveFoldChunk fragments at a time (the group loads each chunk). Exact for any n. Each window costs
//   one coalesced pass over the bucket (the gather) plus the histograms; every pass loads four keys per thread at a
//   time.
//
// The including shader defines VDP1_RESOLVE_NATIVE (1: the native resolve, whose range is one round of 64 G units and
// is compiled without the round loop; 0: N x) and declares g_batch, g_fragments, g_unitCounts, g_bucket, g_scanSums,
// g_unitOffsets and g_batchState before including this file, and defines before including it:
//   uint ResolveUnitLimit()                                    units at or above it are not resolved
//   void ResolveUnitWords(uint unit, uint fb, out uint mainAddress, out uint meshAddress)   the unit's word pair
//   uint ResolveReadWord(uint address)
//   void ResolveReplaceWord(uint address, uint oldValue, uint newValue)
//   void ResolveApply(inout uint m, inout uint s, uint target, uint color)   ApplyFragment on the unit's target
#ifndef BRIMIR_VDP1_RESOLVE_HLSLI
#define BRIMIR_VDP1_RESOLVE_HLSLI

#include "vdp1_defs.hlsli"
#include "vdp1_params.hlsli"

static const uint kSmallMax = 32;       // vdp1_bindings.hpp kResolveSmallMax
static const uint kSortCapacity = 2048; // kResolveSortCapacity
static const uint kBins = 256;          // kResolveHistogramBins
static const uint kFoldChunk = 256;     // kResolveFoldChunk
static const uint kNoKey = 0xFFFFFFFFu;

// 11,536 bytes (vdp1_bindings.hpp kResolveSharedMemorySize)
groupshared uint gs_keys[kSortCapacity];
groupshared uint gs_hist[kBins];
groupshared uint gs_large[64];
groupshared uint gs_fragTarget[kFoldChunk];
groupshared uint gs_fragColor[kFoldChunk];
groupshared uint gs_largeCount;
groupshared uint gs_count;
groupshared uint gs_found;
groupshared uint gs_maxKey;

struct UnitInfo {
    uint mainAddress, meshAddress;
    uint begin, count;
};

UnitInfo GetUnit(uint unit, uint fb) {
    uint offsets, sums, stride;
    g_unitOffsets.GetDimensions(offsets, stride);
    g_scanSums.GetDimensions(sums, stride);
    UnitInfo info;
    ResolveUnitWords(unit, fb, info.mainAddress, info.meshAddress);
    info.count = g_unitCounts[unit];
    const uint end = (unit < offsets && (unit >> 6) < sums) ? g_scanSums[unit >> 6] + g_unitOffsets[unit] : 0;
    info.count = min(info.count, end);
    info.begin = end - info.count;
    return info;
}

// Bucket entry i of the unit, or kNoKey outside the unit or the buffer
uint BucketKey(UnitInfo info, uint i) {
    uint capacity, stride;
    g_bucket.GetDimensions(capacity, stride);
    const uint index = info.begin + i;
    return i < info.count && index < capacity ? g_bucket[index] : kNoKey;
}

void LoadFragment(uint key, out uint target, out uint color) {
    uint capacity, stride;
    g_fragments.GetDimensions(capacity, stride);
    target = 0;
    color = 0;
    if (key < capacity) {
        const Fragment f = g_fragments[key];
        target = f.target;
        color = f.color;
    }
}

void ResolveSmall(UnitInfo info) {
    uint keys[kSmallMax];
    const uint n = min(info.count, kSmallMax);
    for (uint i = 0; i < n; ++i) {
        const uint key = BucketKey(info, i);
        uint j = i;
        while (j > 0 && keys[j - 1] > key) {
            keys[j] = keys[j - 1];
            --j;
        }
        keys[j] = key;
    }
    const uint m0 = ResolveReadWord(info.mainAddress);
    const uint s0 = ResolveReadWord(info.meshAddress);
    uint m = m0;
    uint s = s0;
    for (uint k = 0; k < n; ++k) {
        uint target, color;
        LoadFragment(keys[k], target, color);
        if (Vdp1FragLive(target)) {
            ResolveApply(m, s, target, color);
        }
    }
    ResolveReplaceWord(info.mainAddress, m0, m);
    ResolveReplaceWord(info.meshAddress, s0, s);
}

// The histogram of the unit's keys in [hLo, hHi), bins of width w (the whole group; ends with a barrier)
void BuildHistogram(UnitInfo info, uint t, uint hLo, uint hHi, uint w) {
    for (uint b = t; b < kBins; b += 64) {
        gs_hist[b] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint i = t; i < info.count; i += 4 * 64) {
        uint keys[4];
        [unroll] for (uint j = 0; j < 4; ++j) {
            keys[j] = BucketKey(info, i + j * 64);
        }
        [unroll] for (uint j2 = 0; j2 < 4; ++j2) {
            if (keys[j2] >= hLo && keys[j2] < hHi) {
                InterlockedAdd(gs_hist[(keys[j2] - hLo) / w], 1);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
}

// The whole group resolves one large unit (uniform control flow: every thread calls this with the same unit)
void ResolveLarge(uint unit, uint fb, uint t) {
    const UnitInfo info = GetUnit(unit, fb);
    const uint n = info.count;
    if (t == 0) {
        gs_maxKey = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    {
        uint localMax = 0;
        for (uint i = t; i < n; i += 4 * 64) {
            [unroll] for (uint j = 0; j < 4; ++j) {
                const uint key = BucketKey(info, i + j * 64);
                if (key != kNoKey) {
                    localMax = max(localMax, key);
                }
            }
        }
        InterlockedMax(gs_maxKey, localMax);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint maxKey = gs_maxKey;

    uint m0 = 0, s0 = 0, m = 0, s = 0;
    if (t == 0) {
        m0 = ResolveReadWord(info.mainAddress);
        s0 = ResolveReadWord(info.meshAddress);
        m = m0;
        s = s0;
    }

    uint lo = 0;   // the keys below lo are applied
    uint done = 0; // ... and there are `done` of them
    // The current histogram: [hLo, hHi) in bins of width w; bin `cur` starts at lo; none yet
    uint hLo = 0, hHi = 0, w = 1, cur = kBins;
    while (done < n && lo <= maxKey) {
        // The window [lo, hi)
        uint hi = maxKey + 1;
        if (n - done > kSortCapacity) {
            uint rebuildHi = maxKey + 1;
            for (;;) {
                if (cur >= kBins || lo >= hHi) {
                    hLo = lo;
                    hHi = rebuildHi;
                    w = max((hHi - hLo + kBins - 1) / kBins, 1u);
                    cur = 0;
                    BuildHistogram(info, t, hLo, hHi, w);
                }
                if (t == 0) {
                    uint sum = 0;
                    uint e = cur;
                    while (e < kBins && hLo + e * w < hHi && sum + gs_hist[e] <= kSortCapacity) {
                        sum += gs_hist[e];
                        ++e;
                    }
                    gs_found = e;
                }
                GroupMemoryBarrierWithGroupSync();
                const uint e = gs_found;
                if (e > cur) {
                    hi = min(hLo + e * w, hHi);
                    cur = e;
                    break;
                }
                // Bin `cur` alone holds too many keys: a histogram of just that bin
                rebuildHi = min(lo + w, hHi);
                cur = kBins;
            }
        }

        // Gather the window's keys
        if (t == 0) {
            gs_count = 0;
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint i = t; i < n; i += 4 * 64) {
            uint keys[4];
            [unroll] for (uint j = 0; j < 4; ++j) {
                keys[j] = BucketKey(info, i + j * 64);
            }
            [unroll] for (uint j2 = 0; j2 < 4; ++j2) {
                if (keys[j2] >= lo && keys[j2] < hi) {
                    uint slot;
                    InterlockedAdd(gs_count, 1, slot);
                    if (slot < kSortCapacity) {
                        gs_keys[slot] = keys[j2];
                    }
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
        const uint count = min(gs_count, kSortCapacity);
        uint padded = 1;
        while (padded < count) {
            padded <<= 1;
        }
        for (uint p = count + t; p < padded; p += 64) {
            gs_keys[p] = kNoKey;
        }
        GroupMemoryBarrierWithGroupSync();

        // Bitonic sort, ascending: each step compares padded / 2 pairs (i, i + stride), up to 16 per thread
        const uint pairs = padded >> 1;
        for (uint size = 2; size <= padded; size <<= 1) {
            for (uint stride = size >> 1; stride > 0; stride >>= 1) {
                [unroll] for (uint k = 0; k < kSortCapacity / 128; ++k) {
                    const uint pair = t + k * 64;
                    if (pair < pairs) {
                        const uint i = ((pair & ~(stride - 1)) << 1) | (pair & (stride - 1));
                        const uint j = i | stride;
                        const uint a = gs_keys[i];
                        const uint b = gs_keys[j];
                        if ((a > b) == ((i & size) == 0)) {
                            gs_keys[i] = b;
                            gs_keys[j] = a;
                        }
                    }
                }
                GroupMemoryBarrierWithGroupSync();
            }
        }

        // Apply in order, kFoldChunk fragments at a time
        for (uint base = 0; base < count; base += kFoldChunk) {
            [unroll] for (uint j = 0; j < kFoldChunk / 64; ++j) {
                const uint c = t + j * 64;
                if (base + c < count) {
                    uint target, color;
                    LoadFragment(gs_keys[base + c], target, color);
                    gs_fragTarget[c] = target;
                    gs_fragColor[c] = color;
                }
            }
            GroupMemoryBarrierWithGroupSync();
            if (t == 0) {
                // the next fragment is read while the current one is applied
                const uint chunk = min(kFoldChunk, count - base);
                uint nextTarget = gs_fragTarget[0];
                uint nextColor = gs_fragColor[0];
                for (uint c = 0; c < chunk; ++c) {
                    const uint target = nextTarget;
                    const uint color = nextColor;
                    const uint after = min(c + 1, kFoldChunk - 1);
                    nextTarget = gs_fragTarget[after];
                    nextColor = gs_fragColor[after];
                    if (Vdp1FragLive(target)) {
                        ResolveApply(m, s, target, color);
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }

        done += count;
        if (count == 0 && hi > maxKey) {
            break; // a bucket holding keys it should not (cannot happen with the raster pass's output)
        }
        lo = hi;
    }

    if (t == 0) {
        ResolveReplaceWord(info.mainAddress, m0, m);
        ResolveReplaceWord(info.meshAddress, s0, s);
        g_unitCounts[unit] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
}

[numthreads(64, 1, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    const uint t = tid.x;
    uint words, wordStride;
    g_batchState.GetDimensions(words, wordStride);
    const uint unitLo = words > kBatchStateUnitHi ? g_batchState[kBatchStateUnitLo] : 0;
    const uint unitHi = words > kBatchStateUnitHi ? g_batchState[kBatchStateUnitHi] : 0;
    const uint groups = words > kBatchStateResolveArgs ? g_batchState[kBatchStateResolveArgs] : 0;
    const uint fb = g_batch.drawFB & 1;
    const uint limit = ResolveUnitLimit();
    uint counts, stride;
    g_unitCounts.GetDimensions(counts, stride);
#if VDP1_RESOLVE_NATIVE
    // A native range (at most 64 G units) is one round: no loop, as before the N x passes
    const uint rounds = 1;
    [unroll] for (uint r = 0; r < 1; ++r) {
#else
    // Rounds of 64 G units (uniform over the group)
    const uint range = unitHi > unitLo ? unitHi - unitLo : 0;
    const uint perRound = groups * 64;
    const uint rounds = perRound != 0 ? (range + perRound - 1) / perRound : 0;

    for (uint r = 0; r < rounds; ++r) {
#endif
        if (t == 0) {
            gs_largeCount = 0;
        }
        GroupMemoryBarrierWithGroupSync();

        const uint unit = unitLo + (t + 64 * r) * groups + gid.x;
        if (unit < unitHi && unit < limit && unit < counts && g_unitCounts[unit] != 0) {
            const UnitInfo info = GetUnit(unit, fb);
            if (info.count <= kSmallMax) {
                ResolveSmall(info);
                g_unitCounts[unit] = 0;
            } else {
                // cleared by ResolveLarge once every thread has read the count
                uint index;
                InterlockedAdd(gs_largeCount, 1, index);
                gs_large[index] = unit;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        const uint large = gs_largeCount;
        for (uint li = 0; li < large; ++li) {
            ResolveLarge(gs_large[li], fb, t);
        }
        // Before the next round resets gs_largeCount (uniform: rounds is the group's); a native range has one round and
        // pays no extra barrier
        if (r + 1 < rounds) {
            GroupMemoryBarrierWithGroupSync();
        }
    }
}

#endif
