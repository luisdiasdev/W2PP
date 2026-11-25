/*
 * Schema migration and constraint tests (tasks 2.1 - 2.3).
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "DbTestSupport.h"
#include "storage/Schema.h"

namespace
{

std::vector<unsigned char> Bytes(const char *text)
{
	return std::vector<unsigned char>(text, text + std::char_traits<char>::length(text));
}

std::vector<unsigned char> Zeros(std::size_t count)
{
	return std::vector<unsigned char>(count, 0);
}

long long InsertAccount(storage::Database &db, const std::string &name,
	const std::string &nameKey)
{
	storage::Result result = db.ExecParams(
		"INSERT INTO accounts (name, name_key, password_hash, temp_key, short_skill) "
		"VALUES ($1, $2, $3, $4, $5) RETURNING account_id",
		{
			storage::ParamBytes(Bytes(name.c_str())),
			storage::ParamBytes(Bytes(nameKey.c_str())),
			storage::ParamText("$2b$10$0123456789012345678901234567890123456789012345678901234567890"),
			storage::ParamBytes(Zeros(52)),
			storage::ParamBytes(Zeros(64)),
		}, true);

	REQUIRE(result.Ok());
	return result.Int64(0, 0);
}

long long InsertCharacter(storage::Database &db, long long accountId, int slot,
	const std::string &name, const std::string &nameKey)
{
	storage::Result result = db.ExecParams(
		"INSERT INTO characters (account_id, slot, name, name_key, skill_bar) "
		"VALUES ($1, $2, $3, $4, $5) RETURNING character_id",
		{
			storage::ParamInt64(accountId),
			storage::ParamInt(slot),
			storage::ParamBytes(Bytes(name.c_str())),
			storage::ParamBytes(Bytes(nameKey.c_str())),
			storage::ParamBytes(Zeros(4)),
		}, true);

	REQUIRE(result.Ok());
	return result.Int64(0, 0);
}

} // namespace

TEST_CASE("fresh database migrates to version 1", "[db][schema]")
{
	TestDatabase test;

	REQUIRE(test.Version() == 1);

	storage::Result version = test.Db().Exec("SELECT version FROM schema_migration");
	REQUIRE(version.Ok());
	REQUIRE(version.Rows() == 1);
	REQUIRE(version.Int32(0, 0) == 1);

	const char *expected[] = {
		"schema_migration",
		"store_metadata",
		"accounts",
		"characters",
		"character_equipment",
		"character_inventory",
		"account_cargo",
		"character_affects",
		"character_extras",
		"character_celestial_saves",
		"credential_events",
	};

	for (const char *table : expected)
	{
		storage::Result found = test.Db().ExecParams(
			"SELECT count(*) FROM information_schema.tables "
			"WHERE table_schema = 'w2pp' AND table_name = $1",
			{storage::ParamText(table)}, false);

		REQUIRE(found.Ok());
		REQUIRE(found.Int32(0, 0) == 1);
	}

	storage::Result contract = test.Db().ExecParams(
		"SELECT value FROM store_metadata WHERE key = 'schema_contract'", {}, false);
	REQUIRE(contract.Ok());
	REQUIRE(contract.Text(0, 0) == "1");
}

TEST_CASE("account name key is unique", "[db][schema][constraints]")
{
	TestDatabase test;

	InsertAccount(test.Db(), "Alpha", "ALPHA");

	storage::Result duplicate = test.Db().ExecParams(
		"INSERT INTO accounts (name, name_key, password_hash, temp_key, short_skill) "
		"VALUES ($1, $2, $3, $4, $5)",
		{
			storage::ParamBytes(Bytes("Alpha2")),
			storage::ParamBytes(Bytes("ALPHA")),
			storage::ParamText("hash"),
			storage::ParamBytes(Zeros(52)),
			storage::ParamBytes(Zeros(64)),
		}, false);

	REQUIRE_FALSE(duplicate.Ok());
	REQUIRE(duplicate.SqlState() == "23505");
}

TEST_CASE("account byte widths and state range are enforced", "[db][schema][constraints]")
{
	TestDatabase test;

	storage::Result wide = test.Db().ExecParams(
		"INSERT INTO accounts (name, name_key, password_hash, temp_key, short_skill) "
		"VALUES ($1, $2, $3, $4, $5)",
		{
			storage::ParamBytes(Bytes("ABCDEFGHIJKLMNOPQ")),
			storage::ParamBytes(Bytes("ABCDEFGHIJKLMNOPQ")),
			storage::ParamText("hash"),
			storage::ParamBytes(Zeros(52)),
			storage::ParamBytes(Zeros(64)),
		}, false);

	REQUIRE_FALSE(wide.Ok());
	REQUIRE(wide.SqlState() == "23514");

	storage::Result shortTempKey = test.Db().ExecParams(
		"INSERT INTO accounts (name, name_key, password_hash, temp_key, short_skill) "
		"VALUES ($1, $2, $3, $4, $5)",
		{
			storage::ParamBytes(Bytes("B")),
			storage::ParamBytes(Bytes("B")),
			storage::ParamText("hash"),
			storage::ParamBytes(Zeros(51)),
			storage::ParamBytes(Zeros(64)),
		}, false);

	REQUIRE_FALSE(shortTempKey.Ok());
	REQUIRE(shortTempKey.SqlState() == "23514");
}

TEST_CASE("character names are unique across accounts", "[db][schema][constraints]")
{
	TestDatabase test;

	long long first = InsertAccount(test.Db(), "A", "A");
	long long second = InsertAccount(test.Db(), "B", "B");

	InsertCharacter(test.Db(), first, 0, "Knight", "KNIGHT");

	storage::Result duplicate = test.Db().ExecParams(
		"INSERT INTO characters (account_id, slot, name, name_key, skill_bar) "
		"VALUES ($1, $2, $3, $4, $5)",
		{
			storage::ParamInt64(second),
			storage::ParamInt(0),
			storage::ParamBytes(Bytes("knight")),
			storage::ParamBytes(Bytes("KNIGHT")),
			storage::ParamBytes(Zeros(4)),
		}, false);

	REQUIRE_FALSE(duplicate.Ok());
	REQUIRE(duplicate.SqlState() == "23505");
}

TEST_CASE("a live account slot is unique", "[db][schema][constraints]")
{
	TestDatabase test;

	long long account = InsertAccount(test.Db(), "C", "C");

	InsertCharacter(test.Db(), account, 0, "One", "ONE");

	storage::Result outOfRange = test.Db().ExecParams(
		"INSERT INTO characters (account_id, slot, name, name_key, skill_bar) "
		"VALUES ($1, $2, $3, $4, $5)",
		{
			storage::ParamInt64(account),
			storage::ParamInt(4),
			storage::ParamBytes(Bytes("Four")),
			storage::ParamBytes(Bytes("FOUR")),
			storage::ParamBytes(Zeros(4)),
		}, false);

	REQUIRE_FALSE(outOfRange.Ok());
	REQUIRE(outOfRange.SqlState() == "23514");

	storage::Result duplicate = test.Db().ExecParams(
		"INSERT INTO characters (account_id, slot, name, name_key, skill_bar) "
		"VALUES ($1, $2, $3, $4, $5)",
		{
			storage::ParamInt64(account),
			storage::ParamInt(0),
			storage::ParamBytes(Bytes("Two")),
			storage::ParamBytes(Bytes("TWO")),
			storage::ParamBytes(Zeros(4)),
		}, false);

	REQUIRE_FALSE(duplicate.Ok());
	REQUIRE(duplicate.SqlState() == "23505");
}

TEST_CASE("character owner columns are exclusive", "[db][schema][constraints]")
{
	TestDatabase test;

	long long account = InsertAccount(test.Db(), "D", "D");

	storage::Result both = test.Db().ExecParams(
		"INSERT INTO characters (account_id, slot, capsule_id, name, name_key, skill_bar) "
		"VALUES ($1, $2, $3, $4, $5, $6)",
		{
			storage::ParamInt64(account),
			storage::ParamInt(0),
			storage::ParamInt(7),
			storage::ParamBytes(Bytes("Both")),
			storage::ParamBytes(Bytes("BOTH")),
			storage::ParamBytes(Zeros(4)),
		}, false);

	REQUIRE_FALSE(both.Ok());
	REQUIRE(both.SqlState() == "23514");

	storage::Result neither = test.Db().ExecParams(
		"INSERT INTO characters (name, name_key, skill_bar) VALUES ($1, $2, $3)",
		{
			storage::ParamBytes(Bytes("Neither")),
			storage::ParamBytes(Bytes("NEITHER")),
			storage::ParamBytes(Zeros(4)),
		}, false);

	REQUIRE_FALSE(neither.Ok());
	REQUIRE(neither.SqlState() == "23514");
}

TEST_CASE("array positions are range checked", "[db][schema][constraints]")
{
	TestDatabase test;

	long long account = InsertAccount(test.Db(), "E", "E");
	long long character = InsertCharacter(test.Db(), account, 0, "Five", "FIVE");

	storage::Result equipment = test.Db().ExecParams(
		"INSERT INTO character_equipment (character_id, position, item_index, effect0, effect1, effect2) "
		"VALUES ($1, $2, 1, 0, 0, 0)",
		{
			storage::ParamInt64(character),
			storage::ParamInt(16),
		}, false);

	REQUIRE_FALSE(equipment.Ok());
	REQUIRE(equipment.SqlState() == "23514");

	storage::Result cargo = test.Db().ExecParams(
		"INSERT INTO account_cargo (account_id, position, item_index, effect0, effect1, effect2) "
		"VALUES ($1, $2, 1, 0, 0, 0)",
		{
			storage::ParamInt64(account),
			storage::ParamInt(128),
		}, false);

	REQUIRE_FALSE(cargo.Ok());
	REQUIRE(cargo.SqlState() == "23514");

	storage::Result affect = test.Db().ExecParams(
		"INSERT INTO character_affects (character_id, position, type, value, level, time) "
		"VALUES ($1, $2, 1, 0, 0, 0)",
		{
			storage::ParamInt64(character),
			storage::ParamInt(32),
		}, false);

	REQUIRE_FALSE(affect.Ok());
	REQUIRE(affect.SqlState() == "23514");
}

TEST_CASE("capsule identifiers are unique and capped", "[db][schema][constraints]")
{
	TestDatabase test;

	storage::Result created = test.Db().ExecParams(
		"INSERT INTO characters (capsule_id, name, name_key, skill_bar) "
		"VALUES ($1, $2, $3, $4) RETURNING character_id",
		{
			storage::ParamInt(10),
			storage::ParamBytes(Bytes("Capsule")),
			storage::ParamBytes(Bytes("CAPSULE")),
			storage::ParamBytes(Zeros(4)),
		}, true);

	REQUIRE(created.Ok());

	storage::Result duplicate = test.Db().ExecParams(
		"INSERT INTO characters (capsule_id, name, name_key, skill_bar) "
		"VALUES ($1, $2, $3, $4)",
		{
			storage::ParamInt(10),
			storage::ParamBytes(Bytes("Capsule2")),
			storage::ParamBytes(Bytes("CAPSULE2")),
			storage::ParamBytes(Zeros(4)),
		}, false);

	REQUIRE_FALSE(duplicate.Ok());
	REQUIRE(duplicate.SqlState() == "23505");

	storage::Result tooLarge = test.Db().ExecParams(
		"INSERT INTO characters (capsule_id, name, name_key, skill_bar) "
		"VALUES ($1, $2, $3, $4)",
		{
			storage::ParamInt(65536),
			storage::ParamBytes(Bytes("Capsule3")),
			storage::ParamBytes(Bytes("CAPSULE3")),
			storage::ParamBytes(Zeros(4)),
		}, false);

	REQUIRE_FALSE(tooLarge.Ok());
	REQUIRE(tooLarge.SqlState() == "23514");
}

TEST_CASE("a failing migration keeps the previous version", "[db][schema][migration]")
{
	TestDatabase test;

	std::vector<storage::MigrationStep> steps = storage::DefaultMigrations();
	steps.push_back({2, "broken step",
		"CREATE TABLE migration_probe (id integer); "
		"INSERT INTO table_that_does_not_exist (id) VALUES (1);"});

	std::string error;
	int version = 0;

	REQUIRE_FALSE(storage::ApplyMigrations(test.Db(), steps, &error, &version));
	REQUIRE(error.find("migration 2 (broken step)") != std::string::npos);
	REQUIRE(storage::CurrentVersion(test.Db(), &error) == 1);

	storage::Result probe = test.Db().Exec(
		"SELECT count(*) FROM information_schema.tables "
		"WHERE table_schema = 'w2pp' AND table_name = 'migration_probe'");
	REQUIRE(probe.Ok());
	REQUIRE(probe.Int32(0, 0) == 0);
}
