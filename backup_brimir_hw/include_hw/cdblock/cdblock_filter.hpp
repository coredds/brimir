#pragma once

#include "cdblock_buffer.hpp"

#include <brimir/core/types.hpp>

#include <brimir/util/bit_ops.hpp>

namespace brimir::cdblock {

struct Filter {
    static constexpr uint8 kDisconnected = 0xFF;

    Filter() {
        Reset();
    }

    void Reset() {
        ResetConditions();

        passOutput = index;
        failOutput = kDisconnected;
    }

    void ResetConditions() {
        startFrameAddress = 0;
        frameAddressCount = 0;

        mode = 0;

        fileNum = 0;
        chanNum = 0;

        submodeMask = 0;
        submodeValue = 0;

        codingInfoMask = 0;
        codingInfoValue = 0;
    }

    bool Test(const Buffer &buffer) const {
        bool subheaderPass = true;
        // Filter by file number
        if (bit::test<0>(mode)) {
            subheaderPass &= buffer.subheader.fileNum == fileNum;
        }
        // Filter by channel number
        if (bit::test<1>(mode)) {
            subheaderPass &= buffer.subheader.chanNum == chanNum;
        }
        // Filter by submode
        if (bit::test<2>(mode)) {
            subheaderPass &= (buffer.subheader.submode & submodeMask) == submodeValue;
        }
        // Filter by coding information
        if (bit::test<3>(mode)) {
            subheaderPass &= (buffer.subheader.codingInfo & codingInfoMask) == codingInfoValue;
        }
        if (bit::test<4>(mode)) {
            // Invert subheader conditions
            subheaderPass = !subheaderPass;
        }
        if (!subheaderPass) {
            return false;
        }

        // Filter by frame address
        if (bit::test<6>(mode)) {
            if (buffer.frameAddress < startFrameAddress ||
                buffer.frameAddress >= startFrameAddress + frameAddressCount) {
                return false;
            }
        }
        return true;
    }

    // Frame address filters
    uint32 startFrameAddress;
    uint32 frameAddressCount;

    // Filter mode
    //   0   Filter by file number
    //   1   Filter by channel number
    //   2   Filter by submode
    //   3   Filter by coding information
    //   4   Invert subheader conditions (all but frame address range)
    //   6   Filter by frame address range
    uint8 mode;

    // File and channel number subheader filters
    uint8 fileNum;
    uint8 chanNum;

    // Submode subheader filters
    // (submode & submodeMask) == submodeValue
    uint8 submodeMask;
    uint8 submodeValue;

    // Coding information subheader filters
    // (codingInfo & codingInfoMask) == codingInfoValue
    uint8 codingInfoMask;
    uint8 codingInfoValue;

    // Output connectors
    uint8 passOutput; // buffer index;  0xFF = disconnected
    uint8 failOutput; // filter number; 0xFF = disconnected

    // Filter index. Do not touch!
    uint8 index;
};

} // namespace brimir::cdblock
