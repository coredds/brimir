#pragma once

#include <ymir/core/types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ymir::media::loader::chd {

// Brimir: new file (BRIMIR_FORK.md); not in upstream Ymir.
// The most recently used decompressed CHD hunks, at most `capacity` of them. Bounded so that memory
// does not grow with the amount of disc data read over a session (the whole decompressed image in
// the end); a read cache only, so it does not affect emulation.
class HunkCache {
public:
    HunkCache(size_t capacity, size_t hunkBytes)
        : m_slots(capacity == 0 ? 1 : capacity) {
        for (Slot &slot : m_slots) {
            slot.data.resize(hunkBytes);
        }
    }

    // Returns the data of hunk `index`, valid until the next Get. On a miss, fill(std::span<uint8>)
    // must write the hunk's hunkBytes bytes into the given buffer; it replaces the least recently
    // used hunk.
    template <typename Fill>
    std::span<const uint8> Get(uintmax_t index, Fill &&fill) {
        Slot *victim = &m_slots[0];
        for (Slot &slot : m_slots) {
            if (slot.used && slot.index == index) {
                slot.lastUse = ++m_clock;
                return slot.data;
            }
            if (!slot.used) {
                if (victim->used) {
                    victim = &slot;
                }
            } else if (victim->used && slot.lastUse < victim->lastUse) {
                victim = &slot;
            }
        }
        victim->used = false; // stays empty if fill throws
        fill(std::span<uint8>{victim->data});
        victim->index = index;
        victim->used = true;
        victim->lastUse = ++m_clock;
        return victim->data;
    }

    // Hunks currently held.
    size_t Size() const {
        size_t count = 0;
        for (const Slot &slot : m_slots) {
            count += slot.used ? 1 : 0;
        }
        return count;
    }

private:
    struct Slot {
        uintmax_t index = 0;
        uint64_t lastUse = 0;
        bool used = false;
        std::vector<uint8> data;
    };

    std::vector<Slot> m_slots;
    uint64_t m_clock = 0;
};

} // namespace ymir::media::loader::chd
