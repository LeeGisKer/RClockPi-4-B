#include "util/TimeUtil.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace TimeUtil {

int64_t NowTs() {
    return std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
}

std::tm LocalTime(time_t ts) {
    std::tm out{};
#ifdef _WIN32
    localtime_s(&out, &ts);
#else
    localtime_r(&ts, &out);
#endif
    return out;
}

int64_t StartOfDay(time_t ts) {
    std::tm tm = LocalTime(ts);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    return std::mktime(&tm);
}

int64_t EndOfDay(time_t ts) {
    std::tm tm = LocalTime(ts);
    tm.tm_hour = 23;
    tm.tm_min = 59;
    tm.tm_sec = 59;
    return std::mktime(&tm);
}

std::string FormatTimeHHMM(time_t ts) {
    std::tm tm = LocalTime(ts);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(2) << tm.tm_hour
        << ":" << std::setfill('0') << std::setw(2) << tm.tm_min;
    return oss.str();
}

std::string FormatDateLine(time_t ts) {
    std::tm tm = LocalTime(ts);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%a - %b %d, %Y");
    return oss.str();
}

std::string FormatMonthYear(time_t ts) {
    std::tm tm = LocalTime(ts);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%B %Y");
    return oss.str();
}

int DaysInMonth(int year, int month) {
    static const int kDays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int days = kDays[month - 1];
    bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && leap) {
        days = 29;
    }
    return days;
}

int WeekdayIndex(int year, int month, int day) {
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 12;
    std::mktime(&tm);
    return tm.tm_wday;
}

} // namespace TimeUtil
