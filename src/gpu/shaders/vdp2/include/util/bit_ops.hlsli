// Derived from Ymir libs/ymir-core/res/shaders/include/util/bit_ops.hlsli at 8cdf0a53 (unchanged).
#ifndef YMIR_UTIL_BIT_OPS_HLSLI
#define YMIR_UTIL_BIT_OPS_HLSLI

bool BitTest(uint value, uint bit) {
    return ((value >> bit) & 1) != 0;
}

uint BitExtract(uint value, uint offset, uint length) {
    const uint mask = (1u << length) - 1u;
    return (value >> offset) & mask;
}

uint BitExtract(uint2 value, uint offset, uint length) {
    const uint mask = (1u << length) - 1u;
    if (offset < 32) {
        return (value.x >> offset) & mask;
    } else {
        return (value.y >> (offset - 32)) & mask;
    }
}

int SignExtend(int value, int bits) {
    const uint shift = 32 - bits;
    return (value << shift) >> shift;
}

uint ByteSwap16(uint val) {
    return ((val >> 8) & 0x00FF) |
           ((val << 8) & 0xFF00);
}

uint ByteSwap32(uint val) {
    return ((val >> 24) & 0x000000FF) |
           ((val >> 8) & 0x0000FF00) |
           ((val << 8) & 0x00FF0000) |
           ((val << 24) & 0xFF000000);
}

#endif
