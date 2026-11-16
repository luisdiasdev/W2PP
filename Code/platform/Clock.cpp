#include "Clock.h"

namespace platform
{

std::uint32_t MonotonicMs()
{
	const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
	const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
	return static_cast<std::uint32_t>(millis);
}

std::uint32_t ElapsedMs(std::uint32_t earlier, std::uint32_t later)
{
	return later - earlier;
}

LocalTime LocalTimeFromTimeT(std::time_t value)
{
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &value);
#else
	localtime_r(&value, &local);
#endif

	LocalTime result;
	result.year        = local.tm_year + 1900;
	result.month       = local.tm_mon + 1;
	result.day         = local.tm_mday;
	result.hour        = local.tm_hour;
	result.minute      = local.tm_min;
	result.second      = local.tm_sec;
	result.millisecond = 0;
	result.yearDay     = local.tm_yday;
	result.weekday     = local.tm_wday;
	return result;
}

LocalTime LocalTimeFromSystemTime(std::chrono::system_clock::time_point value)
{
	const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
		value.time_since_epoch()).count();
	const std::time_t seconds = static_cast<std::time_t>(
		std::chrono::system_clock::to_time_t(value));

	LocalTime result = LocalTimeFromTimeT(seconds);
	long long remainder = millis % 1000;

	if (remainder < 0)
		remainder += 1000;

	result.millisecond = static_cast<int>(remainder);
	return result;
}

LocalTime LocalNow()
{
	return LocalTimeFromSystemTime(std::chrono::system_clock::now());
}

} // namespace platform
