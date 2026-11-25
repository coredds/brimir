// Brimir - ST-V cartridge image builder implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_loader.hpp"

#include "stv_cartridge.hpp"

#include <algorithm>
#include <cstddef>

namespace brimir::stv {

namespace {

constexpr size_t kMaxImageSize = ymir::cart::kSTVGameROMMaxSize;

struct ResolvedEntry {
    const char *file;
    Load load; // never Reload
};

size_t NextPow2(size_t v) {
    size_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

// One past the last image byte the entry writes.
size_t MappedEnd(const RomEntry &entry, Load load) {
    const size_t size = entry.size;
    return load == Load::Byte16 ? size_t(entry.offset) + 2 * (size - 1) + 1 : size_t(entry.offset) + size;
}

void AppendName(std::string &list, const std::string &name) {
    if (!list.empty()) {
        list += ", ";
    }
    list += name;
}

} // namespace

// Byte placement, derived from MAME (src/mame/sega/stv.cpp, src/emu/romload.cpp, src/emu/romentry.h):
//
// 1. The cartridge is declared ROM_REGION32_BE( 0x3000000, "cart", ROMREGION_ERASE00 ) in the ROM_START of every
//    supported set (stv.cpp, e.g. bakubaku; only nameclub and nclubv2 use ROMREGION_ERASEFF), and the machine copies it
//    into the "abus" region, which is mapped with map(0x02000000, 0x04ffffff).rom().mirror(0x20000000).region("abus", 0)
//    (stv_mem; the copy is stv_select_game). ROMREGION_ERASE00 fills the gaps with 0x00: MAME 0.289's SH-2 reads
//    0x00530045 at 0x02000000 in bakubaku.
// 2. A ROM_LOAD macro writes the file into the region bytes in file order, at region offset + n * (groupsize + skip)
//    (romload.cpp read_rom_data, lines 812-929; macros in romentry.h):
//      ROM_LOAD16_BYTE       = ROM_SKIP(1): file byte i goes to offset + 2 * i. No odd/even compensation exists, so
//                              an odd offset is the odd addresses (romentry.h:145).
//      ROM_LOAD16_WORD_SWAP  = ROM_GROUPWORD | ROM_REVERSE: each pair of file bytes (b0, b1) goes to (offset + 1,
//                              offset) i.e. swapped (romentry.h:147, romload.cpp:884-893).
//      ROM_LOAD              = the file as is.
//      ROM_RELOAD            = the previous entry's file again with its flags (ROM_INHERITFLAGS), at a new offset.
//      ROM_RELOAD_PLAIN      = the previous entry's file again without flags, i.e. as ROM_LOAD (romentry.h:159-160).
// 3. region_post_process (romload.cpp:688-721) then byte-swaps every 32-bit word of a big-endian region on a little-endian
//    host, so that the host's native 32-bit read at a region address returns the value whose big-endian bytes are the
//    bytes of step 2. The SH-2 is big-endian and reads this region as 32-bit words, so the byte it sees at
//    0x02000000 + a is the byte step 2 put at a, on any host. The image built here is that byte string.
//    (MAME's own header dump in stv.cpp:464-491 agrees: it indexes the little-endian host memory with ^3, finds the
//    header at 0x200000 for the IC7/plain layout, and for the IC13 layout takes every other byte, starting at address 1:
//    "IC13 roms are loaded on ODD bytes only", stv.cpp:66.)
//
// Example: bakubaku's IC13 file "fpr17969.13" starts "SEGA ST-V(TITAN)". ROM_LOAD16_BYTE at offset 1 puts S,E,G,A at
// 0x02000001, 3, 5, 7 (0x00 on the even addresses), and the ROM_RELOAD_PLAIN copies put "SEGA ST-V(TITAN)" at 0x02200000
// and 0x02300000 as a plain byte string.
bool BuildCartImage(ZipReader &zip, std::span<const RomEntry> roms, std::vector<uint8_t> &image, std::string &error) {
    image.clear();
    error.clear();

    if (roms.empty()) {
        error = "no ROM files are listed";
        return false;
    }

    // Resolve Reload against the previous entry, then check the layout before reading any file.
    std::vector<ResolvedEntry> resolved;
    resolved.reserve(roms.size());
    size_t mappedEnd = 0;
    for (size_t i = 0; i < roms.size(); ++i) {
        const RomEntry &entry = roms[i];
        ResolvedEntry r{entry.file, entry.load};
        if (entry.load == Load::Reload) {
            if (i == 0) {
                error = std::string("the first ROM entry (") + entry.file + ") cannot be a reload";
                return false;
            }
            r = resolved[i - 1];
        }
        if (entry.size == 0 || (r.load == Load::WordSwap && (entry.size & 1))) {
            error = std::string("invalid size for ") + entry.file;
            return false;
        }
        const size_t end = MappedEnd(entry, r.load);
        if (end > kMaxImageSize) {
            error = std::string(entry.file) + " does not fit in the cartridge window";
            return false;
        }
        mappedEnd = std::max(mappedEnd, end);
        resolved.push_back(r);
    }

    std::string missing;
    for (const ResolvedEntry &r : resolved) {
        const std::string name = r.file;
        if (!zip.HasEntry(name) && missing.find(name) == std::string::npos) {
            AppendName(missing, name);
        }
    }
    if (!missing.empty()) {
        error = "missing ROM files: " + missing;
        return false;
    }

    image.assign(std::min(NextPow2(mappedEnd), kMaxImageSize), 0x00);

    std::vector<uint8_t> data;
    const char *loaded = nullptr;
    for (size_t i = 0; i < roms.size(); ++i) {
        const RomEntry &entry = roms[i];
        const ResolvedEntry &r = resolved[i];

        if (!loaded || std::string(loaded) != r.file) {
            std::string reason;
            if (!zip.ReadEntry(r.file, data, reason)) {
                error = std::string("cannot read ") + r.file + ": " + reason;
                return false;
            }
            loaded = r.file;
        }
        if (data.size() != entry.size) {
            error = std::string(r.file) + " has the wrong size (" + std::to_string(data.size()) + " bytes, expected " +
                    std::to_string(entry.size) + ")";
            return false;
        }

        uint8_t *dst = image.data() + entry.offset;
        switch (r.load) {
        case Load::WordSwap:
            for (size_t n = 0; n < data.size(); ++n) {
                dst[n ^ 1] = data[n];
            }
            break;
        case Load::Byte16:
            for (size_t n = 0; n < data.size(); ++n) {
                dst[2 * n] = data[n];
            }
            break;
        case Load::Plain:
        case Load::Reload:
            std::copy(data.begin(), data.end(), dst);
            break;
        }
    }
    return true;
}

bool BuildCartImage(ZipReader &zip, const GameInfo &game, std::vector<uint8_t> &image, std::string &error) {
    if (BuildCartImage(zip, game.roms, image, error)) {
        return true;
    }
    error = std::string(game.shortName) + ": " + error;
    return false;
}

// ROM_REGION16_BE( 0x80, "eeprom", 0 ) with ROM_LOAD: the file's bytes are the region's, cell n the big-endian word at
// 2n. MAME's eeprom_base_device::nvram_default reads the region the same way (16-bit cells, big-endian region).
bool ReadDefaultEEPROM(ZipReader &zip, const GameInfo &game, std::array<uint8_t, 128> &cells, std::string &error) {
    error.clear();
    cells.fill(0xFF);
    if (game.eeprom.file == nullptr) {
        return true;
    }
    std::vector<uint8_t> data;
    std::string reason;
    if (!zip.ReadEntry(game.eeprom.file, data, reason)) {
        error = std::string(game.shortName) + ": cannot read " + game.eeprom.file + ": " + reason;
        return false;
    }
    if (data.size() != cells.size()) {
        error = std::string(game.shortName) + ": " + game.eeprom.file + " has the wrong size (" +
                std::to_string(data.size()) + " bytes, expected " + std::to_string(cells.size()) + ")";
        return false;
    }
    std::copy(data.begin(), data.end(), cells.begin());
    return true;
}

} // namespace brimir::stv
