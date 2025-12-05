#pragma once

/***
@file
@brief Common peripheral definitions.
*/

#include <string_view>

namespace brimir::peripheral {

/// @brief Peripheral type enumeration.
enum class PeripheralType { None, ControlPad, AnalogPad, ArcadeRacer, MissionStick };

/// @brief Retrieves the name of a given peripheral type.
/// @param[in] type the peripheral type
/// @return a string with the human-readable name of the peripheral
inline std::string_view GetPeripheralName(PeripheralType type) {
    switch (type) {
    case PeripheralType::None: return "None";
    case PeripheralType::ControlPad: return "Saturn Control Pad";
    case PeripheralType::AnalogPad: return "Saturn 3D Control Pad";
    case PeripheralType::ArcadeRacer: return "Arcade Racer";
    case PeripheralType::MissionStick: return "Mission Stick";
    default: return "Invalid";
    }
}

/// @brief All supported peripheral types.
inline constexpr PeripheralType kTypes[] = {PeripheralType::None, PeripheralType::ControlPad, PeripheralType::AnalogPad,
                                            PeripheralType::ArcadeRacer, PeripheralType::MissionStick};

// Forward declarations of concrete peripheral implementations.
// See the peripheral_impl_* headers.

class NullPeripheral;
class ControlPad;
class AnalogPad;
class ArcadeRacerPeripheral;
class MissionStickPeripheral;

namespace detail {

    /// @brief Metadata about peripheral types.
    /// @tparam type the peripheral type
    template <PeripheralType type>
    struct PeripheralTypeMeta {};

    /// @brief Metadata about the "null" peripheral.
    template <>
    struct PeripheralTypeMeta<PeripheralType::None> {
        using type = NullPeripheral;
    };

    /// @brief Metadata about the Saturn Control Pad.
    template <>
    struct PeripheralTypeMeta<PeripheralType::ControlPad> {
        using type = ControlPad;
    };

    /// @brief Metadata about the Saturn 3D Control Pad.
    template <>
    struct PeripheralTypeMeta<PeripheralType::AnalogPad> {
        using type = AnalogPad;
    };

    /// @brief Metadata about the Arcade Racer.
    template <>
    struct PeripheralTypeMeta<PeripheralType::ArcadeRacer> {
        using type = ArcadeRacerPeripheral;
    };

    /// @brief Metadata about the Mission Stick.
    template <>
    struct PeripheralTypeMeta<PeripheralType::MissionStick> {
        using type = MissionStickPeripheral;
    };

    /// @brief Retrieves the class type of the given `PeripheralType`.
    /// @tparam type the peripheral type
    template <PeripheralType type>
    using PeripheralType_t = typename PeripheralTypeMeta<type>::type;

} // namespace detail

} // namespace brimir::peripheral
