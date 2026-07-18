#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace brimir {

/// Lock-free SPSC stereo ring buffer. Capacity is the number of stereo samples
/// (left+right pairs); the internal slot count is twice that and must be a
/// power of two.
template <size_t CapacityStereoSamples>
class AudioRingBuffer {
    static_assert(CapacityStereoSamples > 1 && (CapacityStereoSamples & (CapacityStereoSamples - 1)) == 0,
                  "Capacity must be a power of two greater than 1");

    static constexpr size_t SlotCount = CapacityStereoSamples * 2;

public:
    bool Push(int16_t left, int16_t right) {
        size_t writePos = m_write.load(std::memory_order_relaxed);
        size_t next = (writePos + 2) & (SlotCount - 1);
        if (next == m_read.load(std::memory_order_acquire)) {
            return false; // full
        }
        m_buffer[writePos] = left;
        m_buffer[writePos + 1] = right;
        m_write.store(next, std::memory_order_release);
        return true;
    }

    size_t Pop(int16_t* out, size_t maxStereoSamples) {
        size_t readPos = m_read.load(std::memory_order_relaxed);
        size_t writePos = m_write.load(std::memory_order_acquire);

        size_t slots = (writePos >= readPos)
            ? (writePos - readPos)
            : (SlotCount - readPos + writePos);
        size_t availableStereo = slots / 2;
        size_t toCopy = std::min(availableStereo, maxStereoSamples);
        size_t slotsToCopy = toCopy * 2;
        size_t untilWrap = SlotCount - readPos;

        if (slotsToCopy <= untilWrap) {
            std::memcpy(out, &m_buffer[readPos], slotsToCopy * sizeof(int16_t));
        } else {
            std::memcpy(out, &m_buffer[readPos], untilWrap * sizeof(int16_t));
            std::memcpy(out + untilWrap, &m_buffer[0], (slotsToCopy - untilWrap) * sizeof(int16_t));
        }

        m_read.store((readPos + slotsToCopy) & (SlotCount - 1), std::memory_order_release);
        return toCopy;
    }

    size_t Available() const {
        size_t readPos = m_read.load(std::memory_order_relaxed);
        size_t writePos = m_write.load(std::memory_order_acquire);
        size_t slots = (writePos >= readPos)
            ? (writePos - readPos)
            : (SlotCount - readPos + writePos);
        return slots / 2;
    }

private:
    std::array<int16_t, SlotCount> m_buffer{};
    std::atomic<size_t> m_write{0};
    std::atomic<size_t> m_read{0};
};

} // namespace brimir
