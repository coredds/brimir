#pragma once

// Brimir: vector helpers of the software renderer's compose fast path (SoftwareVDPRenderer::VDP2ComposeLine), with
// SSE2 (x64) and NEON (aarch64) bodies and a scalar tail. kVector = false runs the scalar code only (the tests compare
// both). See BRIMIR_FORK.md, "Compose fast path".

#include <ymir/core/types.hpp>
#include <ymir/hw/vdp/vdp2_defs.hpp>
#include <ymir/util/inline.hpp>

#include <algorithm>

#if defined(_M_X64) || defined(__x86_64__)
    #include <immintrin.h>
#elif defined(_M_ARM64) || defined(__aarch64__)
    #include <arm_neon.h>
#endif

namespace ymir::vdp::compose {
// Brimir: the layer sort of VDP2ComposeLine's fast path. Inserts one layer into the per-dot top-3 key lists (keys
// stored as three arrays, entry 0 topmost) where its priority is non-zero (and, for the sprite layer, its special type
// is Normal; special == nullptr for the other layers). A key is layerKey | (priority << 3) truncated to 8 bits, as
// upstream's push() computes it. Upstream inserts with "key > entry[i]" and shifts the rest down; for a sorted list
// that equals the max/min network below (an equal key leaves the same values either way), and a skipped dot is key 0,
// which the network leaves unchanged (keys are >= 0).
template <bool kVector = true>
FORCE_INLINE void ComposeSortInsert(uint8 *key0, uint8 *key1, uint8 *key2, const uint8 *priority,
                                    const SpriteData::Special *special, uint8 layerKey, uint32 count) {
    static_assert(static_cast<uint8>(SpriteData::Special::Normal) == 0);
    uint32 i = 0;

    if constexpr (kVector) {
#if defined(_M_X64) || defined(__x86_64__)
        const __m128i zero = _mm_setzero_si128();
        const __m128i keyBits = _mm_set1_epi8(static_cast<char>(layerKey));
        const __m128i prioMask = _mm_set1_epi8(static_cast<char>(0xF8));
        for (; i + 16 <= count; i += 16) {
            const __m128i prio = _mm_loadu_si128(reinterpret_cast<const __m128i *>(priority + i));
            // (priority << 3) & 0xFF per byte: the 16-bit shift moves bits across bytes only into the cleared low
            // 3 bits
            __m128i key = _mm_or_si128(_mm_and_si128(_mm_slli_epi16(prio, 3), prioMask), keyBits);
            key = _mm_andnot_si128(_mm_cmpeq_epi8(prio, zero), key);
            if (special != nullptr) {
                const __m128i spec = _mm_loadu_si128(reinterpret_cast<const __m128i *>(special + i));
                key = _mm_and_si128(_mm_cmpeq_epi8(spec, zero), key);
            }
            const __m128i e0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(key0 + i));
            const __m128i e1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(key1 + i));
            const __m128i e2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(key2 + i));
            const __m128i t0 = _mm_min_epu8(e0, key);
            const __m128i t1 = _mm_min_epu8(e1, t0);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(key0 + i), _mm_max_epu8(e0, key));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(key1 + i), _mm_max_epu8(e1, t0));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(key2 + i), _mm_max_epu8(e2, t1));
        }
#elif defined(_M_ARM64) || defined(__aarch64__)
        const uint8x16_t keyBits = vdupq_n_u8(layerKey);
        for (; i + 16 <= count; i += 16) {
            const uint8x16_t prio = vld1q_u8(priority + i);
            // vshlq_n_u8 truncates to 8 bits like the uint8 key
            uint8x16_t key = vorrq_u8(vshlq_n_u8(prio, 3), keyBits);
            key = vandq_u8(key, vtstq_u8(prio, prio));
            if (special != nullptr) {
                const uint8x16_t spec = vld1q_u8(reinterpret_cast<const uint8 *>(special + i));
                key = vandq_u8(key, vceqq_u8(spec, vdupq_n_u8(0)));
            }
            const uint8x16_t e0 = vld1q_u8(key0 + i);
            const uint8x16_t e1 = vld1q_u8(key1 + i);
            const uint8x16_t e2 = vld1q_u8(key2 + i);
            const uint8x16_t t0 = vminq_u8(e0, key);
            const uint8x16_t t1 = vminq_u8(e1, t0);
            vst1q_u8(key0 + i, vmaxq_u8(e0, key));
            vst1q_u8(key1 + i, vmaxq_u8(e1, t0));
            vst1q_u8(key2 + i, vmaxq_u8(e2, t1));
        }
#endif
    }

    for (; i < count; i++) {
        uint8 key = static_cast<uint8>(layerKey | (priority[i] << 3u));
        if (priority[i] == 0 || (special != nullptr && special[i] != SpriteData::Special::Normal)) {
            key = 0;
        }
        const uint8 e0 = key0[i];
        const uint8 e1 = key1[i];
        const uint8 t0 = std::min(e0, key);
        const uint8 t1 = std::min(e1, t0);
        key0[i] = std::max(e0, key);
        key1[i] = std::max(e1, t0);
        key2[i] = std::max(key2[i], t1);
    }
}

// Brimir: a stack entry of the fast path's sort keys into the layer index (~key & 7) and priority (key >> 3) arrays,
// as upstream converts each entry.
template <bool kVector = true>
FORCE_INLINE void ComposeKeysToStack(const uint8 *keys, uint8 *layers,
                                     uint8 *priorities, uint32 count) {
    uint32 i = 0;
    if constexpr (kVector) {
#if defined(_M_X64) || defined(__x86_64__)
        const __m128i layerMask = _mm_set1_epi8(7);
        const __m128i prioMask = _mm_set1_epi8(0x1F);
        for (; i + 16 <= count; i += 16) {
            const __m128i key = _mm_loadu_si128(reinterpret_cast<const __m128i *>(keys + i));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(layers + i), _mm_andnot_si128(key, layerMask));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(priorities + i),
                             _mm_and_si128(_mm_srli_epi16(key, 3), prioMask));
        }
#elif defined(_M_ARM64) || defined(__aarch64__)
        for (; i + 16 <= count; i += 16) {
            const uint8x16_t key = vld1q_u8(keys + i);
            vst1q_u8(layers + i, vbicq_u8(vdupq_n_u8(7), key));
            vst1q_u8(priorities + i, vshrq_n_u8(key, 3));
        }
#endif
    }
    for (; i < count; i++) {
        layers[i] = static_cast<uint8>(~keys[i] & 7u);
        priorities[i] = keys[i] >> 3u;
    }
}

// Brimir: layer 0 flags of VDP2ComposeLine's fast path, one layer at a time: for the dots whose top stack entry (keys,
// the sort keys of entry 0) is `layer`, ORs in the colour calculation flag (colorCalc[x]), the shadow enable
// (shadowSource[x], or shadowConstant when null) and the colour offset enable. The flag arrays start all false.
template <bool kVector = true>
FORCE_INLINE void ComposeGatherLayerFlags(const uint8 *keys, uint32 layer, const bool *colorCalc,
                                          const bool *shadowSource, bool shadowConstant,
                                          bool colorOffsetConstant, bool *colorCalcOut, bool *shadowOut,
                                          bool *colorOffsetOut, uint32 count) {
    static_assert(sizeof(bool) == 1);
    uint32 i = 0;
    if constexpr (kVector) {
#if defined(_M_X64) || defined(__x86_64__)
        const __m128i layerMask = _mm_set1_epi8(7);
        const __m128i layerValue = _mm_set1_epi8(static_cast<char>(layer));
        const __m128i shadowValue = _mm_set1_epi8(shadowConstant ? 1 : 0);
        const __m128i colorOffsetValue = _mm_set1_epi8(colorOffsetConstant ? 1 : 0);
        for (; i + 16 <= count; i += 16) {
            const __m128i key = _mm_loadu_si128(reinterpret_cast<const __m128i *>(keys + i));
            const __m128i match = _mm_cmpeq_epi8(_mm_andnot_si128(key, layerMask), layerValue);
            const __m128i cc = _mm_loadu_si128(reinterpret_cast<const __m128i *>(colorCalc + i));
            const __m128i shadow = shadowSource != nullptr
                                       ? _mm_loadu_si128(reinterpret_cast<const __m128i *>(shadowSource + i))
                                       : shadowValue;
            __m128i *ccOut = reinterpret_cast<__m128i *>(colorCalcOut + i);
            __m128i *shadowOutV = reinterpret_cast<__m128i *>(shadowOut + i);
            __m128i *offsetOut = reinterpret_cast<__m128i *>(colorOffsetOut + i);
            _mm_storeu_si128(ccOut, _mm_or_si128(_mm_loadu_si128(ccOut), _mm_and_si128(match, cc)));
            _mm_storeu_si128(shadowOutV, _mm_or_si128(_mm_loadu_si128(shadowOutV), _mm_and_si128(match, shadow)));
            _mm_storeu_si128(offsetOut,
                             _mm_or_si128(_mm_loadu_si128(offsetOut), _mm_and_si128(match, colorOffsetValue)));
        }
#elif defined(_M_ARM64) || defined(__aarch64__)
        const uint8x16_t layerMask = vdupq_n_u8(7);
        const uint8x16_t layerValue = vdupq_n_u8(static_cast<uint8>(layer));
        const uint8x16_t shadowValue = vdupq_n_u8(shadowConstant ? 1 : 0);
        const uint8x16_t colorOffsetValue = vdupq_n_u8(colorOffsetConstant ? 1 : 0);
        for (; i + 16 <= count; i += 16) {
            const uint8x16_t key = vld1q_u8(keys + i);
            const uint8x16_t match = vceqq_u8(vbicq_u8(layerMask, key), layerValue);
            const uint8x16_t cc = vld1q_u8(reinterpret_cast<const uint8 *>(colorCalc + i));
            const uint8x16_t shadow =
                shadowSource != nullptr ? vld1q_u8(reinterpret_cast<const uint8 *>(shadowSource + i)) : shadowValue;
            uint8 *ccOut = reinterpret_cast<uint8 *>(colorCalcOut + i);
            uint8 *shadowOutB = reinterpret_cast<uint8 *>(shadowOut + i);
            uint8 *offsetOut = reinterpret_cast<uint8 *>(colorOffsetOut + i);
            vst1q_u8(ccOut, vorrq_u8(vld1q_u8(ccOut), vandq_u8(match, cc)));
            vst1q_u8(shadowOutB, vorrq_u8(vld1q_u8(shadowOutB), vandq_u8(match, shadow)));
            vst1q_u8(offsetOut, vorrq_u8(vld1q_u8(offsetOut), vandq_u8(match, colorOffsetValue)));
        }
#endif
    }
    for (; i < count; i++) {
        if ((~keys[i] & 7u) == layer) {
            colorCalcOut[i] = colorCalcOut[i] || colorCalc[i];
            shadowOut[i] = shadowOut[i] || (shadowSource != nullptr ? shadowSource[i] : shadowConstant);
            colorOffsetOut[i] = colorOffsetOut[i] || colorOffsetConstant;
        }
    }
}

// Brimir: completes the fast path's layer 0 flags: no colour calculation inside the colour calculation window; shadow
// only where the sprite priority is not below the top layer's (keys >> 3) and the sprite dot is a normal shadow or
// (msbShadow: no sprite window) has the shadow/window bit set.
template <bool kVector = true>
FORCE_INLINE void ComposeFinishLayer0Flags(const uint8 *keys, const bool *colorCalcWindow,
                                           const uint8 *spritePriority, const SpriteData::Special *spriteSpecial,
                                           const bool *spriteShadowOrWindow, bool msbShadow,
                                           bool *colorCalcOut, bool *shadowOut, uint32 count) {
    static_assert(static_cast<uint8>(SpriteData::Special::Shadow) == 1);
    uint32 i = 0;
    if constexpr (kVector) {
#if defined(_M_X64) || defined(__x86_64__)
        const __m128i one = _mm_set1_epi8(1);
        const __m128i prioMask = _mm_set1_epi8(0x1F);
        const __m128i msbMask = msbShadow ? _mm_set1_epi8(-1) : _mm_setzero_si128();
        for (; i + 16 <= count; i += 16) {
            __m128i *ccOut = reinterpret_cast<__m128i *>(colorCalcOut + i);
            __m128i *shadowOutV = reinterpret_cast<__m128i *>(shadowOut + i);
            const __m128i window = _mm_loadu_si128(reinterpret_cast<const __m128i *>(colorCalcWindow + i));
            _mm_storeu_si128(ccOut, _mm_andnot_si128(window, _mm_loadu_si128(ccOut)));

            const __m128i key = _mm_loadu_si128(reinterpret_cast<const __m128i *>(keys + i));
            const __m128i topPriority = _mm_and_si128(_mm_srli_epi16(key, 3), prioMask);
            const __m128i priority = _mm_loadu_si128(reinterpret_cast<const __m128i *>(spritePriority + i));
            const __m128i notBelow = _mm_cmpeq_epi8(_mm_max_epu8(priority, topPriority), priority);
            const __m128i special = _mm_loadu_si128(reinterpret_cast<const __m128i *>(spriteSpecial + i));
            const __m128i sow = _mm_loadu_si128(reinterpret_cast<const __m128i *>(spriteShadowOrWindow + i));
            const __m128i shadowDot =
                _mm_or_si128(_mm_cmpeq_epi8(special, one), _mm_and_si128(msbMask, _mm_cmpeq_epi8(sow, one)));
            _mm_storeu_si128(shadowOutV,
                             _mm_and_si128(_mm_loadu_si128(shadowOutV), _mm_and_si128(notBelow, shadowDot)));
        }
#elif defined(_M_ARM64) || defined(__aarch64__)
        const uint8x16_t one = vdupq_n_u8(1);
        const uint8x16_t msbMask = vdupq_n_u8(msbShadow ? 0xFF : 0x00);
        for (; i + 16 <= count; i += 16) {
            uint8 *ccOut = reinterpret_cast<uint8 *>(colorCalcOut + i);
            uint8 *shadowOutB = reinterpret_cast<uint8 *>(shadowOut + i);
            const uint8x16_t window = vld1q_u8(reinterpret_cast<const uint8 *>(colorCalcWindow + i));
            vst1q_u8(ccOut, vbicq_u8(vld1q_u8(ccOut), window));

            const uint8x16_t topPriority = vshrq_n_u8(vld1q_u8(keys + i), 3);
            const uint8x16_t notBelow = vcgeq_u8(vld1q_u8(spritePriority + i), topPriority);
            const uint8x16_t special = vld1q_u8(reinterpret_cast<const uint8 *>(spriteSpecial + i));
            const uint8x16_t sow = vld1q_u8(reinterpret_cast<const uint8 *>(spriteShadowOrWindow + i));
            const uint8x16_t shadowDot = vorrq_u8(vceqq_u8(special, one), vandq_u8(msbMask, vceqq_u8(sow, one)));
            vst1q_u8(shadowOutB, vandq_u8(vld1q_u8(shadowOutB), vandq_u8(notBelow, shadowDot)));
        }
#endif
    }
    for (; i < count; i++) {
        colorCalcOut[i] = colorCalcOut[i] && !colorCalcWindow[i];
        const bool shadowDot =
            spriteSpecial[i] == SpriteData::Special::Shadow || (msbShadow && spriteShadowOrWindow[i]);
        shadowOut[i] = shadowOut[i] && spritePriority[i] >= (keys[i] >> 3u) && shadowDot;
    }
}


} // namespace ymir::vdp::compose
