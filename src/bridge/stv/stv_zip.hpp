// Brimir - Minimal ZIP reader for ST-V ROM sets
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Supports store (method 0) and deflate (method 8) entries. Entries are indexed from the central directory (so data
// descriptors and archive comments work) and matched by base name, ignoring the directory part and the case, as ROM
// managers differ in both. Zip64 archives are not supported: they are reported as not open.

#pragma once

#include <ymir/core/types.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace brimir::stv {

struct ZipEntry {
    uint32_t localHeaderOffset; // File offset of the entry's local header
    uint32_t compressedSize;
    uint32_t uncompressedSize;
    uint32_t crc32;             // As stored in the central directory
    uint16_t compressionMethod; // 0=store, 8=deflate
    uint16_t flags;
};

class ZipReader {
public:
    explicit ZipReader(const std::filesystem::path &path);
    ~ZipReader();

    // True when the file opened and holds a readable zip central directory (not truncated, not zip64, not another
    // kind of file). An archive with no entries is open.
    bool IsOpen() const { return m_file.is_open() && m_valid; }

    bool HasEntry(const std::string &name) const;
    // The entry's uncompressed data. Fails (out empty) when it cannot be read or does not match the CRC-32 in the
    // central directory; the second form says why in error (without the entry's name).
    bool ReadEntry(const std::string &name, std::vector<uint8> &out);
    bool ReadEntry(const std::string &name, std::vector<uint8> &out, std::string &error);

    // The CRC-32 of the uncompressed data as the archive states it (ReadEntry checks the data against it); nullopt if
    // there is no such entry.
    std::optional<uint32_t> EntryCRC(const std::string &name) const;

    // Base names of the file entries as stored (original case, no directory), in archive order.
    std::vector<std::string> EntryNames() const { return m_names; }

private:
    std::ifstream m_file;
    std::streamoff m_fileSize = 0;
    bool m_valid = false;
    std::unordered_map<std::string, ZipEntry> m_entries; // key: lower-case base name
    std::vector<std::string> m_names;
    bool ScanEntries();
};

} // namespace brimir::stv
