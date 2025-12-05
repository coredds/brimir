#pragma once

#include "binary_reader.hpp"

#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace brimir::media {

// Implementation of IBinaryReader that reads from an in-memory buffer.
class MemoryBinaryReader final : public IBinaryReader {
public:
    // Initializes an empty in-memory buffer.
    MemoryBinaryReader() = default;

    // Initializes an in-memory buffer with a copy of the provided data.
    MemoryBinaryReader(std::span<uint8> data) {
        m_data.resize(data.size());
        std::copy(data.begin(), data.end(), m_data.begin());
        m_data.shrink_to_fit();
    }

    // Initializes an in-memory buffer using the vector as the buffer.
    // The given vector is moved into this object.
    MemoryBinaryReader(std::vector<uint8> &&data) {
        m_data.swap(data);
        m_data.shrink_to_fit();
    }

    // Initializes an in-memory buffer with the entire contents of the specified file, if it exists and can be read.
    // If any errors occur while reading the file, initializes an empty buffer and returns the error in the provided
    // std::error_code object.
    MemoryBinaryReader(std::filesystem::path path, std::error_code &error) {
        error.clear();

        // Try opening the file for read
        std::ifstream in{path, std::ios::binary};
        if (!in) {
            error.assign(errno, std::generic_category());
            return;
        }

        // Get the file size
        const auto size = std::filesystem::file_size(path, error);
        if (error) {
            return;
        }

        // Now load in into our buffer
        m_data.resize(size);
        in.read(reinterpret_cast<char *>(m_data.data()), size);
        m_data.shrink_to_fit();
    }

    MemoryBinaryReader(const MemoryBinaryReader &) = default;
    MemoryBinaryReader(MemoryBinaryReader &&) = default;

    MemoryBinaryReader &operator=(const MemoryBinaryReader &) = default;
    MemoryBinaryReader &operator=(MemoryBinaryReader &&) = default;

    uintmax_t Size() const final {
        return m_data.size();
    }

    uintmax_t Read(uintmax_t offset, uintmax_t size, std::span<uint8> output) const final {
        if (offset >= m_data.size()) {
            return 0;
        }
        // Limit size to the smallest of the requested size, the output buffer size and the amount of bytes available in
        // the file starting from offset
        size = std::min(size, m_data.size() - offset);
        size = std::min(size, output.size());
        std::copy_n(m_data.cbegin() + offset, size, output.begin());
        return size;
    }

private:
    std::vector<uint8> m_data;
};

} // namespace brimir::media
