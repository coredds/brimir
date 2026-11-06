// Brimir - Sega Saturn libretro core based on Ymir
// Copyright (C) 2025 coredds
// Licensed under GPL-3.0

#include "brimir/ram_view.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define BRIMIR_RAM_VIEW_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(_M_ARM64)
#define BRIMIR_RAM_VIEW_NEON 1
#include <arm_neon.h>
#endif

namespace brimir {

namespace {

constexpr bool kSwap = std::endian::native == std::endian::little;

// Byte index in the view of the Saturn-order byte i (and back: the mapping is its own inverse).
constexpr size_t ViewIndex(size_t i) {
    return kSwap ? (i ^ 1) : i;
}

// 64 bytes per block: the unit ApplyHalf compares before looking at single bytes.
constexpr size_t kBlock = 64;
static_assert(RamView::kHalfSize % kBlock == 0);

void PublishHalf(const uint8_t *src, uint8_t *view, uint8_t *snapshot) {
    if constexpr (!kSwap) {
        std::memcpy(view, src, RamView::kHalfSize);
        std::memcpy(snapshot, src, RamView::kHalfSize);
        return;
    }
#if BRIMIR_RAM_VIEW_SSE2
    // Only the blocks that changed are stored: most of work RAM is unchanged between frames, and reading is cheaper
    // than writing (the view equals the snapshot here unless a frontend write is pending, which this drops).
    for (size_t block = 0; block < RamView::kHalfSize; block += kBlock) {
        __m128i swapped[kBlock / 16];
        __m128i diff = _mm_setzero_si128();
        for (size_t k = 0; k < kBlock / 16; ++k) {
            const __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + block + k * 16));
            swapped[k] = _mm_or_si128(_mm_slli_epi16(x, 8), _mm_srli_epi16(x, 8));
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(view + block + k * 16));
            diff = _mm_or_si128(diff, _mm_xor_si128(swapped[k], v));
        }
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) == 0xFFFF) {
            continue;
        }
        for (size_t k = 0; k < kBlock / 16; ++k) {
            _mm_storeu_si128(reinterpret_cast<__m128i *>(view + block + k * 16), swapped[k]);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(snapshot + block + k * 16), swapped[k]);
        }
    }
#elif BRIMIR_RAM_VIEW_NEON
    for (size_t block = 0; block < RamView::kHalfSize; block += kBlock) {
        uint8x16_t swapped[kBlock / 16];
        uint8x16_t diff = vdupq_n_u8(0);
        for (size_t k = 0; k < kBlock / 16; ++k) {
            swapped[k] = vrev16q_u8(vld1q_u8(src + block + k * 16));
            diff = vorrq_u8(diff, veorq_u8(swapped[k], vld1q_u8(view + block + k * 16)));
        }
        if (vmaxvq_u8(diff) == 0) {
            continue;
        }
        for (size_t k = 0; k < kBlock / 16; ++k) {
            vst1q_u8(view + block + k * 16, swapped[k]);
            vst1q_u8(snapshot + block + k * 16, swapped[k]);
        }
    }
#else
    constexpr size_t kWords = kBlock / sizeof(uint64_t);
    for (size_t block = 0; block < RamView::kHalfSize; block += kBlock) {
        uint64_t swapped[kWords];
        uint64_t diff = 0;
        for (size_t k = 0; k < kWords; ++k) {
            uint64_t x;
            uint64_t v;
            std::memcpy(&x, src + block + k * sizeof(x), sizeof(x));
            std::memcpy(&v, view + block + k * sizeof(v), sizeof(v));
            swapped[k] = ((x & 0x00FF00FF00FF00FFull) << 8) | ((x >> 8) & 0x00FF00FF00FF00FFull);
            diff |= swapped[k] ^ v;
        }
        if (diff == 0) {
            continue;
        }
        std::memcpy(view + block, swapped, kBlock);
        std::memcpy(snapshot + block, swapped, kBlock);
    }
#endif
}

bool BlockDiffers(const uint8_t *view, const uint8_t *snapshot) {
#if BRIMIR_RAM_VIEW_SSE2
    __m128i diff = _mm_setzero_si128();
    for (size_t i = 0; i < kBlock; i += 16) {
        const __m128i a = _mm_loadu_si128(reinterpret_cast<const __m128i *>(view + i));
        const __m128i b = _mm_loadu_si128(reinterpret_cast<const __m128i *>(snapshot + i));
        diff = _mm_or_si128(diff, _mm_xor_si128(a, b));
    }
    return _mm_movemask_epi8(_mm_cmpeq_epi8(diff, _mm_setzero_si128())) != 0xFFFF;
#elif BRIMIR_RAM_VIEW_NEON
    uint8x16_t diff = vdupq_n_u8(0);
    for (size_t i = 0; i < kBlock; i += 16) {
        diff = vorrq_u8(diff, veorq_u8(vld1q_u8(view + i), vld1q_u8(snapshot + i)));
    }
    return vmaxvq_u8(diff) != 0;
#else
    uint64_t diff = 0;
    for (size_t i = 0; i < kBlock; i += sizeof(uint64_t)) {
        uint64_t a;
        uint64_t b;
        std::memcpy(&a, view + i, sizeof(a));
        std::memcpy(&b, snapshot + i, sizeof(b));
        diff |= a ^ b;
    }
    return diff != 0;
#endif
}

// Bytes [begin, end) of one half (a multiple of kBlock apart).
void ApplyRange(uint8_t *ram, const uint8_t *view, uint8_t *snapshot, size_t begin, size_t end) {
    for (size_t block = begin; block < end; block += kBlock) {
        if (!BlockDiffers(view + block, snapshot + block)) {
            continue;
        }
        for (size_t i = block; i < block + kBlock; ++i) {
            if (view[i] != snapshot[i]) {
                ram[ViewIndex(i)] = view[i];
                snapshot[i] = view[i];
            }
        }
    }
}

#ifdef _WIN32
// One entry per 4 KiB page of the view (larger pages report fewer).
constexpr size_t kMaxWatchedPages = RamView::kSize / 4096;
#endif

} // namespace

RamView::RamView(Tracking tracking)
    : m_snapshot(std::make_unique<uint8_t[]>(kSize)) {
#ifdef _WIN32
    if (tracking == Tracking::Auto) {
        // Zero-filled, like the snapshot.
        m_view = static_cast<uint8_t *>(
            VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
        m_writeWatch = m_view != nullptr;
    }
#else
    (void)tracking;
#endif
    if (m_view == nullptr) {
        m_ownedView = std::make_unique<uint8_t[]>(kSize);
        m_view = m_ownedView.get();
    }
}

RamView::~RamView() {
#ifdef _WIN32
    if (m_writeWatch) {
        VirtualFree(m_view, 0, MEM_RELEASE);
    }
#endif
}

void RamView::ResetWriteWatch() {
#ifdef _WIN32
    if (m_writeWatch && ::ResetWriteWatch(m_view, kSize) != 0) {
        m_writeWatch = false; // cannot happen for our own allocation; full scans from now on
    }
#endif
}

void RamView::Publish(std::span<const uint8_t, kHalfSize> low, std::span<const uint8_t, kHalfSize> high) {
    PublishHalf(low.data(), m_view, m_snapshot.get());
    PublishHalf(high.data(), m_view + kHighOffset, m_snapshot.get() + kHighOffset);
    // Only writes after this point are the frontend's.
    ResetWriteWatch();
}

void RamView::ApplyFrontendWrites(std::span<uint8_t, kHalfSize> low, std::span<uint8_t, kHalfSize> high) {
#ifdef _WIN32
    if (m_writeWatch) {
        void *pages[kMaxWatchedPages];
        ULONG_PTR count = kMaxWatchedPages;
        ULONG pageSize = 0;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, m_view, kSize, pages, &count, &pageSize) == 0 &&
            pageSize % kBlock == 0) {
            for (ULONG_PTR p = 0; p < count; ++p) {
                const size_t begin = static_cast<size_t>(static_cast<uint8_t *>(pages[p]) - m_view);
                const size_t end = std::min(begin + pageSize, kSize);
                // A page lies in one half (both are page multiples).
                if (begin < kHighOffset) {
                    ApplyRange(low.data(), m_view, m_snapshot.get(), begin, std::min(end, kHighOffset));
                } else {
                    ApplyRange(high.data(), m_view + kHighOffset, m_snapshot.get() + kHighOffset,
                               begin - kHighOffset, end - kHighOffset);
                }
            }
            return;
        }
        m_writeWatch = false;
    }
#endif
    ApplyRange(low.data(), m_view, m_snapshot.get(), 0, kHalfSize);
    ApplyRange(high.data(), m_view + kHighOffset, m_snapshot.get() + kHighOffset, 0, kHalfSize);
}

void RamView::Clear() {
    std::memset(m_view, 0, kSize);
    std::memset(m_snapshot.get(), 0, kSize);
    ResetWriteWatch();
}

} // namespace brimir
