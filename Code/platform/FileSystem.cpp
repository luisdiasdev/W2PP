#include "FileSystem.h"

#include <cctype>
#include <system_error>

namespace platform
{
namespace fs = std::filesystem;
namespace
{

// Windows matches file names case-insensitively; Linux treats names as
// byte-exact. Folding only on Windows keeps a scan from returning a name that a
// later fopen cannot open.
char FoldCase(unsigned char c)
{
#if defined(_WIN32)
	return static_cast<char>(std::tolower(c));
#else
	return static_cast<char>(c);
#endif
}

bool WildcardMatch(const std::string &pattern, const std::string &text)
{
	std::size_t patternPos = 0;
	std::size_t textPos = 0;
	std::size_t starPos = std::string::npos;
	std::size_t starText = 0;

	while (textPos < text.size())
	{
		if (patternPos < pattern.size() &&
		    (pattern[patternPos] == '?' ||
		     FoldCase(static_cast<unsigned char>(pattern[patternPos])) ==
		     FoldCase(static_cast<unsigned char>(text[textPos]))))
		{
			++patternPos;
			++textPos;
		}
		else if (patternPos < pattern.size() && pattern[patternPos] == '*')
		{
			starPos = patternPos++;
			starText = textPos;
		}
		else if (starPos != std::string::npos)
		{
			patternPos = starPos + 1;
			textPos = ++starText;
		}
		else
		{
			return false;
		}
	}

	while (patternPos < pattern.size() && pattern[patternPos] == '*')
		++patternPos;

	return patternPos == pattern.size();
}

} // namespace

FsResult RenameFile(const char *from, const char *to)
{
	if (from == nullptr || to == nullptr || from[0] == '\0' || to[0] == '\0')
		return FsResult::Failed;

	std::error_code ec;
	const bool exists = fs::exists(from, ec);

	if (ec)
		return FsResult::Failed;

	if (!exists)
		return FsResult::NotFound;

	if (fs::exists(to, ec))
		return FsResult::Failed;

	if (ec)
		return FsResult::Failed;

	fs::rename(from, to, ec);
	return ec ? FsResult::Failed : FsResult::Ok;
}

FsResult DeleteFile(const char *path)
{
	if (path == nullptr || path[0] == '\0')
		return FsResult::Failed;

	std::error_code ec;
	const bool exists = fs::exists(path, ec);

	if (ec)
		return FsResult::Failed;

	if (!exists)
		return FsResult::NotFound;

	if (!fs::remove(path, ec) || ec)
		return FsResult::Failed;

	return FsResult::Ok;
}

DirectoryIterator::DirectoryIterator() = default;

DirectoryIterator::DirectoryIterator(const char *pattern)
{
	Open(pattern);
}

DirectoryIterator::~DirectoryIterator()
{
	Close();
}

FsResult DirectoryIterator::Open(const char *pattern)
{
	Close();

	result_ = FsResult::Ok;

	if (pattern == nullptr || pattern[0] == '\0')
	{
		result_ = FsResult::Failed;
		return result_;
	}

	const fs::path full(pattern);
	std::string leaf = full.filename().string();

	std::error_code patternEc;

	if (fs::is_directory(full, patternEc))
	{
		baseDir_ = full;
		mask_ = "*";
	}
	else if (leaf.empty())
	{
		baseDir_ = full;
		mask_ = "*";
	}
	else
	{
		baseDir_ = full.parent_path();
		mask_ = leaf;
	}

	if (mask_.empty())
		mask_ = "*";

	if (baseDir_.empty())
		baseDir_ = ".";

	std::error_code ec;
	const bool isDirectory = fs::is_directory(baseDir_, ec);

	if (ec)
	{
		result_ = FsResult::NotFound;
		return result_;
	}

	if (!isDirectory)
	{
		result_ = FsResult::Failed;
		return result_;
	}

	iterator_ = fs::directory_iterator(baseDir_, ec);

	if (ec)
	{
		result_ = FsResult::Failed;
		return result_;
	}

	open_ = true;
	return result_;
}

bool DirectoryIterator::Next(DirectoryEntry &entry)
{
	if (!open_)
		return false;

	std::error_code ec;

	while (iterator_ != end_)
	{
		const fs::directory_entry current = *iterator_;
		iterator_.increment(ec);

		if (ec)
		{
			result_ = FsResult::Failed;
			open_ = false;
			return false;
		}

		const std::string name = current.path().filename().string();

		if (!WildcardMatch(mask_, name))
			continue;

		entry.name = name;
		entry.path = current.path().string();
		entry.isDirectory = current.is_directory(ec);

		if (ec)
			entry.isDirectory = false;

		const fs::file_time_type writeTime = current.last_write_time(ec);

		if (!ec)
		{
			const auto systemTime =
				std::chrono::time_point_cast<std::chrono::system_clock::duration>(
					writeTime - fs::file_time_type::clock::now() +
					std::chrono::system_clock::now());
			entry.lastWriteTime = LocalTimeFromSystemTime(systemTime);
		}
		else
		{
			entry.lastWriteTime = LocalTime{};
			ec.clear();
		}

		return true;
	}

	open_ = false;
	return false;
}

void DirectoryIterator::Close()
{
	iterator_ = fs::directory_iterator();
	open_ = false;
}

} // namespace platform
