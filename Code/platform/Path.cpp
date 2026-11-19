#include "Path.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
#endif

namespace platform
{
namespace fs = std::filesystem;
namespace
{

// Native, platform-specific source of the running executable path. Returns an
// empty string when the platform cannot report it, so the existing boot
// failure path applies.
std::string NativeExecutablePath()
{
#if defined(_WIN32)
	const char *programPath = _pgmptr;

	if (programPath == nullptr || programPath[0] == '\0')
		return {};

	return programPath;
#elif defined(__linux__)
	std::vector<char> buffer(256);

	for (;;)
	{
		const ssize_t length =
			::readlink("/proc/self/exe", buffer.data(), buffer.size());

		if (length < 0)
			return {};

		if (static_cast<std::size_t>(length) < buffer.size())
			return std::string(buffer.data(), static_cast<std::size_t>(length));

		buffer.resize(buffer.size() * 2);
	}
#else
	return {};
#endif
}

} // namespace

std::string ExecutablePath()
{
	const std::string programPath = NativeExecutablePath();

	if (programPath.empty())
		return {};

	std::error_code ec;
	const fs::path absolute = fs::absolute(fs::path(programPath), ec);

	if (ec)
		return fs::path(programPath).string();

	return absolute.lexically_normal().string();
}

std::string ExecutableDirectory()
{
	const std::string path = ExecutablePath();

	if (path.empty())
		return {};

	return fs::path(path).parent_path().string();
}

std::string ExecutableFileName()
{
	const std::string path = ExecutablePath();

	if (path.empty())
		return {};

	return fs::path(path).filename().string();
}

bool SetWorkingDirectoryToExecutable()
{
	const std::string path = ExecutablePath();

	if (path.empty())
		return false;

	const fs::path directory = fs::path(path).parent_path();

	if (directory.empty())
		return false;

	std::error_code ec;
	fs::current_path(directory, ec);
	return !ec;
}

} // namespace platform
