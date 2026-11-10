#include <ymir/hw/smpc/peripheral/peripheral_impl_shuttle_mouse.hpp>

#include <ymir/util/bit_ops.hpp>

#include <algorithm>

namespace ymir::peripheral {

ShuttleMouse::ShuttleMouse(CBPeripheralReport callback)
    : BasePeripheral(PeripheralType::ShuttleMouse, 0xE, callback) {}

void ShuttleMouse::UpdateInputs() {
    // Brimir: the report is taken by each read (TakeReport), not by every port access
}

// Brimir: was the body of UpdateInputs
void ShuttleMouse::TakeReport() {
    PeripheralReport report{
        .type = PeripheralType::ShuttleMouse,
        .report = {.shuttleMouse = {.start = false, .left = false, .middle = false, .right = false, .x = 0, .y = 0}}};
    m_cbPeripheralReport(report);
    m_report = report.report.shuttleMouse;
    m_report.y = -m_report.y; // for some reason this is inverted
}

uint8 ShuttleMouse::GetReportLength() const {
    return 3;
}

void ShuttleMouse::Read(std::span<uint8> out) {
    assert(out.size() == 3);

    TakeReport(); // Brimir

    const sint16 x = std::clamp<sint16>(m_report.x, -256, 255);
    const sint16 y = std::clamp<sint16>(m_report.y, -256, 255);
    const bool xOver = m_report.x < -256 || m_report.x > 255;
    const bool yOver = m_report.y < -256 || m_report.y > 255;
    const bool xSign = m_report.x < 0;
    const bool ySign = m_report.y < 0;

    // [0] 7-0 = Y Over, X Over, Y Sign, X Sign, Start, Middle, Right, Left
    // [1] 7-0 = XD7-XD0
    // [2] 7-0 = YD7-YD0
    out[0] = (yOver << 7u) | (xOver << 6u) | (ySign << 5u) | (xSign << 4u) | (m_report.start << 3u) |
             (m_report.middle << 2u) | (m_report.right << 1u) | (m_report.left << 0u);
    out[1] = bit::extract<0, 7>(x);
    out[2] = bit::extract<0, 7>(y);
}

uint8 ShuttleMouse::WritePDR(uint8 ddr, uint8 value, bool exle) {
    switch (ddr & 0x7F) {
    case 0x40: // TH control mode
        if (value & 0x40) {
            return 0x70 | 0b0100;
        } else {
            return 0x30 | 0b0101;
        }
        break;
    case 0x60: // TH/TR control mode
        // TODO: check correctness
        const bool th = bit::test<6>(value);
        const bool tr = bit::test<5>(value);
        if (th) {
            m_reportPos = 0;
            m_tl = false;
            m_reset ^= tr;
            return (!m_reset << 4) | 0b0000;
        } else if (m_reportPos == 0 || tr != m_tl) {
            if (m_reportPos == 0) {
                TakeReport(); // Brimir: the first step of a handshake is the read
            }
            const sint16 x = std::clamp<sint16>(m_report.x, -256, 255);
            const sint16 y = std::clamp<sint16>(m_report.y, -256, 255);
            const bool xOver = m_report.x < -256 || m_report.x > 255;
            const bool yOver = m_report.y < -256 || m_report.y > 255;
            const bool xSign = m_report.x < 0;
            const bool ySign = m_report.y < 0;
            const bool start = m_report.start;
            const bool middle = m_report.middle;
            const bool right = m_report.right;
            const bool left = m_report.left;

            m_tl = tr;
            const uint8 pos = m_reportPos;
            // Brimir: past the last nibble the mouse repeats it (as Mednafen) instead of starting over, which would
            // take a new report in the middle of a read; only TH high starts a new one
            m_reportPos = std::min<uint8>(m_reportPos + 1, 8);
            switch (pos) {
            case 0: return (m_tl << 4) | 0b1011;
            case 1: return (m_tl << 4) | 0b1111;
            case 2: return (m_tl << 4) | 0b1111;
            case 3: return (m_tl << 4) | (yOver << 3u) | (xOver << 2u) | (ySign << 1u) | (xSign << 0u);
            case 4: return (m_tl << 4) | (start << 3u) | (middle << 2u) | (right << 1u) | (left << 0u);
            case 5: return (m_tl << 4) | bit::extract<4, 7>(x);
            case 6: return (m_tl << 4) | bit::extract<0, 3>(x);
            case 7: return (m_tl << 4) | bit::extract<4, 7>(y);
            case 8: return (m_tl << 4) | bit::extract<0, 3>(y);
            }
        } else {
            return (m_tl << 4);
        }
    }

    return 0xFF;
}

// Brimir: save states
void ShuttleMouse::SaveBrimirState(savestate::BrimirSaveState::Peripherals::Port &state) const {
    state.reportPos = m_reportPos;
    state.tl = m_tl;
    state.mouseReset = m_reset;
}

void ShuttleMouse::LoadBrimirState(const savestate::BrimirSaveState::Peripherals::Port &state) {
    m_reportPos = state.reportPos % 9;
    m_tl = state.tl;
    m_reset = state.mouseReset;
}

void ShuttleMouse::SaveBrimirReport(savestate::BrimirSaveState::Mouse::Port &state) const {
    state.reportX = m_report.x;
    state.reportY = m_report.y;
    state.reportButtons = (m_report.left << 0u) | (m_report.right << 1u) | (m_report.middle << 2u) |
                          (m_report.start << 3u);
}

void ShuttleMouse::LoadBrimirReport(const savestate::BrimirSaveState::Mouse::Port &state) {
    m_report.x = state.reportX;
    m_report.y = state.reportY;
    m_report.left = bit::test<0>(state.reportButtons);
    m_report.right = bit::test<1>(state.reportButtons);
    m_report.middle = bit::test<2>(state.reportButtons);
    m_report.start = bit::test<3>(state.reportButtons);
}

} // namespace ymir::peripheral
