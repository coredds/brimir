// Brimir - ST-V ROM file loader implementation (multi-file MAME ROM sets + zip)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_loader.hpp"
#include "stv_cartridge.hpp"
#include "stv_zip.hpp"

#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>

namespace brimir::stv {

static bool ReadFile(const std::filesystem::path &path, std::vector<uint8> &out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    auto size = file.tellg();
    if (size <= 0) return false;
    file.seekg(0);
    out.resize(static_cast<size_t>(size));
    file.read(reinterpret_cast<char *>(out.data()), size);
    return !file.fail();
}

// Map raw bytes into the ROM buffer according to the mapping type
static void MapROMData(std::vector<uint8> &romData, const STVROMLayoutEntry &layout,
                       const std::vector<uint8> &fileData) {
    if (layout.map == STV_MAP_BYTE) {
        for (uint32_t j = 0; j < layout.size && layout.offset + (j << 1) < romData.size(); j++) {
            romData[layout.offset + (j << 1)] = fileData[j];
        }
    } else {
        uint32_t copySize = layout.size;
        uint32_t dstOff = layout.offset;
        if (dstOff + copySize > romData.size()) {
            copySize = static_cast<uint32_t>(romData.size() - dstOff);
        }
        std::memcpy(&romData[dstOff], fileData.data(), copySize);

        if (layout.map == STV_MAP_16LE) {
#ifdef _MSC_VER
            for (uint32_t j = 0; j + 1 < copySize; j += 2) {
                uint8_t t = romData[dstOff + j];
                romData[dstOff + j] = romData[dstOff + j + 1];
                romData[dstOff + j + 1] = t;
            }
#else
            if constexpr (std::endian::native == std::endian::little) {
                for (uint32_t j = 0; j + 1 < copySize; j += 2) {
                    uint8_t t = romData[dstOff + j];
                    romData[dstOff + j] = romData[dstOff + j + 1];
                    romData[dstOff + j + 1] = t;
                }
            }
#endif
        }
    }
}

STVLoadResult LoadSTVGameROM(const std::filesystem::path &romPath,
                             std::vector<uint8> &romData,
                             const STVGameInfo *&outGameInfo) {
    STVLoadResult result;
    outGameInfo = nullptr;

    std::string pathExt = romPath.extension().string();
    for (auto &c : pathExt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    // --- ZIP file path ---
    if (pathExt == ".zip") {
        auto zip = std::make_unique<ZipReader>(romPath);
        if (!zip->IsOpen()) {
            result.errorMessage = "ST-V ROM: Failed to open zip: " + romPath.string();
            return result;
        }

        // Find which game this zip contains by matching layout entries
        const STVGameInfo *game = LookupSTVByZipEntries(*zip);
        if (!game) {
            result.errorMessage = "ST-V ROM: Unknown ROM set in zip: " + romPath.string();
            return result;
        }

        romData.assign(ymir::cart::kSTVGameROMMaxSize, 0xFFu);

        for (const auto &layout : game->rom_layout) {
            if (!layout.fname || layout.size == 0) break;
            std::vector<uint8> fileData;
            if (!zip->ReadEntry(layout.fname, fileData)) {
                result.errorMessage = std::string("ST-V ROM: Missing in zip: ") + layout.fname;
                return result;
            }
            MapROMData(romData, layout, fileData);
        }

        outGameInfo = game;
        result.succeeded = true;
        return result;
    }

    // --- Single file / directory path ---
    const STVGameInfo *game = LookupSTVGame(romPath.filename().string().c_str());

    if (!game) {
        // Not in database by filename — scan directory for matching ROM files
        std::filesystem::path romDir = romPath.parent_path();
        if (romDir.empty()) romDir = ".";

        game = LookupSTVGameInDir(romDir);
        if (!game) {
            // Still not found — try as single merged ROM
            std::ifstream file(romPath, std::ios::binary | std::ios::ate);
            if (!file) {
                result.errorMessage = "ST-V ROM: Cannot open file: " + romPath.string();
                return result;
            }
            auto fileSize = file.tellg();
            if (fileSize <= 0 || static_cast<size_t>(fileSize) > ymir::cart::kSTVGameROMMaxSize) {
                result.errorMessage = "ST-V ROM: File size invalid";
                return result;
            }
            file.seekg(0);
            romData.resize(static_cast<size_t>(fileSize));
            file.read(reinterpret_cast<char *>(romData.data()), fileSize);
            if (file.fail()) {
                result.errorMessage = "ST-V ROM: Failed to read file";
                return result;
            }
            outGameInfo = nullptr;
            result.succeeded = true;
            return result;
        }
        // Game found via directory scan — use the directory from the loaded file's parent
    }

    // Multi-file ROM set from directory
    std::filesystem::path romDir = romPath.parent_path();
    if (romDir.empty()) romDir = ".";

    romData.assign(ymir::cart::kSTVGameROMMaxSize, 0xFFu);

    for (const auto &layout : game->rom_layout) {
        if (!layout.fname || layout.size == 0) break;

        std::vector<uint8> fileData;
        std::filesystem::path filePath = romDir / layout.fname;

        if (!ReadFile(filePath, fileData)) {
            if (romPath.filename() == layout.fname) {
                std::ifstream f(romPath, std::ios::binary | std::ios::ate);
                if (!f) {
                    result.errorMessage = std::string("Missing ROM file: ") + layout.fname;
                    return result;
                }
                auto sz = f.tellg();
                f.seekg(0);
                fileData.resize(static_cast<size_t>(sz));
                f.read(reinterpret_cast<char *>(fileData.data()), sz);
                if (f.fail()) {
                    result.errorMessage = std::string("Failed to read: ") + layout.fname;
                    return result;
                }
            } else {
                result.errorMessage = std::string("Missing ROM file: ") + layout.fname;
                return result;
            }
        }

        if (fileData.size() < layout.size) {
            result.errorMessage = std::string("ROM file too small: ") + layout.fname;
            return result;
        }

        MapROMData(romData, layout, fileData);
    }

    outGameInfo = game;
    result.succeeded = true;
    return result;
}

} // namespace brimir::stv
