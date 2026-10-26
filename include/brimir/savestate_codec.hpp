#pragma once

// Save-state payload codec: serializes ymir::savestate::SaveState member by member so that its
// variable-size fields (the SCU cartridge image and the SMPC INTBACK report, both std::vector) are
// stored by value. The payload is what CoreWrapper LZ4-compresses behind the BRI2 header.
//
// Layout (host byte order, no padding between fields):
//   every trivially copyable member of SaveState, in declaration order, as its raw bytes, except
//   - scu:  each field in declaration order; cartData as uint32 length + bytes
//   - smpc: each field in declaration order; intback.report as uint32 length + bytes and
//           intback.reportOffset as uint64

#include <ymir/savestate/savestate.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace brimir {

/// @brief Largest INTBACK report SMPC builds: per port, 2 header bytes plus a peripheral report whose
/// length is a uint8 (PeripheralPort::UpdateInputs)
inline constexpr size_t kMaxSaveStateINTBACKReportSize = 2 * (2 + 255);

/// @brief Largest cartridge image in a state (48 Mbit DRAM cartridge)
inline constexpr size_t kMaxSaveStateCartDataSize = 6 * 1024 * 1024;

/// @brief Size of the payload EncodeSaveState writes for this state
size_t EncodedSaveStateSize(const ymir::savestate::SaveState& state);

/// @brief Largest payload of a state whose cartridge image is cartDataSize bytes (any INTBACK report)
size_t MaxEncodedSaveStateSize(size_t cartDataSize);

/// @brief Serializes the state into out (resized to EncodedSaveStateSize(state))
/// @return false if a variable-size field exceeds its limit (the decoder would reject it)
bool EncodeSaveState(const ymir::savestate::SaveState& state, std::vector<uint8_t>& out);

/// @brief Zeroes every member as value-initialization would (padding included); the vectors are
/// emptied but keep their capacity. Lets a SaveState object be reused: Saturn::SaveState does not
/// write every member (only one of the HLE and LLE CD block states, for instance).
void ResetSaveState(ymir::savestate::SaveState& state);

/// @brief Fills every member of state from a payload written by EncodeSaveState
/// @return false if the payload is truncated, has trailing bytes, or holds an invalid length,
/// cartridge type, bool or INTBACK report offset; state is then partially overwritten
bool DecodeSaveState(std::span<const uint8_t> payload, ymir::savestate::SaveState& state);

/// @brief Converts a state decoded from a version 4 payload to the current meaning of its fields.
/// Version 4 stored the VDP2 VRAM fetchers' character palette numbers shifted right by 4, which lost
/// the low bits of 16-colour characters; version 5 stores them whole. The conversion restores the
/// value a version 4 state loaded as.
void UpgradeVersion4SaveState(ymir::savestate::SaveState& state);

} // namespace brimir
