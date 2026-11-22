// Brimir - Test helpers for the ST-V cases that run real ROM sets: the local folders from the environment, a scratch
// system directory with the BIOS files, frame hashing, PNG screenshots and a session that drives a game frame by frame
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#pragma once

#include <brimir/core_wrapper.hpp>

#include <stv/stv_input.hpp>
#include <stv/stv_system.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace brimir::test {

struct ScratchDir {
    std::filesystem::path path;
    explicit ScratchDir(const char *name) : path(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;
};

// The folder named by an environment variable, or empty when it is unset or not a folder
inline std::filesystem::path EnvDir(const char *name) {
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return {};
    }
    std::error_code ec;
    return std::filesystem::is_directory(value, ec) ? std::filesystem::path(value) : std::filesystem::path{};
}

// Copies the 512 KiB files of a folder (the BIOS files) into a scratch system directory
inline void CopyBiosFiles(const std::filesystem::path &from, const std::filesystem::path &to) {
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(from, ec)) {
        if (entry.is_regular_file(ec) && entry.file_size(ec) == stv::kSTVBiosSize) {
            std::filesystem::copy_file(entry.path(), to / entry.path().filename(),
                                       std::filesystem::copy_options::skip_existing, ec);
        }
    }
}

// FNV-1a over the frame's size and pixels only (the audio changes every frame even on a still screen)
inline uint64_t VideoHash(CoreWrapper &core) {
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](const void *data, size_t size) {
        const auto *bytes = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < size; i++) {
            hash = (hash ^ bytes[i]) * 0x100000001b3ull;
        }
    };
    const unsigned width = core.GetFramebufferWidth();
    const unsigned height = core.GetFramebufferHeight();
    const unsigned pitch = core.GetFramebufferPitch();
    mix(&width, sizeof(width));
    mix(&height, sizeof(height));
    const auto *pixels = static_cast<const uint8_t *>(core.GetFramebuffer());
    for (unsigned y = 0; pixels != nullptr && y < height; y++) {
        mix(pixels + static_cast<size_t>(y) * pitch, static_cast<size_t>(width) * 4);
    }
    return hash;
}

inline uint32_t Crc32(const uint8_t *data, size_t size, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// The frame (XRGB8888) as an RGB PNG with stored (uncompressed) deflate blocks, for a human to compare with MAME
inline void WritePng(CoreWrapper &core, const std::filesystem::path &path) {
    const unsigned width = core.GetFramebufferWidth();
    const unsigned height = core.GetFramebufferHeight();
    const unsigned pitch = core.GetFramebufferPitch();
    const auto *pixels = static_cast<const uint8_t *>(core.GetFramebuffer());
    if (pixels == nullptr || width == 0 || height == 0) {
        return;
    }
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
    for (unsigned y = 0; y < height; y++) {
        raw.push_back(0); // filter: none
        const uint8_t *row = pixels + static_cast<size_t>(y) * pitch;
        for (unsigned x = 0; x < width; x++) {
            raw.push_back(row[x * 4 + 2]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 0]);
        }
    }
    std::vector<uint8_t> zlib = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t len = std::min<size_t>(raw.size() - pos, 0xFFFF);
        const bool last = pos + len == raw.size();
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(static_cast<uint8_t>(len));
        zlib.push_back(static_cast<uint8_t>(len >> 8));
        zlib.push_back(static_cast<uint8_t>(~len));
        zlib.push_back(static_cast<uint8_t>(~len >> 8));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
                    raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
        pos += len;
        if (last) {
            break;
        }
    }
    uint32_t a = 1, b = 0;
    for (const uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int shift = 24; shift >= 0; shift -= 8) {
        zlib.push_back(static_cast<uint8_t>(adler >> shift));
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const auto be32 = [&](uint32_t v) {
        const uint8_t bytes[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                                  static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        out.write(reinterpret_cast<const char *>(bytes), 4);
    };
    const auto chunk = [&](const char *type, const std::vector<uint8_t> &data) {
        be32(static_cast<uint32_t>(data.size()));
        out.write(type, 4);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        uint32_t crc = Crc32(reinterpret_cast<const uint8_t *>(type), 4);
        crc = Crc32(data.data(), data.size(), crc);
        be32(crc);
    };
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    out.write(reinterpret_cast<const char *>(kSignature), sizeof(kSignature));
    std::vector<uint8_t> ihdr;
    for (const uint32_t v : {width, height}) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            ihdr.push_back(static_cast<uint8_t>(v >> shift));
        }
    }
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
    chunk("IHDR", ihdr);
    chunk("IDAT", zlib);
    chunk("IEND", {});
}

} // namespace brimir::test
