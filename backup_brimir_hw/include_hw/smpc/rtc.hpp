#pragma once

#include <brimir/core/configuration.hpp>
#include <brimir/sys/clocks.hpp>

#include <brimir/state/state_smpc.hpp>

#include <brimir/util/date_time.hpp>

#include <brimir/core/types.hpp>

#include <iosfwd>

// -----------------------------------------------------------------------------
// Forward declarations

namespace brimir::smpc {

class SMPC;

} // namespace brimir::smpc

// -----------------------------------------------------------------------------

namespace brimir::smpc::rtc {

class RTC {
public:
    RTC(core::Configuration::RTC &config);

    void Reset(bool hard);

    sint64 &HostTimeOffset() {
        return m_offset;
    }
    const sint64 &HostTimeOffset() const {
        return m_offset;
    }

    void UpdateSysClock(uint64 sysClock);

    util::datetime::DateTime GetDateTime() const;
    void SetDateTime(const util::datetime::DateTime &dateTime);

    // TODO: replace std iostream with custom I/O class with managed endianness
    void ReadPersistentData(std::ifstream &in);
    void WritePersistentData(std::ofstream &out) const;

    // -------------------------------------------------------------------------
    // Save states

    void SaveState(state::SMPCState &state) const;
    [[nodiscard]] bool ValidateState(const state::SMPCState &state) const;
    void LoadState(const state::SMPCState &state);

private:
    friend class brimir::smpc::SMPC;
    void UpdateClockRatios(const sys::ClockRatios &clockRatios);

    bool IsVirtualMode() const {
        return m_mode == core::config::rtc::Mode::Virtual;
    }

    core::Configuration::RTC &m_config;

    // RTC mode (host or virtual).
    // Replicated here to eliminate one indirection in the hot path.
    core::config::rtc::Mode m_mode;

    // RTC host mode
    sint64 m_offset; // Offset in seconds added to host time

    // Virtual RTC mode
    sint64 m_timestamp;       // Current RTC timestamp in seconds since Unix epoch
    uint64 m_sysClockCount;   // System clock count since last update to virtual RTC
    uint64 m_sysClockRateNum; // System clock ratio numerator
    uint64 m_sysClockRateDen; // System clock ratio denominator
};

} // namespace brimir::smpc::rtc
