#pragma once

#include "binary_reader.hpp"

#include <memory>

namespace brimir::media {

// Implementation of IBinaryReader that reads from a subview of a shared pointer to an IBinaryReader.
class SharedSubviewBinaryReader final : public IBinaryReader {
public:
    // Initializes a subview of the specified IBinaryReader that views the entire contents of the file.
    SharedSubviewBinaryReader(std::shared_ptr<IBinaryReader> binaryReader)
        : m_fileContent(binaryReader)
        , m_offset(0)
        , m_size(binaryReader->Size()) {}

    // Initializes a subview of the specified IBinaryReader that views the given portion of the file.
    // If the offset is out of range, the resulting view is empty.
    // The size will be clamped to not exceed the end of the given file contents.
    SharedSubviewBinaryReader(std::shared_ptr<IBinaryReader> binaryReader, uintmax_t offset, uintmax_t size)
        : m_fileContent(binaryReader)
        , m_offset(std::min(offset, binaryReader->Size()))
        , m_size(std::min(size, binaryReader->Size() - m_offset)) {}

    uintmax_t Size() const final {
        return m_size;
    }

    uintmax_t Read(uintmax_t offset, uintmax_t size, std::span<uint8> output) const final {
        if (offset >= m_size) {
            return 0;
        }
        // Limit size to the smallest of the requested size, the output buffer size and the amount of bytes available in
        // the file starting from offset
        size = std::min(size, m_size - offset);
        size = std::min(size, output.size());
        return m_fileContent->Read(offset + m_offset, size, output);
    }

private:
    std::shared_ptr<IBinaryReader> m_fileContent;
    uintmax_t m_offset;
    uintmax_t m_size;
};

} // namespace brimir::media
