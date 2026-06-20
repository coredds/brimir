#include "stv_315_5881.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace ymir::cart {

namespace {

struct SBox {
    uint8_t table[64];
    int8_t  inputs[6];
    uint8_t outputs[2];
};

constexpr int kFn1GameKeyBits = 38;
constexpr int kFn2GameKeyBits = 32;
constexpr int kLineSize       = 512;
constexpr uint32_t kFlagCompressed = 0x20000;

constexpr uint32_t Bit(uint32_t value, int bit) {
    return (value >> bit) & 0x1u;
}

static const SBox kFn1SBoxes[4][4] = {
    {
         {{0,3,2,2,1,3,1,2,3,2,1,2,1,2,3,1,3,2,2,0,2,1,3,0,0,3,2,3,2,1,2,0,2,3,1,1,2,2,1,1,1,0,2,3,3,0,2,1,1,1,1,1,3,0,3,2,1,0,1,2,0,3,1,3},{3,4,5,7,-1,-1},{0,4}},
         {{2,2,2,0,3,3,0,1,2,2,3,2,3,0,2,2,1,1,0,3,3,2,0,2,0,1,0,1,2,3,1,1,0,1,3,3,1,3,3,1,2,3,2,0,0,0,2,2,0,3,1,3,0,3,2,2,0,3,0,3,1,1,0,2},{0,1,2,5,6,7},{1,6}},
         {{0,1,3,0,3,1,1,1,1,2,3,1,3,0,2,3,3,2,0,2,1,1,2,1,1,3,1,0,0,2,0,1,1,3,1,0,0,3,2,3,2,0,3,3,0,0,0,0,1,2,3,3,2,0,3,2,1,0,0,0,2,2,3,3},{0,2,5,6,7,-1},{2,3}},
         {{3,2,1,2,1,2,3,2,0,3,2,2,3,1,3,3,0,2,3,0,3,3,2,1,1,1,2,0,2,2,0,1,1,3,3,0,0,3,0,3,0,2,1,3,2,1,0,0,0,1,1,2,0,1,0,0,0,1,3,3,2,0,3,3},{1,2,3,4,6,7},{5,7}},
     },
    {
         {{3,3,1,2,0,0,2,2,2,1,2,1,3,1,1,3,3,0,0,3,0,3,3,2,1,1,3,2,3,2,1,3,2,3,0,1,3,2,0,1,2,1,3,1,2,2,3,3,3,1,2,2,0,3,1,2,2,1,3,0,3,0,1,3},{0,1,3,4,5,7},{0,4}},
         {{2,0,1,0,0,3,2,0,3,3,1,2,1,3,0,2,0,2,0,0,0,2,3,1,3,1,1,2,3,0,3,0,3,0,2,0,0,2,2,1,0,2,3,3,1,3,1,0,1,3,3,0,0,1,3,1,0,2,0,3,2,1,0,1},{0,1,3,4,6,-1},{1,5}},
         {{2,2,2,3,1,1,0,1,3,3,1,1,2,2,2,0,0,3,2,3,3,0,2,1,2,2,3,0,1,3,0,0,3,2,0,3,2,0,1,0,0,1,2,2,3,3,0,2,2,1,3,1,1,1,1,2,0,3,1,0,0,2,3,2},{1,2,5,6,7,6},{2,7}},
         {{0,1,3,3,3,1,3,3,1,0,2,0,2,0,0,3,1,2,1,3,1,2,3,2,2,0,1,3,0,3,3,3,0,0,0,2,1,1,2,3,2,2,3,1,1,2,0,2,0,2,1,3,1,1,3,3,1,1,3,0,2,3,0,0},{2,3,4,5,6,7},{3,6}},
     },
    {
         {{0,0,1,0,1,0,0,3,2,0,0,3,0,1,0,2,0,3,0,0,2,0,3,2,2,1,3,2,2,1,1,2,0,0,0,3,0,1,1,0,0,2,1,0,3,1,2,2,2,0,3,1,3,0,1,2,2,1,1,1,0,2,3,1},{1,2,3,4,5,7},{0,5}},
         {{1,2,1,0,3,1,1,2,0,0,2,3,2,3,1,3,2,0,3,2,2,3,1,1,1,1,0,3,2,0,0,1,1,0,0,1,3,1,2,3,0,0,2,3,3,0,1,0,0,2,3,0,1,2,0,1,3,3,3,1,2,0,2,1},{0,2,4,5,6,7},{1,6}},
         {{0,3,0,2,1,2,0,0,1,1,0,0,3,1,1,0,0,3,0,0,2,3,3,2,3,1,2,0,0,2,3,0,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255},{0,2,4,6,7,-1},{2,3}},
         {{0,0,1,0,0,1,0,2,3,3,0,3,3,2,3,0,2,2,2,0,3,2,0,3,1,0,0,3,3,0,0,0,2,2,1,0,2,0,3,2,0,0,3,1,3,3,0,0,2,1,1,2,1,0,1,1,0,3,1,2,0,2,0,3},{0,1,2,3,6,-1},{4,7}},
     },
    {
         {{0,3,3,3,3,3,2,0,0,1,2,0,2,2,2,2,1,1,0,2,2,1,3,2,3,2,0,1,2,3,2,1,3,2,2,3,1,0,1,0,0,2,0,1,2,1,2,3,1,2,1,1,2,2,1,0,1,3,2,3,2,0,3,1},{0,1,3,4,5,6},{0,5}},
         {{0,3,0,0,2,0,3,1,1,1,2,2,2,1,3,1,2,2,1,3,2,2,3,3,0,3,1,0,3,2,0,1,3,0,2,0,1,0,2,1,3,3,1,2,2,0,2,3,3,2,3,0,1,1,3,3,0,2,1,3,0,2,2,3},{0,1,2,3,5,7},{1,7}},
         {{0,1,2,3,3,3,3,1,2,0,2,3,2,1,0,1,2,2,1,2,0,3,2,0,1,1,0,1,3,1,3,1,3,1,0,0,1,0,0,0,0,1,2,2,1,1,3,3,1,2,3,3,3,2,3,0,2,2,1,3,3,0,2,0},{2,3,4,5,6,7},{2,3}},
         {{0,2,1,1,3,2,0,3,1,0,1,0,3,2,1,1,2,2,0,3,1,0,1,2,2,2,3,3,0,0,0,0,1,2,1,0,2,1,2,2,2,3,2,3,0,1,3,0,0,1,3,0,0,1,1,0,1,0,0,0,0,2,0,1},{0,1,2,4,6,7},{4,6}},
     },
};

static const SBox kFn2SBoxes[4][4] = {
    {
         {{3,3,0,1,0,1,0,0,0,3,0,0,1,3,1,2,0,3,3,3,2,1,0,1,1,1,2,2,2,3,2,2,2,1,3,3,1,3,1,1,0,0,1,2,0,2,2,1,1,2,3,1,2,1,3,1,2,2,0,1,3,0,2,2},{1,3,4,5,6,7},{0,7}},
         {{0,1,3,0,1,1,2,3,2,0,0,3,2,1,3,1,3,3,0,0,1,0,0,3,0,3,3,2,3,2,0,1,3,2,3,2,2,1,3,1,1,1,0,3,3,2,2,1,1,2,0,2,0,1,1,0,1,0,1,1,2,0,3,0},{0,3,5,6,5,0},{1,2}},
         {{0,2,2,1,0,1,2,1,2,0,1,2,3,3,0,1,3,1,1,2,1,2,1,3,3,2,3,3,2,1,0,1,0,1,0,2,0,1,1,3,2,0,3,2,1,1,1,3,2,3,0,2,3,0,2,2,1,3,0,1,1,2,2,2},{0,2,3,4,7,-1},{3,4}},
         {{2,3,1,3,2,0,1,2,0,0,3,3,3,3,3,1,2,0,2,1,2,3,0,2,0,1,0,3,0,2,1,0,2,3,0,1,3,0,3,2,3,1,2,0,3,1,1,2,0,3,0,0,2,0,2,1,2,2,3,2,1,2,3,1},{1,2,5,6,-1,-1},{5,6}},
     },
    {
         {{2,3,1,3,1,0,3,3,3,2,3,3,2,0,0,3,2,3,0,3,1,1,2,3,1,1,2,2,0,1,0,0,2,1,0,1,2,0,1,2,0,3,1,1,2,3,1,2,0,2,0,1,3,0,1,0,2,2,3,0,3,2,3,0},{0,1,4,5,6,7},{0,7}},
         {{0,2,2,0,2,2,0,3,2,3,2,1,3,2,3,3,1,1,0,0,3,0,2,1,1,3,3,2,3,2,0,1,1,2,3,0,1,0,3,0,3,1,0,2,1,2,0,3,2,3,1,2,2,0,3,2,3,0,0,1,2,3,3,3},{0,2,3,6,7,-1},{1,5}},
         {{1,0,3,0,0,1,2,1,0,0,1,0,0,0,2,3,2,2,0,2,0,1,3,0,2,0,1,3,2,3,0,1,1,2,2,2,1,3,0,3,0,1,1,0,3,2,3,3,2,0,0,3,1,2,1,3,3,2,1,0,2,1,2,3},{2,3,4,6,7,2},{2,3}},
         {{2,3,1,3,1,1,2,3,3,1,1,0,1,0,2,3,2,1,0,0,2,2,0,1,0,2,2,2,0,2,1,0,3,1,2,3,1,3,0,2,1,0,1,0,0,1,2,2,3,2,3,1,3,2,1,1,2,0,2,1,3,3,1,0},{1,2,3,4,5,6},{4,6}},
     },
    {
         {{0,3,0,1,3,0,0,2,1,0,1,3,2,2,2,0,3,3,3,0,2,2,0,3,0,0,2,3,0,3,2,1,3,3,0,3,0,2,3,3,1,1,1,0,2,2,1,1,3,0,3,1,2,0,2,0,0,0,3,2,1,1,0,0},{1,4,5,6,7,5},{0,5}},
         {{0,3,0,1,3,0,3,1,3,2,2,2,3,0,3,2,2,1,2,2,0,3,2,2,0,0,2,1,1,3,2,3,2,3,3,1,2,0,1,2,2,1,0,0,0,0,2,3,1,2,0,3,1,3,1,2,3,2,1,0,3,0,0,2},{0,2,3,4,6,7},{1,7}},
         {{2,2,0,3,0,3,1,0,1,1,2,3,2,3,1,0,0,0,3,2,2,0,2,3,1,3,2,0,3,3,1,3,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255},{1,2,4,7,2,-1},{2,4}},
         {{0,2,3,1,3,1,1,0,0,1,3,0,2,1,3,3,2,0,2,1,1,2,3,3,0,0,0,2,0,2,3,0,3,3,3,3,2,3,3,2,3,0,1,0,2,3,3,2,0,1,3,1,0,1,2,3,3,0,2,0,3,0,3,3},{0,1,2,3,5,7},{3,6}},
     },
    {
         {{0,1,1,0,0,1,0,2,3,3,0,1,2,3,0,2,1,0,3,3,2,0,3,0,0,2,1,0,1,0,1,3,0,3,3,1,2,0,3,0,1,3,2,0,3,3,1,3,0,2,3,3,2,1,1,2,2,1,2,1,2,0,1,1},{0,1,2,4,7,-1},{0,5}},
         {{2,0,0,2,3,0,2,3,3,1,1,1,2,1,1,0,0,2,1,0,0,3,1,0,0,3,3,0,1,0,1,2,0,2,0,2,0,1,2,3,2,1,1,0,3,3,3,3,3,3,1,0,3,0,0,2,0,3,2,0,2,2,0,1},{0,1,3,5,6,-1},{1,3}},
         {{0,1,1,2,1,3,1,1,0,0,3,1,1,1,2,0,3,2,0,1,1,2,3,3,3,0,3,0,0,2,0,3,3,2,0,0,3,2,3,1,2,3,0,3,2,0,1,2,2,2,0,2,0,1,2,2,3,1,2,2,1,1,1,1},{0,2,3,4,5,7},{2,7}},
         {{0,1,2,0,3,3,0,3,2,1,3,3,0,3,1,1,3,2,3,2,3,0,0,0,3,0,2,2,3,2,2,3,2,2,3,1,2,3,1,2,0,3,0,2,3,1,0,0,3,2,1,2,1,2,1,3,1,0,2,3,3,1,3,2},{2,3,4,5,6,7},{4,6}},
     },
};

static const int kFn1GameKeyScheduling[kFn1GameKeyBits][2] = {
    {1,29}, {1,71}, {2,4}, {2,54}, {3,8}, {4,56}, {4,73}, {5,11}, {6,51}, {7,92}, {8,89}, {9,9}, {9,39}, {9,58}, {10,90}, {11,6},
    {12,64}, {13,49}, {14,44}, {15,40}, {16,69}, {17,15}, {18,23}, {18,43}, {19,82}, {20,81}, {21,32}, {22,5}, {23,66}, {24,13}, {24,45}, {25,12},
    {25,35}, {26,61}, {27,10}, {27,59}, {28,25}, {29,86},
};

static const int kFn2GameKeyScheduling[kFn2GameKeyBits][2] = {
    {0,0}, {1,3}, {2,11}, {3,20}, {4,22}, {5,23}, {6,29}, {7,38}, {8,39}, {9,55}, {9,86}, {9,87}, {10,50}, {11,57}, {12,59}, {13,61},
    {14,63}, {15,67}, {16,72}, {17,83}, {18,88}, {19,94}, {20,35}, {21,17}, {22,6}, {23,85}, {24,16}, {25,25}, {26,92}, {27,47}, {28,28}, {29,90},
};

static const int kFn1SequenceKeyScheduling[20][2] = {
    {0,52}, {1,34}, {2,17}, {3,36}, {4,84}, {4,88}, {5,57}, {6,48}, {6,68}, {7,76}, {8,83}, {9,30}, {10,22}, {10,41}, {11,38}, {12,55}, {13,74}, {14,19}, {14,80}, {15,26},
};

static const uint8_t kTrees[9][2][32] = {
    {{0x01,0x10,0x0f,0x05,0xc4,0x13,0x87,0x0a,0xcc,0x81,0xce,0x0c,0x86,0x0e,0x84,0xc2,0x11,0xc1,0xc3,0xcf,0x15,0xc8,0xcd,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},{0xc7,0x02,0x03,0x04,0x80,0x06,0x07,0x08,0x09,0xc9,0x0b,0x0d,0x82,0x83,0x85,0xc0,0x12,0xc6,0xc5,0x14,0x16,0xca,0xcb,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {{0x02,0x80,0x05,0x04,0x81,0x10,0x15,0x82,0x09,0x83,0x0b,0x0c,0x0d,0xdc,0x0f,0xde,0x1c,0xcf,0xc5,0xdd,0x86,0x16,0x87,0x18,0x19,0x1a,0xda,0xca,0xc9,0x1e,0xce,0xff},{0x01,0x17,0x03,0x0a,0x08,0x06,0x07,0xc2,0xd9,0xc4,0xd8,0xc8,0x0e,0x84,0xcb,0x85,0x11,0x12,0x13,0x14,0xcd,0x1b,0xdb,0xc7,0xc0,0xc1,0x1d,0xdf,0xc3,0xc6,0xcc,0xff}},
    {{0xc6,0x80,0x03,0x0b,0x05,0x07,0x82,0x08,0x15,0xdc,0xdd,0x0c,0xd9,0xc2,0x14,0x10,0x85,0x86,0x18,0x16,0xc5,0xc4,0xc8,0xc9,0xc0,0xcc,0xff,0xff,0xff,0xff,0xff,0xff},{0x01,0x02,0x12,0x04,0x81,0x06,0x83,0xc3,0x09,0x0a,0x84,0x11,0x0d,0x0e,0x0f,0x19,0xca,0xc1,0x13,0xd8,0xda,0xdb,0x17,0xde,0xcd,0xcb,0xff,0xff,0xff,0xff,0xff,0xff}},
    {{0x01,0x80,0x0d,0x04,0x05,0x15,0x83,0x08,0xd9,0x10,0x0b,0x0c,0x84,0x0e,0xc0,0x14,0x12,0xcb,0x13,0xca,0xc8,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},{0xc5,0x02,0x03,0x07,0x81,0x06,0x82,0xcc,0x09,0x0a,0xc9,0x11,0xc4,0x0f,0x85,0xd8,0xda,0xdb,0xc3,0xdc,0xdd,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {{0x01,0x80,0x06,0x0c,0x05,0x81,0xd8,0x84,0x09,0xdc,0x0b,0x0f,0x0d,0x0e,0x10,0xdb,0x11,0xca,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},{0xc4,0x02,0x03,0x04,0xcb,0x0a,0x07,0x08,0xd9,0x82,0xc8,0x83,0xc0,0xc1,0xda,0xc2,0xc9,0xc3,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {{0x01,0x02,0x06,0x0a,0x83,0x0b,0x07,0x08,0x09,0x82,0xd8,0x0c,0xd9,0xda,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},{0xc3,0x80,0x03,0x04,0x05,0x81,0xca,0xc8,0xdb,0xc9,0xc0,0xc1,0x0d,0xc2,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {{0x01,0x02,0x03,0x04,0x81,0x07,0x08,0xd8,0xda,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},{0xc2,0x80,0x05,0xc9,0xc8,0x06,0x82,0xc0,0x09,0xc1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {{0x01,0x80,0x04,0xc8,0xc0,0xd9,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},{0xc1,0x02,0x03,0x81,0x05,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
    {{0x01,0xd8,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff},{0xc0,0x80,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff}},
};

static const int kFn2SequenceKeyScheduling[16] = {77,34,8,42,36,27,69,66,13,9,79,31,49,7,24,64};
static const int kFn2MiddleResultScheduling[16] = {1,10,44,68,74,78,81,95,2,4,30,40,41,51,53,58};

int FeistelFunction(int input, const SBox* sboxes, uint32_t subkeys) {
    int result = 0;
    for (int m = 0; m < 4; ++m) {
        int aux = 0;
        for (int k = 0; k < 6; ++k) {
            if (sboxes[m].inputs[k] != -1) {
                aux |= static_cast<int>(Bit(static_cast<uint32_t>(input), sboxes[m].inputs[k])) << k;
            }
        }

        aux = sboxes[m].table[(aux ^ static_cast<int>(subkeys)) & 0x3f];

        for (int k = 0; k < 2; ++k) {
            result |= static_cast<int>(Bit(static_cast<uint32_t>(aux), k)) << sboxes[m].outputs[k];
        }

        subkeys >>= 6;
    }

    return result;
}

uint16_t BitSwap16(uint16_t in, const uint8_t* vec) {
    uint16_t ret = 0;
    for (int i = 0; i < 16; ++i) {
        ret |= ((in >> vec[i]) & 0x1) << (15 - i);
    }
    return ret;
}

static const uint8_t kVec1[16] = {5,12,14,13,9,3,6,4,8,1,15,11,0,7,10,2};
static const uint8_t kVec2[16] = {14,3,8,12,13,7,15,4,6,2,9,5,11,0,1,10};
static const uint8_t kVec3[16] = {15,7,6,14,13,12,5,4,3,2,11,10,9,1,0,8};

uint16_t BlockDecrypt(uint32_t gameKey, uint16_t sequenceKey, uint16_t counter, uint16_t data) {
    int aux = 0;
    int aux2 = 0;
    int A = 0;
    int B = 0;
    int middleResult = 0;
    uint32_t fn1SubKeys[4] = {};
    uint32_t fn2SubKeys[4] = {};

    for (int j = 0; j < kFn1GameKeyBits; ++j) {
        if (Bit(gameKey, kFn1GameKeyScheduling[j][0]) != 0) {
            aux = kFn1GameKeyScheduling[j][1] % 24;
            aux2 = kFn1GameKeyScheduling[j][1] / 24;
            fn1SubKeys[aux2] ^= (1u << aux);
        }
    }

    for (int j = 0; j < kFn2GameKeyBits; ++j) {
        if (Bit(gameKey, kFn2GameKeyScheduling[j][0]) != 0) {
            aux = kFn2GameKeyScheduling[j][1] % 24;
            aux2 = kFn2GameKeyScheduling[j][1] / 24;
            fn2SubKeys[aux2] ^= (1u << aux);
        }
    }

    for (int j = 0; j < 20; ++j) {
        if (Bit(sequenceKey, kFn1SequenceKeyScheduling[j][0]) != 0) {
            aux = kFn1SequenceKeyScheduling[j][1] % 24;
            aux2 = kFn1SequenceKeyScheduling[j][1] / 24;
            fn1SubKeys[aux2] ^= (1u << aux);
        }
    }

    for (int j = 0; j < 16; ++j) {
        if (Bit(sequenceKey, j) != 0) {
            aux = kFn2SequenceKeyScheduling[j] % 24;
            aux2 = kFn2SequenceKeyScheduling[j] / 24;
            fn2SubKeys[aux2] ^= (1u << aux);
        }
    }

    aux = BitSwap16(counter, kVec1);

    B = aux >> 8;
    A = (aux & 0xff) ^ FeistelFunction(B, kFn1SBoxes[0], fn1SubKeys[0]);
    B ^= FeistelFunction(A, kFn1SBoxes[1], fn1SubKeys[1]);
    A ^= FeistelFunction(B, kFn1SBoxes[2], fn1SubKeys[2]);
    B ^= FeistelFunction(A, kFn1SBoxes[3], fn1SubKeys[3]);

    middleResult = (B << 8) | A;

    for (int j = 0; j < 16; ++j) {
        if (Bit(middleResult, j) != 0) {
            aux = kFn2MiddleResultScheduling[j] % 24;
            aux2 = kFn2MiddleResultScheduling[j] / 24;
            fn2SubKeys[aux2] ^= (1u << aux);
        }
    }

    aux = BitSwap16(data, kVec2);

    B = aux >> 8;
    A = (aux & 0xff) ^ FeistelFunction(B, kFn2SBoxes[0], fn2SubKeys[0]);
    B ^= FeistelFunction(A, kFn2SBoxes[1], fn2SubKeys[1]);
    A ^= FeistelFunction(B, kFn2SBoxes[2], fn2SubKeys[2]);
    B ^= FeistelFunction(A, kFn2SBoxes[3], fn2SubKeys[3]);

    aux = (B << 8) | A;
    aux = BitSwap16(static_cast<uint16_t>(aux), kVec3);

    return static_cast<uint16_t>(aux);
}

} // namespace

void STV3155881::Reset() {
    m_subKey = 0;
    m_addr = 0;
    m_ready = false;
    m_bufferBit = 0;
    m_bufferBit2 = 0;

    m_decHist = 0;
    m_decHeader = 0;

    m_bufferPos = 0;
    m_lineBufferPos = 0;
    m_lineBufferSize = 0;
    m_doneCompression = false;

    m_blockPos = 0;
    m_blockSize = 0;
    m_blockNumLines = 0;

    m_buffer2a = 0;
    std::fill(m_buffer.begin(), m_buffer.end(), uint8_t{0});
    std::fill(m_buffer2.begin(), m_buffer2.end(), uint8_t{0});
    std::fill(m_lineBuffer.begin(), m_lineBuffer.end(), uint8_t{0});
    std::fill(m_lineBufferPrev.begin(), m_lineBufferPrev.end(), uint8_t{0});
}

void STV3155881::SetROM(std::span<const uint8_t> rom) {
    m_rom = rom;
}

void STV3155881::SetEnabled(bool enabled) {
    m_enabled = enabled;
}

void STV3155881::SetKey(uint32_t key) {
    m_key = key;
}

void STV3155881::SetLowAddress(uint16_t value) {
    m_addr = (m_addr & 0xffff0000u) | value;
    m_ready = false;
}

void STV3155881::SetHighAddress(uint16_t value) {
    m_addr = (m_addr & 0x0000ffffu) | (static_cast<uint32_t>(value) << 16);
    m_ready = false;
    m_bufferBit = 7;
    m_bufferBit2 = 15;
}

void STV3155881::SetSubKey(uint16_t value) {
    m_subKey = value;
    m_ready = false;
}

uint16_t STV3155881::ReadEncryptedWord(uint32_t addr) const {
    if (m_rom.empty()) return 0xffff;

    const uint64_t byteAddr = 0x02000000ull + (static_cast<uint64_t>(addr) * 2ull);
    const size_t size = m_rom.size();
    const size_t i0 = static_cast<size_t>(byteAddr % size);
    const size_t i1 = (i0 + 1u) % size;

    const uint16_t data = (static_cast<uint16_t>(m_rom[i0]) << 8) | m_rom[i1];
    return static_cast<uint16_t>((data >> 8) | (data << 8));
}

uint16_t STV3155881::GetDecrypted16() {
    const uint16_t enc = ReadEncryptedWord(m_addr);
    const uint16_t dec = BlockDecrypt(m_key, static_cast<uint16_t>(m_subKey), static_cast<uint16_t>(m_addr), enc);

    const uint16_t res = static_cast<uint16_t>((dec & 3u) | (m_decHist & 0xfffcu));
    m_decHist = dec;
    ++m_addr;

    return res;
}

void STV3155881::CryptoStart() {
    m_blockPos = 0;
    m_doneCompression = false;
    m_bufferPos = static_cast<uint32_t>(m_buffer.size());

    if (m_bufferBit2 < 14) {
        m_decHeader = static_cast<uint32_t>(m_buffer2a & 0x0003u) << 16;
    } else {
        m_decHist = 0;
        m_decHeader = static_cast<uint32_t>(GetDecrypted16()) << 16;
    }

    m_decHeader |= GetDecrypted16();

    m_blockNumLines = ((m_decHeader & 0x000000ffu) >> 0) + 1;
    const uint32_t blocky = ((m_decHeader & 0x0001ff00u) >> 8) + 1;
    m_blockSize = m_blockNumLines * blocky;

    if ((m_decHeader & kFlagCompressed) != 0) {
        m_lineBufferSize = std::min<uint32_t>(blocky, kLineSize);
        m_lineBufferPos = m_lineBufferSize;
        m_bufferBit = 7;
        m_bufferBit2 = 15;
    }

    m_ready = true;
}

uint32_t STV3155881::GetCompressedBit() {
    if (m_bufferBit2 == 15) {
        m_bufferBit2 = 0;
        m_buffer2a = GetDecrypted16();
        m_buffer2[0] = static_cast<uint8_t>(m_buffer2a);
        m_buffer2[1] = static_cast<uint8_t>(m_buffer2a >> 8);
        m_bufferPos = 0;
    } else {
        ++m_bufferBit2;
    }

    const uint32_t res = (m_buffer2[(m_bufferPos & 1u) ^ 1u] >> m_bufferBit) & 1u;
    --m_bufferBit;
    if (m_bufferBit == -1) {
        m_bufferBit = 7;
        ++m_bufferPos;
    }

    return res;
}

void STV3155881::LineFill() {
    uint8_t* lc = m_lineBuffer.data();
    uint8_t* lp = m_lineBufferPrev.data();

    std::memcpy(lp, lc, kLineSize);

    m_lineBufferPos = 0;

    int i = 0;
    while (i != static_cast<int>(m_lineBufferSize)) {
        const int slot = i ? (i < static_cast<int>(m_lineBufferSize) - 7 ? 1 : (i & 7) + 1) : 0;
        uint32_t tmp = 0;
        while ((tmp & 0x80) == 0) {
            if (GetCompressedBit()) {
                tmp = kTrees[slot][1][tmp];
            } else {
                tmp = kTrees[slot][0][tmp];
            }
        }

        if (tmp != 0xff) {
            const int count = (tmp & 7) + 1;

            if ((tmp & 0x40) != 0) {
                static constexpr int offsets[4] = {0, 1, 0, -1};
                const int offset = offsets[(tmp & 0x18) >> 3];
                for (int j = 0; j != count; ++j) {
                    int src = (i + offset) % static_cast<int>(m_lineBufferSize);
                    if (src < 0) src += static_cast<int>(m_lineBufferSize);
                    lc[i ^ 1] = lp[src ^ 1];
                    ++i;
                }
            } else {
                uint8_t byte = static_cast<uint8_t>(GetCompressedBit() << 1);
                byte = static_cast<uint8_t>((byte | GetCompressedBit()) << 1);
                byte = static_cast<uint8_t>((byte | GetCompressedBit()) << 1);
                byte = static_cast<uint8_t>((byte | GetCompressedBit()) << 1);
                byte = static_cast<uint8_t>((byte | GetCompressedBit()) << 1);
                byte = static_cast<uint8_t>((byte | GetCompressedBit()) << 1);
                byte = static_cast<uint8_t>((byte | GetCompressedBit()) << 1);
                byte = static_cast<uint8_t>(byte | GetCompressedBit());

                for (int j = 0; j != count; ++j) {
                    lc[(i++) ^ 1] = byte;
                }
            }
        }
    }

    ++m_blockPos;
    if (m_blockNumLines == m_blockPos) {
        m_doneCompression = true;
    }
}

void STV3155881::EncFill() {
    for (size_t i = 0; i != m_buffer.size(); i += 2) {
        const uint16_t value = GetDecrypted16();
        m_buffer[i] = static_cast<uint8_t>(value);
        m_buffer[i + 1] = static_cast<uint8_t>(value >> 8);
        m_blockPos += 2;

        if ((m_decHeader & kFlagCompressed) == 0) {
            if (m_blockPos == m_blockSize) {
                CryptoStart();
            }
        }
    }
    m_bufferPos = 0;
}

uint16_t STV3155881::ReadDecryptedWord() {
    if (!m_enabled) return 0xffff;

    uint8_t* base = nullptr;
    if (!m_ready) {
        CryptoStart();
    }

    if ((m_decHeader & kFlagCompressed) != 0) {
        if (m_lineBufferPos == m_lineBufferSize) {
            if (m_doneCompression) {
                CryptoStart();
            }

            LineFill();
        }

        base = m_lineBuffer.data() + m_lineBufferPos;
        m_lineBufferPos += 2;
    } else {
        if (m_bufferPos == m_buffer.size()) {
            EncFill();
        }

        base = m_buffer.data() + m_bufferPos;
        m_bufferPos += 2;
    }

    return static_cast<uint16_t>((base[0] << 8) | base[1]);
}

} // namespace ymir::cart
