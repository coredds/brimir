#pragma once

#include <cstdint>
#include <cstring>

// SIMD intrinsics - Windows x64 only (SSE2/AVX2)
#if defined(__AVX2__)
    #include <immintrin.h>
    #define VDP_SIMD_AVX2 1
#elif defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(_M_X64) || defined(__x86_64__)
    #include <emmintrin.h>
    #define VDP_SIMD_SSE2 1
#endif

namespace brimir::vdp::simd {

// SIMD pixel batch sizes - x64 only
#if defined(VDP_SIMD_AVX2)
    constexpr int kPixelBatchSize = 8;  // AVX2 processes 8 pixels at once (256-bit)
#elif defined(VDP_SIMD_SSE2)
    constexpr int kPixelBatchSize = 4;  // SSE2 processes 4 pixels at once (128-bit)
#else
    constexpr int kPixelBatchSize = 1;  // Scalar fallback
#endif

// Forward declarations
struct Color888;
struct Pixel;

// ============================================================================
// SIMD Transparency Checks
// ============================================================================

/// @brief Check if multiple pixels are transparent using SIMD
/// @param transparent Pointer to transparency array
/// @param count Number of pixels to check
/// @return Bitmask where set bits indicate transparent pixels
inline uint32_t CheckTransparencySIMD(const bool* transparent, int count) {
#if defined(VDP_SIMD_AVX2)
    if (count >= 8) {
        // Load 8 bool values (stored as bytes)
        __m256i trans = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(transparent));
        // Compare with zero (transparent = true = 1)
        __m256i cmp = _mm256_cmpeq_epi8(trans, _mm256_set1_epi8(1));
        // Create bitmask
        return static_cast<uint32_t>(_mm256_movemask_epi8(cmp));
    }
#elif defined(VDP_SIMD_SSE2)
    if (count >= 4) {
        __m128i trans = _mm_loadu_si128(reinterpret_cast<const __m128i*>(transparent));
        __m128i cmp = _mm_cmpeq_epi8(trans, _mm_set1_epi8(1));
        return static_cast<uint32_t>(_mm_movemask_epi8(cmp)) & 0xF;
    }
#endif
    
    // Scalar fallback
    uint32_t mask = 0;
    for (int i = 0; i < count; i++) {
        if (transparent[i]) {
            mask |= (1u << i);
        }
    }
    return mask;
}

// ============================================================================
// SIMD Memory Operations
// ============================================================================

/// @brief Set transparency flags for multiple pixels using SIMD
/// @param transparent Pointer to transparency array
/// @param value Value to set (true or false)
/// @param count Number of pixels to set
inline void SetTransparencySIMD(bool* transparent, bool value, int count) {
    const uint8_t val = value ? 1 : 0;
    
#if defined(VDP_SIMD_AVX2)
    const __m256i vec_val = _mm256_set1_epi8(val);
    int i = 0;
    for (; i + 32 <= count; i += 32) {
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(transparent + i), vec_val);
    }
    // Handle remaining pixels
    for (; i < count; i++) {
        transparent[i] = value;
    }
#elif defined(VDP_SIMD_SSE2)
    const __m128i vec_val = _mm_set1_epi8(val);
    int i = 0;
    for (; i + 16 <= count; i += 16) {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(transparent + i), vec_val);
    }
    // Handle remaining pixels
    for (; i < count; i++) {
        transparent[i] = value;
    }
#else
    // Scalar fallback
    std::memset(transparent, val, count);
#endif
}

/// @brief Copy pixel data using SIMD (for CopyPixel optimization)
/// @param src_color Source color array
/// @param dst_color Destination color array
/// @param src_priority Source priority array
/// @param dst_priority Destination priority array
/// @param src_transparent Source transparency array
/// @param dst_transparent Destination transparency array
/// @param src_specialCC Source special color calc array
/// @param dst_specialCC Destination special color calc array
/// @param src_idx Source index
/// @param dst_idx Destination index
/// @param count Number of pixels to copy
inline void CopyPixelsSIMD(const uint32_t* src_color, uint32_t* dst_color,
                           const uint8_t* src_priority, uint8_t* dst_priority,
                           const bool* src_transparent, bool* dst_transparent,
                           const bool* src_specialCC, bool* dst_specialCC,
                           int src_idx, int dst_idx, int count) {
#if defined(VDP_SIMD_AVX2)
    int i = 0;
    // Process 8 pixels at a time for colors (256 bits = 8 x 32-bit)
    for (; i + 8 <= count; i += 8) {
        // Copy colors (8 x uint32_t = 256 bits)
        __m256i colors = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_color + src_idx + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_color + dst_idx + i), colors);
    }
    // Handle remaining colors
    for (; i < count; i++) {
        dst_color[dst_idx + i] = src_color[src_idx + i];
    }
    
    // Copy priority, transparent, specialCC (all byte-sized)
    i = 0;
    for (; i + 32 <= count; i += 32) {
        // Priority
        __m256i prio = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_priority + src_idx + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_priority + dst_idx + i), prio);
        // Transparent
        __m256i trans = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_transparent + src_idx + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_transparent + dst_idx + i), trans);
        // SpecialCC
        __m256i scc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src_specialCC + src_idx + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst_specialCC + dst_idx + i), scc);
    }
    // Handle remaining bytes
    for (; i < count; i++) {
        dst_priority[dst_idx + i] = src_priority[src_idx + i];
        dst_transparent[dst_idx + i] = src_transparent[src_idx + i];
        dst_specialCC[dst_idx + i] = src_specialCC[src_idx + i];
    }
#elif defined(VDP_SIMD_SSE2)
    int i = 0;
    // Process 4 pixels at a time for colors (128 bits = 4 x 32-bit)
    for (; i + 4 <= count; i += 4) {
        // Copy colors (4 x uint32_t = 128 bits)
        __m128i colors = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_color + src_idx + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_color + dst_idx + i), colors);
    }
    // Handle remaining colors
    for (; i < count; i++) {
        dst_color[dst_idx + i] = src_color[src_idx + i];
    }
    
    // Copy priority, transparent, specialCC (all byte-sized)
    i = 0;
    for (; i + 16 <= count; i += 16) {
        // Priority
        __m128i prio = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_priority + src_idx + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_priority + dst_idx + i), prio);
        // Transparent
        __m128i trans = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_transparent + src_idx + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_transparent + dst_idx + i), trans);
        // SpecialCC
        __m128i scc = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_specialCC + src_idx + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst_specialCC + dst_idx + i), scc);
    }
    // Handle remaining bytes
    for (; i < count; i++) {
        dst_priority[dst_idx + i] = src_priority[src_idx + i];
        dst_transparent[dst_idx + i] = src_transparent[src_idx + i];
        dst_specialCC[dst_idx + i] = src_specialCC[src_idx + i];
    }
#else
    // Scalar fallback
    for (int i = 0; i < count; i++) {
        dst_color[dst_idx + i] = src_color[src_idx + i];
        dst_priority[dst_idx + i] = src_priority[src_idx + i];
        dst_transparent[dst_idx + i] = src_transparent[src_idx + i];
        dst_specialCC[dst_idx + i] = src_specialCC[src_idx + i];
    }
#endif
}

/// @brief Fast memory copy for color buffers using SIMD
/// @param dst Destination buffer
/// @param src Source buffer
/// @param count Number of uint32_t elements to copy
inline void MemcpyColor888SIMD(uint32_t* dst, const uint32_t* src, int count) {
#if defined(VDP_SIMD_AVX2)
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256i data = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), data);
    }
    // Handle remaining
    for (; i < count; i++) {
        dst[i] = src[i];
    }
#elif defined(VDP_SIMD_SSE2)
    int i = 0;
    for (; i + 4 <= count; i += 4) {
        __m128i data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), data);
    }
    // Handle remaining
    for (; i < count; i++) {
        dst[i] = src[i];
    }
#else
    std::memcpy(dst, src, count * sizeof(uint32_t));
#endif
}

// ============================================================================
// Window Masking (SIMD)
// ============================================================================

/// @brief Apply window mask to transparency array using SIMD
/// @param transparent Output transparency array
/// @param windowState Window state array (true = inside window = should be transparent)
/// @param count Number of pixels to process
inline void ApplyWindowMaskSIMD(bool* transparent, const bool* windowState, int count) {
#if defined(VDP_SIMD_AVX2)
    int i = 0;
    for (; i + 32 <= count; i += 32) {
        __m256i window = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(windowState + i));
        __m256i trans = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(transparent + i));
        // OR the window state into transparency (if window is true, pixel becomes transparent)
        __m256i result = _mm256_or_si256(trans, window);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(transparent + i), result);
    }
    // Handle remaining
    for (; i < count; i++) {
        if (windowState[i]) {
            transparent[i] = true;
        }
    }
#elif defined(VDP_SIMD_SSE2)
    int i = 0;
    for (; i + 16 <= count; i += 16) {
        __m128i window = _mm_loadu_si128(reinterpret_cast<const __m128i*>(windowState + i));
        __m128i trans = _mm_loadu_si128(reinterpret_cast<const __m128i*>(transparent + i));
        __m128i result = _mm_or_si128(trans, window);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(transparent + i), result);
    }
    // Handle remaining
    for (; i < count; i++) {
        if (windowState[i]) {
            transparent[i] = true;
        }
    }
#elif defined(VDP_SIMD_NEON)
    int i = 0;
    for (; i + 16 <= count; i += 16) {
        uint8x16_t window = vld1q_u8(reinterpret_cast<const uint8_t*>(windowState + i));
        uint8x16_t trans = vld1q_u8(reinterpret_cast<uint8_t*>(transparent + i));
        uint8x16_t result = vorrq_u8(trans, window);
        vst1q_u8(reinterpret_cast<uint8_t*>(transparent + i), result);
    }
    // Handle remaining
    for (; i < count; i++) {
        if (windowState[i]) {
            transparent[i] = true;
        }
    }
#else
    // Scalar fallback
    for (int i = 0; i < count; i++) {
        if (windowState[i]) {
            transparent[i] = true;
        }
    }
#endif
}

/// @brief Apply sprite window to window state using SIMD (OR logic)
/// @param windowState Window state array to modify (true = inside window)
/// @param shadowOrWindow Sprite shadow/window array
/// @param inverted If true, invert the shadowOrWindow values
/// @param count Number of pixels to process
inline void ApplySpriteWindowOR_SIMD(bool* windowState, const bool* shadowOrWindow, bool inverted, int count) {
#if defined(VDP_SIMD_AVX2)
    const __m256i invertMask = inverted ? _mm256_set1_epi8(0x01) : _mm256_setzero_si256();
    int i = 0;
    for (; i + 32 <= count; i += 32) {
        __m256i sprite = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shadowOrWindow + i));
        __m256i state = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(windowState + i));
        // XOR with invert mask to flip if needed, then OR into state
        __m256i spriteXor = _mm256_xor_si256(sprite, invertMask);
        __m256i result = _mm256_or_si256(state, spriteXor);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(windowState + i), result);
    }
    for (; i < count; i++) {
        windowState[i] |= shadowOrWindow[i] != inverted;
    }
#elif defined(VDP_SIMD_SSE2)
    const __m128i invertMask = inverted ? _mm_set1_epi8(0x01) : _mm_setzero_si128();
    int i = 0;
    for (; i + 16 <= count; i += 16) {
        __m128i sprite = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shadowOrWindow + i));
        __m128i state = _mm_loadu_si128(reinterpret_cast<const __m128i*>(windowState + i));
        __m128i spriteXor = _mm_xor_si128(sprite, invertMask);
        __m128i result = _mm_or_si128(state, spriteXor);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(windowState + i), result);
    }
    for (; i < count; i++) {
        windowState[i] |= shadowOrWindow[i] != inverted;
    }
#else
    for (int i = 0; i < count; i++) {
        windowState[i] |= shadowOrWindow[i] != inverted;
    }
#endif
}

/// @brief Apply sprite window to window state using SIMD (AND logic)
/// @param windowState Window state array to modify (true = inside window)
/// @param shadowOrWindow Sprite shadow/window array
/// @param inverted If true, invert the shadowOrWindow values
/// @param count Number of pixels to process
inline void ApplySpriteWindowAND_SIMD(bool* windowState, const bool* shadowOrWindow, bool inverted, int count) {
#if defined(VDP_SIMD_AVX2)
    const __m256i invertMask = inverted ? _mm256_set1_epi8(0x01) : _mm256_setzero_si256();
    int i = 0;
    for (; i + 32 <= count; i += 32) {
        __m256i sprite = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(shadowOrWindow + i));
        __m256i state = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(windowState + i));
        // XOR with invert mask to flip if needed, then AND into state
        __m256i spriteXor = _mm256_xor_si256(sprite, invertMask);
        __m256i result = _mm256_and_si256(state, spriteXor);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(windowState + i), result);
    }
    for (; i < count; i++) {
        windowState[i] &= shadowOrWindow[i] != inverted;
    }
#elif defined(VDP_SIMD_SSE2)
    const __m128i invertMask = inverted ? _mm_set1_epi8(0x01) : _mm_setzero_si128();
    int i = 0;
    for (; i + 16 <= count; i += 16) {
        __m128i sprite = _mm_loadu_si128(reinterpret_cast<const __m128i*>(shadowOrWindow + i));
        __m128i state = _mm_loadu_si128(reinterpret_cast<const __m128i*>(windowState + i));
        __m128i spriteXor = _mm_xor_si128(sprite, invertMask);
        __m128i result = _mm_and_si128(state, spriteXor);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(windowState + i), result);
    }
    for (; i < count; i++) {
        windowState[i] &= shadowOrWindow[i] != inverted;
    }
#else
    for (int i = 0; i < count; i++) {
        windowState[i] &= shadowOrWindow[i] != inverted;
    }
#endif
}

/// @brief Masked color copy using SIMD
/// @param dst Destination color array (32-bit RGBA)
/// @param src Source color array (32-bit RGBA)
/// @param mask Boolean mask array (copy where mask is true/non-zero)
/// @param count Number of pixels to process
inline void MaskedColorCopySIMD(uint32_t* dst, const uint32_t* src, const bool* mask, int count) {
#if defined(VDP_SIMD_AVX2)
    int i = 0;
    // Process 8 pixels at a time (256 bits = 8 x 32-bit colors)
    for (; i + 8 <= count; i += 8) {
        // Load 8 mask bytes and expand to 32-bit masks
        __m128i mask8 = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(mask + i));
        // Expand bytes to 32-bit: compare each byte with zero
        __m256i mask32 = _mm256_cvtepi8_epi32(mask8);
        // Convert to full 32-bit mask (0x00000000 or 0xFFFFFFFF)
        mask32 = _mm256_cmpgt_epi32(mask32, _mm256_setzero_si256());
        
        // Load source and destination
        __m256i srcVec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        __m256i dstVec = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        
        // Blend: select src where mask is set, dst where mask is clear
        __m256i result = _mm256_blendv_epi8(dstVec, srcVec, mask32);
        
        // Store result
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), result);
    }
    // Handle remaining pixels
    for (; i < count; i++) {
        if (mask[i]) {
            dst[i] = src[i];
        }
    }
#elif defined(VDP_SIMD_SSE2)
    int i = 0;
    // Process 4 pixels at a time (128 bits = 4 x 32-bit colors)
    for (; i + 4 <= count; i += 4) {
        // Expand each mask byte to 32 bits: 0x00 -> 0x00000000, non-zero -> 0xFFFFFFFF
        __m128i maskVec = _mm_set_epi32(
            (mask[i + 3] ? -1 : 0),
            (mask[i + 2] ? -1 : 0),
            (mask[i + 1] ? -1 : 0),
            (mask[i + 0] ? -1 : 0)
        );
        
        // Load source and destination
        __m128i srcVec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
        __m128i dstVec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dst + i));
        
        // Blend using AND/OR: result = (src & mask) | (dst & ~mask)
        __m128i result = _mm_or_si128(
            _mm_and_si128(srcVec, maskVec),
            _mm_andnot_si128(maskVec, dstVec)
        );
        
        // Store result
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i), result);
    }
    // Handle remaining pixels
    for (; i < count; i++) {
        if (mask[i]) {
            dst[i] = src[i];
        }
    }
#else
    // Scalar fallback
    for (int i = 0; i < count; i++) {
        if (mask[i]) {
            dst[i] = src[i];
        }
    }
#endif
}

} // namespace brimir::vdp::simd




