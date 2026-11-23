// A synthetic ST-V BIOS for the [stv] tests: every vector runs an idle loop, so the master SH-2 waits quietly, also after
// an NMI (the SMPC's clock change raises one) or any other exception. A BIOS of zeros is not idle: opcode 0x0000 is
// illegal, and each exception pushes SR and PC through SP 0 into the on-chip registers at the top of the address space,
// which the core logs on every write (CI's ctest slows down badly on that much output).

#pragma once

#include <stv/stv_system.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <utility>
#include <vector>

namespace brimir::test {

// Where the idle loop is (after the 256-entry vector table)
inline constexpr uint32_t kIdleSTVBiosLoop = 0x1000;

// Fills a BIOS-sized buffer in CPU byte order, as LoadSTVBiosImage returns it: every vector points at `bra $; nop` at
// kIdleSTVBiosLoop, except the two reset stack pointers, which are 0x06100000 (the real BIOS's). The image is 512 KiB:
// callers keep it on the heap or in a static object.
inline void FillIdleSTVBiosImage(std::span<uint8_t, stv::kSTVBiosSize> image) {
    std::fill(image.begin(), image.end(), uint8_t{0});
    const auto put32 = [&](size_t at, uint32_t value) {
        image[at] = static_cast<uint8_t>(value >> 24);
        image[at + 1] = static_cast<uint8_t>(value >> 16);
        image[at + 2] = static_cast<uint8_t>(value >> 8);
        image[at + 3] = static_cast<uint8_t>(value);
    };
    for (size_t vector = 0; vector < 256; vector++) {
        const bool stackPointer = vector == 1 || vector == 3; // power-on and manual reset SP
        put32(vector * 4, stackPointer ? 0x0610'0000u : kIdleSTVBiosLoop);
    }
    put32(kIdleSTVBiosLoop, 0xAFFE'0009u); // bra $; nop
}

// The same image as a BIOS file: the dumps are 16-bit byte-swapped (LoadSTVBiosImage swaps them back)
inline std::vector<uint8_t> IdleSTVBiosFile() {
    std::vector<uint8_t> file(stv::kSTVBiosSize);
    FillIdleSTVBiosImage(std::span<uint8_t, stv::kSTVBiosSize>(file.data(), file.size()));
    for (size_t i = 0; i + 1 < file.size(); i += 2) {
        std::swap(file[i], file[i + 1]);
    }
    return file;
}

inline void WriteIdleSTVBios(const std::filesystem::path &path) {
    const std::vector<uint8_t> file = IdleSTVBiosFile();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size()));
}

} // namespace brimir::test
