#pragma once

// Beetle-compatible view of the Saturn work RAM, the memory the frontend sees as RETRO_MEMORY_SYSTEM_RAM and through
// the memory map (RetroAchievements, RetroArch cheats and memory viewer). Saturn achievement sets and RetroArch .cht
// files were authored on Beetle Saturn, which exposes one 2 MiB array: Work RAM Low (0x00200000) at offset 0, Work
// RAM High (0x06000000) at offset 0x100000, stored as host-endian 16-bit words, so on little-endian hosts the bytes
// of each 16-bit word are swapped relative to the Saturn. Ymir's work RAM is in Saturn (big-endian) byte order and
// cannot be reinterpreted in place, so the view is a mirror:
//
//   ApplyFrontendWrites (before the frame) -> [cheats, Task 3] -> RunFrame -> Publish (after the frame)
//
// Publish also follows a state load and a reset, so the bytes the frontend changed in the view before the load are
// dropped instead of being written over the loaded RAM. The view's address stays the same for the object's lifetime
// (the frontend keeps the pointer).
//
// Cost: Publish reads work RAM and the view (it stores only the 64-byte blocks that changed). ApplyFrontendWrites
// compares the view with the snapshot; on Windows the view is allocated with write watching, so it compares only the
// pages written since the last Publish (none in most frames) instead of the whole 2 MiB.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace brimir {

class RamView {
public:
    static constexpr size_t kHalfSize = 0x100000;  // 1 MiB, Work RAM Low or High
    static constexpr size_t kHighOffset = kHalfSize;
    static constexpr size_t kSize = 2 * kHalfSize; // 2 MiB

    // FullScan: ApplyFrontendWrites always compares all 2 MiB (the behaviour without write watching; for tests).
    enum class Tracking { Auto, FullScan };

    explicit RamView(Tracking tracking = Tracking::Auto);
    ~RamView();
    RamView(const RamView &) = delete;
    RamView &operator=(const RamView &) = delete;

    // Whether ApplyFrontendWrites uses the operating system's write watching.
    bool WriteWatched() const { return m_writeWatch; }

    // Copies Saturn-order work RAM into the view (each 16-bit word byte-swapped on little-endian hosts) and records
    // it as the last published snapshot.
    void Publish(std::span<const uint8_t, kHalfSize> low, std::span<const uint8_t, kHalfSize> high);

    // Writes back into work RAM the bytes of the view that differ from the last published snapshot (frontend writes),
    // and nothing else. The snapshot is updated, so a write is applied once.
    void ApplyFrontendWrites(std::span<uint8_t, kHalfSize> low, std::span<uint8_t, kHalfSize> high);

    // Zeroes the view and the snapshot (no content).
    void Clear();

    uint8_t *Data() { return m_view; }
    const uint8_t *Data() const { return m_view; }

private:
    void ResetWriteWatch();

    uint8_t *m_view = nullptr;
    bool m_writeWatch = false;
    std::unique_ptr<uint8_t[]> m_ownedView; // without write watching
    std::unique_ptr<uint8_t[]> m_snapshot;
};

} // namespace brimir
