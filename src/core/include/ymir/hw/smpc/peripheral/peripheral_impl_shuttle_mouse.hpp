#pragma once

#include "peripheral_base.hpp"

namespace ymir::peripheral {

/// @brief Implements the Shuttle Mouse (ID 0xE/3 bytes) with:
/// - 4 digital buttons: Start Left Middle Right
/// - 2D relative movement axis
class ShuttleMouse final : public BasePeripheral {
public:
    explicit ShuttleMouse(CBPeripheralReport callback);

    void UpdateInputs() override;

    [[nodiscard]] uint8 GetReportLength() const override;

    void Read(std::span<uint8> out) override;

    [[nodiscard]] uint8 WritePDR(uint8 ddr, uint8 value, bool exle) override;

    // Brimir: save states
    void SaveBrimirState(savestate::BrimirSaveState::Peripherals::Port &state) const override;
    void LoadBrimirState(const savestate::BrimirSaveState::Peripherals::Port &state) override;
    // Brimir: the report the last read took (a TH/TR handshake in progress reads it)
    void SaveBrimirReport(savestate::BrimirSaveState::Mouse::Port &state) const;
    void LoadBrimirReport(const savestate::BrimirSaveState::Mouse::Port &state);

private:
    // Brimir: asks for the report at the start of each read (Read, the first step of a TH/TR handshake), not at every
    // port access (UpdateInputs): the report's movement is what was built up since the previous read.
    void TakeReport();

    ShuttleMouseReport m_report{}; // Brimir: initialized (a handshake resumed by a state load may read it untaken)

    uint8 m_reportPos = 0;
    bool m_tl = false;
    bool m_reset = true;
};

} // namespace ymir::peripheral
