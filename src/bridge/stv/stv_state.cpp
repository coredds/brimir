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

// Layout: magic, version, game hash, I/O board, protection, scheduler count, remainder, flags
constexpr size_t kBlockSize = sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(STVIOBoardState) +
                              sizeof(ymir::cart::STVGameROMCartridge::ProtectionState) + sizeof(uint64_t) +
                              sizeof(uint64_t) + sizeof(uint32_t);

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

void EncodeSTVState(const STVStateBlock &block, std::vector<uint8_t> &out) {
    out.reserve(out.size() + STVStateTailSize());
    Put(out, kSTVStateMagic);
    Put(out, block.version);
    Put(out, block.gameHash);
    Put(out, block.io);
    Put(out, block.protection);
    Put(out, block.schedulerCount);
    Put(out, block.nsRemainderBits);
    Put(out, block.flags);
    Put(out, static_cast<uint32_t>(kBlockSize));
    out.insert(out.end(), std::begin(kSTVTrailerMagic), std::end(kSTVTrailerMagic));
}

bool HasSTVTrailer(std::span<const uint8_t> payload) {
    if (payload.size() < STVStateTailSize()) {
        return false;
    }
    const uint8_t *trailer = payload.data() + payload.size() - kSTVTrailerSize;
    uint32_t blockSize = 0;
    std::memcpy(&blockSize, trailer, sizeof(blockSize));
    return blockSize == kBlockSize && std::memcmp(trailer + sizeof(blockSize), kSTVTrailerMagic, sizeof(kSTVTrailerMagic)) == 0;
}

bool DecodeSTVState(std::span<const uint8_t> in, STVStateBlock &out) {
    if (in.size() != STVStateTailSize()) {
        return false;
    }
    if (std::memcmp(in.data() + kBlockSize + sizeof(uint32_t), kSTVTrailerMagic, sizeof(kSTVTrailerMagic)) != 0) {
        return false;
    }
    uint32_t blockSize = 0;
    std::memcpy(&blockSize, in.data() + kBlockSize, sizeof(blockSize));
    if (blockSize != kBlockSize) {
        return false;
    }

    STVStateBlock block;
    size_t at = 0;
    uint32_t magic = 0;
    Take(in, at, magic);
    Take(in, at, block.version);
    if (magic != kSTVStateMagic || block.version != kSTVStateVersion) {
        return false;
    }
    Take(in, at, block.gameHash);
    Take(in, at, block.io);
    Take(in, at, block.protection);
    Take(in, at, block.schedulerCount);
    Take(in, at, block.nsRemainderBits);
    Take(in, at, block.flags);
    if ((block.flags & ~kSTVStateFlagsKnown) != 0) {
        return false;
    }
    out = block;
    return true;
}

} // namespace brimir::stv
