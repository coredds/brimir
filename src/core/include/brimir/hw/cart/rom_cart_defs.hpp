#pragma once

#include <brimir/core/types.hpp>

#include <brimir/util/size_ops.hpp>

namespace brimir::cart {

inline constexpr size_t kROMCartSize = 2_MiB;
inline constexpr uint64 kROMCartHashSeed = 0xED19D10410708CB7ull;

} // namespace brimir::cart
