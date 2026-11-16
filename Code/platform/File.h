#ifndef _PLATFORM_FILE_H_
#define _PLATFORM_FILE_H_

#include <cstddef>
#include <cstdint>
#include <fstream>

namespace platform
{

enum class FileError
{
	None,
	NotFound,
	AccessDenied,
	IoError
};

// Binary file with explicit size/seek and short-read/short-write detection.
// The wrapper replaces the legacy _open/_read/_write/_lseek/_filelength calls
// without changing their byte semantics.
class File
{
public:
	File();
	~File();

	File(const File &) = delete;
	File &operator=(const File &) = delete;

	// Open an existing file for reading.
	bool OpenRead(const char *path);

	// Open for reading and writing, creating an empty file when absent.
	// An existing file is not truncated.
	bool OpenReadWrite(const char *path);

	void Close();
	bool IsOpen() const;

	// Reads exactly |size| bytes. Returns false on a short read or I/O error.
	bool Read(void *data, std::size_t size);

	// Writes exactly |size| bytes. Returns false on a short write or I/O error.
	bool Write(const void *data, std::size_t size);

	// origin is SEEK_SET, SEEK_CUR or SEEK_END.
	bool Seek(std::int64_t offset, int origin);

	// Size in bytes, or -1 when it cannot be determined. The position is
	// preserved, matching _filelength.
	std::int64_t Size();

	// Current position, or -1 on error.
	std::int64_t Tell();

	FileError LastError() const { return lastError_; }

private:
	std::fstream stream_;
	bool readable_ = false;
	bool writable_ = false;
	FileError lastError_ = FileError::None;
};

} // namespace platform

#endif
