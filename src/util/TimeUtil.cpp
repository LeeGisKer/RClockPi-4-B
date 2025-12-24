#include "util/TimeUtil.h"

#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace TimeUtil {

namespace {

time_t TimegmPortable(std::tm* tm) {
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

bool ParseInt(const std::string& text, size_t start, size_t len, int* out) {
    if (start + len > text.size()) {
        return false;
    }
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = text[start + i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    *out = value;
    return true;
}

} // namespace

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

std::string ToRfc3339Utc(time_t ts) {
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &ts);
#else
    gmtime_r(&ts, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

bool ParseRfc3339(const std::string& text, time_t* out) {
    // Supports YYYY-MM-DDTHH:MM:SS[.fff](Z|+HH:MM|-HH:MM)
    if (text.size() < 19) {
        return false;
    }
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    if (!ParseInt(text, 0, 4, &year) || text[4] != '-' ||
        !ParseInt(text, 5, 2, &month) || text[7] != '-' ||
        !ParseInt(text, 8, 2, &day) || (text[10] != 'T' && text[10] != 't') ||
        !ParseInt(text, 11, 2, &hour) || text[13] != ':' ||
        !ParseInt(text, 14, 2, &min) || text[16] != ':' ||
        !ParseInt(text, 17, 2, &sec)) {
        return false;
    }

    size_t pos = 19;
    if (pos < text.size() && text[pos] == '.') {
        pos++;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
            pos++;
        }
    }

    int offset_sign = 0;
    int offset_hour = 0;
    int offset_min = 0;
    if (pos < text.size()) {
        char tz = text[pos];
        if (tz == 'Z' || tz == 'z') {
            offset_sign = 0;
            pos++;
        } else if (tz == '+' || tz == '-') {
            offset_sign = (tz == '+') ? 1 : -1;
            if (!ParseInt(text, pos + 1, 2, &offset_hour) || text[pos + 3] != ':' ||
                !ParseInt(text, pos + 4, 2, &offset_min)) {
                return false;
            }
            pos += 6;
        } else {
            return false;
        }
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;

    time_t utc = TimegmPortable(&tm);
    if (utc == static_cast<time_t>(-1)) {
        return false;
    }

    int offset_sec = offset_sign * (offset_hour * 3600 + offset_min * 60);
    utc -= offset_sec;
    *out = utc;
    return true;
}

bool ParseDateLocal(const std::string& text, time_t* out) {
    // YYYY-MM-DD in local time.
    if (text.size() < 10) {
        return false;
    }
    int year = 0, month = 0, day = 0;
    if (!ParseInt(text, 0, 4, &year) || text[4] != '-' ||
        !ParseInt(text, 5, 2, &month) || text[7] != '-' ||
        !ParseInt(text, 8, 2, &day)) {
        return false;
    }
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    time_t local = std::mktime(&tm);
    if (local == static_cast<time_t>(-1)) {
        return false;
    }
    *out = local;
    return true;
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
