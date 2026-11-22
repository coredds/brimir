// Brimir - Minimal ZIP reader implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_zip.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>

namespace brimir::stv {

static constexpr uint32_t kLocalFileHeaderSig = 0x04034b50;
static constexpr uint32_t kCentralHeaderSig = 0x02014b50;
static constexpr uint32_t kEndRecordSig = 0x06054b50;
static constexpr uint32_t kZip64LocatorSig = 0x07064b50;
static constexpr size_t kLocalHeaderSize = 30;
static constexpr size_t kCentralHeaderSize = 46;
static constexpr size_t kEndRecordSize = 22;
static constexpr uint16_t kFlagEncrypted = 0x0001;
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
    if (m_fileSize < static_cast<std::streamoff>(kEndRecordSize)) return false;

    // The end record sits at the end of the file, before an optional comment of up to 64 KiB.
    const std::streamoff tailSize =
        std::min<std::streamoff>(m_fileSize, static_cast<std::streamoff>(kEndRecordSize + 0xFFFF));
    std::vector<uint8> tail(static_cast<size_t>(tailSize));
    m_file.seekg(m_fileSize - tailSize);
    m_file.read(reinterpret_cast<char *>(tail.data()), tailSize);
    if (m_file.gcount() != tailSize) {
        m_file.clear();
        return false;
    }

    size_t end = tail.size() - kEndRecordSize + 1;
    const uint8 *record = nullptr;
    while (end-- > 0) {
        const uint8 *p = tail.data() + end;
        if (ReadU32LE(p) == kEndRecordSig && end + kEndRecordSize + ReadU16LE(p + 20) == tail.size()) {
            record = p;
            break;
        }
    }
    if (record == nullptr) return false;
    const std::streamoff recordPos = m_fileSize - tailSize + static_cast<std::streamoff>(record - tail.data());

    const uint16_t diskNumber = ReadU16LE(record + 4);
    const uint16_t entryCount = ReadU16LE(record + 10);
    const uint32_t directorySize = ReadU32LE(record + 12);
    const uint32_t directoryOffset = ReadU32LE(record + 16);

    // Zip64 archives mark the fields they moved to the zip64 end record
    const bool locatorBefore = record >= tail.data() + 20 && ReadU32LE(record - 20) == kZip64LocatorSig;
    if (locatorBefore || diskNumber != 0 || entryCount == 0xFFFF || directorySize == 0xFFFFFFFFu ||
        directoryOffset == 0xFFFFFFFFu) {
        return false;
    }
    if (static_cast<std::streamoff>(directoryOffset) + static_cast<std::streamoff>(directorySize) > recordPos) {
        return false;
    }

    std::vector<uint8> directory(directorySize);
    m_file.seekg(directoryOffset);
    m_file.read(reinterpret_cast<char *>(directory.data()), directorySize);
    if (m_file.gcount() != static_cast<std::streamsize>(directorySize)) {
        m_file.clear();
        return false;
    }

    size_t pos = 0;
    for (unsigned i = 0; i < entryCount; i++) {
        if (pos + kCentralHeaderSize > directory.size()) break;
        const uint8 *h = directory.data() + pos;
        if (ReadU32LE(h) != kCentralHeaderSig) break;

        ZipEntry entry;
        entry.flags             = ReadU16LE(h + 8);
        entry.compressionMethod = ReadU16LE(h + 10);
        entry.crc32             = ReadU32LE(h + 16);
        entry.compressedSize    = ReadU32LE(h + 20);
        entry.uncompressedSize  = ReadU32LE(h + 24);
        const size_t nameLength    = ReadU16LE(h + 28);
        const size_t extraLength   = ReadU16LE(h + 30);
        const size_t commentLength = ReadU16LE(h + 32);
        entry.localHeaderOffset = ReadU32LE(h + 42);

        if (pos + kCentralHeaderSize + nameLength + extraLength + commentLength > directory.size()) break;
        if (entry.compressedSize == 0xFFFFFFFFu || entry.uncompressedSize == 0xFFFFFFFFu ||
            entry.localHeaderOffset == 0xFFFFFFFFu) {
            m_entries.clear();
            m_names.clear();
            return false;
        }

        const std::string filename(reinterpret_cast<const char *>(h + kCentralHeaderSize), nameLength);
        pos += kCentralHeaderSize + nameLength + extraLength + commentLength;

        // Directory entries have no base name
        const std::string baseName = BaseName(filename);
        if (baseName.empty()) continue;

        const std::string key = LookupKey(baseName);
        if (m_entries.find(key) == m_entries.end()) {
            m_names.push_back(baseName);
        }
        m_entries[key] = entry;
    }

    m_valid = true;
    return true;
}

std::optional<uint32_t> ZipReader::EntryCRC(const std::string &name) const {
    const auto it = m_entries.find(LookupKey(name));
    if (it == m_entries.end()) return std::nullopt;
    return it->second.crc32;
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
    if (entry.flags & kFlagEncrypted) return false;
    if (entry.uncompressedSize > kMaxEntrySize) return false;

    // The central directory gives the sizes; the local header is only read to skip its name and extra fields
    // (which may differ from the central ones) and reach the data.
    uint8 local[kLocalHeaderSize];
    if (static_cast<std::streamoff>(entry.localHeaderOffset) + static_cast<std::streamoff>(kLocalHeaderSize) > m_fileSize) {
        return false;
    }
    m_file.clear();
    m_file.seekg(entry.localHeaderOffset);
    m_file.read(reinterpret_cast<char *>(local), kLocalHeaderSize);
    if (m_file.gcount() != static_cast<std::streamsize>(kLocalHeaderSize) || ReadU32LE(local) != kLocalFileHeaderSig) {
        return false;
    }
    const std::streamoff dataOffset = static_cast<std::streamoff>(entry.localHeaderOffset) + kLocalHeaderSize +
                                      ReadU16LE(local + 26) + ReadU16LE(local + 28);

    // An entry whose data runs past the end of the file is cut off
    if (dataOffset + static_cast<std::streamoff>(entry.compressedSize) > m_fileSize) {
        return false;
    }

    m_file.seekg(dataOffset);

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
