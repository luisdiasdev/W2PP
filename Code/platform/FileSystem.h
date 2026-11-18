#ifndef _PLATFORM_FILESYSTEM_H_
#define _PLATFORM_FILESYSTEM_H_

#include "Clock.h"

#include <filesystem>
#include <string>

// <Windows.h> maps DeleteFile to DeleteFileA/DeleteFileW. The wrapper keeps
// the portable name, so remove the macro in translation units that see both.
#ifdef DeleteFile
#undef DeleteFile
#endif

namespace platform
{

enum class FsResult
{
	Ok,
	NotFound,
	Failed
};

struct DirectoryEntry
{
	std::string name;          // leaf name
	std::string path;          // path constructed from the scan directory
	bool isDirectory = false;
	LocalTime lastWriteTime{};
};

// Renames |from| to |to|. Fails when |from| is absent (NotFound) or when the
// rename itself fails (Failed). A destination that already exists is not
// replaced, matching the legacy MoveFile behavior.
FsResult RenameFile(const char *from, const char *to);

// Deletes |path|. Reports NotFound when the file is already absent and Failed
// when the operation fails for another reason.
FsResult DeleteFile(const char *path);

// Directory enumeration over a legacy-style pattern such as "./account/*" or
// "./Import/Item/*.txt". Matching is case-insensitive on the leaf name, and
// "." / ".." are never returned.
class DirectoryIterator
{
public:
	DirectoryIterator();
	explicit DirectoryIterator(const char *pattern);
	~DirectoryIterator();

	DirectoryIterator(const DirectoryIterator &) = delete;
	DirectoryIterator &operator=(const DirectoryIterator &) = delete;

	FsResult Open(const char *pattern);
	bool Next(DirectoryEntry &entry);
	FsResult Result() const { return result_; }
	void Close();

private:
	std::filesystem::directory_iterator iterator_;
	std::filesystem::directory_iterator end_;
	std::filesystem::path baseDir_;
	std::string mask_;
	bool open_ = false;
	FsResult result_ = FsResult::Ok;
};

} // namespace platform

#endif
