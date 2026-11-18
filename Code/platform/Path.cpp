#include "Path.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace platform
{
namespace fs = std::filesystem;

std::string ExecutablePath()
{
	const char *programPath = _pgmptr;

	if (programPath == nullptr || programPath[0] == '\0')
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
