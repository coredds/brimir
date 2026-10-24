#pragma once

#include <ymir/util/bit_ops.hpp> // Brimir: upstream relies on a transitive include for bit::is_power_of_two

#include <algorithm> // Brimir: std::min
#include <array>
#include <bit>
#include <cstdint>
#include <type_traits>

namespace util {

/// @brief Tracks dirty bits and allows processing ranges of dirty bits.
/// @tparam numBits the number of bits in the bitmap
template <size_t numBits>
struct DirtyBitmap {
    using TEntry = uint64_t;
    static constexpr size_t kBitsPerEntry = sizeof(TEntry) * 8;
    static constexpr size_t kEntryMask = kBitsPerEntry - 1;
    static constexpr size_t kEntryShift = std::countr_zero(kBitsPerEntry);
    static constexpr size_t kNumEntries = (numBits + kBitsPerEntry - 1) >> kEntryShift;
    static constexpr TEntry kAllBits = ~static_cast<TEntry>(0);

    /// @brief Sets the specified bit as dirty.
    /// @param[in] index the bit to set
    void Set(TEntry index) {
        if (index < numBits) {
            m_bitmap[index >> kEntryShift] |= 1ull << (index & kEntryMask);
            m_anySet = true;
        }
    }

    /// @brief Sets all bits as dirty.
    void SetAll() {
        m_bitmap.fill(kAllBits);
        if constexpr ((numBits & kEntryMask) != 0) {
            m_bitmap.back() = kAllBits >> (-numBits & kEntryMask);
        }
        m_anySet = true;
    }

    /// @brief Resets all dirty bits.
    void ClearAll() {
        m_bitmap.fill(0);
        m_anySet = false;
    }

    /// @brief Checks if any bit is set in the bitmap.
    /// @return `true` if any bit is set
    bool AnySet() const {
        // Fast path
        if (!m_anySet) {
            return false;
        }
        // Slow path: check everything
        for (TEntry entry : m_bitmap) {
            if (entry != 0) {
                return true;
            }
        }
        // Update result
        m_anySet = false;
        return false;
    }

    /// @brief Checks if the specified bit is set.
    /// @param[in] index the bit to check
    /// @return `true` if the bit is marked as dirty, `false` if not
    bool Get(TEntry index) const {
        if (index < numBits) {
            return (m_bitmap[index >> kEntryShift] & (1ull << (index & kEntryMask))) != 0;
        }
        return false;
    }

    /// @brief Returns `true` if any bit is set.
    operator bool() {
        return AnySet();
    }

    /// @brief Finds the next sequence of set bits from the starting offset (inclusive).
    /// @param[in] offset the starting offset, inclusive
    /// @param[out] outSetCount receives the number of bits set in a row
    /// @return the offset to the next sequence of set bits, or `numBits` if not found.
    size_t FindNext(size_t &outSetCount, size_t offset = 0) {
        if (offset >= numBits) {
            return numBits;
        }
        TEntry accumOnes = 0;
        size_t i = offset >> kEntryShift;
        TEntry entry = m_bitmap[i] >> (offset & kEntryMask);
        TEntry remaining = std::min<TEntry>(kBitsPerEntry - (offset & kEntryMask), numBits);
        while (i < kNumEntries) {
            // Zeros search phase
            while (entry == 0) {
                offset += remaining;
                ++i;
                if (i >= kNumEntries) {
                    break;
                }
                entry = m_bitmap[i];
                remaining = kBitsPerEntry;
                continue;
            }
            if (i >= kNumEntries) {
                break;
            }

            const TEntry zeros = std::min<TEntry>(std::countr_zero(entry), remaining);
            offset += zeros;
            remaining -= zeros;
            entry >>= zeros;

            // Ones search phase
            while (true) {
                const TEntry ones = std::countr_one(entry);
                accumOnes += ones;
                // Brimir: shifting a 64-bit value by 64 is undefined; a fully set entry gives ones == 64
                entry = ones == kBitsPerEntry ? 0 : entry >> ones;
                remaining -= ones;
                if (remaining > 0) {
                    outSetCount = accumOnes;
                    return offset;
                }
                ++i;
                if (i >= kNumEntries) {
                    break;
                }
                entry = m_bitmap[i];
                remaining = kBitsPerEntry;
            }
        }
        if (accumOnes != 0) {
            outSetCount = accumOnes;
            return offset;
        }
        return numBits;
    }

    /// @brief Finds the next sequence of bit groups with at least one bit set from the starting offset (inclusive).
    /// @tparam N the size of the bit cluster. Must be a power of two not greater than 64
    /// @param[in] offset the starting offset, inclusive, rounded down to the nearest multiple of N
    /// @param[out] outSetCount receives the number of bits set in a row
    /// @return the offset to the next sequence of set bits, or `numBits` if not found.
    template <size_t N>
        requires(bit::is_power_of_two<size_t>(N) && N < sizeof(TEntry) * 8u) // Brimir: explicit T, MSVC 14.44 rejects the deduced call here
    size_t FindNextGroup(size_t &outSetCount, size_t offset = 0) {
        static constexpr TEntry kMask = (1ull << N) - 1ull;
        offset &= ~(N - 1u);
        if (offset >= numBits) {
            return numBits;
        }
        TEntry accumNonEmpty = 0;
        size_t i = offset >> kEntryShift;
        TEntry entry = m_bitmap[i] >> (offset & kEntryMask);
        TEntry remaining = std::min<TEntry>(kBitsPerEntry - (offset & kEntryMask), numBits);
        while (i < kNumEntries) {
            // Zeros search phase
            while (entry == 0) {
                offset += remaining;
                ++i;
                if (i >= kNumEntries) {
                    break;
                }
                entry = m_bitmap[i];
                remaining = kBitsPerEntry;
                continue;
            }
            if (i >= kNumEntries) {
                break;
            }

            const TEntry zeroClusters = std::min<TEntry>(std::countr_zero(entry), remaining) / N;
            offset += zeroClusters * N;
            remaining -= zeroClusters * N;
            entry >>= zeroClusters * N;

            // Non-zeros search phase
            while (true) {
                while (remaining > 0) {
                    const TEntry value = entry & kMask;
                    if (value == 0) {
                        break;
                    }
                    accumNonEmpty += N;
                    entry >>= N;
                    remaining -= N;
                }
                if (remaining > 0) {
                    outSetCount = accumNonEmpty;
                    return offset;
                }
                ++i;
                if (i >= kNumEntries) {
                    break;
                }
                entry = m_bitmap[i];
                remaining = kBitsPerEntry;
            }
        }
        if (accumNonEmpty != 0) {
            outSetCount = accumNonEmpty;
            return offset;
        }
        return numBits;
    }

    /// @brief Returns a pointer to the raw data of this bitmap.
    /// @return a pointer to the raw bitmap
    const TEntry *GetData() const {
        return m_bitmap.data();
    }

    /// @brief Returns the number of bits in the bitmap.
    /// @return the number of bits in the bitmap
    size_t Size() const {
        return numBits;
    }

private:
    alignas(16) std::array<TEntry, kNumEntries> m_bitmap = {};
    mutable bool m_anySet = false;
};

} // namespace util
