#include <brimir/media/cdrom_crc.hpp>

#include <array>

namespace brimir::media {

static constexpr auto kCRCTable = [] {
    std::array<uint32, 256> crcTable{};
    for (uint32 i = 0; i < 256; ++i) {
        uint32 c = i;
        for (uint32 j = 0; j < 8; ++j) {
            c = (c >> 1) ^ ((c & 0x1) ? 0xD8018001 : 0);
        }
        crcTable[i] = c;
    }
    return crcTable;
}();

uint32 CalcCRC(std::span<uint8, 2064> sector) {
    uint32 crc = 0;
    for (uint8 b : sector) {
        crc ^= b;
        crc = (crc >> 8) ^ kCRCTable[crc & 0xFF];
    }
    return crc;
}

} // namespace brimir::media
