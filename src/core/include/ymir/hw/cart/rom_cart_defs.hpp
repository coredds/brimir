#pragma once

#include <ymir/core/types.hpp>

#include <ymir/util/size_ops.hpp>

namespace ymir::cart {

inline constexpr size_t kROMCartSize = 2_MiB;
inline constexpr uint64 kROMCartHashSeed = 0xED19D10410708CB7ull;

} // namespace ymir::cart
