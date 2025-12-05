#pragma once

#include <array>

namespace brimir::m68k {

// All valid addressing modes
inline constexpr auto kValidAddrModes = [] {
    std::array<bool, 0b111'111 + 1> arr{};
    arr.fill(false);
    for (int i = 0b000; i <= 0b111; i++) {
        arr[(0b000 << 3) | i] = true; // Dn
        arr[(0b001 << 3) | i] = true; // An
        arr[(0b010 << 3) | i] = true; // (An)
        arr[(0b011 << 3) | i] = true; // (An)+
        arr[(0b100 << 3) | i] = true; // -(An)
        arr[(0b101 << 3) | i] = true; // (disp, An)
        arr[(0b110 << 3) | i] = true; // (disp, An, Xn)
    }
    arr[0b111'010] = true; // (disp, PC)
    arr[0b111'011] = true; // (disp, PC, Xn)
    arr[0b111'000] = true; // (xxx).w
    arr[0b111'001] = true; // (xxx).l
    arr[0b111'100] = true; // #imm
    return arr;
}();

// Valid data addressing modes
inline constexpr auto kValidDataAddrModes = [] {
    std::array<bool, 0b111'111 + 1> arr = kValidAddrModes;
    for (int i = 0b000; i <= 0b111; i++) {
        arr[(0b001 << 3) | i] = false; // An
    }
    return arr;
}();

// Valid memory addressing modes
inline constexpr auto kValidMemoryAddrModes = [] {
    std::array<bool, 0b111'111 + 1> arr = kValidAddrModes;
    for (int i = 0b000; i <= 0b111; i++) {
        arr[(0b000 << 3) | i] = false; // Dn
        arr[(0b001 << 3) | i] = false; // An
    }
    return arr;
}();

// Valid control addressing modes
inline constexpr auto kValidControlAddrModes = [] {
    std::array<bool, 0b111'111 + 1> arr = kValidAddrModes;
    for (int i = 0b000; i <= 0b111; i++) {
        arr[(0b000 << 3) | i] = false; // Dn
        arr[(0b001 << 3) | i] = false; // An
        arr[(0b011 << 3) | i] = false; // (An)+
        arr[(0b100 << 3) | i] = false; // -(An)
    }
    arr[0b111'100] = false; // #imm
    return arr;
}();

// Valid alterable addressing modes
inline constexpr auto kValidAlterableAddrModes = [] {
    std::array<bool, 0b111'111 + 1> arr = kValidAddrModes;
    arr[0b111'010] = false; // (disp, PC)
    arr[0b111'011] = false; // (disp, PC, Xn)
    arr[0b111'100] = false; // #imm
    return arr;
}();

// Valid data alterable addressing modes
inline constexpr auto kValidDataAlterableAddrModes = [] {
    std::array<bool, 0b111'111 + 1> arr{};
    for (int i = 0; i < arr.size(); i++) {
        arr[i] = kValidDataAddrModes[i] && kValidAlterableAddrModes[i];
    }
    return arr;
}();

// Valid memory alterable addressing modes
inline constexpr auto kValidMemoryAlterableAddrModes = [] {
    std::array<bool, 0b111'111 + 1> arr{};
    for (int i = 0; i < arr.size(); i++) {
        arr[i] = kValidMemoryAddrModes[i] && kValidAlterableAddrModes[i];
    }
    return arr;
}();

// Valid control alterable addressing modes
inline constexpr auto kValidControlAlterableAddrModes = [] {
    std::array<bool, 0b111'111 + 1> arr{};
    for (int i = 0; i < arr.size(); i++) {
        arr[i] = kValidControlAddrModes[i] && kValidAlterableAddrModes[i];
    }
    return arr;
}();

} // namespace brimir::m68k
