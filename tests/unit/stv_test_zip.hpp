// Brimir - Test helper: writes small ZIP files (stored or deflated entries) for the ST-V zip tests
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#pragma once

#include <zlib.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace brimir::test {

struct TestZipEntry {
    std::string name;          // stored verbatim, so "sub/c.bin" and "A.BIN" are possible
    std::vector<uint8_t> data; // uncompressed bytes
    bool deflate = false;      // false = stored (method 0), true = raw deflate (method 8)
    std::optional<uint32_t> crcOverride; // written as the CRC-32 field instead of the computed one
    bool dataDescriptor = false; // bit 3: the local header has zero CRC/sizes, a descriptor follows the data
};

namespace detail {

inline void PutU16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

inline void PutU32(std::vector<uint8_t> &out, uint32_t v) {
    PutU16(out, static_cast<uint16_t>(v));
    PutU16(out, static_cast<uint16_t>(v >> 16));
}

inline std::vector<uint8_t> RawDeflate(const std::vector<uint8_t> &in) {
    z_stream strm = {};
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        throw std::runtime_error("deflateInit2 failed");
    }
    std::vector<uint8_t> out(deflateBound(&strm, static_cast<uLong>(in.size())) + 16);
    strm.next_in = const_cast<Bytef *>(in.data());
    strm.avail_in = static_cast<uInt>(in.size());
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());
    const int ret = deflate(&strm, Z_FINISH);
    out.resize(strm.total_out);
    deflateEnd(&strm);
    if (ret != Z_STREAM_END) {
        throw std::runtime_error("deflate failed");
    }
    return out;
}

inline const std::array<uint32_t, 256> &CrcTable() {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    return table;
}

} // namespace detail

// size zero bytes, except the last four, which are chosen so that the CRC-32 of the whole is crc: stands in for a ROM
// file of the database (its size and CRC-32) where the loader checks the data against the archive's CRC-32.
// After the last four bytes the CRC register is T[i3] ^ T[i2] >> 8 ^ T[i1] >> 16 ^ T[i0] >> 24, whatever came before;
// the top byte of each table entry is unique, so the indices follow from the wanted register one at a time.
inline std::vector<uint8_t> DataWithCrc32(size_t size, uint32_t crc) {
    if (size < 4) throw std::runtime_error("DataWithCrc32 needs at least four bytes");
    const auto &table = detail::CrcTable();
    uint8_t index[4];
    uint32_t reg = crc ^ 0xFFFFFFFFu;
    for (int i = 3; i >= 0; --i) {
        uint32_t found = 256;
        for (uint32_t j = 0; j < 256; ++j) {
            if ((table[j] >> 24) == (reg >> 24)) {
                found = j;
                break;
            }
        }
        index[i] = static_cast<uint8_t>(found);
        reg = (reg ^ table[found]) << 8;
    }

    std::vector<uint8_t> data(size, 0);
    uint32_t state = 0xFFFFFFFFu;
    for (size_t n = 0; n < size - 4; ++n) state = (state >> 8) ^ table[state & 0xFF]; // zero bytes
    for (int i = 0; i < 4; ++i) {
        data[size - 4 + i] = static_cast<uint8_t>((state ^ index[i]) & 0xFF);
        state = (state >> 8) ^ table[index[i]];
    }
    return data;
}

// Serializes a ZIP archive: local headers with data, then the central directory and end record.
// comment is the archive comment after the end record.
inline std::vector<uint8_t> BuildTestZip(const std::vector<TestZipEntry> &entries, const std::string &comment = {}) {
    std::vector<uint8_t> out;
    std::vector<uint8_t> central;

    for (const TestZipEntry &entry : entries) {
        const uint32_t crc = entry.crcOverride.value_or(static_cast<uint32_t>(
            crc32(0, entry.data.empty() ? Z_NULL : entry.data.data(), static_cast<uInt>(entry.data.size()))));
        const std::vector<uint8_t> payload = entry.deflate ? detail::RawDeflate(entry.data) : entry.data;
        const uint16_t method = entry.deflate ? 8 : 0;
        const uint32_t localOffset = static_cast<uint32_t>(out.size());
        const uint16_t nameLen = static_cast<uint16_t>(entry.name.size());

        const uint16_t flags = entry.dataDescriptor ? 0x0008 : 0;

        detail::PutU32(out, 0x04034b50);
        detail::PutU16(out, 20);     // version needed
        detail::PutU16(out, flags);
        detail::PutU16(out, method);
        detail::PutU16(out, 0);      // mod time
        detail::PutU16(out, 0x21);   // mod date (1980-01-01)
        detail::PutU32(out, entry.dataDescriptor ? 0 : crc);
        detail::PutU32(out, entry.dataDescriptor ? 0 : static_cast<uint32_t>(payload.size()));
        detail::PutU32(out, entry.dataDescriptor ? 0 : static_cast<uint32_t>(entry.data.size()));
        detail::PutU16(out, nameLen);
        detail::PutU16(out, 0);      // extra length
        out.insert(out.end(), entry.name.begin(), entry.name.end());
        out.insert(out.end(), payload.begin(), payload.end());
        if (entry.dataDescriptor) {
            detail::PutU32(out, 0x08074b50);
            detail::PutU32(out, crc);
            detail::PutU32(out, static_cast<uint32_t>(payload.size()));
            detail::PutU32(out, static_cast<uint32_t>(entry.data.size()));
        }

        detail::PutU32(central, 0x02014b50);
        detail::PutU16(central, 20); // version made by
        detail::PutU16(central, 20); // version needed
        detail::PutU16(central, flags);
        detail::PutU16(central, method);
        detail::PutU16(central, 0);
        detail::PutU16(central, 0x21);
        detail::PutU32(central, crc);
        detail::PutU32(central, static_cast<uint32_t>(payload.size()));
        detail::PutU32(central, static_cast<uint32_t>(entry.data.size()));
        detail::PutU16(central, nameLen);
        detail::PutU16(central, 0);  // extra length
        detail::PutU16(central, 0);  // comment length
        detail::PutU16(central, 0);  // disk number
        detail::PutU16(central, 0);  // internal attributes
        detail::PutU32(central, 0);  // external attributes
        detail::PutU32(central, localOffset);
        central.insert(central.end(), entry.name.begin(), entry.name.end());
    }

    const uint32_t centralOffset = static_cast<uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());

    detail::PutU32(out, 0x06054b50);
    detail::PutU16(out, 0);
    detail::PutU16(out, 0);
    detail::PutU16(out, static_cast<uint16_t>(entries.size()));
    detail::PutU16(out, static_cast<uint16_t>(entries.size()));
    detail::PutU32(out, static_cast<uint32_t>(central.size()));
    detail::PutU32(out, centralOffset);
    detail::PutU16(out, static_cast<uint16_t>(comment.size()));
    out.insert(out.end(), comment.begin(), comment.end());
    return out;
}

inline void WriteTestZip(const std::filesystem::path &path, const std::vector<TestZipEntry> &entries,
                         const std::string &comment = {}) {
    const std::vector<uint8_t> bytes = BuildTestZip(entries, comment);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

} // namespace brimir::test
