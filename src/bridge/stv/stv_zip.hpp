// Brimir - Minimal ZIP reader for ST-V ROM sets
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Supports store (method 0) and deflate (method 8) entries.

#pragma once

#include <ymir/core/types.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace brimir::stv {

struct ZipEntry {
    uint32_t offset;           // File offset of local header
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint16_t compressionMethod; // 0=store, 8=deflate
    uint16_t filenameLength;
    uint16_t extraLength;
};

class ZipReader {
public:
    explicit ZipReader(const std::filesystem::path &path);
    ~ZipReader();

    bool IsOpen() const { return m_file.is_open(); }

    bool HasEntry(const std::string &name) const;
    bool ReadEntry(const std::string &name, std::vector<uint8> &out);

private:
    std::ifstream m_file;
    std::unordered_map<std::string, ZipEntry> m_entries;
    bool ScanEntries();
};

} // namespace brimir::stv
