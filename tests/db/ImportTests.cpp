/*
 * Legacy importer integration tests (tasks 7.2 - 7.6).
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "Basedef.h"
#include "DbTestSupport.h"
#include "storage/AccountRepository.h"
#include "storage/LegacyImporter.h"
#include "storage/RecordCodec.h"

namespace
{

namespace fs = std::filesystem;

struct TempTree
{
	fs::path root;

	TempTree()
	{
		static unsigned int counter = 0;
		root = fs::temp_directory_path()
			/ ("w2pp_import_" + std::to_string(++counter));
		std::error_code error;
		fs::remove_all(root, error);
		fs::create_directories(root / "account");
		fs::create_directories(root / "char");
		fs::create_directories(root / "capsule");
	}

	~TempTree()
	{
		std::error_code error;
		fs::remove_all(root, error);
	}

	std::string Bucket(const std::string &name) const
	{
		char source[NAME_LENGTH + 1];
		char key[16];
		std::memset(source, 0, sizeof(source));
		std::memset(key, 0, sizeof(key));
		std::strncpy(source, name.c_str(), NAME_LENGTH);
		BASE_GetFirstKey(source, key);
		return key;
	}

	void WriteAccount(const STRUCT_ACCOUNTFILE &file)
	{
		std::string name(file.Info.AccountName,
			strnlen(file.Info.AccountName, ACCOUNTNAME_LENGTH));
		fs::create_directories(root / "account" / Bucket(name));

		std::ofstream stream(root / "account" / Bucket(name) / name,
			std::ios::binary | std::ios::trunc);
		stream.write(reinterpret_cast<const char *>(&file), sizeof(file));
	}

	void WriteMarker(const std::string &character, const std::string &account)
	{
		fs::create_directories(root / "char" / Bucket(character));

		char data[ACCOUNTNAME_LENGTH];
		std::memset(data, 0, sizeof(data));
		std::strncpy(data, account.c_str(), sizeof(data) - 1);

		std::ofstream stream(root / "char" / Bucket(character) / character,
			std::ios::binary | std::ios::trunc);
		stream.write(data, sizeof(data));
	}

	void WriteCapsule(int id, const STRUCT_CAPSULE &capsule)
	{
		std::ofstream stream(root / "capsule" / std::to_string(id),
			std::ios::binary | std::ios::trunc);
		stream.write(reinterpret_cast<const char *>(&capsule), sizeof(capsule));
	}
};

STRUCT_ACCOUNTFILE MakeLegacy(const char *account, const char *character,
	int level = 20)
{
	STRUCT_ACCOUNTFILE file;
	std::memset(&file, 0, sizeof(file));

	std::strncpy(file.Info.AccountName, account, ACCOUNTNAME_LENGTH - 1);
	std::strncpy(file.Info.AccountPass, "hunter2", ACCOUNTPASS_LENGTH - 1);
	file.Info.NumericToken[0] = -1;
	std::strncpy(file.Info.RealName, "Legacy Name", REALNAME_LENGTH - 1);
	std::strncpy(file.Info.Email, "legacy@example.test", EMAIL_LENGTH - 1);
	file.Info.SSN1 = 10;
	file.Info.SSN2 = 20;
	file.Coin = 500;
	file.Donate = 3;

	std::strncpy(file.Char[0].MobName, character, NAME_LENGTH - 1);
	file.Char[0].BaseScore.Level = level;
	file.Char[0].CurrentScore.Level = level;
	file.Char[0].Exp = 9000;
	file.Char[0].Equip[0].sIndex = 321;
	file.Char[0].Carry[0].sIndex = 654;
	file.Cargo[7].sIndex = 401;

	file.affect[0][0].Type = 3;
	file.affect[0][0].Value = 4;
	file.affect[0][0].Time = 0xFFFFFFFFu;

	file.mobExtra[0].ClassMaster = MORTAL;
	file.mobExtra[0].Fame = 88;
	file.mobExtra[0].DayLog.YearDay = 41;

	return file;
}

STRUCT_CAPSULE MakeLegacyCapsule(const char *name, int level)
{
	STRUCT_CAPSULE capsule;
	std::memset(&capsule, 0, sizeof(capsule));

	std::strncpy(capsule.MOB.MobName, name, NAME_LENGTH - 1);
	capsule.MOB.BaseScore.Level = level;
	capsule.MOB.CurrentScore.Level = level;
	capsule.MOB.Exp = 4444;
	capsule.MOB.Equip[0].sIndex = 111;
	capsule.extra.ClassMaster = ARCH;
	capsule.extra.Fame = 12;

	return capsule;
}

storage::AccountRepository MakeRepo(TestDatabase &test)
{
	storage::StorageConfig config = test.Config();
	config.bcrypt_cost = 12;
	return storage::AccountRepository(test.Db(), config);
}

void ShowReport(const storage::ImportReport &report)
{
	if (report.success)
		return;

	for (const std::string &line : report.lines)
		WARN(line);
}

} // namespace

TEST_CASE("a legacy tree imports accounts, characters and capsules",
	"[db][import]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepo(test);
	TempTree tree;

	STRUCT_ACCOUNTFILE account = MakeLegacy("LEGACY", "LEGACYCHAR");
	tree.WriteAccount(account);
	tree.WriteMarker("LEGACYCHAR", "LEGACY");
	tree.WriteCapsule(42, MakeLegacyCapsule("SEALEDCHAR", 30));

	storage::ImportOptions options;
	options.source = tree.root.string();
	std::string error;

	storage::LegacyImporter importer(test.Db(), repository);
	storage::ImportReport report = importer.Run(options, &error);

	ShowReport(report);

	REQUIRE(report.success);
	REQUIRE(report.accounts == 1);
	REQUIRE(report.characters == 1);
	REQUIRE(report.capsules == 1);
	REQUIRE(report.rejected == 0);

	storage::AccountRecord loaded;
	bool found = false;
	REQUIRE(repository.LoadAccount(
		std::vector<unsigned char>{'L', 'E', 'G', 'A', 'C', 'Y'},
		&loaded, &found, &error));
	REQUIRE(found);
	REQUIRE(loaded.characters.size() == 1);
	REQUIRE(loaded.coin == 500);
	REQUIRE(loaded.characters[0].exp == 9000);
	REQUIRE(loaded.characters[0].equipment[0].index == 321);
	REQUIRE(loaded.characters[0].inventory[0].index == 654);
	REQUIRE(loaded.characters[0].affects[0].time == 0xFFFFFFFFLL);

	storage::CharacterRecord capsule;
	REQUIRE(repository.LoadCapsule(42, &capsule, &found, &error));
	REQUIRE(found);
	REQUIRE(std::string(capsule.name.begin(), capsule.name.end()) == "SEALEDCHAR");

	// The imported credential is a hash and the account logs in with it.
	storage::AccountRecord login;
	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'L', 'E', 'G', 'A', 'C', 'Y'},
		"hunter2", "", &login, nullptr, &error) == storage::LoginStatus::Success);

	// Capsule identifiers continue above the imported maximum.
	long long accountId = loaded.account_id;
	int nextId = 0;
	REQUIRE(repository.PutInCapsule(accountId, 0, &nextId, &error));
	REQUIRE(nextId > 42);

	// import_state is complete.
	std::string state;
	REQUIRE(storage::GetMetadata(test.Db(), "import_state", &state, &error));
	REQUIRE(state == "complete");
}

TEST_CASE("a second import is idempotent", "[db][import]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepo(test);
	TempTree tree;

	tree.WriteAccount(MakeLegacy("REPEAT", "REPEATCHAR"));
	tree.WriteMarker("REPEATCHAR", "REPEAT");

	storage::ImportOptions options;
	options.source = tree.root.string();
	std::string error;

	storage::LegacyImporter importer(test.Db(), repository);
	REQUIRE(importer.Run(options, &error).success);

	storage::ImportReport second = importer.Run(options, &error);
	REQUIRE(second.success);
	REQUIRE(second.accounts == 1);
	REQUIRE(second.skipped == 1);
	REQUIRE(second.rejected == 0);

	storage::Result count = test.Db().Exec("SELECT count(*) FROM accounts");
	REQUIRE(count.Ok());
	REQUIRE(count.Int32(0, 0) == 1);
}

TEST_CASE("a truncated record is rejected and fails the import",
	"[db][import]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepo(test);
	TempTree tree;

	tree.WriteAccount(MakeLegacy("VALID", "VALIDCHAR"));
	tree.WriteMarker("VALIDCHAR", "VALID");

	fs::create_directories(tree.root / "account" / "BAD");
	{
		std::ofstream stream(tree.root / "account" / "BAD" / "BAD",
			std::ios::binary | std::ios::trunc);
		char garbage[100] = {0};
		stream.write(garbage, sizeof(garbage));
	}

	storage::ImportOptions options;
	options.source = tree.root.string();
	std::string error;

	storage::LegacyImporter importer(test.Db(), repository);
	storage::ImportReport report = importer.Run(options, &error);

	REQUIRE_FALSE(report.success);
	REQUIRE(report.rejected >= 1);
	REQUIRE(report.accounts == 1);

	std::string state;
	REQUIRE(storage::GetMetadata(test.Db(), "import_state", &state, &error));
	REQUIRE(state == "failed");
}

TEST_CASE("differing records require --replace", "[db][import]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepo(test);
	TempTree tree;

	STRUCT_ACCOUNTFILE account = MakeLegacy("CHANGED", "CHANGEDCHAR");
	tree.WriteAccount(account);
	tree.WriteMarker("CHANGEDCHAR", "CHANGED");

	storage::ImportOptions options;
	options.source = tree.root.string();
	std::string error;

	storage::LegacyImporter importer(test.Db(), repository);
	REQUIRE(importer.Run(options, &error).success);

	account.Coin = 999;
	tree.WriteAccount(account);

	storage::ImportReport withoutReplace = importer.Run(options, &error);
	REQUIRE_FALSE(withoutReplace.success);
	REQUIRE(withoutReplace.rejected == 1);

	options.replace = true;
	storage::ImportReport withReplace = importer.Run(options, &error);
	REQUIRE(withReplace.success);
	REQUIRE(withReplace.rejected == 0);

	storage::AccountRecord loaded;
	bool found = false;
	REQUIRE(repository.LoadAccount(
		std::vector<unsigned char>{'C', 'H', 'A', 'N', 'G', 'E', 'D'},
		&loaded, &found, &error));
	REQUIRE(found);
	REQUIRE(loaded.coin == 999);
}

TEST_CASE("a dry run reports without writing", "[db][import]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepo(test);
	TempTree tree;

	tree.WriteAccount(MakeLegacy("DRYRUN", "DRYRUNCHAR"));
	tree.WriteMarker("DRYRUNCHAR", "DRYRUN");

	storage::ImportOptions options;
	options.source = tree.root.string();
	options.dry_run = true;
	std::string error;

	storage::LegacyImporter importer(test.Db(), repository);
	storage::ImportReport report = importer.Run(options, &error);

	ShowReport(report);

	REQUIRE(report.success);
	REQUIRE(report.accounts == 1);

	storage::Result count = test.Db().Exec("SELECT count(*) FROM accounts");
	REQUIRE(count.Ok());
	REQUIRE(count.Int32(0, 0) == 0);

	std::string state;
	REQUIRE(storage::GetMetadata(test.Db(), "import_state", &state, &error));
	REQUIRE(state == "pending");
}

TEST_CASE("export writes a restorable legacy tree", "[db][import][export]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepo(test);
	TempTree tree;

	STRUCT_ACCOUNTFILE account = MakeLegacy("EXPORTME", "EXPORTCHAR");
	tree.WriteAccount(account);
	tree.WriteMarker("EXPORTCHAR", "EXPORTME");
	tree.WriteCapsule(7, MakeLegacyCapsule("EXPORTSEALED", 12));

	storage::ImportOptions options;
	options.source = tree.root.string();
	std::string error;

	storage::LegacyImporter importer(test.Db(), repository);
	REQUIRE(importer.Run(options, &error).success);

	fs::path exportRoot = tree.root / "restore";
	options.legacy_export = exportRoot.string();

	storage::ImportReport exportReport = importer.Run(options, &error);
	REQUIRE(exportReport.success);

	fs::path exportedAccount =
		exportRoot / "account" / tree.Bucket("EXPORTME") / "EXPORTME";
	REQUIRE(fs::exists(exportedAccount));
	REQUIRE(fs::file_size(exportedAccount) == sizeof(STRUCT_ACCOUNTFILE));
	REQUIRE(fs::file_size(exportRoot / "capsule" / "7") == sizeof(STRUCT_CAPSULE));
	REQUIRE(fs::exists(exportRoot / "Config.txt"));

	// The exported account has empty credential fields.
	STRUCT_ACCOUNTFILE restored;
	std::memset(&restored, 0, sizeof(restored));
	{
		std::ifstream stream(exportedAccount, std::ios::binary);
		stream.read(reinterpret_cast<char *>(&restored), sizeof(restored));
	}
	for (unsigned char byte : restored.Info.AccountPass)
		REQUIRE(byte == 0);
	REQUIRE(restored.Info.NumericToken[0] == -1);
	REQUIRE(std::strcmp(restored.Char[0].MobName, "EXPORTCHAR") == 0);
	REQUIRE(restored.Cargo[7].sIndex == 401);

	// Importing the exported account tree into a fresh database reproduces the
	// gameplay fields. The export intentionally omits credentials and the
	// legacy name markers, so add the marker for the exported character first
	// (a complete restore tree includes ./char).
	fs::create_directories(exportRoot / "char" / tree.Bucket("EXPORTCHAR"));
	{
		char marker[ACCOUNTNAME_LENGTH];
		std::memset(marker, 0, sizeof(marker));
		std::strncpy(marker, "EXPORTME", sizeof(marker) - 1);

		std::ofstream stream(exportRoot / "char" / tree.Bucket("EXPORTCHAR") / "EXPORTCHAR",
			std::ios::binary | std::ios::trunc);
		stream.write(marker, sizeof(marker));
	}

	TestDatabase secondTest;
	storage::AccountRepository secondRepository = MakeRepo(secondTest);
	storage::LegacyImporter secondImporter(secondTest.Db(), secondRepository);

	storage::ImportOptions secondOptions;
	secondOptions.source = exportRoot.string();
	storage::ImportReport secondReport = secondImporter.Run(secondOptions, &error);
	ShowReport(secondReport);
	REQUIRE(secondReport.success);

	STRUCT_ACCOUNTFILE firstExport;
	STRUCT_ACCOUNTFILE secondExport;
	bool found = false;

	REQUIRE(repository.ExportAccount(
		std::vector<unsigned char>{'E', 'X', 'P', 'O', 'R', 'T', 'M', 'E'},
		&firstExport, &found, &error));
	REQUIRE(found);
	REQUIRE(secondRepository.ExportAccount(
		std::vector<unsigned char>{'E', 'X', 'P', 'O', 'R', 'T', 'M', 'E'},
		&secondExport, &found, &error));
	REQUIRE(found);

	REQUIRE(std::memcmp(&firstExport, &secondExport, sizeof(firstExport)) == 0);

	storage::CharacterRecord firstCapsule;
	storage::CharacterRecord secondCapsule;

	REQUIRE(repository.LoadCapsule(7, &firstCapsule, &found, &error));
	REQUIRE(found);
	REQUIRE(secondRepository.LoadCapsule(7, &secondCapsule, &found, &error));
	REQUIRE(found);
	REQUIRE(firstCapsule.name == secondCapsule.name);
	REQUIRE(firstCapsule.exp == secondCapsule.exp);
	REQUIRE(firstCapsule.celestial[0].clazz == secondCapsule.celestial[0].clazz);
}
