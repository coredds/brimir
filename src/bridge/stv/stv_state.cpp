// Brimir - ST-V save state block
// Copyright (C) 2026 coredds
// Licensed under GPL-3.0

#include "stv_state.hpp"

#include <xxhash.h>

#include <cstring>
#include <type_traits>

namespace brimir::stv {

namespace {

// The structs are written as raw bytes (host byte order, like the Saturn payload): none of them has padding.
static_assert(std::has_unique_object_representations_v<STVIOBoardState>);
static_assert(std::has_unique_object_representations_v<ymir::cart::STVGameROMCartridge::ProtectionState>);
static_assert(std::has_unique_object_representations_v<ymir::cart::STV3155838::State>);

// Version 1 (0.10.0): magic, version, game hash, I/O board, protection, scheduler count, remainder, flags
constexpr size_t kBlockSizeV1 = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(STVIOBoardState) +
                                sizeof(ymir::cart::STVGameROMCartridge::ProtectionState) + sizeof(uint64_t) +
                                sizeof(uint64_t) + sizeof(uint32_t);
// Version 2: the 315-5838 before the flags
constexpr size_t kBlockSizeV2 = kBlockSizeV1 + sizeof(ymir::cart::STV3155838::State);
constexpr size_t kBlockSize = kBlockSizeV2;

template <typename T>
void Put(std::vector<uint8_t> &out, const T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}

template <typename T>
void Take(std::span<const uint8_t> in, size_t &at, T &value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(&value, in.data() + at, sizeof(T));
    at += sizeof(T);
}

} // namespace

uint64_t STVGameHash(std::string_view shortName) {
    return XXH64(shortName.data(), shortName.size(), 0);
}

size_t STVStateBlockSize() {
    return kBlockSize;
}

size_t STVStateBlockSize(uint32_t version) {
    switch (version) {
    case 1: return kBlockSizeV1;
    case 2: return kBlockSizeV2;
    default: return 0;
    }
}

void EncodeSTVState(const STVStateBlock &block, std::vector<uint8_t> &out) {
    out.reserve(out.size() + STVStateTailSize());
    Put(out, kSTVStateMagic);
    Put(out, kSTVStateVersion);
    Put(out, block.gameHash);
    Put(out, block.io);
    Put(out, block.protection);
    Put(out, block.schedulerCount);
    Put(out, block.nsRemainderBits);
    Put(out, block.comp5838);
    Put(out, block.flags);
    Put(out, static_cast<uint32_t>(kBlockSize));
    out.insert(out.end(), std::begin(kSTVTrailerMagic), std::end(kSTVTrailerMagic));
}

size_t STVStateTailSizeIn(std::span<const uint8_t> payload) {
    if (payload.size() < kSTVTrailerSize) {
        return 0;
    }
    const uint8_t *trailer = payload.data() + payload.size() - kSTVTrailerSize;
    if (std::memcmp(trailer + sizeof(uint32_t), kSTVTrailerMagic, sizeof(kSTVTrailerMagic)) != 0) {
        return 0;
    }
    uint32_t blockSize = 0;
    std::memcpy(&blockSize, trailer, sizeof(blockSize));
    // at least the magic and the version
    if (blockSize < 2 * sizeof(uint32_t) || blockSize > payload.size() - kSTVTrailerSize) {
        return 0;
    }
    return blockSize + kSTVTrailerSize;
}

bool HasSTVTrailer(std::span<const uint8_t> payload) {
    return STVStateTailSizeIn(payload) != 0;
}

bool DecodeSTVState(std::span<const uint8_t> in, STVStateBlock &out, std::string *why) {
    const auto fail = [&](const char *reason) {
        if (why != nullptr) {
            *why = reason;
        }
        return false;
    };
    if (STVStateTailSizeIn(in) != in.size()) {
        return fail("the state has no ST-V block");
    }
    const size_t blockSize = in.size() - kSTVTrailerSize;

    STVStateBlock block;
    size_t at = 0;
    uint32_t magic = 0;
    Take(in, at, magic);
    Take(in, at, block.version);
    if (magic != kSTVStateMagic) {
        return fail("the state's ST-V block is damaged");
    }
    if (block.version < kSTVStateOldestVersion || block.version > kSTVStateVersion) {
        if (why != nullptr) {
            *why = "the state's ST-V block is version " + std::to_string(block.version) + "; this core reads versions " +
                   std::to_string(kSTVStateOldestVersion) + " to " + std::to_string(kSTVStateVersion);
        }
        return false;
    }
    if (blockSize != STVStateBlockSize(block.version)) {
        return fail("the state's ST-V block is damaged");
    }
    Take(in, at, block.gameHash);
    Take(in, at, block.io);
    Take(in, at, block.protection);
    Take(in, at, block.schedulerCount);
    Take(in, at, block.nsRemainderBits);
    if (block.version >= 2) {
        Take(in, at, block.comp5838);
    } else {
        block.comp5838 = {}; // a version 1 block: the chip at its reset value (no 0.10.0 game had it)
    }
    Take(in, at, block.flags);
    if ((block.flags & ~kSTVStateFlagsKnown) != 0) {
        return fail("the state's ST-V block is damaged");
    }
    out = block;
    return true;
}

} // namespace brimir::stv
