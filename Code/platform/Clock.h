#ifndef _PLATFORM_CLOCK_H_
#define _PLATFORM_CLOCK_H_

#include <chrono>
#include <cstdint>
#include <ctime>

namespace platform
{

struct LocalTime
{
	int year;         // full year, e.g. 2026
	int month;        // 1-12
	int day;          // 1-31
	int hour;         // 0-23
	int minute;       // 0-59
	int second;       // 0-59
	int millisecond;  // 0-999
	int yearDay;      // 0-365, matching tm_yday
	int weekday;      // 0 = Sunday, matching tm_wday
};

// Monotonic milliseconds since an opaque epoch, truncated to 32 bits.
// Difference arithmetic on uint32_t is wrap-safe, exactly like timeGetTime.
std::uint32_t MonotonicMs();

// Wrap-safe elapsed milliseconds: later - earlier.
std::uint32_t ElapsedMs(std::uint32_t earlier, std::uint32_t later);

// Local calendar time for log names and calendar scheduling.
LocalTime LocalNow();

LocalTime LocalTimeFromTimeT(std::time_t value);
LocalTime LocalTimeFromSystemTime(std::chrono::system_clock::time_point value);

} // namespace platform

#endif
