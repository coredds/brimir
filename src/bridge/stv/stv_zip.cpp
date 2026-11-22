// Brimir - Minimal ZIP reader implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_zip.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>

namespace brimir::stv {

static constexpr uint32_t kLocalFileHeaderSig = 0x04034b50;
static constexpr uint16_t kFlagDataDescriptor = 0x0008; // sizes follow the data, the header has zeros
static constexpr uint32_t kMaxEntrySize = 256u * 1024u * 1024u;

static uint32_t ReadU32LE(const uint8 *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint16_t ReadU16LE(const uint8 *p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static std::string BaseName(const std::string &name) {
    const auto slash = name.find_last_of("/\\");
    return (slash != std::string::npos) ? name.substr(slash + 1) : name;
}

static std::string LookupKey(const std::string &name) {
    std::string key = BaseName(name);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

ZipReader::ZipReader(const std::filesystem::path &path) {
    m_file.open(path, std::ios::binary);
    if (m_file.is_open()) {
        ScanEntries();
    }
}

ZipReader::~ZipReader() {
    if (m_file.is_open()) m_file.close();
}

bool ZipReader::ScanEntries() {
    m_file.seekg(0, std::ios::end);
    m_fileSize = static_cast<std::streamoff>(m_file.tellg());

    std::streamoff pos = 0;
    uint8 header[30];
    while (pos + 30 <= m_fileSize) {
        m_file.seekg(pos);
        m_file.read(reinterpret_cast<char *>(header), 30);
        if (m_file.gcount() < 30) break;

        if (ReadU32LE(header) != kLocalFileHeaderSig) break;

        // Sizes are only known from the central directory when a data descriptor follows the data.
        if (ReadU16LE(header + 6) & kFlagDataDescriptor) break;

        ZipEntry entry;
        entry.compressionMethod = ReadU16LE(header + 8);
        entry.compressedSize    = ReadU32LE(header + 18);
        entry.uncompressedSize  = ReadU32LE(header + 22);
        entry.filenameLength    = ReadU16LE(header + 26);
        entry.extraLength       = ReadU16LE(header + 28);

        std::string filename(entry.filenameLength, '\0');
        m_file.read(filename.data(), entry.filenameLength);
        if (m_file.gcount() < entry.filenameLength) break;

        const std::streamoff dataPos = pos + 30 + entry.filenameLength + entry.extraLength;
        entry.offset = static_cast<uint32_t>(dataPos);
        pos = dataPos + static_cast<std::streamoff>(entry.compressedSize);

        // Directory entries have no base name
        const std::string baseName = BaseName(filename);
        if (baseName.empty()) continue;

        const std::string key = LookupKey(baseName);
        if (m_entries.find(key) == m_entries.end()) {
            m_names.push_back(baseName);
        }
        m_entries[key] = entry;
    }

    m_file.clear();
    return !m_entries.empty();
}

bool ZipReader::HasEntry(const std::string &name) const {
    return m_entries.find(LookupKey(name)) != m_entries.end();
}

bool ZipReader::ReadEntry(const std::string &name, std::vector<uint8> &out) {
    out.clear();
    auto it = m_entries.find(LookupKey(name));
    if (it == m_entries.end()) return false;

    const auto &entry = it->second;
    if (entry.compressionMethod != 0 && entry.compressionMethod != 8) return false;
    if (entry.uncompressedSize > kMaxEntrySize) return false;

    // An entry whose data runs past the end of the file is cut off
    if (static_cast<std::streamoff>(entry.offset) + static_cast<std::streamoff>(entry.compressedSize) > m_fileSize) {
        return false;
    }

    m_file.clear();
    m_file.seekg(entry.offset);

    if (entry.compressionMethod == 0) {
        if (entry.compressedSize != entry.uncompressedSize) return false;
        out.resize(entry.compressedSize);
        m_file.read(reinterpret_cast<char *>(out.data()), entry.compressedSize);
        if (m_file.gcount() != static_cast<std::streamsize>(entry.compressedSize)) {
            out.clear();
            return false;
        }
        return true;
    }

    std::vector<uint8> compressed(entry.compressedSize);
    m_file.read(reinterpret_cast<char *>(compressed.data()), entry.compressedSize);
    if (m_file.gcount() != static_cast<std::streamsize>(entry.compressedSize)) return false;

    if (entry.uncompressedSize == 0) return true;

    out.resize(entry.uncompressedSize);

    z_stream strm = {};
    strm.next_in = compressed.data();
    strm.avail_in = entry.compressedSize;
    strm.next_out = out.data();
    strm.avail_out = entry.uncompressedSize;

    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        out.clear();
        return false;
    }
    const int ret = inflate(&strm, Z_FINISH);
    const uLong produced = strm.total_out;
    inflateEnd(&strm);
    if (ret != Z_STREAM_END || produced != entry.uncompressedSize) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace brimir::stv
