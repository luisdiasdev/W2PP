#include "LegacyImporter.h"

#include "../Basedef.h"
#include "../security/CredentialBytes.h"
#include "RecordCodec.h"
#include "Schema.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace storage
{
namespace
{

namespace fs = std::filesystem;

std::vector<unsigned char> ReadAll(const fs::path &path)
{
	std::ifstream stream(path, std::ios::binary);
	return std::vector<unsigned char>(
		std::istreambuf_iterator<char>(stream),
		std::istreambuf_iterator<char>());
}

bool WriteAll(const fs::path &path, const void *data, std::size_t size)
{
	std::error_code error;
	fs::create_directories(path.parent_path(), error);

	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	if (!stream)
		return false;

	stream.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
	return stream.good();
}

std::string FirstKey(const std::vector<unsigned char> &name)
{
	char source[NAME_LENGTH + 1];
	char dest[16];

	memset(source, 0, sizeof(source));
	memset(dest, 0, sizeof(dest));

	std::memcpy(source, name.data(), std::min<std::size_t>(name.size(), NAME_LENGTH));
	BASE_GetFirstKey(source, dest);

	return std::string(dest);
}

std::string StringOf(const std::vector<unsigned char> &bytes)
{
	return std::string(bytes.begin(), bytes.end());
}

std::vector<std::string> ListFiles(const fs::path &root, std::string *error)
{
	std::vector<std::string> files;

	std::error_code code;
	if (!fs::is_directory(root, code))
		return files;

	for (const auto &entry : fs::recursive_directory_iterator(root, code))
	{
		if (code)
			break;
		if (!entry.is_regular_file())
			continue;
		files.push_back(entry.path().string());
	}

	if (code && error != nullptr)
		*error = "cannot walk " + root.string() + ": " + code.message();

	std::sort(files.begin(), files.end());
	return files;
}

int LegacyLastCapsule(const std::string &source)
{
	std::ifstream stream(fs::path(source) / "Config.txt");
	if (!stream)
		return 0;

	std::string key;
	int value = 0;

	while (stream >> key)
	{
		if (key == "LastCapsule")
		{
			stream >> value;
			break;
		}
	}

	return value;
}

} // namespace

std::string ImportReport::Summary() const
{
	char buffer[512];
	std::snprintf(buffer, sizeof(buffer),
		"accounts=%d characters=%d capsules=%d skipped=%d rejected=%d result=%s",
		accounts, characters, capsules, skipped, rejected,
		success ? "complete" : "failed");
	return buffer;
}

LegacyImporter::LegacyImporter(Database &database, AccountRepository &repository)
	: database_(database), repository_(repository)
{
}

ImportReport LegacyImporter::Run(const ImportOptions &options,
	std::string *error)
{
	ImportReport report;

	if (options.source.empty() && !options.empty_store)
	{
		if (error != nullptr)
			*error = "import: --source is required";
		return report;
	}

	if (!options.dry_run)
	{
		Result lock = database_.ExecParams("SELECT pg_advisory_lock($1)",
			{ParamInt64(kImportLockKey)}, false);

		if (!lock.Ok())
		{
			if (error != nullptr)
				*error = "import: cannot acquire the import advisory lock: "
					+ lock.Error();
			return report;
		}

		SetMetadata(database_, "import_state", "pending", error);
	}

	if (options.empty_store)
	{
		if (!options.dry_run)
		{
			repository_.SetCapsuleSequence(0, error);
			SetMetadata(database_, "import_state", "complete", error);
		}

		report.success = true;
		report.lines.push_back("empty store initialized");
	}
	else
	{
		const fs::path source(options.source);

		for (const std::string &path : ListFiles(source / "account", error))
		{
			fs::path filePath(path);
			std::error_code code;
			const std::uintmax_t size = fs::file_size(filePath, code);

			if (code || !((size >= 7500 && size <= 7600)
					|| size == sizeof(STRUCT_ACCOUNTFILE)))
			{
				++report.rejected;
				report.lines.push_back("rejected " + path + ": unexpected size "
					+ std::to_string(size));
				continue;
			}

			std::vector<unsigned char> bytes = ReadAll(filePath);
			if (bytes.empty())
			{
				++report.rejected;
				report.lines.push_back("rejected " + path + ": empty file");
				continue;
			}

			STRUCT_ACCOUNTFILE file;
			std::memset(&file, 0, sizeof(file));
			std::memcpy(&file, bytes.data(),
				std::min<std::size_t>(bytes.size(), sizeof(file)));

			STRUCT_ACCOUNTFILE normalized;
			NormalizeAccountFile(file, &normalized);

			// Credentials are not part of the normalized record (the checksum
			// must exclude them), but the save path needs the legacy bytes to
			// hash them.
			STRUCT_ACCOUNTFILE saveFile = normalized;

			std::memcpy(saveFile.Info.AccountPass, file.Info.AccountPass,
				ACCOUNTPASS_LENGTH);
			std::memcpy(saveFile.Info.NumericToken, file.Info.NumericToken,
				sizeof(saveFile.Info.NumericToken));

			std::vector<unsigned char> name =
				NameBytes(normalized.Info.AccountName, ACCOUNTNAME_LENGTH);

			if (name.empty())
			{
				++report.rejected;
				report.lines.push_back("rejected " + path + ": empty account name");
				continue;
			}

			AccountRecord existing;
			bool found = false;

			if (!repository_.LoadAccount(name, &existing, &found, error))
			{
				++report.rejected;
				report.lines.push_back("rejected " + path + ": " + *error);
				continue;
			}

			security::LegacyCredential credential =
				security::SplitLegacyCredential(saveFile.Info.AccountPass,
					ACCOUNTPASS_LENGTH);

			if (!credential.usable)
			{
				report.lines.push_back("locked " + StringOf(name)
					+ ": unusable legacy credential, imported disabled");

				// A random placeholder gives the row a verifiable hash while
				// the disabled state refuses every login.
				char placeholder[ACCOUNTPASS_LENGTH];
				std::snprintf(placeholder, sizeof(placeholder), "#IMP%u",
					static_cast<unsigned>(report.accounts + report.rejected + 1));
				std::memset(saveFile.Info.AccountPass, 0,
					sizeof(saveFile.Info.AccountPass));
				std::strncpy(saveFile.Info.AccountPass, placeholder,
					sizeof(saveFile.Info.AccountPass) - 1);
			}

			const unsigned long long sourceChecksum =
				StructuralChecksum(normalized);

			int characterCount = 0;
			for (int slot = 0; slot < MOB_PER_ACCOUNT; ++slot)
			{
				if (normalized.Char[slot].MobName[0] != '\0')
					++characterCount;
			}

			if (found)
			{
				STRUCT_ACCOUNTFILE current;
				bool currentFound = false;

				if (!repository_.LoadAccountFile(normalized, &current,
						&currentFound, error) || !currentFound)
				{
					++report.rejected;
					report.lines.push_back("rejected " + path + ": cannot read existing row");
					continue;
				}

				if (StructuralChecksum(current) == sourceChecksum)
				{
					++report.skipped;
					report.accounts++;
					report.characters += characterCount;
					report.lines.push_back("skipped " + StringOf(name) + ": unchanged");
					continue;
				}

				if (!options.replace)
				{
					++report.rejected;
					report.lines.push_back("rejected " + path
						+ ": differs from the stored account (use --replace)");
					continue;
				}

				if (!options.dry_run
					&& !repository_.SaveAccountFile(saveFile, false, nullptr, error))
				{
					++report.rejected;
					report.lines.push_back("rejected " + path + ": " + *error);
					continue;
				}
			}
			else
			{
				if (!options.dry_run)
				{
					bool inserted = false;

					if (!repository_.SaveAccountFile(saveFile, true, &inserted,
							error))
					{
						++report.rejected;
						report.lines.push_back("rejected " + path + ": " + *error);
						continue;
					}
				}
			}

			if (!options.dry_run)
			{
				AccountRecord reloaded;
				bool reloadedFound = false;

				if (!repository_.LoadAccount(name, &reloaded, &reloadedFound, error)
					|| !reloadedFound)
				{
					++report.rejected;
					report.lines.push_back("rejected " + StringOf(name)
						+ ": verification read failed");
					continue;
				}

				STRUCT_ACCOUNTFILE rebuilt;
				AccountFileFromRecord(reloaded, &rebuilt);

				if (StructuralChecksum(rebuilt) != sourceChecksum)
				{
					++report.rejected;
					report.lines.push_back("rejected " + StringOf(name)
						+ ": structural checksum mismatch after import");
					continue;
				}
			}

			report.accounts++;
			report.characters += characterCount;
			report.lines.push_back("imported " + StringOf(name));
		}

		// Marker verification requires the imported rows, so a dry run skips
		// it; it still reports account/capsule validation above.
		if (!options.dry_run)
		{
		// Marker verification: every ./char marker must name an existing
		// character owned by the account stored in the marker file, and every
		// live character needs a marker. The marker's file name is the
		// character name; its content is the owning account name.
		std::set<std::string> markerNames;

		for (const std::string &path : ListFiles(source / "char", error))
		{
			std::vector<unsigned char> bytes = ReadAll(fs::path(path));
			if (bytes.size() < ACCOUNTNAME_LENGTH)
			{
				++report.rejected;
				report.lines.push_back("rejected marker " + path + ": short file");
				continue;
			}

			std::vector<unsigned char> owner =
				NameBytes(reinterpret_cast<const char *>(bytes.data()), ACCOUNTNAME_LENGTH);
			std::vector<unsigned char> character =
				NameBytes(fs::path(path).filename().string().c_str(), NAME_LENGTH);

			std::vector<unsigned char> dbOwner;
			bool found = false;

			error->clear();

			const bool lookup = repository_.FindAccountByCharacterName(character,
				&dbOwner, &found, error);

			if (!lookup || !found)
			{
				++report.rejected;
				report.lines.push_back("rejected marker " + path + ": "
					+ (lookup ? std::string("no character with that name") : *error));
				continue;
			}

			if (NameKey(dbOwner) != NameKey(owner))
			{
				++report.rejected;
				report.lines.push_back("rejected marker " + path
					+ ": owner mismatch");
				continue;
			}

			markerNames.insert(StringOf(NameKey(character)));
		}

		Result live = database_.ExecParams(
			"SELECT c.name, c.name_key FROM characters c "
			"JOIN accounts a ON a.account_id = c.account_id "
			"WHERE c.account_id IS NOT NULL", {}, true);

		if (live.Ok())
		{
			for (int row = 0; row < live.Rows(); ++row)
			{
				std::vector<unsigned char> nameKey = live.Bytes(row, 1);
				std::string key = StringOf(nameKey);

				if (markerNames.find(key) == markerNames.end())
				{
					++report.rejected;
					report.lines.push_back("missing marker for character "
						+ StringOf(live.Bytes(row, 0)));
				}
			}
		}
		}

		// Capsules.
		int maxCapsule = LegacyLastCapsule(options.source);

		for (const std::string &path : ListFiles(source / "capsule", error))
		{
			const std::string fileName = fs::path(path).filename().string();
			int capsuleId = std::atoi(fileName.c_str());

			if (capsuleId <= 0 || capsuleId > 65535)
			{
				++report.rejected;
				report.lines.push_back("rejected capsule " + path + ": bad id");
				continue;
			}

			std::error_code code;
			if (fs::file_size(fs::path(path), code) != sizeof(STRUCT_CAPSULE))
			{
				++report.rejected;
				report.lines.push_back("rejected capsule " + path + ": unexpected size");
				continue;
			}

			std::vector<unsigned char> bytes = ReadAll(fs::path(path));
			STRUCT_CAPSULE capsule;
			std::memset(&capsule, 0, sizeof(capsule));
			std::memcpy(&capsule, bytes.data(),
				std::min<std::size_t>(bytes.size(), sizeof(capsule)));

			maxCapsule = std::max(maxCapsule, capsuleId);

			CharacterRecord record;
			CharacterRecordFromMob(capsule.MOB, capsule.extra, nullptr, -1, &record);
			record.live = false;
			record.capsule_id = capsuleId;

			CharacterRecord existingCapsule;
			bool found = false;

			if (!repository_.LoadCapsule(capsuleId, &existingCapsule, &found, error))
			{
				++report.rejected;
				report.lines.push_back("rejected capsule " + path + ": " + *error);
				continue;
			}

			if (found)
			{
				++report.skipped;
				continue;
			}

			if (!options.dry_run
				&& !repository_.ImportCapsule(capsuleId, record, error))
			{
				++report.rejected;
				report.lines.push_back("rejected capsule " + path + ": " + *error);
				continue;
			}

			report.capsules++;
		}

		if (!options.dry_run)
			repository_.SetCapsuleSequence(maxCapsule, error);

		report.success = report.rejected == 0;

		if (!options.dry_run)
		{
			SetMetadata(database_, "import_state",
				report.success ? "complete" : "failed", error);
		}
	}

	if (!options.legacy_export.empty())
	{
		if (!ExportLegacyTree(options, &report, error))
			report.success = false;
	}

	report.lines.push_back(report.Summary());

	if (!options.report_path.empty())
	{
		std::ofstream reportFile(options.report_path, std::ios::trunc);
		for (const std::string &line : report.lines)
			reportFile << line << '\n';
	}

	if (!options.dry_run)
		database_.ExecParams("SELECT pg_advisory_unlock($1)",
			{ParamInt64(kImportLockKey)}, false);

	return report;
}

bool LegacyImporter::ExportLegacyTree(const ImportOptions &options,
	ImportReport *report, std::string *error)
{
	const fs::path root(options.legacy_export);

	Result accounts = database_.ExecParams(
		"SELECT name FROM accounts ORDER BY name_key", {}, true);

	if (!accounts.Ok())
	{
		if (error != nullptr)
			*error = "legacy export: " + accounts.Error();
		return false;
	}

	for (int row = 0; row < accounts.Rows(); ++row)
	{
		std::vector<unsigned char> name = accounts.Bytes(row, 0);
		STRUCT_ACCOUNTFILE file;
		bool found = false;

		if (!repository_.ExportAccount(name, &file, &found, error) || !found)
		{
			if (error != nullptr && error->empty())
				*error = "legacy export: account disappeared";
			return false;
		}

		std::string filePath = StringOf(name);
		fs::path target = root / "account" / FirstKey(name) / filePath;

		if (!WriteAll(target, &file, sizeof(file)))
		{
			if (error != nullptr)
				*error = "legacy export: cannot write " + target.string();
			return false;
		}
	}

	Result capsules = database_.Exec(
		"SELECT capsule_id FROM characters WHERE capsule_id IS NOT NULL "
		"ORDER BY capsule_id");

	if (!capsules.Ok())
	{
		if (error != nullptr)
			*error = "legacy export: " + capsules.Error();
		return false;
	}

	int maxCapsule = 0;

	for (int row = 0; row < capsules.Rows(); ++row)
	{
		const int capsuleId = capsules.Int32(row, 0);
		maxCapsule = std::max(maxCapsule, capsuleId);

		CharacterRecord record;
		bool found = false;

		if (!repository_.LoadCapsule(capsuleId, &record, &found, error) || !found)
			return false;

		STRUCT_CAPSULE capsule;
		std::memset(&capsule, 0, sizeof(capsule));
		MobFromCharacterRecord(record, &capsule.MOB, &capsule.extra, nullptr, nullptr);

		fs::path target = root / "capsule" / std::to_string(capsuleId);

		if (!WriteAll(target, &capsule, sizeof(capsule)))
		{
			if (error != nullptr)
				*error = "legacy export: cannot write " + target.string();
			return false;
		}
	}

	char config[64];
	std::snprintf(config, sizeof(config), "Sapphire 8\nLastCapsule %d\n", maxCapsule);

	if (!WriteAll(root / "Config.txt", config, std::strlen(config)))
	{
		if (error != nullptr)
			*error = "legacy export: cannot write Config.txt";
		return false;
	}

	if (report != nullptr)
		report->lines.push_back("legacy export written to " + options.legacy_export);

	return true;
}

} // namespace storage
