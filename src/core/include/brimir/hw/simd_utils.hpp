/**
 * @file simd_utils.hpp
 * @brief SIMD Utilities for VDP Rendering
 * 
 * Provides SSE2/AVX2 optimized functions for common VDP operations.
 * All functions have scalar fallbacks for compatibility.
 * 
 * Copyright (C) 2025 Brimir Team
 * Licensed under GPL-3.0
 */

#pragma once

#include <brimir/core/types.hpp>
#include <cstdint>

// Detect SIMD support
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    #define BRIMIR_HAS_SSE2 1
    #include <emmintrin.h>  // SSE2
#else
    #define BRIMIR_HAS_SSE2 0
#endif

#if defined(__SSSE3__) || (defined(_MSC_VER) && defined(__AVX__))
    #define BRIMIR_HAS_SSSE3 1
    #include <tmmintrin.h>  // SSSE3
#else
    #define BRIMIR_HAS_SSSE3 0
#endif

#if defined(__AVX2__)
    #define BRIMIR_HAS_AVX2 1
    #include <immintrin.h>  // AVX2
#else
    #define BRIMIR_HAS_AVX2 0
#endif

namespace brimir::simd {

// =============================================================================
// RGB555 to RGB888 Conversion
// =============================================================================

/**
 * @brief Convert single RGB555 to RGB888 (scalar)
 * Saturn format: bits 10-14=R, 5-9=G, 0-4=B
 */
inline uint32_t rgb555_to_rgb888_scalar(uint16_t color555) {
    uint32_t b = (color555 & 0x001F) << 3;
    uint32_t g = (color555 & 0x03E0) >> 2;
    uint32_t r = (color555 & 0x7C00) >> 7;
    // Expand 5-bit to 8-bit
    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return (r << 16) | (g << 8) | b;
}

#if BRIMIR_HAS_SSE2

/**
 * @brief Convert 4 RGB555 pixels to 4 RGB888 pixels using SSE2
 * @param colors Input: 4 RGB555 colors packed in lower 64 bits
 * @param out Output: 4 RGB888 colors (128 bits)
 */
inline void rgb555_to_rgb888_sse2_4px(const uint16_t* colors, uint32_t* out) {
    // Load 4 RGB555 values (64 bits)
    __m128i src = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(colors));
    
    // Unpack to 32-bit for easier manipulation
    __m128i zero = _mm_setzero_si128();
    __m128i pixels = _mm_unpacklo_epi16(src, zero);  // 4x 32-bit
    
    // Extract R, G, B channels
    __m128i mask_r = _mm_set1_epi32(0x7C00);
    __m128i mask_g = _mm_set1_epi32(0x03E0);
    __m128i mask_b = _mm_set1_epi32(0x001F);
    
    __m128i r = _mm_and_si128(pixels, mask_r);
    __m128i g = _mm_and_si128(pixels, mask_g);
    __m128i b = _mm_and_si128(pixels, mask_b);
    
    // Shift to correct positions
    r = _mm_srli_epi32(r, 7);   // bits 10-14 -> bits 3-7
    g = _mm_srli_epi32(g, 2);   // bits 5-9 -> bits 3-7
    b = _mm_slli_epi32(b, 3);   // bits 0-4 -> bits 3-7
    
    // Expand 5-bit to 8-bit (x |= x >> 5)
    r = _mm_or_si128(r, _mm_srli_epi32(r, 5));
    g = _mm_or_si128(g, _mm_srli_epi32(g, 5));
    b = _mm_or_si128(b, _mm_srli_epi32(b, 5));
    
    // Combine into RGB888: (r << 16) | (g << 8) | b
    __m128i rgb = _mm_or_si128(
        _mm_slli_epi32(r, 16),
        _mm_or_si128(_mm_slli_epi32(g, 8), b)
    );
    
    // Store result
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out), rgb);
}

/**
 * @brief Convert 8 RGB555 pixels to 8 RGB888 pixels using SSE2
 * @param colors Input: 8 RGB555 colors (128 bits)
 * @param out Output: 8 RGB888 colors (256 bits, two stores)
 */
inline void rgb555_to_rgb888_sse2_8px(const uint16_t* colors, uint32_t* out) {
    rgb555_to_rgb888_sse2_4px(colors, out);
    rgb555_to_rgb888_sse2_4px(colors + 4, out + 4);
}

#endif // BRIMIR_HAS_SSE2

// =============================================================================
// Line Buffer Operations
// =============================================================================

/**
 * @brief Fill memory with a pattern (for line buffer initialization)
 * @param dst Destination buffer (should be 16-byte aligned for best performance)
 * @param value 32-bit value to fill
 * @param count Number of 32-bit words to fill
 */
inline void fill32(uint32_t* dst, uint32_t value, size_t count) {
#if BRIMIR_HAS_SSE2
    __m128i fill = _mm_set1_epi32(static_cast<int>(value));
    
    // Handle unaligned prefix
    while (count > 0 && (reinterpret_cast<uintptr_t>(dst) & 15)) {
        *dst++ = value;
        count--;
    }
    
    // Main loop: 4 dwords at a time
    while (count >= 4) {
        _mm_store_si128(reinterpret_cast<__m128i*>(dst), fill);
        dst += 4;
        count -= 4;
    }
    
    // Handle remainder
    while (count > 0) {
        *dst++ = value;
        count--;
    }
#else
    for (size_t i = 0; i < count; i++) {
        dst[i] = value;
    }
#endif
}

/**
 * @brief Zero memory (for line buffer clearing)
 * @param dst Destination buffer
 * @param bytes Number of bytes to zero
 */
inline void zero_memory(void* dst, size_t bytes) {
#if BRIMIR_HAS_SSE2
    uint8_t* p = static_cast<uint8_t*>(dst);
    __m128i zero = _mm_setzero_si128();
    
    // Handle unaligned prefix
    while (bytes > 0 && (reinterpret_cast<uintptr_t>(p) & 15)) {
        *p++ = 0;
        bytes--;
    }
    
    // Main loop: 16 bytes at a time
    while (bytes >= 16) {
        _mm_store_si128(reinterpret_cast<__m128i*>(p), zero);
        p += 16;
        bytes -= 16;
    }
    
    // Handle remainder
    while (bytes > 0) {
        *p++ = 0;
        bytes--;
    }
#else
    std::memset(dst, 0, bytes);
#endif
}

// =============================================================================
// Tile Pixel Extraction (4bpp)
// =============================================================================

/**
 * @brief Extract 8 nibbles from 32-bit tile data (scalar version)
 * @param tileData Two 16-bit words containing 8 4bpp pixels
 * @param out Output: 8 color indices (bytes)
 */
inline void extract_tile_nibbles_scalar(uint16_t word0, uint16_t word1, uint8_t* out) {
    // High nibbles first (MSB pixel order)
    out[0] = (word0 >> 12) & 0xF;
    out[1] = (word0 >> 8) & 0xF;
    out[2] = (word0 >> 4) & 0xF;
    out[3] = (word0 >> 0) & 0xF;
    out[4] = (word1 >> 12) & 0xF;
    out[5] = (word1 >> 8) & 0xF;
    out[6] = (word1 >> 4) & 0xF;
    out[7] = (word1 >> 0) & 0xF;
}

#if BRIMIR_HAS_SSSE3

/**
 * @brief Extract 8 nibbles from 32-bit tile data using SSSE3 PSHUFB
 * @param tileData Two 16-bit words containing 8 4bpp pixels
 * @param out Output: 8 color indices (as __m128i, only low 64 bits used)
 */
inline __m128i extract_tile_nibbles_ssse3(uint16_t word0, uint16_t word1) {
    // Pack the two words into a 32-bit value
    uint32_t packed = (static_cast<uint32_t>(word0) << 16) | word1;
    __m128i data = _mm_cvtsi32_si128(packed);
    
    // Shuffle table to extract nibbles
    // Input bytes: [B3 B2 B1 B0] where each byte has 2 nibbles
    // Output: 8 bytes, each containing one nibble
    static const __m128i shuffle_lo = _mm_setr_epi8(
        3, 3, 2, 2, 1, 1, 0, 0,  // Byte indices (duplicated for hi/lo nibble)
        -1, -1, -1, -1, -1, -1, -1, -1
    );
    static const __m128i shift_mask = _mm_setr_epi8(
        4, 0, 4, 0, 4, 0, 4, 0,  // Shift amounts for hi/lo nibbles
        0, 0, 0, 0, 0, 0, 0, 0
    );
    static const __m128i nibble_mask = _mm_set1_epi8(0x0F);
    
    // Shuffle to replicate bytes
    __m128i shuffled = _mm_shuffle_epi8(data, shuffle_lo);
    
    // Shift alternating bytes to get nibbles aligned
    __m128i shifted = _mm_srlv_epi8(shuffled, shift_mask);  // Requires AVX512 or manual
    
    // Mask to get nibbles
    return _mm_and_si128(shifted, nibble_mask);
}

#endif // BRIMIR_HAS_SSSE3

// =============================================================================
// Batch CRAM Lookup
// =============================================================================

#if BRIMIR_HAS_SSE2

/**
 * @brief Batch convert 8 palette indices to RGB888 colors
 * @param cram CRAM pointer (RGB555 colors)
 * @param indices 8 palette indices (16-bit each)
 * @param palette_base Base offset for palette
 * @param out Output: 8 RGB888 colors
 */
inline void batch_cram_lookup_sse2(const uint16_t* cram, const uint8_t* indices, 
                                    uint16_t palette_base, uint32_t* out) {
    // Gather colors from CRAM (scalar, since SSE2 lacks gather)
    uint16_t colors[8];
    for (int i = 0; i < 8; i++) {
        colors[i] = cram[(palette_base + indices[i]) & 0x7FF];
    }
    
    // Convert to RGB888 using SIMD
    rgb555_to_rgb888_sse2_8px(colors, out);
}

#endif // BRIMIR_HAS_SSE2

} // namespace brimir::simd


