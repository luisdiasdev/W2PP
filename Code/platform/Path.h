#ifndef _PLATFORM_PATH_H_
#define _PLATFORM_PATH_H_

#include <string>

namespace platform
{

// Absolute path of the running executable, or an empty string when it cannot
// be determined.
std::string ExecutablePath();

// File name of the running executable (no directory), or an empty string.
std::string ExecutableFileName();

// Directory that contains the running executable, or an empty string.
std::string ExecutableDirectory();

// Sets the process working directory to the executable's directory, matching
// the legacy host so relative data paths (../../Common, ./account, ...) keep
// resolving. Returns false when the directory is unavailable.
bool SetWorkingDirectoryToExecutable();

} // namespace platform

#endif
