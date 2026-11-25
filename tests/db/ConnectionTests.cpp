/*
 * Connection/transaction behavior tests (task 4.1) and capsule rollback
 * (task 4.5).
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "Basedef.h"
#include "DbTestSupport.h"
#include "storage/AccountRepository.h"

TEST_CASE("connection failure names the endpoint", "[db][connection]")
{
	storage::StorageConfig config;
	std::string error;

	REQUIRE(storage::LoadStorageConfig(nullptr, &config, &error));

	config.host = "127.0.0.1";
	config.port = 1;
	config.connect_timeout_seconds = 2;

	storage::Database database;
	REQUIRE_FALSE(database.Connect(config, &error));
	REQUIRE(error.find("127.0.0.1:1") != std::string::npos);
	REQUIRE_FALSE(database.IsOpen());
}

TEST_CASE("an explicit rollback leaves no rows", "[db][connection]")
{
	TestDatabase test;

	storage::Transaction transaction(test.Db());
	std::string error;
	REQUIRE(transaction.Begin(&error, "SERIALIZABLE"));

	storage::Result inserted = test.Db().ExecParams(
		"INSERT INTO accounts (name, name_key, password_hash, temp_key, short_skill) "
		"VALUES ($1, $2, $3, $4, $5)",
		{
			storage::ParamBytes({65}),
			storage::ParamBytes({65}),
			storage::ParamText("hash"),
			storage::ParamBytes(std::vector<unsigned char>(52, 0)),
			storage::ParamBytes(std::vector<unsigned char>(64, 0)),
		}, false);
	REQUIRE(inserted.Ok());

	transaction.Rollback();

	storage::Result count = test.Db().Exec("SELECT count(*) FROM accounts");
	REQUIRE(count.Ok());
	REQUIRE(count.Int32(0, 0) == 0);
}

TEST_CASE("statement timeout aborts a slow query", "[db][connection]")
{
	TestDatabase test;

	storage::StorageConfig config = test.Config();
	config.statement_timeout_ms = 100;

	storage::Database database;
	std::string error;
	REQUIRE(database.Connect(config, &error));

	storage::Result slow = database.Exec("SELECT pg_sleep(1)");
	REQUIRE_FALSE(slow.Ok());
	REQUIRE(slow.SqlState() == "57014");
}

TEST_CASE("capsule put-in rolls back when clearing the slot fails",
	"[db][capsule]")
{
	TestDatabase test;

	// Fail every account short-skill update after the capsule row would move.
	test.Db().Exec(
		"CREATE FUNCTION fail_short_skill() RETURNS trigger AS $$ "
		"BEGIN RAISE EXCEPTION 'short skill write refused'; END; "
		"$$ LANGUAGE plpgsql");
	test.Db().Exec(
		"CREATE TRIGGER fail_short_skill BEFORE UPDATE OF short_skill "
		"ON accounts FOR EACH ROW EXECUTE FUNCTION fail_short_skill()");

	storage::StorageConfig config = test.Config();
	config.bcrypt_cost = 10;
	storage::AccountRepository repository(test.Db(), config);

	STRUCT_ACCOUNTFILE file;
	std::memset(&file, 0, sizeof(file));
	std::strncpy(file.Info.AccountName, "ROLLBACK", ACCOUNTNAME_LENGTH);
	std::strncpy(file.Info.AccountPass, "hunter2", ACCOUNTPASS_LENGTH);
	file.Info.NumericToken[0] = -1;
	std::strncpy(file.Char[0].MobName, "STAYS", NAME_LENGTH);
	file.Char[0].Equip[0].sIndex = 42;

	std::string error;
	bool inserted = false;
	REQUIRE(repository.SaveAccountFile(file, true, &inserted, &error));

	long long accountId = 0;
	storage::Result id = test.Db().Exec(
		"SELECT account_id FROM accounts WHERE name = 'ROLLBACK'");
	REQUIRE(id.Ok());
	accountId = id.Int64(0, 0);

	int capsuleId = 0;
	REQUIRE_FALSE(repository.PutInCapsule(accountId, 0, &capsuleId, &error));

	storage::Result live = test.Db().ExecParams(
		"SELECT count(*) FROM characters WHERE account_id = $1",
		{storage::ParamInt64(accountId)}, false);
	REQUIRE(live.Ok());
	REQUIRE(live.Int32(0, 0) == 1);

	storage::Result capsules = test.Db().Exec(
		"SELECT count(*) FROM characters WHERE capsule_id IS NOT NULL");
	REQUIRE(capsules.Ok());
	REQUIRE(capsules.Int32(0, 0) == 0);
}
