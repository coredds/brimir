#pragma once

#include <functional>
#include <string>

namespace brimir::media {

enum class MessageType {
    InvalidFormat, // Parser has not detected valid magic signature
    Error,         // Problem found parsing media; must be reported to the user
    NotValid,      // None of the parsers matched the file format
    Debug,         // Debug messages, detailed parser logs
};

// Callback function for loader messages.
using CbLoaderMessage = std::function<void(MessageType category, std::string message)>;

} // namespace brimir::media
