#pragma once

#include <brimir/media/subheader.hpp>

#include <brimir/core/types.hpp>

#include <array>

namespace brimir::cdblock {

struct Buffer {
    std::array<uint8, 2352> data;
    uint16 size;
    uint32 frameAddress;
    media::Subheader subheader;
};

} // namespace brimir::cdblock
