// Brimir - compatibility sweep helpers for brimir_bench (--input-script, --screenshot-every, --sweep-summary)
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0
//
// Pure, header-only helpers (tested by tests/unit/test_bench_sweep.cpp): the input script parser, a PNG encoder for
// the output frame, the tracker behind the sweep summary (longest run of identical frames, audio peak, longest silent
// stretch) and the summary's JSON text. See tools/README.md.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace brimir_tools {

// ---- input script ---------------------------------------------------------------------------------------------------

// libretro RETRO_DEVICE_ID_JOYPAD_* ids that the Control Pad layout (src/bridge/peripheral_input.cpp, kControlPadMap)
// maps to each Saturn button; the script names the Saturn buttons.
struct ScriptButton {
    const char* name;
    unsigned joypadId;
};

inline constexpr ScriptButton kScriptButtons[] = {
    {"up", 4},  {"down", 5}, {"left", 6}, {"right", 7}, {"start", 3}, {"a", 0}, {"b", 8},
    {"c", 1},   {"x", 9},    {"y", 12},   {"z", 13},    {"l", 10},    {"r", 11},
};

// One "button:hold/period" item: the button is down for `hold` frames at the start of every `period` frames.
struct InputItem {
    unsigned joypadId = 0;
    int hold = 0;
    int period = 0;
};

using InputScript = std::vector<InputItem>;

namespace detail {

inline bool ParseCount(std::string_view text, int& out) {
    if (text.empty() || text.size() > 8) {
        return false;
    }
    int value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + (ch - '0');
    }
    out = value;
    return true;
}

} // namespace detail

// Parses e.g. "start:120/600,a:30/900" (frames count from 0: Start is down in frames 0-119, 600-719, ...). Button
// names are the Saturn Control Pad's (up down left right start a b c x y z l r, any case); hold and period are whole
// frames with 1 <= hold <= period. Returns false and sets `error` for anything else, including an empty spec.
inline bool ParseInputScript(std::string_view spec, InputScript& out, std::string& error) {
    out.clear();
    if (spec.empty()) {
        error = "empty input script";
        return false;
    }
    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t comma = std::min(spec.find(',', pos), spec.size());
        const std::string_view item = spec.substr(pos, comma - pos);
        pos = comma + 1;

        const size_t colon = item.find(':');
        const size_t slash = item.find('/');
        if (colon == std::string_view::npos || slash == std::string_view::npos || slash < colon) {
            error = "invalid item '" + std::string(item) + "' (expected button:hold/period)";
            return false;
        }
        std::string name(item.substr(0, colon));
        for (char& ch : name) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch - 'A' + 'a');
            }
        }
        const ScriptButton* button = nullptr;
        for (const ScriptButton& candidate : kScriptButtons) {
            if (name == candidate.name) {
                button = &candidate;
            }
        }
        if (button == nullptr) {
            error = "unknown button '" + std::string(item.substr(0, colon)) +
                    "' (up down left right start a b c x y z l r)";
            return false;
        }
        InputItem parsed;
        parsed.joypadId = button->joypadId;
        if (!detail::ParseCount(item.substr(colon + 1, slash - colon - 1), parsed.hold) ||
            !detail::ParseCount(item.substr(slash + 1), parsed.period) || parsed.hold < 1 ||
            parsed.hold > parsed.period) {
            error = "invalid frame counts in '" + std::string(item) + "' (expected 1 <= hold <= period)";
            return false;
        }
        out.push_back(parsed);
    }
    return true;
}

// The libretro joypad bit mask (bit n = RETRO_DEVICE_ID_JOYPAD_n) held in `frame` (from 0)
inline uint16_t JoypadAt(const InputScript& script, int frame) {
    uint16_t mask = 0;
    for (const InputItem& item : script) {
        if (frame % item.period < item.hold) {
            mask = static_cast<uint16_t>(mask | (1u << item.joypadId));
        }
    }
    return mask;
}

// ---- PNG ------------------------------------------------------------------------------------------------------------

inline uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// The frame (XRGB8888, `pitchBytes` apart per row) as an RGB PNG with stored (uncompressed) deflate blocks. Empty for a
// missing or empty frame.
inline std::vector<uint8_t> EncodePng(uint32_t width, uint32_t height, const void* pixels, size_t pitchBytes) {
    std::vector<uint8_t> png;
    if (pixels == nullptr || width == 0 || height == 0) {
        return png;
    }
    const auto* src = static_cast<const uint8_t*>(pixels);
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) * 3 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0); // filter: none
        const uint8_t* row = src + static_cast<size_t>(y) * pitchBytes;
        for (uint32_t x = 0; x < width; ++x) {
            raw.push_back(row[x * 4 + 2]);
            raw.push_back(row[x * 4 + 1]);
            raw.push_back(row[x * 4 + 0]);
        }
    }
    std::vector<uint8_t> zlib = {0x78, 0x01};
    for (size_t pos = 0;;) {
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

    const auto be32 = [&png](uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            png.push_back(static_cast<uint8_t>(v >> shift));
        }
    };
    const auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(static_cast<uint32_t>(data.size()));
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>(type), 4);
        crc = Crc32(data.data(), data.size(), crc);
        be32(crc);
    };
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), kSignature, kSignature + sizeof(kSignature));
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
    return png;
}

// ---- hang and silence detection -------------------------------------------------------------------------------------

// A sample counts as silence when |sample| <= this
inline constexpr int kSilenceThreshold = 16;

struct SweepStats {
    int frames = 0;
    int identicalStart = 0;  // first frame of the longest run of identical frame hashes
    int identicalLength = 0; // its length in frames (the first of equally long runs)
    int audioPeak = 0;       // largest |sample| seen
    int silentStart = 0;     // first frame of the longest silent stretch
    int silentLength = 0;    // its length in frames (a frame is silent when every sample is within the threshold, or
                             // it has none)
};

class SweepTracker {
public:
    // One completed frame: the hash of its picture and its audio (`stereoFrames` L/R pairs of int16)
    void AddFrame(uint64_t frameHash, const int16_t* samples, size_t stereoFrames) {
        const int frame = m_stats.frames++;

        if (frame > 0 && frameHash == m_lastHash) {
            ++m_sameLength;
        } else {
            m_sameStart = frame;
            m_sameLength = 1;
        }
        m_lastHash = frameHash;
        if (m_sameLength > m_stats.identicalLength) {
            m_stats.identicalStart = m_sameStart;
            m_stats.identicalLength = m_sameLength;
        }

        bool silent = true;
        for (size_t i = 0; i < stereoFrames * 2; ++i) {
            const int magnitude = std::abs(static_cast<int>(samples[i]));
            m_stats.audioPeak = std::max(m_stats.audioPeak, magnitude);
            if (magnitude > kSilenceThreshold) {
                silent = false;
            }
        }
        if (silent) {
            if (m_silentLength == 0) {
                m_silentStart = frame;
            }
            ++m_silentLength;
            if (m_silentLength > m_stats.silentLength) {
                m_stats.silentStart = m_silentStart;
                m_stats.silentLength = m_silentLength;
            }
        } else {
            m_silentLength = 0;
        }
    }

    const SweepStats& Stats() const { return m_stats; }

private:
    SweepStats m_stats;
    uint64_t m_lastHash = 0;
    int m_sameStart = 0;
    int m_sameLength = 0;
    int m_silentStart = 0;
    int m_silentLength = 0;
};

// ---- summary --------------------------------------------------------------------------------------------------------

struct SweepSummary {
    SweepStats stats;
    std::string loadError; // empty: the content loaded
    bool hasPC = false;    // the SH-2 PCs below were read (content loaded)
    uint32_t masterPC = 0;
    uint32_t slavePC = 0;
    double wallSeconds = 0.0;
    std::string bios;
    std::string cartridge;
    std::string cartridgeMessage;
};

inline std::string JsonString(std::string_view text) {
    std::string out = "\"";
    for (const char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(ch));
                out += buf;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
    return out;
}

inline std::string FormatSweepSummary(const SweepSummary& s) {
    char buf[96];
    const auto pc = [&](uint32_t value) {
        if (!s.hasPC) {
            return std::string("null");
        }
        std::snprintf(buf, sizeof(buf), "\"0x%08X\"", static_cast<unsigned>(value));
        return std::string(buf);
    };
    std::string out = "{\n";
    out += "  \"frames_run\": " + std::to_string(s.stats.frames) + ",\n";
    out += "  \"load_error\": " + (s.loadError.empty() ? std::string("null") : JsonString(s.loadError)) + ",\n";
    out += "  \"identical_frames\": {\"start\": " + std::to_string(s.stats.identicalStart) +
           ", \"length\": " + std::to_string(s.stats.identicalLength) + "},\n";
    out += "  \"audio\": {\"peak\": " + std::to_string(s.stats.audioPeak) +
           ", \"silence_threshold\": " + std::to_string(kSilenceThreshold) +
           ", \"longest_silent_start\": " + std::to_string(s.stats.silentStart) +
           ", \"longest_silent_frames\": " + std::to_string(s.stats.silentLength) + "},\n";
    out += "  \"master_pc\": " + pc(s.masterPC) + ",\n";
    out += "  \"slave_pc\": " + pc(s.slavePC) + ",\n";
    std::snprintf(buf, sizeof(buf), "%.3f", s.wallSeconds);
    out += std::string("  \"wall_seconds\": ") + buf + ",\n";
    out += "  \"bios\": " + JsonString(s.bios) + ",\n";
    out += "  \"cartridge\": " + JsonString(s.cartridge) + ",\n";
    out += "  \"cartridge_message\": " + JsonString(s.cartridgeMessage) + "\n";
    out += "}\n";
    return out;
}

} // namespace brimir_tools
