#pragma once

#include <brimir/core/types.hpp>

#include <span>

namespace brimir::media {

// Interface that specifies the contract for reading binary files.
class IBinaryReader {
public:
    virtual ~IBinaryReader() = default;

    // Returns the file size.
    virtual uintmax_t Size() const = 0;

    // Reads up to size bytes starting at offset from the file into the output buffer.
    // Returns the number of bytes actually read, which may be limited by the output buffer size or the amount of data
    // available in the file. If the number of bytes read is less than the output size, only the first bytes of the
    // buffer are modified; the rest of the buffer is left untouched.
    virtual uintmax_t Read(uintmax_t offset, uintmax_t size, std::span<uint8> output) const = 0;
};

} // namespace brimir::media
