#include <brimir/util/backup_datetime.hpp>

#include <ctime>

namespace util {

/// @brief The origin point corresponding to the raw value of 0.
static const auto kOrigin = [] {
    std::tm tm{};
    tm.tm_year = 1980 - 1900;
    tm.tm_mon = 1 - 1;
    tm.tm_mday = 1;
    tm.tm_wday = 2;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return mktime(&tm);
}();

BackupDateTime::BackupDateTime(uint32 raw) noexcept {
    const auto dt = kOrigin + raw * 60;
    tm tm{};
#ifdef _MSC_VER
    localtime_s(&tm, &dt);
#else
    tm = *localtime(&dt);
#endif

    year = tm.tm_year + 1900;
    month = tm.tm_mon + 1;
    day = tm.tm_mday;
    hour = tm.tm_hour;
    minute = tm.tm_min;
}

uint32 BackupDateTime::ToRaw() const noexcept {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = 0;
    return (mktime(&tm) - kOrigin) / 60;
}

} // namespace util
