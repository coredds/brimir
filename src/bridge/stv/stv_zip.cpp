// Brimir - Minimal ZIP reader implementation
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_zip.hpp"

#include <cstring>

// Declare zlib functions manually to avoid pulling in zlib.lib (DLL import).
// zlibstatic.lib is already linked via chdr-static.
// Brimir ships zlib 1.3.1 via libchdr.

typedef unsigned char  Bytef;
typedef unsigned int   uInt;
typedef unsigned long  uLong;
typedef void          *voidpf;

typedef struct z_stream_s {
    const Bytef *next_in;   /* next input byte */
    uInt         avail_in;  /* number of bytes available at next_in */
    uLong        total_in;  /* total number of input bytes read so far */
    Bytef       *next_out;  /* next output byte will go here */
    uInt         avail_out; /* remaining free space at next_out */
    uLong        total_out; /* total number of bytes output so far */
    const char  *msg;       /* last error message, NULL if no error */
    void        *state;     /* not visible by applications */
    voidpf     (*zalloc)(voidpf opaque, uInt items, uInt size);
    void       (*zfree)(voidpf opaque, voidpf address);
    voidpf       opaque;    /* private data object passed to zalloc and zfree */
    int          data_type; /* best guess about the data type: binary or text */
    uLong        adler;     /* Adler-32 or CRC-32 value of the uncompressed data */
    uLong        reserved;  /* reserved for future use */
} z_stream;
typedef z_stream *z_streamp;

extern "C" {
    int inflateInit2_(z_streamp strm, int windowBits, const char *version, int stream_size);
    int inflate(z_streamp strm, int flush);
    int inflateEnd(z_streamp strm);
}
#define Z_OK 0
#define Z_STREAM_END 1
#define Z_FINISH 4
#define MAX_WBITS 15

static int inflateInit2(z_stream *strm, int windowBits) {
    return inflateInit2_(strm, windowBits, "1.3.1", (int)sizeof(z_stream));
}

static int inflateRaw(z_stream *strm) {
    return inflate(strm, Z_FINISH);
}

namespace brimir::stv {

static constexpr uint32_t kLocalFileHeaderSig = 0x04034b50;

static uint32_t ReadU32LE(const uint8 *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static uint16_t ReadU16LE(const uint8 *p) {
    return p[0] | (p[1] << 8);
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
    auto fileSize = static_cast<std::streamoff>(m_file.tellg());
    m_file.seekg(0);

    uint8 header[30];
    while (m_file.tellg() < fileSize - 30) {
        m_file.read(reinterpret_cast<char *>(header), 30);
        if (m_file.gcount() < 30) break;

        uint32_t sig = ReadU32LE(header);
        if (sig != kLocalFileHeaderSig) break;

        ZipEntry entry;
        entry.compressionMethod = ReadU16LE(header + 8);
        entry.compressedSize    = ReadU32LE(header + 18);
        entry.uncompressedSize  = ReadU32LE(header + 22);
        entry.filenameLength    = ReadU16LE(header + 26);
        entry.extraLength       = ReadU16LE(header + 28);

        // Read filename
        std::string filename(entry.filenameLength, '\0');
        m_file.read(&filename[0], entry.filenameLength);
        if (m_file.gcount() < entry.filenameLength) break;

        // Skip extra field
        m_file.ignore(entry.extraLength);

        // Record entry position (data starts here)
        auto dataPos = m_file.tellg();
        entry.offset = static_cast<uint32_t>(dataPos);

        // Store entry (use basename for matching)
        auto slash = filename.find_last_of("/\\");
        std::string basename = (slash != std::string::npos) ? filename.substr(slash + 1) : filename;
        m_entries[basename] = entry;

        // Skip compressed data
        m_file.seekg(dataPos + static_cast<std::streamoff>(entry.compressedSize));
    }

    return !m_entries.empty();
}

bool ZipReader::HasEntry(const std::string &name) const {
    return m_entries.find(name) != m_entries.end();
}

bool ZipReader::ReadEntry(const std::string &name, std::vector<uint8> &out) {
    auto it = m_entries.find(name);
    if (it == m_entries.end()) return false;

    const auto &entry = it->second;

    // Seek to data
    m_file.seekg(entry.offset);

    if (entry.compressionMethod == 0) {
        out.resize(entry.compressedSize);
        m_file.read(reinterpret_cast<char *>(out.data()), entry.compressedSize);
        return m_file.gcount() == static_cast<std::streamsize>(entry.compressedSize);
    }

    if (entry.compressionMethod == 8) {
        std::vector<uint8> compressed(entry.compressedSize);
        m_file.read(reinterpret_cast<char *>(compressed.data()), entry.compressedSize);
        if (m_file.gcount() != static_cast<std::streamsize>(entry.compressedSize)) return false;

        out.resize(entry.uncompressedSize);

        z_stream strm = {};
        strm.next_in = compressed.data();
        strm.avail_in = entry.compressedSize;
        strm.next_out = out.data();
        strm.avail_out = entry.uncompressedSize;

        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return false;
        int ret = inflateRaw(&strm);
        inflateEnd(&strm);
        return ret == Z_STREAM_END;
    }

    return false;
}

} // namespace brimir::stv
