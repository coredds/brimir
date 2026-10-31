// Derived from Ymir libs/ymir-core/res/shaders/include/util/data_ops.hlsli at 8cdf0a53 (unchanged).
#ifndef YMIR_UTIL_DATA_OPS_HLSLI
#define YMIR_UTIL_DATA_OPS_HLSLI

#include "bit_ops.hlsli"

uint Read4(ByteAddressBuffer buf, uint address, uint nibble) {
    return BitExtract(buf.Load(address & ~3), (address & 3) * 8 + nibble * 4, 4);
}

uint Read8(ByteAddressBuffer buf, uint address) {
    return BitExtract(buf.Load(address & ~3), (address & 3) * 8, 8);
}

uint Read16(ByteAddressBuffer buf, uint address) {
    return ByteSwap16(BitExtract(buf.Load(address & ~3), (address & 2) * 8, 16));
}

uint Read32(ByteAddressBuffer buf, uint address) {
    return ByteSwap32(buf.Load(address & ~3));
}

void WriteOr8(RWByteAddressBuffer buf, uint address, uint value) {
    value &= 0xFF;
    value <<= (address & 3) * 8;
    uint dummy;
    buf.InterlockedOr(address & ~3, value, dummy);
}

void WriteOr16(RWByteAddressBuffer buf, uint address, uint value) {
    value = ByteSwap16(value); // also masks to 16 bits
    value <<= (address & 2) * 8;
    uint dummy;
    buf.InterlockedOr(address & ~3, value, dummy);
}

uint4 Uint16ToColor555(uint rawValue) {
    return uint4(
        BitExtract(rawValue, 0, 5),
        BitExtract(rawValue, 5, 5),
        BitExtract(rawValue, 10, 5),
        BitExtract(rawValue, 15, 1)
    );
}

uint Color555ToUint16(uint4 color) {
    return color.r | (color.g << 5) | (color.b << 10) | (color.a << 15);
}

#endif
