/*
 * Repository integration tests (tasks 4.2 - 4.8 and 5.3 - 5.6).
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "Basedef.h"
#include "DbTestSupport.h"
#include "security/CredentialBytes.h"
#include "security/PasswordHash.h"
#include "storage/AccountRepository.h"
#include "storage/RecordCodec.h"

namespace
{

STRUCT_ACCOUNTFILE MakeAccount(const char *name, const char *password = "hunter2")
{
	STRUCT_ACCOUNTFILE file;
	std::memset(&file, 0, sizeof(file));

	std::strncpy(file.Info.AccountName, name, ACCOUNTNAME_LENGTH);
	std::strncpy(file.Info.AccountPass, password, ACCOUNTPASS_LENGTH);
	file.Info.NumericToken[0] = -1;
	std::strncpy(file.Info.RealName, "Real Name", REALNAME_LENGTH);
	file.Info.SSN1 = 111;
	file.Info.SSN2 = 222;
	std::strncpy(file.Info.Email, "user@example.test", EMAIL_LENGTH);
	std::strncpy(file.Info.Telephone, "555-0100", TELEPHONE_LENGTH);
	std::strncpy(file.Info.Address, "1 Example Street", ADDRESS_LENGTH);
	file.Info.Year = 123;
	file.Info.YearDay = 45;

	file.Coin = 12345;
	file.Donate = 7;

	for (int i = 0; i < 52; ++i)
		file.TempKey[i] = static_cast<char>(i + 1);

	for (int i = 0; i < 64; ++i)
		file.ShortSkill[0][i % 16] = static_cast<unsigned char>(i);

	return file;
}

void AddCharacter(STRUCT_ACCOUNTFILE *file, int slot, const char *name, int level)
{
	STRUCT_MOB &mob = file->Char[slot];

	std::strncpy(mob.MobName, name, NAME_LENGTH);
	mob.Class = static_cast<unsigned char>(slot);
	mob.Coin = 1000 + slot;
	mob.Exp = 5000LL + slot;
	mob.BaseScore.Level = level;
	mob.CurrentScore.Level = level;
	mob.CurrentScore.Hp = 100 + slot;
	mob.Magic = 0xFFFFFFFFu;
	mob.Resist[0] = static_cast<char>(-128);

	mob.Equip[0].sIndex = 100 + slot;
	mob.Equip[0].stEffect[0].cEffect = 3;
	mob.Equip[0].stEffect[0].cValue = 7;

	mob.Carry[0].sIndex = 200 + slot;
	mob.Carry[0].stEffect[1].cEffect = 4;

	file->affect[slot][0].Type = 1;
	file->affect[slot][0].Value = 2;
	file->affect[slot][0].Level = 3;
	file->affect[slot][0].Time = 0xFFFFFFFFu;

	file->mobExtra[slot].ClassMaster = MORTAL;
	file->mobExtra[slot].Fame = 100 + slot;
	file->mobExtra[slot].DayLog.Exp = 777 + slot;
	file->mobExtra[slot].DayLog.YearDay = 41;
	file->mobExtra[slot].SaveCelestial[0].Class = 2;
	file->mobExtra[slot].SaveCelestial[0].BaseScore.Level = 50 + slot;
}

storage::AccountRepository MakeRepository(TestDatabase &test, int cost = 10)
{
	storage::StorageConfig config = test.Config();
	config.bcrypt_cost = cost;
	return storage::AccountRepository(test.Db(), config);
}

long long AccountId(TestDatabase &test, const char *name)
{
	std::vector<unsigned char> key = storage::NameKey(
		std::vector<unsigned char>(name, name + std::strlen(name)));

	storage::Result result = test.Db().ExecParams(
		"SELECT account_id FROM accounts WHERE name_key = $1",
		{storage::ParamBytes(key)}, true);

	REQUIRE(result.Ok());
	REQUIRE(result.Rows() == 1);
	return result.Int64(0, 0);
}

STRUCT_ACCOUNTFILE LoadFile(TestDatabase &test, storage::AccountRepository &repo,
	const char *name)
{
	STRUCT_ACCOUNTFILE key;
	std::memset(&key, 0, sizeof(key));
	std::strncpy(key.Info.AccountName, name, ACCOUNTNAME_LENGTH);

	STRUCT_ACCOUNTFILE loaded;
	bool found = false;
	std::string error;
	REQUIRE(repo.LoadAccountFile(key, &loaded, &found, &error));
	REQUIRE(found);
	return loaded;
}

STRUCT_ACCOUNTFILE SaveNew(TestDatabase &test,
	storage::AccountRepository &repo, STRUCT_ACCOUNTFILE file)
{
	bool inserted = false;
	std::string error;
	const bool saved = repo.SaveAccountFile(file, true, &inserted, &error);
	if (!saved)
		WARN("SaveAccountFile failed: " << error);
	REQUIRE(saved);
	REQUIRE(inserted);
	return file;
}

} // namespace

TEST_CASE("account save and load round-trip every field", "[db][repository]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE file = MakeAccount("ACCOUNTONE");
	AddCharacter(&file, 0, "HEROONE", 10);
	AddCharacter(&file, 1, "HEROTWO", 20);
	file.Cargo[0].sIndex = 401;
	file.Cargo[127].sIndex = 406;

	SaveNew(test, repository, file);

	STRUCT_ACCOUNTFILE loaded = LoadFile(test, repository, "ACCOUNTONE");

	REQUIRE(loaded.Coin == file.Coin);
	REQUIRE(loaded.Donate == file.Donate);
	REQUIRE(loaded.Info.SSN1 == file.Info.SSN1);
	REQUIRE(loaded.Info.Year == file.Info.Year);
	REQUIRE(std::memcmp(loaded.TempKey, file.TempKey, 52) == 0);
	REQUIRE(loaded.Cargo[0].sIndex == 401);
	REQUIRE(loaded.Cargo[127].sIndex == 406);
	REQUIRE(loaded.Char[0].Exp == file.Char[0].Exp);
	REQUIRE(loaded.Char[1].CurrentScore.Hp == file.Char[1].CurrentScore.Hp);
	REQUIRE(loaded.Char[0].Magic == 0xFFFFFFFFu);
	REQUIRE(loaded.Char[0].Resist[0] == static_cast<char>(-128));
	REQUIRE(loaded.Char[0].Equip[0].sIndex == 100);
	REQUIRE(loaded.Char[0].Carry[0].sIndex == 200);
	REQUIRE(loaded.affect[0][0].Time == 0xFFFFFFFFu);
	REQUIRE(loaded.mobExtra[0].Fame == 100);
	REQUIRE(loaded.mobExtra[1].SaveCelestial[0].BaseScore.Level == 51);
	REQUIRE(loaded.mobExtra[0].DayLog.Exp == 777);

	// Update scalars and arrays, then reload.
	loaded.Coin = 999;
	loaded.Char[0].Exp = 123456;
	loaded.Char[0].Equip[5].sIndex = 999;
	loaded.Cargo[0].sIndex = 0;

	std::string error;
	REQUIRE(repository.SaveAccountFile(loaded, false, nullptr, &error));

	STRUCT_ACCOUNTFILE reloaded = LoadFile(test, repository, "ACCOUNTONE");
	REQUIRE(reloaded.Coin == 999);
	REQUIRE(reloaded.Char[0].Exp == 123456);
	REQUIRE(reloaded.Char[0].Equip[5].sIndex == 999);
	REQUIRE(reloaded.Cargo[0].sIndex == 0);
	REQUIRE(reloaded.Cargo[127].sIndex == 406);
}

TEST_CASE("an unchanged save does not rewrite child arrays", "[db][repository]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE file = MakeAccount("NOOP");
	AddCharacter(&file, 0, "STILL", 10);
	file.Cargo[0].sIndex = 401;
	SaveNew(test, repository, file);

	long long accountId = AccountId(test, "NOOP");

	auto childIds = [&](const char *sql, long long id)
	{
		storage::Result result = test.Db().ExecParams(sql,
			{storage::ParamInt64(id)}, false);
		REQUIRE(result.Ok());
		std::string joined;
		for (int row = 0; row < result.Rows(); ++row)
			joined += result.Text(row, 0) + ";";
		return joined;
	};

	std::string equipmentBefore = childIds(
		"SELECT xmin::text FROM character_equipment WHERE character_id = "
		"(SELECT character_id FROM characters WHERE account_id = $1)",
		accountId);
	std::string cargoBefore = childIds(
		"SELECT xmin::text FROM account_cargo WHERE account_id = $1", accountId);
	REQUIRE_FALSE(equipmentBefore.empty());

	STRUCT_ACCOUNTFILE loaded = LoadFile(test, repository, "NOOP");
	std::string error;
	REQUIRE(repository.SaveAccountFile(loaded, false, nullptr, &error));

	std::string equipmentAfter = childIds(
		"SELECT xmin::text FROM character_equipment WHERE character_id = "
		"(SELECT character_id FROM characters WHERE account_id = $1)",
		accountId);
	std::string cargoAfter = childIds(
		"SELECT xmin::text FROM account_cargo WHERE account_id = $1", accountId);

	REQUIRE(equipmentBefore == equipmentAfter);
	REQUIRE(cargoBefore == cargoAfter);
}

TEST_CASE("character names are unique and released on delete",
	"[db][repository]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE first = MakeAccount("OWNERA");
	AddCharacter(&first, 0, "KNIGHT", 10);
	SaveNew(test, repository, first);

	bool available = false;
	std::string error;
	REQUIRE(repository.CharacterNameAvailable(
		std::vector<unsigned char>{'K', 'N', 'I', 'G', 'H', 'T'},
		&available, &error));
	REQUIRE_FALSE(available);

	STRUCT_ACCOUNTFILE second = MakeAccount("OWNERB");
	AddCharacter(&second, 0, "OTHER", 10);
	SaveNew(test, repository, second);

	STRUCT_ACCOUNTFILE conflicting = second;
	AddCharacter(&conflicting, 1, "knight", 10);
	conflicting.Coin = 1;

	bool inserted = false;
	REQUIRE_FALSE(repository.SaveAccountFile(conflicting, false, &inserted, &error));

	storage::Result none = test.Db().ExecParams(
		"SELECT count(*) FROM characters WHERE name_key = $1",
		{storage::ParamBytes(storage::NameKey(
			std::vector<unsigned char>{'K', 'N', 'I', 'G', 'H', 'T'}))}, false);
	REQUIRE(none.Ok());
	REQUIRE(none.Int32(0, 0) == 1); // still only OWNERA's KNIGHT

	long long ownerA = AccountId(test, "OWNERA");
	REQUIRE(repository.DeleteCharacterByName(ownerA,
		std::vector<unsigned char>{'K', 'N', 'I', 'G', 'H', 'T'}, &error));

	REQUIRE(repository.CharacterNameAvailable(
		std::vector<unsigned char>{'K', 'N', 'I', 'G', 'H', 'T'},
		&available, &error));
	REQUIRE(available);

	REQUIRE(repository.SaveAccountFile(conflicting, false, &inserted, &error));
	std::vector<unsigned char> account;
	bool found = false;
	REQUIRE(repository.FindAccountByCharacterName(
		std::vector<unsigned char>{'K', 'N', 'I', 'G', 'H', 'T'},
		&account, &found, &error));
	REQUIRE(found);
	REQUIRE(std::string(account.begin(), account.end()) == "OWNERB");
}

TEST_CASE("capsules seal and restore the same character", "[db][repository]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE file = MakeAccount("CAPSULE");
	AddCharacter(&file, 0, "SEALED", 30);
	AddCharacter(&file, 1, "KEEPER", 40);
	SaveNew(test, repository, file);

	long long accountId = AccountId(test, "CAPSULE");

	int id = 0;
	std::string error;
	REQUIRE(repository.PutInCapsule(accountId, 0, &id, &error));
	REQUIRE(id > 0);

	STRUCT_ACCOUNTFILE afterSeal = LoadFile(test, repository, "CAPSULE");
	REQUIRE(afterSeal.Char[0].MobName[0] == '\0');
	REQUIRE(std::strcmp(afterSeal.Char[1].MobName, "KEEPER") == 0);

	storage::CharacterRecord capsule;
	bool found = false;
	REQUIRE(repository.LoadCapsule(id, &capsule, &found, &error));
	REQUIRE(found);
	REQUIRE(std::string(capsule.name.begin(), capsule.name.end()) == "SEALED");
	REQUIRE(capsule.equipment[0].index == 100);
	REQUIRE_FALSE(capsule.live);

	// A capsule cannot be restored into an occupied slot.
	REQUIRE_FALSE(repository.TakeOutCapsule(accountId, id, 1,
		std::vector<unsigned char>{'N', 'E', 'W'}, &error));

	REQUIRE(repository.TakeOutCapsule(accountId, id, 0,
		std::vector<unsigned char>{'R', 'E', 'S', 'T', 'O', 'R', 'E', 'D'},
		&error));

	STRUCT_ACCOUNTFILE restored = LoadFile(test, repository, "CAPSULE");
	REQUIRE(std::strcmp(restored.Char[0].MobName, "RESTORED") == 0);
	REQUIRE(restored.Char[0].Exp == file.Char[0].Exp);
	REQUIRE(restored.Char[0].Equip[0].sIndex == 100);

	REQUIRE(repository.LoadCapsule(id, &capsule, &found, &error));
	REQUIRE_FALSE(found);

	int next = 0;
	REQUIRE(repository.PutInCapsule(accountId, 1, &next, &error));
	REQUIRE(next > id);

	// Capsule data survives a fresh connection (restart persistence).
	storage::StorageConfig config = test.Config();
	config.bcrypt_cost = 10;
	storage::Database reopened;
	REQUIRE(reopened.Connect(config, &error));
	storage::AccountRepository second(reopened, config);
	REQUIRE(second.LoadCapsule(next, &capsule, &found, &error));
	REQUIRE(found);
	REQUIRE(std::string(capsule.name.begin(), capsule.name.end()) == "KEEPER");
}

TEST_CASE("login admission verifies hashes and persists clamps",
	"[db][repository][security]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE file = MakeAccount("LOGIN");
	AddCharacter(&file, 0, "PLAYER", 10);
	SaveNew(test, repository, file);

	long long accountId = AccountId(test, "LOGIN");
	std::string error;
	storage::AccountRecord record;

	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'L', 'O', 'G', 'I', 'N'},
		"hunter2", "", &record, nullptr, &error) == storage::LoginStatus::Success);
	REQUIRE(record.characters.size() == 1);

	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'L', 'O', 'G', 'I', 'N'},
		"wrong", "", &record, nullptr, &error) == storage::LoginStatus::WrongPassword);

	// The stored credential is a BCrypt hash, never the plaintext.
	storage::Result stored = test.Db().ExecParams(
		"SELECT password_hash FROM accounts WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(stored.Ok());
	REQUIRE(stored.Text(0, 0).rfind("$2b$", 0) == 0);
	REQUIRE(stored.Text(0, 0).find("hunter2") == std::string::npos);
	REQUIRE(security::PasswordHash::Verify("hunter2", stored.Text(0, 0))
		== security::VerifyResult::Match);

	// Negative coin is clamped and persisted by the admission transaction.
	test.Db().ExecParams("UPDATE accounts SET coin = -50 WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'L', 'O', 'G', 'I', 'N'},
		"hunter2", "", &record, nullptr, &error) == storage::LoginStatus::Success);
	REQUIRE(record.coin == 0);
	REQUIRE(LoadFile(test, repository, "LOGIN").Coin == 0);

	// A blocked account is refused before hashing.
	test.Db().ExecParams("UPDATE accounts SET state = 1 WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'L', 'O', 'G', 'I', 'N'},
		"hunter2", "", &record, nullptr, &error) == storage::LoginStatus::Blocked);
}

TEST_CASE("server-change token bypasses the password and is cleared",
	"[db][repository][security]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE file = MakeAccount("TEMPKEY");
	file.Info.NumericToken[0] = -1;
	SaveNew(test, repository, file);

	long long accountId = AccountId(test, "TEMPKEY");

	std::vector<unsigned char> token(52);
	for (int i = 0; i < 52; ++i)
		token[i] = static_cast<unsigned char>(i + 1);

	test.Db().ExecParams("UPDATE accounts SET temp_key = $1 WHERE account_id = $2",
		{storage::ParamBytes(token), storage::ParamInt64(accountId)}, false);

	std::string error;
	storage::AccountRecord record;
	const std::string tokenText(token.begin(), token.end());

	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'T', 'E', 'M', 'P', 'K', 'E', 'Y'},
		"not-the-password", tokenText, &record, nullptr, &error)
		== storage::LoginStatus::Success);

	storage::Result stored = test.Db().ExecParams(
		"SELECT temp_key FROM accounts WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, true);
	REQUIRE(stored.Ok());
	std::vector<unsigned char> cleared = stored.Bytes(0, 0);
	REQUIRE(cleared == std::vector<unsigned char>(52, 0));

	// A non-matching token clears the key without admitting the login.
	test.Db().ExecParams("UPDATE accounts SET temp_key = $1 WHERE account_id = $2",
		{storage::ParamBytes(token), storage::ParamInt64(accountId)}, false);

	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'T', 'E', 'M', 'P', 'K', 'E', 'Y'},
		"not-the-password", std::string(52, 'X'), &record, nullptr, &error)
		== storage::LoginStatus::TempKeyMismatch);
}

TEST_CASE("failed password attempts lock the account durably",
	"[db][repository][security]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE file = MakeAccount("LOCKED");
	SaveNew(test, repository, file);

	long long accountId = AccountId(test, "LOCKED");
	std::string error;
	storage::AccountRecord record;

	for (int i = 0; i < 4; ++i)
	{
		REQUIRE(repository.AdmitLogin(
			std::vector<unsigned char>{'L', 'O', 'C', 'K', 'E', 'D'},
			"bad", "", &record, nullptr, &error) == storage::LoginStatus::WrongPassword);
	}

	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'L', 'O', 'C', 'K', 'E', 'D'},
		"bad", "", &record, nullptr, &error) == storage::LoginStatus::WrongPassword);

	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'L', 'O', 'C', 'K', 'E', 'D'},
		"hunter2", "", &record, nullptr, &error) == storage::LoginStatus::Locked);

	// Lockout survives a new connection (DBSrv restart).
	storage::StorageConfig config = test.Config();
	config.bcrypt_cost = 10;
	storage::Database reopened;
	REQUIRE(reopened.Connect(config, &error));
	storage::AccountRepository second(reopened, config);
	REQUIRE(second.AdmitLogin(
		std::vector<unsigned char>{'L', 'O', 'C', 'K', 'E', 'D'},
		"hunter2", "", &record, nullptr, &error) == storage::LoginStatus::Locked);

	storage::Result events = test.Db().ExecParams(
		"SELECT count(*) FROM credential_events WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(events.Ok());
	REQUIRE(events.Int32(0, 0) >= 5);

	long long passwordFailures = 0;
	storage::Result failures = test.Db().ExecParams(
		"SELECT password_failures FROM accounts WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(failures.Ok());
	passwordFailures = failures.Int32(0, 0);
	REQUIRE(passwordFailures >= 5);

	// Once the window expires, the correct credential is accepted again and
	// the counter resets.
	test.Db().ExecParams(
		"UPDATE accounts SET password_locked_until = now() - interval '1 second' "
		"WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);

	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'L', 'O', 'C', 'K', 'E', 'D'},
		"hunter2", "", &record, nullptr, &error) == storage::LoginStatus::Success);

	storage::Result reset = test.Db().ExecParams(
		"SELECT password_failures FROM accounts WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(reset.Ok());
	REQUIRE(reset.Int32(0, 0) == 0);
}

TEST_CASE("low-cost hashes are upgraded on successful login",
	"[db][repository][security]")
{
	TestDatabase test;

	{
		storage::StorageConfig config = test.Config();
		config.bcrypt_cost = 10;
		storage::AccountRepository repository(test.Db(), config);

		STRUCT_ACCOUNTFILE file = MakeAccount("UPGRADE");
		SaveNew(test, repository, file);
	}

	long long accountId = AccountId(test, "UPGRADE");

	storage::Result before = test.Db().ExecParams(
		"SELECT password_hash FROM accounts WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(before.Ok());
	int cost = 0;
	REQUIRE(security::PasswordHash::HashCost(before.Text(0, 0), &cost));
	REQUIRE(cost == 10);

	storage::StorageConfig upgradedConfig = test.Config();
	upgradedConfig.bcrypt_cost = 11;
	storage::AccountRepository upgraded(test.Db(), upgradedConfig);

	std::string error;
	storage::AccountRecord record;
	REQUIRE(upgraded.AdmitLogin(
		std::vector<unsigned char>{'U', 'P', 'G', 'R', 'A', 'D', 'E'},
		"hunter2", "", &record, nullptr, &error) == storage::LoginStatus::Success);

	storage::Result after = test.Db().ExecParams(
		"SELECT password_hash FROM accounts WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(after.Ok());
	REQUIRE(security::PasswordHash::HashCost(after.Text(0, 0), &cost));
	REQUIRE(cost == 11);
}

TEST_CASE("password change and token binding use hashes",
	"[db][repository][security]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE file = MakeAccount("SECURE");
	SaveNew(test, repository, file);

	long long accountId = AccountId(test, "SECURE");
	std::string error;
	storage::AccountRecord record;

	REQUIRE(repository.ChangePassword(accountId, "newpass", &error));

	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'S', 'E', 'C', 'U', 'R', 'E'},
		"hunter2", "", &record, nullptr, &error) == storage::LoginStatus::WrongPassword);
	REQUIRE(repository.AdmitLogin(
		std::vector<unsigned char>{'S', 'E', 'C', 'U', 'R', 'E'},
		"newpass", "", &record, nullptr, &error) == storage::LoginStatus::Success);

	// First token binding, then verify/mismatch.
	REQUIRE(repository.BindToken(accountId, "1234", false, &error));
	REQUIRE_FALSE(repository.BindToken(accountId, "5678", false, &error));

	bool matched = false;
	REQUIRE(repository.VerifyToken(accountId, "1234", &matched, &error));
	REQUIRE(matched);

	REQUIRE(repository.VerifyToken(accountId, "9999", &matched, &error));
	REQUIRE_FALSE(matched);

	REQUIRE(repository.BindToken(accountId, "5678", true, &error));
	REQUIRE(repository.VerifyToken(accountId, "5678", &matched, &error));
	REQUIRE(matched);

	storage::Result stored = test.Db().ExecParams(
		"SELECT token_hash FROM accounts WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(stored.Ok());
	REQUIRE(stored.Text(0, 0).rfind("$2b$", 0) == 0);
	REQUIRE(stored.Text(0, 0).find("5678") == std::string::npos);
}

TEST_CASE("export keeps the 7808-byte layout with empty credentials",
	"[db][repository][export]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE file = MakeAccount("EXPORTED");
	AddCharacter(&file, 0, "EXPORTER", 15);
	file.Cargo[5].sIndex = 401;
	SaveNew(test, repository, file);

	STRUCT_ACCOUNTFILE exported;
	bool found = false;
	std::string error;

	REQUIRE(repository.ExportAccount(
		std::vector<unsigned char>{'E', 'X', 'P', 'O', 'R', 'T', 'E', 'D'},
		&exported, &found, &error));
	REQUIRE(found);
	REQUIRE(sizeof(exported) == 7808);

	for (unsigned char byte : exported.Info.AccountPass)
		REQUIRE(byte == 0);

	REQUIRE(exported.Info.NumericToken[0] == -1);
	REQUIRE(exported.Coin == file.Coin);
	REQUIRE(exported.Cargo[5].sIndex == 401);
	REQUIRE(std::strcmp(exported.Char[0].MobName, "EXPORTER") == 0);
	REQUIRE(exported.Char[0].Exp == file.Char[0].Exp);
	REQUIRE(exported.mobExtra[0].Fame == 100);
}

TEST_CASE("day-log and ranking queries follow the legacy selection",
	"[db][repository][queries]")
{
	TestDatabase test;
	storage::AccountRepository repository = MakeRepository(test);

	STRUCT_ACCOUNTFILE recent = MakeAccount("RECENT");
	AddCharacter(&recent, 0, "FRESH", 50);
	AddCharacter(&recent, 1, "ADMINLIKE", 1200);
	recent.mobExtra[0].DayLog.YearDay = 41;
	recent.mobExtra[0].DayLog.Exp = 4242;
	recent.mobExtra[1].DayLog.YearDay = 40;
	SaveNew(test, repository, recent);

	STRUCT_ACCOUNTFILE stale = MakeAccount("STALE");
	AddCharacter(&stale, 0, "OLDONE", 50);
	stale.mobExtra[0].DayLog.YearDay = 41;
	SaveNew(test, repository, stale);

	test.Db().ExecParams(
		"UPDATE accounts SET updated_at = now() - interval '2 months' "
		"WHERE account_id = $1",
		{storage::ParamInt64(AccountId(test, "STALE"))}, false);

	std::vector<storage::AccountRepository::DayLogEntry> dayLog;
	std::string error;

	REQUIRE(repository.DayLogEntries(41, &dayLog, &error));

	bool sawFresh = false;
	for (const auto &entry : dayLog)
	{
		REQUIRE(std::string(entry.account.begin(), entry.account.end()) != "STALE");
		if (std::string(entry.character.begin(), entry.character.end()) == "FRESH")
		{
			sawFresh = true;
			REQUIRE(entry.exp == 4242);
		}
		REQUIRE(std::string(entry.character.begin(), entry.character.end()) != "ADMINLIKE");
	}
	REQUIRE(sawFresh);

	std::vector<storage::AccountRepository::RankingEntry> ranking;
	REQUIRE(repository.RankingEntries(&ranking, &error));

	bool sawRanked = false;
	for (const auto &entry : ranking)
	{
		REQUIRE(std::string(entry.name.begin(), entry.name.end()) != "ADMINLIKE");
		if (std::string(entry.name.begin(), entry.name.end()) == "FRESH")
		{
			sawRanked = true;
			REQUIRE(entry.level == 50);
			REQUIRE(entry.exp == recent.Char[0].Exp);
		}
	}
	REQUIRE(sawRanked);
}
