#pragma once

#include <brimir/core/types.hpp>

#include <brimir/util/callback.hpp>

namespace brimir::scsp {

// Sample output callback, invoked every sample
using CBOutputSample = util::OptionalCallback<void(sint16 left, sint16 right)>;

// MIDI message output callback, invoked when a complete midi message is ready to send
using CBSendMidiOutputMessage = util::OptionalCallback<void(std::span<uint8> msg)>;

} // namespace brimir::scsp
