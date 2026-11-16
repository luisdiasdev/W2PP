#include <catch2/catch_test_macros.hpp>

#include "../../Code/platform/Clock.h"
#include "../../Code/platform/Diagnostics.h"
#include "../../Code/platform/File.h"
#include "../../Code/platform/FileSystem.h"
#include "../../Code/platform/Path.h"

#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{

struct TempDir
{
	fs::path path;

	TempDir()
	{
		std::error_code ec;
		const fs::path base = fs::temp_directory_path(ec);
		path = base / ("w2pp_platform_" +
			std::to_string(platform::MonotonicMs()) + "_" +
			std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		fs::create_directories(path, ec);
	}

	~TempDir()
	{
		std::error_code ec;
		fs::remove_all(path, ec);
	}
};

void WriteFile(const fs::path &path, const std::vector<std::uint8_t> &bytes)
{
	std::ofstream stream(path, std::ios::binary);
	stream.write(reinterpret_cast<const char *>(bytes.data()),
	             static_cast<std::streamsize>(bytes.size()));
}

std::set<std::string> CollectNames(const std::string &pattern)
{
	std::set<std::string> names;
	platform::DirectoryIterator iterator(pattern.c_str());
	platform::DirectoryEntry entry;

	while (iterator.Next(entry))
		names.insert(entry.name);

	return names;
}

} // namespace

TEST_CASE("platform file round trip preserves bytes and offsets", "[platform]")
{
	TempDir temp;
	const std::string path = (temp.path / "record.bin").string();

	std::vector<std::uint8_t> bytes(256);
	for (std::size_t i = 0; i < bytes.size(); ++i)
		bytes[i] = static_cast<std::uint8_t>((i * 17u + 3u) & 0xFFu);

	{
		platform::File file;
		REQUIRE(file.OpenReadWrite(path.c_str()));
		REQUIRE(file.Write(bytes.data(), bytes.size()));
		REQUIRE(file.Size() == static_cast<std::int64_t>(bytes.size()));
	}

	{
		platform::File file;
		REQUIRE(file.OpenRead(path.c_str()));
		REQUIRE(file.Size() == static_cast<std::int64_t>(bytes.size()));

		std::vector<std::uint8_t> readBack(bytes.size());
		REQUIRE(file.Read(readBack.data(), readBack.size()));
		REQUIRE(std::memcmp(readBack.data(), bytes.data(), bytes.size()) == 0);

		REQUIRE(file.Seek(100, SEEK_SET));
		std::vector<std::uint8_t> part(10);
		REQUIRE(file.Read(part.data(), part.size()));
		REQUIRE(std::memcmp(part.data(), bytes.data() + 100, part.size()) == 0);
	}
}

TEST_CASE("platform file reports missing files and short reads", "[platform]")
{
	TempDir temp;

	platform::File missing;
	REQUIRE_FALSE(missing.OpenRead((temp.path / "missing.bin").string().c_str()));
	REQUIRE(missing.LastError() == platform::FileError::NotFound);

	const std::string path = (temp.path / "short.bin").string();
	const std::uint8_t ten[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	WriteFile(path, std::vector<std::uint8_t>(ten, ten + 10));

	platform::File file;
	REQUIRE(file.OpenRead(path.c_str()));

	std::uint8_t twenty[20] = {};
	REQUIRE_FALSE(file.Read(twenty, sizeof(twenty)));
	REQUIRE(file.LastError() == platform::FileError::IoError);
}

TEST_CASE("platform filesystem distinguishes not found from failure", "[platform]")
{
	TempDir temp;

	const std::string missing = (temp.path / "missing.bin").string();
	const std::string source = (temp.path / "source.bin").string();
	const std::string target = (temp.path / "target.bin").string();
	const std::string existing = (temp.path / "existing.bin").string();

	REQUIRE(platform::DeleteFile(missing.c_str()) == platform::FsResult::NotFound);
	REQUIRE(platform::RenameFile(missing.c_str(), target.c_str()) ==
	        platform::FsResult::NotFound);

	{
		platform::File file;
		REQUIRE(file.OpenReadWrite(source.c_str()));
		const std::uint8_t byte = 0x42;
		REQUIRE(file.Write(&byte, 1));
	}

	REQUIRE(platform::RenameFile(source.c_str(),
		(temp.path / "missing-dir" / "target.bin").string().c_str()) ==
		platform::FsResult::Failed);
	REQUIRE(fs::exists(source));

	REQUIRE(platform::RenameFile(source.c_str(), target.c_str()) ==
	        platform::FsResult::Ok);
	REQUIRE(fs::exists(target));
	REQUIRE_FALSE(fs::exists(source));

	{
		platform::File file;
		REQUIRE(file.OpenReadWrite(existing.c_str()));
	}

	REQUIRE(platform::RenameFile(target.c_str(), existing.c_str()) ==
	        platform::FsResult::Failed);
	REQUIRE(fs::exists(target));

	REQUIRE(platform::DeleteFile(existing.c_str()) == platform::FsResult::Ok);
	REQUIRE(platform::DeleteFile(existing.c_str()) ==
	        platform::FsResult::NotFound);
}

TEST_CASE("platform directory iterator matches a wildcard scan", "[platform]")
{
	TempDir temp;

	WriteFile(temp.path / "a.txt", std::vector<std::uint8_t>{ 1 });
	WriteFile(temp.path / "b.txt", std::vector<std::uint8_t>{ 2 });
	WriteFile(temp.path / "c.dat", std::vector<std::uint8_t>{ 3 });

	std::error_code ec;
	REQUIRE(fs::create_directory(temp.path / "sub", ec));

	REQUIRE(CollectNames((temp.path / "*").string()) ==
	        std::set<std::string>{ "a.txt", "b.txt", "c.dat", "sub" });
	REQUIRE(CollectNames((temp.path / "*.txt").string()) ==
	        std::set<std::string>{ "a.txt", "b.txt" });
	REQUIRE(CollectNames((temp.path / "*.none").string()).empty());

	platform::DirectoryIterator missing((temp.path / "nope" / "*").string().c_str());
	platform::DirectoryEntry entry;
	REQUIRE(missing.Result() == platform::FsResult::NotFound);
	REQUIRE_FALSE(missing.Next(entry));

	platform::DirectoryIterator all((temp.path / "*").string().c_str());
	bool sawDirectory = false;

	while (all.Next(entry))
	{
		if (entry.name == "sub")
		{
			sawDirectory = true;
			REQUIRE(entry.isDirectory);
		}
	}

	REQUIRE(sawDirectory);
}

TEST_CASE("platform clock provides wrap-safe monotonic and local time", "[platform]")
{
	const std::uint32_t first = platform::MonotonicMs();
	const std::uint32_t second = platform::MonotonicMs();

	REQUIRE(platform::ElapsedMs(first, second) < 10000u);
	REQUIRE(platform::ElapsedMs(0xFFFFFFF0u, 0x00000010u) == 32u);

	const std::time_t sample = 1700000000;
	std::tm expected{};
#ifdef _WIN32
	localtime_s(&expected, &sample);
#else
	localtime_r(&sample, &expected);
#endif

	const platform::LocalTime local = platform::LocalTimeFromTimeT(sample);
	REQUIRE(local.year == expected.tm_year + 1900);
	REQUIRE(local.month == expected.tm_mon + 1);
	REQUIRE(local.day == expected.tm_mday);
	REQUIRE(local.hour == expected.tm_hour);
	REQUIRE(local.minute == expected.tm_min);
	REQUIRE(local.second == expected.tm_sec);
	REQUIRE(local.yearDay == expected.tm_yday);
	REQUIRE(local.weekday == expected.tm_wday);

	const platform::LocalTime now = platform::LocalNow();
	REQUIRE(now.year >= 2020);
	REQUIRE(now.month >= 1);
	REQUIRE(now.month <= 12);
	REQUIRE(now.day >= 1);
	REQUIRE(now.day <= 31);
	REQUIRE(now.millisecond >= 0);
	REQUIRE(now.millisecond < 1000);
}

TEST_CASE("platform path resolves siblings from a foreign working directory",
          "[platform]")
{
	const fs::path original = fs::current_path();

	REQUIRE_FALSE(platform::ExecutablePath().empty());
	REQUIRE_FALSE(platform::ExecutableDirectory().empty());
	REQUIRE(platform::SetWorkingDirectoryToExecutable());
	REQUIRE(fs::current_path() == fs::path(platform::ExecutableDirectory()));
	REQUIRE(fs::exists(fs::current_path() / "w2pp_tests.exe"));

	std::error_code ec;
	fs::current_path(original, ec);
	REQUIRE_FALSE(ec);
}

TEST_CASE("platform diagnostics records status without a window", "[platform]")
{
	static std::string lastStatus;
	static bool sawWarning = false;

	platform::SetStatusSink([](const char *line) { lastStatus = line; });
	platform::SetDiagnosticSink([](const char *line) {
		sawWarning = line != nullptr && line[0] != '\0';
	});

	platform::PublishStatus("status line");
	REQUIRE(lastStatus == "status line");

	REQUIRE(platform::ReportWarning(nullptr, "warning text", "caption", 0) == 1);
	REQUIRE(sawWarning);

	platform::SetStatusSink(nullptr);
	platform::SetDiagnosticSink(nullptr);
}
