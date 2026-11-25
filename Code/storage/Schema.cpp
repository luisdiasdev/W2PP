#include "Schema.h"

#include <cstdlib>
#include <string>

namespace storage
{
namespace
{

const char *kBootstrapSql = R"sql(
CREATE TABLE IF NOT EXISTS schema_migration (
	version integer PRIMARY KEY,
	applied_at timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE IF NOT EXISTS store_metadata (
	key text PRIMARY KEY,
	value text NOT NULL
);

INSERT INTO store_metadata (key, value) VALUES
	('schema_contract', '1'),
	('import_state', 'pending')
ON CONFLICT (key) DO NOTHING;
)sql";

const char *kVersionOneSql = R"sql(
CREATE TABLE accounts (
	account_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	name bytea NOT NULL,
	name_key bytea NOT NULL,
	password_hash text NOT NULL,
	token_hash text,
	state smallint NOT NULL DEFAULT 0,
	real_name bytea NOT NULL DEFAULT '\x'::bytea,
	ssn1 integer NOT NULL DEFAULT 0,
	ssn2 integer NOT NULL DEFAULT 0,
	email bytea NOT NULL DEFAULT '\x'::bytea,
	telephone bytea NOT NULL DEFAULT '\x'::bytea,
	address bytea NOT NULL DEFAULT '\x'::bytea,
	block_year integer NOT NULL DEFAULT 0,
	block_year_day integer NOT NULL DEFAULT 0,
	temp_key bytea NOT NULL DEFAULT '\x'::bytea,
	coin integer NOT NULL DEFAULT 0,
	donate integer NOT NULL DEFAULT 0,
	short_skill bytea NOT NULL DEFAULT '\x'::bytea,
	password_failures integer NOT NULL DEFAULT 0,
	password_locked_until timestamptz,
	token_failures integer NOT NULL DEFAULT 0,
	token_locked_until timestamptz,
	created_at timestamptz NOT NULL DEFAULT now(),
	updated_at timestamptz NOT NULL DEFAULT now(),
	CONSTRAINT accounts_name_width CHECK (octet_length(name) <= 16),
	CONSTRAINT accounts_name_key_width CHECK (octet_length(name_key) <= 16),
	CONSTRAINT accounts_name_key_unique UNIQUE (name_key),
	CONSTRAINT accounts_state_range CHECK (state BETWEEN 0 AND 3),
	CONSTRAINT accounts_real_name_width CHECK (octet_length(real_name) <= 24),
	CONSTRAINT accounts_email_width CHECK (octet_length(email) <= 48),
	CONSTRAINT accounts_telephone_width CHECK (octet_length(telephone) <= 16),
	CONSTRAINT accounts_address_width CHECK (octet_length(address) <= 78),
	CONSTRAINT accounts_temp_key_width CHECK (octet_length(temp_key) = 52),
	CONSTRAINT accounts_short_skill_width CHECK (octet_length(short_skill) = 64)
);

CREATE INDEX accounts_updated_at_idx ON accounts (updated_at);

CREATE TABLE characters (
	character_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	account_id bigint REFERENCES accounts (account_id) ON DELETE CASCADE,
	slot smallint,
	capsule_id integer,
	name bytea NOT NULL,
	name_key bytea NOT NULL,
	clan smallint NOT NULL DEFAULT 0,
	merchant smallint NOT NULL DEFAULT 0,
	guild integer NOT NULL DEFAULT 0,
	class smallint NOT NULL DEFAULT 0,
	rsv integer NOT NULL DEFAULT 0,
	quest smallint NOT NULL DEFAULT 0,
	coin integer NOT NULL DEFAULT 0,
	exp bigint NOT NULL DEFAULT 0,
	spx smallint NOT NULL DEFAULT 0,
	spy smallint NOT NULL DEFAULT 0,
	learned_skill integer NOT NULL DEFAULT 0,
	magic bigint NOT NULL DEFAULT 0,
	score_bonus integer NOT NULL DEFAULT 0,
	special_bonus integer NOT NULL DEFAULT 0,
	skill_bonus integer NOT NULL DEFAULT 0,
	critical smallint NOT NULL DEFAULT 0,
	save_mana smallint NOT NULL DEFAULT 0,
	skill_bar bytea NOT NULL DEFAULT '\x'::bytea,
	guild_level smallint NOT NULL DEFAULT 0,
	regen_hp integer NOT NULL DEFAULT 0,
	regen_mp integer NOT NULL DEFAULT 0,
	resist0 smallint NOT NULL DEFAULT 0,
	resist1 smallint NOT NULL DEFAULT 0,
	resist2 smallint NOT NULL DEFAULT 0,
	resist3 smallint NOT NULL DEFAULT 0,
	base_level integer NOT NULL DEFAULT 0,
	base_ac integer NOT NULL DEFAULT 0,
	base_damage integer NOT NULL DEFAULT 0,
	base_merchant smallint NOT NULL DEFAULT 0,
	base_attack_run smallint NOT NULL DEFAULT 0,
	base_direction smallint NOT NULL DEFAULT 0,
	base_chaos_rate smallint NOT NULL DEFAULT 0,
	base_max_hp integer NOT NULL DEFAULT 0,
	base_max_mp integer NOT NULL DEFAULT 0,
	base_hp integer NOT NULL DEFAULT 0,
	base_mp integer NOT NULL DEFAULT 0,
	base_str smallint NOT NULL DEFAULT 0,
	base_int smallint NOT NULL DEFAULT 0,
	base_dex smallint NOT NULL DEFAULT 0,
	base_con smallint NOT NULL DEFAULT 0,
	base_special0 smallint NOT NULL DEFAULT 0,
	base_special1 smallint NOT NULL DEFAULT 0,
	base_special2 smallint NOT NULL DEFAULT 0,
	base_special3 smallint NOT NULL DEFAULT 0,
	current_level integer NOT NULL DEFAULT 0,
	current_ac integer NOT NULL DEFAULT 0,
	current_damage integer NOT NULL DEFAULT 0,
	current_merchant smallint NOT NULL DEFAULT 0,
	current_attack_run smallint NOT NULL DEFAULT 0,
	current_direction smallint NOT NULL DEFAULT 0,
	current_chaos_rate smallint NOT NULL DEFAULT 0,
	current_max_hp integer NOT NULL DEFAULT 0,
	current_max_mp integer NOT NULL DEFAULT 0,
	current_hp integer NOT NULL DEFAULT 0,
	current_mp integer NOT NULL DEFAULT 0,
	current_str smallint NOT NULL DEFAULT 0,
	current_int smallint NOT NULL DEFAULT 0,
	current_dex smallint NOT NULL DEFAULT 0,
	current_con smallint NOT NULL DEFAULT 0,
	current_special0 smallint NOT NULL DEFAULT 0,
	current_special1 smallint NOT NULL DEFAULT 0,
	current_special2 smallint NOT NULL DEFAULT 0,
	current_special3 smallint NOT NULL DEFAULT 0,
	CONSTRAINT characters_name_width CHECK (octet_length(name) <= 16),
	CONSTRAINT characters_name_key_width CHECK (octet_length(name_key) <= 16),
	CONSTRAINT characters_skill_bar_width CHECK (octet_length(skill_bar) = 4),
	CONSTRAINT characters_slot_range CHECK (slot IS NULL OR (slot >= 0 AND slot < 4)),
	CONSTRAINT characters_capsule_range CHECK (capsule_id IS NULL OR (capsule_id > 0 AND capsule_id <= 65535)),
	CONSTRAINT characters_owner_exclusive CHECK (
		(account_id IS NOT NULL AND slot IS NOT NULL AND capsule_id IS NULL)
		OR (account_id IS NULL AND slot IS NULL AND capsule_id IS NOT NULL))
);

CREATE UNIQUE INDEX characters_name_key_key ON characters (name_key);
CREATE UNIQUE INDEX characters_live_slot_key ON characters (account_id, slot)
	WHERE account_id IS NOT NULL;
CREATE UNIQUE INDEX characters_capsule_id_key ON characters (capsule_id)
	WHERE capsule_id IS NOT NULL;
CREATE INDEX characters_account_id_idx ON characters (account_id);

CREATE TABLE character_equipment (
	character_id bigint NOT NULL REFERENCES characters (character_id) ON DELETE CASCADE,
	position smallint NOT NULL,
	item_index smallint NOT NULL,
	effect0 smallint NOT NULL,
	effect1 smallint NOT NULL,
	effect2 smallint NOT NULL,
	PRIMARY KEY (character_id, position),
	CONSTRAINT character_equipment_position_range CHECK (position >= 0 AND position < 16)
);

CREATE TABLE character_inventory (
	character_id bigint NOT NULL REFERENCES characters (character_id) ON DELETE CASCADE,
	position smallint NOT NULL,
	item_index smallint NOT NULL,
	effect0 smallint NOT NULL,
	effect1 smallint NOT NULL,
	effect2 smallint NOT NULL,
	PRIMARY KEY (character_id, position),
	CONSTRAINT character_inventory_position_range CHECK (position >= 0 AND position < 64)
);

CREATE TABLE account_cargo (
	account_id bigint NOT NULL REFERENCES accounts (account_id) ON DELETE CASCADE,
	position smallint NOT NULL,
	item_index smallint NOT NULL,
	effect0 smallint NOT NULL,
	effect1 smallint NOT NULL,
	effect2 smallint NOT NULL,
	PRIMARY KEY (account_id, position),
	CONSTRAINT account_cargo_position_range CHECK (position >= 0 AND position < 128)
);

CREATE TABLE character_affects (
	character_id bigint NOT NULL REFERENCES characters (character_id) ON DELETE CASCADE,
	position smallint NOT NULL,
	type smallint NOT NULL,
	value smallint NOT NULL,
	level integer NOT NULL,
	time bigint NOT NULL,
	PRIMARY KEY (character_id, position),
	CONSTRAINT character_affects_position_range CHECK (position >= 0 AND position < 32),
	CONSTRAINT character_affects_type_range CHECK (type >= 0 AND type <= 255),
	CONSTRAINT character_affects_value_range CHECK (value >= -128 AND value <= 255),
	CONSTRAINT character_affects_level_range CHECK (level >= 0 AND level <= 65535),
	CONSTRAINT character_affects_time_range CHECK (time >= 0 AND time <= 4294967295)
);

CREATE TABLE character_extras (
	character_id bigint PRIMARY KEY REFERENCES characters (character_id) ON DELETE CASCADE,
	class_master smallint NOT NULL DEFAULT 0,
	citizen smallint NOT NULL DEFAULT 0,
	fame integer NOT NULL DEFAULT 0,
	soul smallint NOT NULL DEFAULT 0,
	mortal_face smallint NOT NULL DEFAULT 0,
	quest_newbie smallint NOT NULL DEFAULT 0,
	quest_terra_mistica smallint NOT NULL DEFAULT 0,
	quest_molar_gargula smallint NOT NULL DEFAULT 0,
	quest_pilula_orc smallint NOT NULL DEFAULT 0,
	arch_mortal_slot smallint NOT NULL DEFAULT 0,
	arch_mortal_level smallint NOT NULL DEFAULT 0,
	arch_level_355 smallint NOT NULL DEFAULT 0,
	arch_level_370 smallint NOT NULL DEFAULT 0,
	arch_cristal smallint NOT NULL DEFAULT 0,
	celestial_arch_level smallint NOT NULL DEFAULT 0,
	celestial_celestial_level smallint NOT NULL DEFAULT 0,
	celestial_sub_celestial_level smallint NOT NULL DEFAULT 0,
	celestial_lv40 smallint NOT NULL DEFAULT 0,
	celestial_lv90 smallint NOT NULL DEFAULT 0,
	celestial_add120 smallint NOT NULL DEFAULT 0,
	celestial_add150 smallint NOT NULL DEFAULT 0,
	celestial_add180 smallint NOT NULL DEFAULT 0,
	celestial_add200 smallint NOT NULL DEFAULT 0,
	celestial_arcana smallint NOT NULL DEFAULT 0,
	celestial_reset smallint NOT NULL DEFAULT 0,
	circle smallint NOT NULL DEFAULT 0,
	last_nt bigint NOT NULL DEFAULT 0,
	nt integer NOT NULL DEFAULT 0,
	kefra_ticket integer NOT NULL DEFAULT 0,
	divine_end bigint NOT NULL DEFAULT 0,
	hold bigint NOT NULL DEFAULT 0,
	daylog_exp bigint NOT NULL DEFAULT 0,
	daylog_year_day integer NOT NULL DEFAULT 0,
	donate_last_time bigint NOT NULL DEFAULT 0,
	donate_count integer NOT NULL DEFAULT 0
);

CREATE TABLE character_celestial_saves (
	character_id bigint NOT NULL REFERENCES characters (character_id) ON DELETE CASCADE,
	save_slot smallint NOT NULL,
	class smallint NOT NULL DEFAULT 0,
	exp bigint NOT NULL DEFAULT 0,
	spx smallint NOT NULL DEFAULT 0,
	spy smallint NOT NULL DEFAULT 0,
	base_level integer NOT NULL DEFAULT 0,
	base_ac integer NOT NULL DEFAULT 0,
	base_damage integer NOT NULL DEFAULT 0,
	base_merchant smallint NOT NULL DEFAULT 0,
	base_attack_run smallint NOT NULL DEFAULT 0,
	base_direction smallint NOT NULL DEFAULT 0,
	base_chaos_rate smallint NOT NULL DEFAULT 0,
	base_max_hp integer NOT NULL DEFAULT 0,
	base_max_mp integer NOT NULL DEFAULT 0,
	base_hp integer NOT NULL DEFAULT 0,
	base_mp integer NOT NULL DEFAULT 0,
	base_str smallint NOT NULL DEFAULT 0,
	base_int smallint NOT NULL DEFAULT 0,
	base_dex smallint NOT NULL DEFAULT 0,
	base_con smallint NOT NULL DEFAULT 0,
	base_special0 smallint NOT NULL DEFAULT 0,
	base_special1 smallint NOT NULL DEFAULT 0,
	base_special2 smallint NOT NULL DEFAULT 0,
	base_special3 smallint NOT NULL DEFAULT 0,
	learned_skill integer NOT NULL DEFAULT 0,
	score_bonus integer NOT NULL DEFAULT 0,
	special_bonus integer NOT NULL DEFAULT 0,
	skill_bonus integer NOT NULL DEFAULT 0,
	skill_bar1 bytea NOT NULL DEFAULT '\x'::bytea,
	skill_bar2 bytea NOT NULL DEFAULT '\x'::bytea,
	soul smallint NOT NULL DEFAULT 0,
	PRIMARY KEY (character_id, save_slot),
	CONSTRAINT character_celestial_slots CHECK (save_slot >= 0 AND save_slot < 2),
	CONSTRAINT character_celestial_bar1_width CHECK (octet_length(skill_bar1) = 4),
	CONSTRAINT character_celestial_bar2_width CHECK (octet_length(skill_bar2) = 16)
);

CREATE TABLE credential_events (
	event_id bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
	account_id bigint NOT NULL REFERENCES accounts (account_id) ON DELETE CASCADE,
	credential_kind smallint NOT NULL,
	occurred_at timestamptz NOT NULL DEFAULT now(),
	source text NOT NULL DEFAULT '',
	CONSTRAINT credential_events_kind CHECK (credential_kind IN (1, 2))
);

CREATE INDEX credential_events_account_idx ON credential_events (account_id, occurred_at);

CREATE SEQUENCE capsule_id_seq AS integer MINVALUE 1 MAXVALUE 65535 START WITH 1;
)sql";

} // namespace

const std::vector<MigrationStep> &DefaultMigrations()
{
	static const std::vector<MigrationStep> steps = {
		{1, "initial schema", kVersionOneSql},
	};

	return steps;
}

bool GetMetadata(Database &database, const std::string &key,
	std::string *value, std::string *error)
{
	Result result = database.ExecParams(
		"SELECT value FROM store_metadata WHERE key = $1",
		{ParamText(key)}, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "cannot read store_metadata." + key + ": " + result.Error();
		return false;
	}

	if (result.Rows() == 0)
	{
		if (value != nullptr)
			value->clear();
		return true;
	}

	if (value != nullptr)
		*value = result.Text(0, 0);

	return true;
}

bool SetMetadata(Database &database, const std::string &key,
	const std::string &value, std::string *error)
{
	Result result = database.ExecParams(
		"INSERT INTO store_metadata (key, value) VALUES ($1, $2) "
		"ON CONFLICT (key) DO UPDATE SET value = EXCLUDED.value",
		{ParamText(key), ParamText(value)}, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "cannot write store_metadata." + key + ": " + result.Error();
		return false;
	}

	return true;
}

int CurrentVersion(Database &database, std::string *error)
{
	Result result = database.Exec("SELECT COALESCE(MAX(version), 0) FROM schema_migration");

	if (!result.Ok())
	{
		if (result.SqlState() == "42P01") // undefined_table: not migrated yet
			return 0;

		if (error != nullptr)
			*error = "cannot read schema_migration: " + result.Error();
		return -1;
	}

	return result.Int32(0, 0);
}

bool CheckSchemaContract(Database &database, std::string *error)
{
	std::string text;

	if (!GetMetadata(database, "schema_contract", &text, error))
		return false;

	if (text.empty())
		return true; // fresh store; the migration runner records it

	int contract = std::atoi(text.c_str());

	if (contract > StorageConfig::kSchemaContract)
	{
		if (error != nullptr)
		{
			*error = "database schema contract is " + std::to_string(contract)
				+ " but this DBSrv understands " + std::to_string(StorageConfig::kSchemaContract)
				+ "; upgrade the DBSrv binary before starting it";
		}
		return false;
	}

	return true;
}

bool ApplyMigrations(Database &database,
	const std::vector<MigrationStep> &steps,
	std::string *error, int *appliedVersion)
{
	Result lock = database.ExecParams("SELECT pg_advisory_lock($1)",
		{ParamInt64(kMigrationLockKey)}, false);

	if (!lock.Ok())
	{
		if (error != nullptr)
			*error = "cannot acquire the migration advisory lock: " + lock.Error();
		return false;
	}

	bool ok = true;

	{
		Result schema = database.Exec("CREATE SCHEMA IF NOT EXISTS w2pp");
		Result path = database.Exec("SET search_path TO w2pp, public");
		Result bootstrap = database.Exec(kBootstrapSql);

		if (!schema.Ok() || !path.Ok() || !bootstrap.Ok())
		{
			if (error != nullptr)
				*error = "cannot initialize the store schema: "
					+ (schema.Ok() ? (path.Ok() ? bootstrap.Error() : path.Error()) : schema.Error());
			ok = false;
		}
	}

	int version = ok ? CurrentVersion(database, error) : -1;

	if (version < 0)
		ok = false;

	if (ok && !CheckSchemaContract(database, error))
		ok = false;

	for (const MigrationStep &step : steps)
	{
		if (!ok || step.version <= version)
			continue;

		Transaction transaction(database);
		std::string beginError;

		if (!transaction.Begin(&beginError, "SERIALIZABLE"))
		{
			if (error != nullptr)
				*error = "migration " + std::to_string(step.version) + " ("
					+ step.name + ") failed to start: " + beginError;
			ok = false;
			break;
		}

		Result result = database.Exec(step.sql);

		if (!result.Ok())
		{
			transaction.Rollback();

			if (error != nullptr)
				*error = "migration " + std::to_string(step.version) + " ("
					+ step.name + ") failed at step: " + result.Error();
			ok = false;
			break;
		}

		Result record = database.ExecParams(
			"INSERT INTO schema_migration (version) VALUES ($1)",
			{ParamInt(step.version)}, false);

		if (!record.Ok())
		{
			transaction.Rollback();

			if (error != nullptr)
				*error = "migration " + std::to_string(step.version) + " ("
					+ step.name + ") could not record its version: " + record.Error();
			ok = false;
			break;
		}

		if (!transaction.Commit(error))
		{
			ok = false;
			break;
		}

		version = step.version;
	}

	database.Exec("SELECT pg_advisory_unlock(" + std::to_string(kMigrationLockKey) + ")");

	if (ok)
	{
		if (!CheckSchemaContract(database, error))
			ok = false;
	}

	if (ok && appliedVersion != nullptr)
		*appliedVersion = version;

	return ok;
}

bool Migrate(Database &database, std::string *error, int *appliedVersion)
{
	return ApplyMigrations(database, DefaultMigrations(), error, appliedVersion);
}

} // namespace storage
