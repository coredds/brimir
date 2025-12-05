#pragma once

#include <brimir/core/types.hpp>

#include <brimir/util/bitmask_enum.hpp>

namespace brimir::peripheral {

enum class Button : uint16 {
    None = 0,

    Right = 1u << 15u,
    Left = 1u << 14u,
    Down = 1u << 13u,
    Up = 1u << 12u,
    Start = 1u << 11u,
    A = 1u << 10u,
    C = 1u << 9u,
    B = 1u << 8u,
    R = 1u << 7u,
    X = 1u << 6u,
    Y = 1u << 5u,
    Z = 1u << 4u,
    L = 1u << 3u,

    All = Right | Left | Down | Up | Start | A | C | B | R | X | Y | Z | L,

    Default = All,
};

} // namespace brimir::peripheral

ENABLE_BITMASK_OPERATORS(brimir::peripheral::Button);
