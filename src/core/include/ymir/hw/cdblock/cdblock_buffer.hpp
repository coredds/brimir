#pragma once

#include <ymir/media/subheader.hpp>

#include <ymir/core/types.hpp>

#include <array>

namespace ymir::cdblock {

struct Buffer {
    std::array<uint8, 2352> data;
    uint16 size;
    uint32 frameAddress;
    media::Subheader subheader;
};

} // namespace ymir::cdblock
