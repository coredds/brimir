// A synthetic ST-V BIOS for the [stv] tests: the reset vector runs an idle loop, so the master SH-2 waits quietly.
// A BIOS of zeros is not idle: opcode 0x0000 is illegal, and each exception pushes SR and PC through SP 0 into the
// on-chip registers at the top of the address space, which the core logs on every write.

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

// Fills a BIOS-sized buffer in CPU byte order, as LoadSTVBiosImage returns it: PC = 0x100, SP = 0x06100000 (the real
// BIOS's), and `bra $; nop` at 0x100. The image is 512 KiB: callers keep it on the heap or in a static object.
inline void FillIdleSTVBiosImage(std::span<uint8_t, stv::kSTVBiosSize> image) {
    std::fill(image.begin(), image.end(), uint8_t{0});
    const auto put16 = [&](size_t at, uint16_t value) {
        image[at] = static_cast<uint8_t>(value >> 8);
        image[at + 1] = static_cast<uint8_t>(value);
    };
    put16(0x0, 0x0000); // power-on reset PC
    put16(0x2, 0x0100);
    put16(0x4, 0x0610); // power-on reset SP
    put16(0x6, 0x0000);
    put16(0x100, 0xAFFE); // bra $
    put16(0x102, 0x0009); // nop
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
