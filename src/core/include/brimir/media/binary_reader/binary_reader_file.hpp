#pragma once

#include "binary_reader.hpp"

#include <filesystem>
#include <fstream>
#include <span>

namespace brimir::media {

// Implementation of IBinaryReader backed by a file.
class FileBinaryReader final : public IBinaryReader {
public:
    // Initializes a file content pointing to no file.
    FileBinaryReader() = default;

    // Initializes a file content pointing to the specified file.
    // If any errors occur while reading the file, initializes an empty file content and returns the error in the
    // provided std::error_code object.
    FileBinaryReader(std::filesystem::path path, std::error_code &error) {
        error.clear();

        // Try opening the file for read
        m_in = std::ifstream{path, std::ios::binary};
        if (!m_in) {
            error.assign(errno, std::generic_category());
            return;
        }

        // Get the file size
        m_size = std::filesystem::file_size(path, error);
        if (error) {
            return;
        }
    }

    FileBinaryReader(const FileBinaryReader &) = delete;
    FileBinaryReader(FileBinaryReader &&) = default;

    FileBinaryReader &operator=(const FileBinaryReader &) = delete;
    FileBinaryReader &operator=(FileBinaryReader &&) = default;

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
        m_in.seekg(offset, std::ios::beg);
        m_in.read(reinterpret_cast<char *>(output.data()), size);
        return m_in.gcount();
    }

private:
    mutable std::ifstream m_in;
    uintmax_t m_size;
};

} // namespace brimir::media
