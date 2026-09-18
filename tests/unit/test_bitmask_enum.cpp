#include <ymir/util/bitmask_enum.hpp>

namespace bitmask_regression {

enum class Flags : unsigned { None = 0, A = 1, B = 2 };
enum class Plain { Value };

struct Foreign {
    unsigned value;

    friend constexpr Foreign operator|(Foreign lhs, Foreign rhs) {
        return {lhs.value | rhs.value};
    }
};

} // namespace bitmask_regression

ENABLE_BITMASK_OPERATORS(bitmask_regression::Flags)

static_assert(BitmaskType<bitmask_regression::Flags>);
static_assert(!BitmaskType<bitmask_regression::Plain>);
static_assert(!BitmaskType<int>);
static_assert(!BitmaskType<bitmask_regression::Foreign>);
static_assert(static_cast<unsigned>(bitmask_regression::Flags::A | bitmask_regression::Flags::B) == 3u);
static_assert((bitmask_regression::Foreign{1} | bitmask_regression::Foreign{2}).value == 3u);
