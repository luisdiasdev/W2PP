#include "File.h"

#include <filesystem>
#include <system_error>

namespace platform
{
namespace fs = std::filesystem;
namespace
{

std::ios_base::seekdir ToSeekDir(int origin)
{
	switch (origin)
	{
	case SEEK_CUR:
		return std::ios_base::cur;
	case SEEK_END:
		return std::ios_base::end;
	default:
		return std::ios_base::beg;
	}
}

} // namespace

File::File() = default;

File::~File()
{
	Close();
}

void File::Close()
{
	if (stream_.is_open())
	{
		stream_.close();
		stream_.clear();
	}

	readable_ = false;
	writable_ = false;
}

bool File::OpenRead(const char *path)
{
	Close();
	lastError_ = FileError::None;

	if (path == nullptr || path[0] == '\0')
	{
		lastError_ = FileError::NotFound;
		return false;
	}

	std::error_code ec;
	const bool exists = fs::exists(path, ec);

	if (ec || !exists)
	{
		lastError_ = FileError::NotFound;
		return false;
	}

	stream_.open(path, std::ios::in | std::ios::binary);

	if (!stream_.is_open())
	{
		lastError_ = FileError::AccessDenied;
		return false;
	}

	readable_ = true;
	writable_ = false;
	return true;
}

bool File::OpenReadWrite(const char *path)
{
	Close();
	lastError_ = FileError::None;

	if (path == nullptr || path[0] == '\0')
	{
		lastError_ = FileError::NotFound;
		return false;
	}

	std::error_code ec;
	const bool exists = fs::exists(path, ec);

	if (ec)
	{
		lastError_ = FileError::IoError;
		return false;
	}

	if (!exists)
	{
		std::ofstream create(path, std::ios::binary);

		if (!create.is_open())
		{
			lastError_ = FileError::AccessDenied;
			return false;
		}
	}

	// in|out never truncates an existing file.
	stream_.open(path, std::ios::in | std::ios::out | std::ios::binary);

	if (!stream_.is_open())
	{
		lastError_ = FileError::AccessDenied;
		return false;
	}

	readable_ = true;
	writable_ = true;
	return true;
}

bool File::IsOpen() const
{
	return stream_.is_open();
}

bool File::Read(void *data, std::size_t size)
{
	if (!readable_ || !stream_.is_open())
	{
		lastError_ = FileError::IoError;
		return false;
	}

	if (size == 0)
		return true;

	stream_.clear();
	stream_.read(static_cast<char *>(data), static_cast<std::streamsize>(size));

	if (stream_.gcount() != static_cast<std::streamsize>(size))
	{
		lastError_ = FileError::IoError;
		return false;
	}

	return true;
}

bool File::Write(const void *data, std::size_t size)
{
	if (!writable_ || !stream_.is_open())
	{
		lastError_ = FileError::IoError;
		return false;
	}

	if (size == 0)
		return true;

	stream_.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
	stream_.flush();

	if (!stream_.good())
	{
		lastError_ = FileError::IoError;
		return false;
	}

	return true;
}

bool File::Seek(std::int64_t offset, int origin)
{
	if (!stream_.is_open())
	{
		lastError_ = FileError::IoError;
		return false;
	}

	const std::ios_base::seekdir dir = ToSeekDir(origin);
	bool ok = true;

	stream_.clear();

	if (readable_)
	{
		stream_.seekg(static_cast<std::streamoff>(offset), dir);

		if (!stream_.good())
			ok = false;
	}

	stream_.clear();

	if (writable_)
	{
		stream_.seekp(static_cast<std::streamoff>(offset), dir);

		if (!stream_.good())
			ok = false;
	}

	if (!ok)
		lastError_ = FileError::IoError;

	return ok;
}

std::int64_t File::Tell()
{
	if (!stream_.is_open())
		return -1;

	const std::streampos pos = readable_ ? stream_.tellg() : stream_.tellp();

	if (pos == std::streampos(-1))
		return -1;

	return static_cast<std::int64_t>(pos);
}

std::int64_t File::Size()
{
	const std::int64_t saved = Tell();

	if (saved < 0)
		return -1;

	if (!Seek(0, SEEK_END))
		return -1;

	const std::int64_t size = Tell();

	if (!Seek(saved, SEEK_SET))
		return -1;

	return size;
}

} // namespace platform
