#pragma once

#include <cstdint>
#include <string>
#include <ctime>

namespace TimeUtil {

int64_t NowTs();
std::tm LocalTime(time_t ts);
int64_t StartOfDay(time_t ts);
int64_t EndOfDay(time_t ts);
std::string FormatTimeHHMM(time_t ts);
std::string FormatDateLine(time_t ts);
std::string FormatMonthYear(time_t ts);
int DaysInMonth(int year, int month); // month: 1-12
int WeekdayIndex(int year, int month, int day); // 0=Sun
}
