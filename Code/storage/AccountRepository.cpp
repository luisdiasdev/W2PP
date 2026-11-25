#include "AccountRepository.h"

#include "../Basedef.h"
#include "../security/CredentialBytes.h"
#include "../security/PasswordHash.h"
#include "RecordCodec.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace storage
{
namespace
{

std::string Join(const std::vector<std::string> &values, const char *separator)
{
	std::string result;

	for (std::size_t i = 0; i < values.size(); ++i)
	{
		if (i != 0)
			result += separator;
		result += values[i];
	}

	return result;
}

std::string Placeholders(std::size_t count, std::size_t first)
{
	std::string result;

	for (std::size_t i = 0; i < count; ++i)
	{
		if (i != 0)
			result += ',';
		result += '$';
		result += std::to_string(first + i);
	}

	return result;
}

std::string Assignments(const std::vector<std::string> &columns,
	std::size_t first, const char *prefix = "")
{
	std::string result;

	for (std::size_t i = 0; i < columns.size(); ++i)
	{
		if (i != 0)
			result += ',';
		result += prefix;
		result += columns[i];
		result += "=$";
		result += std::to_string(first + i);
	}

	return result;
}

std::string ExcludedAssignments(const std::vector<std::string> &columns)
{
	std::string result;

	for (std::size_t i = 0; i < columns.size(); ++i)
	{
		if (i != 0)
			result += ',';
		result += columns[i];
		result += "=EXCLUDED.";
		result += columns[i];
	}

	return result;
}

std::vector<unsigned char> Bytes(const unsigned char *data, std::size_t size)
{
	return std::vector<unsigned char>(data, data + size);
}

std::vector<unsigned char> ByteVector(const std::string &value)
{
	return std::vector<unsigned char>(value.begin(), value.end());
}

std::string StringOf(const std::vector<unsigned char> &bytes)
{
	return std::string(bytes.begin(), bytes.end());
}

const std::vector<std::string> &CharacterColumns()
{
	static const std::vector<std::string> columns = {
		"name", "name_key", "clan", "merchant", "guild", "\"class\"", "rsv",
		"quest", "coin", "exp", "spx", "spy", "learned_skill", "magic",
		"score_bonus", "special_bonus", "skill_bonus", "critical", "save_mana",
		"guild_level", "regen_hp", "regen_mp",
		"resist0", "resist1", "resist2", "resist3",
		"base_level", "base_ac", "base_damage", "base_merchant",
		"base_attack_run", "base_direction", "base_chaos_rate",
		"base_max_hp", "base_max_mp", "base_hp", "base_mp",
		"base_str", "base_int", "base_dex", "base_con",
		"base_special0", "base_special1", "base_special2", "base_special3",
		"current_level", "current_ac", "current_damage", "current_merchant",
		"current_attack_run", "current_direction", "current_chaos_rate",
		"current_max_hp", "current_max_mp", "current_hp", "current_mp",
		"current_str", "current_int", "current_dex", "current_con",
		"current_special0", "current_special1", "current_special2", "current_special3",
		"skill_bar",
	};

	return columns;
}

const std::vector<std::string> &ExtraColumns()
{
	static const std::vector<std::string> columns = {
		"class_master", "citizen", "fame", "soul", "mortal_face",
		"quest_newbie", "quest_terra_mistica", "quest_molar_gargula", "quest_pilula_orc",
		"arch_mortal_slot", "arch_mortal_level", "arch_level_355", "arch_level_370",
		"arch_cristal", "celestial_arch_level", "celestial_celestial_level",
		"celestial_sub_celestial_level", "celestial_lv40", "celestial_lv90",
		"celestial_add120", "celestial_add150", "celestial_add180", "celestial_add200",
		"celestial_arcana", "celestial_reset", "circle", "last_nt", "nt",
		"kefra_ticket", "divine_end", "hold", "daylog_exp", "daylog_year_day",
		"donate_last_time", "donate_count",
	};

	return columns;
}

const std::vector<std::string> &CelestialColumns()
{
	static const std::vector<std::string> columns = {
		"class", "exp", "spx", "spy",
		"base_level", "base_ac", "base_damage", "base_merchant",
		"base_attack_run", "base_direction", "base_chaos_rate",
		"base_max_hp", "base_max_mp", "base_hp", "base_mp",
		"base_str", "base_int", "base_dex", "base_con",
		"base_special0", "base_special1", "base_special2", "base_special3",
		"learned_skill", "score_bonus", "special_bonus", "skill_bonus",
		"skill_bar1", "skill_bar2", "soul",
	};

	return columns;
}

void AppendScoreParams(std::vector<Param> &params, const ScoreRecord &score)
{
	params.push_back(ParamInt(score.level));
	params.push_back(ParamInt(score.ac));
	params.push_back(ParamInt(score.damage));
	params.push_back(ParamInt(score.merchant));
	params.push_back(ParamInt(score.attack_run));
	params.push_back(ParamInt(score.direction));
	params.push_back(ParamInt(score.chaos_rate));
	params.push_back(ParamInt(score.max_hp));
	params.push_back(ParamInt(score.max_mp));
	params.push_back(ParamInt(score.hp));
	params.push_back(ParamInt(score.mp));
	params.push_back(ParamInt(score.str));
	params.push_back(ParamInt(score.int_stat));
	params.push_back(ParamInt(score.dex));
	params.push_back(ParamInt(score.con));
	for (int i = 0; i < 4; ++i)
		params.push_back(ParamInt(score.special[i]));
}

void AppendCharacterParams(std::vector<Param> &params,
	const CharacterRecord &character)
{
	params.push_back(ParamBytes(character.name));
	params.push_back(ParamBytes(NameKey(character.name)));
	params.push_back(ParamInt(character.clan));
	params.push_back(ParamInt(character.merchant));
	params.push_back(ParamInt(character.guild));
	params.push_back(ParamInt(character.clazz));
	params.push_back(ParamInt(character.rsv));
	params.push_back(ParamInt(character.quest));
	params.push_back(ParamInt(character.coin));
	params.push_back(ParamInt64(character.exp));
	params.push_back(ParamInt(character.spx));
	params.push_back(ParamInt(character.spy));
	params.push_back(ParamInt(static_cast<int>(character.learned_skill)));
	params.push_back(ParamInt64(character.magic));
	params.push_back(ParamInt(character.score_bonus));
	params.push_back(ParamInt(character.special_bonus));
	params.push_back(ParamInt(character.skill_bonus));
	params.push_back(ParamInt(character.critical));
	params.push_back(ParamInt(character.save_mana));
	params.push_back(ParamInt(character.guild_level));
	params.push_back(ParamInt(character.regen_hp));
	params.push_back(ParamInt(character.regen_mp));
	for (int i = 0; i < 4; ++i)
		params.push_back(ParamInt(character.resist[i]));
	AppendScoreParams(params, character.base_score);
	AppendScoreParams(params, character.current_score);
	params.push_back(ParamBytes(Bytes(character.skill_bar, 4)));
}

void ReadScore(const Result &result, int row, int *column, ScoreRecord *score)
{
	score->level = result.Int32(row, (*column)++);
	score->ac = result.Int32(row, (*column)++);
	score->damage = result.Int32(row, (*column)++);
	score->merchant = result.Int16(row, (*column)++);
	score->attack_run = result.Int16(row, (*column)++);
	score->direction = result.Int16(row, (*column)++);
	score->chaos_rate = result.Int16(row, (*column)++);
	score->max_hp = result.Int32(row, (*column)++);
	score->max_mp = result.Int32(row, (*column)++);
	score->hp = result.Int32(row, (*column)++);
	score->mp = result.Int32(row, (*column)++);
	score->str = result.Int16(row, (*column)++);
	score->int_stat = result.Int16(row, (*column)++);
	score->dex = result.Int16(row, (*column)++);
	score->con = result.Int16(row, (*column)++);

	for (int i = 0; i < 4; ++i)
		score->special[i] = result.Int16(row, (*column)++);
}

void ReadCharacterRow(const Result &result, int row, CharacterRecord *character)
{
	int column = 0;

	character->character_id = result.Int64(row, column++);
	character->slot = result.Int16(row, column++);
	character->live = character->slot >= 0;
	character->capsule_id = 0;

	character->name = result.Bytes(row, column++);
	character->name_key = result.Bytes(row, column++);
	character->clan = result.Int16(row, column++);
	character->merchant = result.Int16(row, column++);
	character->guild = result.Int32(row, column++);
	character->clazz = result.Int16(row, column++);
	character->rsv = result.Int32(row, column++);
	character->quest = result.Int16(row, column++);
	character->coin = result.Int32(row, column++);
	character->exp = result.Int64(row, column++);
	character->spx = result.Int16(row, column++);
	character->spy = result.Int16(row, column++);
	character->learned_skill = result.Int32(row, column++);
	character->magic = result.Int64(row, column++);
	character->score_bonus = result.Int32(row, column++);
	character->special_bonus = result.Int32(row, column++);
	character->skill_bonus = result.Int32(row, column++);
	character->critical = result.Int16(row, column++);
	character->save_mana = result.Int16(row, column++);
	character->guild_level = result.Int16(row, column++);
	character->regen_hp = result.Int32(row, column++);
	character->regen_mp = result.Int32(row, column++);

	for (int i = 0; i < 4; ++i)
		character->resist[i] = result.Int16(row, column++);

	ReadScore(result, row, &column, &character->base_score);
	ReadScore(result, row, &column, &character->current_score);

	character->skill_bar[0] = 0;
	character->skill_bar[1] = 0;
	character->skill_bar[2] = 0;
	character->skill_bar[3] = 0;

	std::vector<unsigned char> bar = result.Bytes(row, column++);
	for (std::size_t i = 0; i < bar.size() && i < 4; ++i)
		character->skill_bar[i] = bar[i];
}

void AppendExtraParams(std::vector<Param> &params, const ExtraRecord &extra)
{
	params.push_back(ParamInt(extra.class_master));
	params.push_back(ParamInt(extra.citizen));
	params.push_back(ParamInt(extra.fame));
	params.push_back(ParamInt(extra.soul));
	params.push_back(ParamInt(extra.mortal_face));
	params.push_back(ParamInt(extra.quest_newbie));
	params.push_back(ParamInt(extra.quest_terra_mistica));
	params.push_back(ParamInt(extra.quest_molar_gargula));
	params.push_back(ParamInt(extra.quest_pilula_orc));
	params.push_back(ParamInt(extra.arch_mortal_slot));
	params.push_back(ParamInt(extra.arch_mortal_level));
	params.push_back(ParamInt(extra.arch_level_355));
	params.push_back(ParamInt(extra.arch_level_370));
	params.push_back(ParamInt(extra.arch_cristal));
	params.push_back(ParamInt(extra.celestial_arch_level));
	params.push_back(ParamInt(extra.celestial_celestial_level));
	params.push_back(ParamInt(extra.celestial_sub_celestial_level));
	params.push_back(ParamInt(extra.celestial_lv40));
	params.push_back(ParamInt(extra.celestial_lv90));
	params.push_back(ParamInt(extra.celestial_add120));
	params.push_back(ParamInt(extra.celestial_add150));
	params.push_back(ParamInt(extra.celestial_add180));
	params.push_back(ParamInt(extra.celestial_add200));
	params.push_back(ParamInt(extra.celestial_arcana));
	params.push_back(ParamInt(extra.celestial_reset));
	params.push_back(ParamInt(extra.circle));
	params.push_back(ParamInt64(extra.last_nt));
	params.push_back(ParamInt(extra.nt));
	params.push_back(ParamInt(extra.kefra_ticket));
	params.push_back(ParamInt64(extra.divine_end));
	params.push_back(ParamInt64(extra.hold));
	params.push_back(ParamInt64(extra.daylog_exp));
	params.push_back(ParamInt(extra.daylog_year_day));
	params.push_back(ParamInt64(extra.donate_last_time));
	params.push_back(ParamInt(extra.donate_count));
}

void ReadExtraRow(const Result &result, int row, ExtraRecord *extra)
{
	int column = 0;

	extra->class_master = result.Int16(row, column++);
	extra->citizen = result.Int16(row, column++);
	extra->fame = result.Int32(row, column++);
	extra->soul = result.Int16(row, column++);
	extra->mortal_face = result.Int16(row, column++);
	extra->quest_newbie = result.Int16(row, column++);
	extra->quest_terra_mistica = result.Int16(row, column++);
	extra->quest_molar_gargula = result.Int16(row, column++);
	extra->quest_pilula_orc = result.Int16(row, column++);
	extra->arch_mortal_slot = result.Int16(row, column++);
	extra->arch_mortal_level = result.Int16(row, column++);
	extra->arch_level_355 = result.Int16(row, column++);
	extra->arch_level_370 = result.Int16(row, column++);
	extra->arch_cristal = result.Int16(row, column++);
	extra->celestial_arch_level = result.Int16(row, column++);
	extra->celestial_celestial_level = result.Int16(row, column++);
	extra->celestial_sub_celestial_level = result.Int16(row, column++);
	extra->celestial_lv40 = result.Int16(row, column++);
	extra->celestial_lv90 = result.Int16(row, column++);
	extra->celestial_add120 = result.Int16(row, column++);
	extra->celestial_add150 = result.Int16(row, column++);
	extra->celestial_add180 = result.Int16(row, column++);
	extra->celestial_add200 = result.Int16(row, column++);
	extra->celestial_arcana = result.Int16(row, column++);
	extra->celestial_reset = result.Int16(row, column++);
	extra->circle = result.Int16(row, column++);
	extra->last_nt = result.Int64(row, column++);
	extra->nt = result.Int32(row, column++);
	extra->kefra_ticket = result.Int32(row, column++);
	extra->divine_end = result.Int64(row, column++);
	extra->hold = result.Int64(row, column++);
	extra->daylog_exp = result.Int64(row, column++);
	extra->daylog_year_day = result.Int32(row, column++);
	extra->donate_last_time = result.Int64(row, column++);
	extra->donate_count = result.Int32(row, column++);
}

void AppendCelestialParams(std::vector<Param> &params,
	const CelestialRecord &celestial)
{
	params.push_back(ParamInt(celestial.clazz));
	params.push_back(ParamInt64(celestial.exp));
	params.push_back(ParamInt(celestial.spx));
	params.push_back(ParamInt(celestial.spy));
	AppendScoreParams(params, celestial.base_score);
	params.push_back(ParamInt(static_cast<int>(celestial.learned_skill)));
	params.push_back(ParamInt(celestial.score_bonus));
	params.push_back(ParamInt(celestial.special_bonus));
	params.push_back(ParamInt(celestial.skill_bonus));
	params.push_back(ParamBytes(Bytes(celestial.skill_bar1, 4)));
	params.push_back(ParamBytes(Bytes(celestial.skill_bar2, 16)));
	params.push_back(ParamInt(celestial.soul));
}

void ReadCelestialRow(const Result &result, int row, CelestialRecord *celestial)
{
	int column = 1; // column 0 is save_slot

	celestial->clazz = result.Int16(row, column++);
	celestial->exp = result.Int64(row, column++);
	celestial->spx = result.Int16(row, column++);
	celestial->spy = result.Int16(row, column++);
	ReadScore(result, row, &column, &celestial->base_score);
	celestial->learned_skill = result.Int32(row, column++);
	celestial->score_bonus = result.Int32(row, column++);
	celestial->special_bonus = result.Int32(row, column++);
	celestial->skill_bonus = result.Int32(row, column++);

	std::vector<unsigned char> bar1 = result.Bytes(row, column++);
	std::vector<unsigned char> bar2 = result.Bytes(row, column++);
	for (std::size_t i = 0; i < bar1.size() && i < 4; ++i)
		celestial->skill_bar1[i] = bar1[i];
	for (std::size_t i = 0; i < bar2.size() && i < 16; ++i)
		celestial->skill_bar2[i] = bar2[i];

	celestial->soul = result.Int16(row, column++);
}

struct ItemRow
{
	int position;
	int index;
	int effect0;
	int effect1;
	int effect2;
};

bool LoadItemRows(Database &database, const char *table, const char *ownerColumn,
	long long ownerId, std::vector<ItemRow> *rows, std::string *error)
{
	std::string sql = std::string("SELECT position, item_index, effect0, effect1, effect2 FROM ")
		+ table + " WHERE " + ownerColumn + " = $1 ORDER BY position";

	Result result = database.ExecParams(sql, {ParamInt64(ownerId)}, true);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = std::string("cannot read ") + table + ": " + result.Error();
		return false;
	}

	rows->clear();

	for (int row = 0; row < result.Rows(); ++row)
	{
		ItemRow item;
		item.position = result.Int16(row, 0);
		item.index = result.Int16(row, 1);
		item.effect0 = result.Int16(row, 2);
		item.effect1 = result.Int16(row, 3);
		item.effect2 = result.Int16(row, 4);
		rows->push_back(item);
	}

	return true;
}

struct AffectRow
{
	int position;
	int type;
	int value;
	int level;
	long long time;
};

bool LoadAffectRows(Database &database, long long characterId,
	std::vector<AffectRow> *rows, std::string *error)
{
	Result result = database.ExecParams(
		"SELECT position, type, value, level, time FROM character_affects "
		"WHERE character_id = $1 ORDER BY position",
		{ParamInt64(characterId)}, true);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "cannot read character_affects: " + result.Error();
		return false;
	}

	rows->clear();

	for (int row = 0; row < result.Rows(); ++row)
	{
		AffectRow affect;
		affect.position = result.Int16(row, 0);
		affect.type = result.Int16(row, 1);
		affect.value = result.Int16(row, 2);
		affect.level = result.Int32(row, 3);
		affect.time = result.Int64(row, 4);
		rows->push_back(affect);
	}

	return true;
}

void RecordCredentialEvent(Database &database, long long accountId,
	int kind, const char *source)
{
	database.ExecParams(
		"INSERT INTO credential_events (account_id, credential_kind, source) "
		"VALUES ($1, $2, $3)",
		{ParamInt64(accountId), ParamInt(kind), ParamText(source)}, false);
}

long long LocalEpoch(int year, int month, int day)
{
	std::tm value;
	std::memset(&value, 0, sizeof(value));
	value.tm_year = year - 1900;
	value.tm_mon = month;
	value.tm_mday = day;
	value.tm_hour = 0;
	value.tm_min = 0;
	value.tm_sec = 0;
	value.tm_isdst = -1;

	return static_cast<long long>(std::mktime(&value));
}

} // namespace

AccountRepository::AccountRepository(Database &database,
	const StorageConfig &config)
	: database_(database), config_(config)
{
}

std::string AccountRepository::NameKeyFor(
	const std::vector<unsigned char> &name) const
{
	return StringOf(NameKey(name));
}

bool AccountRepository::LoadAccount(const std::vector<unsigned char> &name,
	AccountRecord *out, bool *found, std::string *error)
{
	if (out == nullptr)
		return false;

	*out = AccountRecord();
	if (found != nullptr)
		*found = false;

	Transaction transaction(database_);
	std::string transactionError;

	if (!transaction.Begin(&transactionError, "REPEATABLE READ"))
	{
		if (error != nullptr)
			*error = "load account: " + transactionError;
		return false;
	}

	Result account = database_.ExecParams(
		"SELECT account_id, name, password_hash, token_hash, state, real_name, "
		"ssn1, ssn2, email, telephone, address, block_year, block_year_day, "
		"temp_key, coin, donate, short_skill, password_failures, token_failures "
		"FROM accounts WHERE name_key = $1",
		{ParamBytes(NameKey(name))}, true);

	if (!account.Ok())
	{
		if (error != nullptr)
			*error = "load account: " + account.Error();
		return false;
	}

	if (account.Rows() == 0)
	{
		transaction.Commit(&transactionError);
		return true;
	}

	out->account_id = account.Int64(0, 0);
	out->name = account.Bytes(0, 1);
	out->name_key = NameKey(out->name);
	out->password_hash = account.Text(0, 2);
	out->has_token = !account.IsNull(0, 3);
	out->token_hash = out->has_token ? account.Text(0, 3) : std::string();
	out->state = account.Int16(0, 4);
	out->real_name = account.Bytes(0, 5);
	out->ssn1 = account.Int32(0, 6);
	out->ssn2 = account.Int32(0, 7);
	out->email = account.Bytes(0, 8);
	out->telephone = account.Bytes(0, 9);
	out->address = account.Bytes(0, 10);
	out->block_year = account.Int32(0, 11);
	out->block_year_day = account.Int32(0, 12);

	std::vector<unsigned char> tempKey = account.Bytes(0, 13);
	std::memcpy(out->temp_key, tempKey.data(),
		std::min<std::size_t>(tempKey.size(), sizeof(out->temp_key)));

	out->coin = account.Int32(0, 14);
	out->donate = account.Int32(0, 15);

	std::vector<unsigned char> shortSkill = account.Bytes(0, 16);
	std::memcpy(out->short_skill, shortSkill.data(),
		std::min<std::size_t>(shortSkill.size(), sizeof(out->short_skill)));

	out->password_failures = account.Int32(0, 17);
	out->token_failures = account.Int32(0, 18);

	if (!LoadCargo(out->account_id, out, error)
		|| !LoadCharacters(out->account_id, out, error))
		return false;

	if (!transaction.Commit(&transactionError))
	{
		if (error != nullptr)
			*error = "load account: " + transactionError;
		return false;
	}

	if (found != nullptr)
		*found = true;

	return true;
}

bool AccountRepository::LoadAccountFile(const STRUCT_ACCOUNTFILE &key,
	STRUCT_ACCOUNTFILE *out, bool *found, std::string *error)
{
	std::vector<unsigned char> name =
		NameBytes(key.Info.AccountName, ACCOUNTNAME_LENGTH);

	AccountRecord record;

	if (!LoadAccount(name, &record, found, error))
		return false;

	if (out != nullptr)
	{
		if (found != nullptr && *found)
			AccountFileFromRecord(record, out);
		else
			std::memset(out, 0, sizeof(*out));
	}

	return true;
}

bool AccountRepository::LoadCargo(long long accountId, AccountRecord *out,
	std::string *error)
{
	std::vector<ItemRow> rows;

	if (!LoadItemRows(database_, "account_cargo", "account_id", accountId,
			&rows, error))
		return false;

	for (const ItemRow &row : rows)
	{
		if (row.position < 0 || row.position >= MAX_CARGO)
			continue;

		ItemSlot &slot = out->cargo[row.position];
		slot.index = row.index;
		slot.effect[0] = row.effect0;
		slot.effect[1] = row.effect1;
		slot.effect[2] = row.effect2;
	}

	return true;
}

bool AccountRepository::LoadCharacters(long long accountId, AccountRecord *out,
	std::string *error)
{
	std::string sql = "SELECT character_id, slot, " + Join(CharacterColumns(), ",")
		+ " FROM characters WHERE account_id = $1 AND account_id IS NOT NULL "
		"ORDER BY slot";

	Result result = database_.ExecParams(sql, {ParamInt64(accountId)}, true);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "load characters: " + result.Error();
		return false;
	}

	for (int row = 0; row < result.Rows(); ++row)
	{
		CharacterRecord character;

		ReadCharacterRow(result, row, &character);

		if (!LoadCharacterChildren(&character, error))
			return false;

		out->characters.push_back(character);
	}

	return true;
}

bool AccountRepository::LoadCharacterChildren(CharacterRecord *character,
	std::string *error)
{
	std::vector<ItemRow> rows;

	if (!LoadItemRows(database_, "character_equipment", "character_id",
			character->character_id, &rows, error))
		return false;

	for (const ItemRow &row : rows)
	{
		if (row.position < 0 || row.position >= MAX_EQUIP)
			continue;

		ItemSlot &slot = character->equipment[row.position];
		slot.index = row.index;
		slot.effect[0] = row.effect0;
		slot.effect[1] = row.effect1;
		slot.effect[2] = row.effect2;
	}

	if (!LoadItemRows(database_, "character_inventory", "character_id",
			character->character_id, &rows, error))
		return false;

	for (const ItemRow &row : rows)
	{
		if (row.position < 0 || row.position >= MAX_CARRY)
			continue;

		ItemSlot &slot = character->inventory[row.position];
		slot.index = row.index;
		slot.effect[0] = row.effect0;
		slot.effect[1] = row.effect1;
		slot.effect[2] = row.effect2;
	}

	std::vector<AffectRow> affects;

	if (!LoadAffectRows(database_, character->character_id, &affects, error))
		return false;

	for (const AffectRow &row : affects)
	{
		if (row.position < 0 || row.position >= MAX_AFFECT)
			continue;

		AffectRecord &affect = character->affects[row.position];
		affect.type = row.type;
		affect.value = row.value;
		affect.level = row.level;
		affect.time = row.time;
	}

	Result extra = database_.ExecParams(
		"SELECT " + Join(ExtraColumns(), ",") + " FROM character_extras "
		"WHERE character_id = $1",
		{ParamInt64(character->character_id)}, true);

	if (!extra.Ok())
	{
		if (error != nullptr)
			*error = "load character extras: " + extra.Error();
		return false;
	}

	if (extra.Rows() > 0)
		ReadExtraRow(extra, 0, &character->extra);

	Result celestial = database_.ExecParams(
		"SELECT save_slot, " + Join(CelestialColumns(), ",")
			+ " FROM character_celestial_saves WHERE character_id = $1 "
			"ORDER BY save_slot",
		{ParamInt64(character->character_id)}, true);

	if (!celestial.Ok())
	{
		if (error != nullptr)
			*error = "load celestial saves: " + celestial.Error();
		return false;
	}

	for (int row = 0; row < celestial.Rows(); ++row)
	{
		int slot = celestial.Int16(row, 0);
		if (slot < 0 || slot >= 2)
			continue;

		ReadCelestialRow(celestial, row, &character->celestial[slot]);
	}

	return true;
}

bool AccountRepository::SaveAccountFile(const STRUCT_ACCOUNTFILE &file,
	bool allowInsert, bool *inserted, std::string *error)
{
	AccountRecord record;

	if (!AccountRecordFromFile(file, &record))
	{
		if (error != nullptr)
			*error = "save account: invalid record";
		return false;
	}

	if (record.name.empty())
	{
		if (error != nullptr)
			*error = "save account: empty account name";
		return false;
	}

	if (inserted != nullptr)
		*inserted = false;

	Transaction transaction(database_);
	std::string transactionError;

	if (!transaction.Begin(&transactionError, "SERIALIZABLE"))
	{
		if (error != nullptr)
			*error = "save account: " + transactionError;
		return false;
	}

	Result existing = database_.ExecParams(
		"SELECT account_id FROM accounts WHERE name_key = $1 FOR UPDATE",
		{ParamBytes(record.name_key)}, true);

	if (!existing.Ok())
	{
		if (error != nullptr)
			*error = "save account: " + existing.Error();
		return false;
	}

	if (existing.Rows() > 0)
	{
		record.account_id = existing.Int64(0, 0);

		Result update = database_.ExecParams(
			"UPDATE accounts SET name=$1, name_key=$2, real_name=$3, ssn1=$4, "
			"ssn2=$5, email=$6, telephone=$7, address=$8, block_year=$9, "
			"block_year_day=$10, temp_key=$11, coin=$12, donate=$13, "
			"short_skill=$14, updated_at=now() WHERE account_id=$15",
			{
				ParamBytes(record.name),
				ParamBytes(record.name_key),
				ParamBytes(record.real_name),
				ParamInt(record.ssn1),
				ParamInt(record.ssn2),
				ParamBytes(record.email),
				ParamBytes(record.telephone),
				ParamBytes(record.address),
				ParamInt(record.block_year),
				ParamInt(record.block_year_day),
				ParamBytes(Bytes(record.temp_key, sizeof(record.temp_key))),
				ParamInt(record.coin),
				ParamInt(record.donate),
				ParamBytes(Bytes(record.short_skill, sizeof(record.short_skill))),
				ParamInt64(record.account_id),
			}, false);

		if (!update.Ok())
		{
			if (error != nullptr)
				*error = "save account: " + update.Error();
			return false;
		}
	}
	else
	{
		if (!allowInsert)
		{
			if (error != nullptr)
				*error = "save account: account does not exist";
			return false;
		}

		security::LegacyCredential credential =
			security::SplitLegacyCredential(file.Info.AccountPass,
				ACCOUNTPASS_LENGTH);

		if (!credential.usable)
		{
			if (error != nullptr)
				*error = "save account: new account has an empty password";
			return false;
		}

		std::string passwordHash;

		if (!security::PasswordHash::Generate(credential.credential,
				config_.bcrypt_cost, &passwordHash, error))
			return false;

		std::string tokenHash;
		bool hasToken = false;

		if (file.Info.NumericToken[0] != -1)
		{
			hasToken = true;

			std::string token = security::SubmittedCredential(
				file.Info.NumericToken, sizeof(file.Info.NumericToken));

			if (!security::PasswordHash::Generate(token,
					config_.bcrypt_cost, &tokenHash, error))
				return false;
		}

		Result insert = database_.ExecParams(
			"INSERT INTO accounts (name, name_key, password_hash, token_hash, "
			"state, real_name, ssn1, ssn2, email, telephone, address, "
			"block_year, block_year_day, temp_key, coin, donate, short_skill) "
			"VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14,$15,$16,$17) "
			"RETURNING account_id",
			{
				ParamBytes(record.name),
				ParamBytes(record.name_key),
				ParamText(passwordHash),
				hasToken ? ParamText(tokenHash) : ParamNull(),
				ParamInt(static_cast<int>(credential.state)),
				ParamBytes(record.real_name),
				ParamInt(record.ssn1),
				ParamInt(record.ssn2),
				ParamBytes(record.email),
				ParamBytes(record.telephone),
				ParamBytes(record.address),
				ParamInt(record.block_year),
				ParamInt(record.block_year_day),
				ParamBytes(Bytes(record.temp_key, sizeof(record.temp_key))),
				ParamInt(record.coin),
				ParamInt(record.donate),
				ParamBytes(Bytes(record.short_skill, sizeof(record.short_skill))),
			}, true);

		if (!insert.Ok())
		{
			if (error != nullptr)
				*error = "save account: " + insert.Error();
			return false;
		}

		record.account_id = insert.Int64(0, 0);

		if (inserted != nullptr)
			*inserted = true;
	}

	if (!SaveCharacters(record.account_id, record, error))
		return false;

	if (!ReplaceItems("account_cargo", "account_id", record.account_id,
			record.cargo, MAX_CARGO, error))
		return false;

	if (!transaction.Commit(&transactionError))
	{
		if (error != nullptr)
			*error = "save account: " + transactionError;
		return false;
	}

	return true;
}

bool AccountRepository::SaveCharacters(long long accountId,
	const AccountRecord &record, std::string *error)
{
	Result existing = database_.ExecParams(
		"SELECT character_id, slot FROM characters "
		"WHERE account_id = $1 AND account_id IS NOT NULL",
		{ParamInt64(accountId)}, true);

	if (!existing.Ok())
	{
		if (error != nullptr)
			*error = "save characters: " + existing.Error();
		return false;
	}

	std::map<int, long long> existingBySlot;

	for (int row = 0; row < existing.Rows(); ++row)
		existingBySlot[existing.Int16(row, 1)] = existing.Int64(row, 0);

	// Desired live slots.
	std::map<int, const CharacterRecord *> desired;

	for (const CharacterRecord &character : record.characters)
	{
		if (character.live && character.slot >= 0 && character.slot < MOB_PER_ACCOUNT
			&& !character.name.empty())
			desired[character.slot] = &character;
	}

	// Release names of slots that no longer hold a character before inserting
	// or renaming any other slot.
	for (const auto &entry : existingBySlot)
	{
		if (desired.find(entry.first) != desired.end())
			continue;

		Result deleted = database_.ExecParams(
			"DELETE FROM characters WHERE character_id = $1",
			{ParamInt64(entry.second)}, false);

		if (!deleted.Ok())
		{
			if (error != nullptr)
				*error = "save characters: " + deleted.Error();
			return false;
		}
	}

	const std::vector<std::string> &columns = CharacterColumns();

	for (const auto &entry : desired)
	{
		const int slot = entry.first;
		const CharacterRecord &character = *entry.second;
		long long characterId = 0;

		auto found = existingBySlot.find(slot);

		if (found != existingBySlot.end())
		{
			characterId = found->second;

			Result update = database_.ExecParams(
				"UPDATE characters SET " + Assignments(columns, 1)
					+ " WHERE character_id=$" + std::to_string(columns.size() + 1),
				[&]()
				{
					std::vector<Param> params;
					AppendCharacterParams(params, character);
					params.push_back(ParamInt64(characterId));
					return params;
				}(), false);

			if (!update.Ok())
			{
				if (error != nullptr)
					*error = "save character " + std::to_string(slot) + ": "
						+ update.Error();
				return false;
			}
		}
		else
		{
			Result insert = database_.ExecParams(
				"INSERT INTO characters (account_id, slot, "
					+ Join(columns, ",") + ") VALUES ($1,$2,"
					+ Placeholders(columns.size(), 3) + ") RETURNING character_id",
				[&]()
				{
					std::vector<Param> params;
					params.push_back(ParamInt64(accountId));
					params.push_back(ParamInt(slot));
					AppendCharacterParams(params, character);
					return params;
				}(), true);

			if (!insert.Ok())
			{
				if (error != nullptr)
					*error = "save character " + std::to_string(slot) + ": "
						+ insert.Error();
				return false;
			}

			characterId = insert.Int64(0, 0);
		}

		if (!ReplaceItems("character_equipment", "character_id", characterId,
				character.equipment, MAX_EQUIP, error))
			return false;

		if (!ReplaceItems("character_inventory", "character_id", characterId,
				character.inventory, MAX_CARRY, error))
			return false;

		if (!ReplaceAffects(characterId, character, error))
			return false;

		if (!UpsertExtra(characterId, character.extra, error))
			return false;

		for (int save = 0; save < 2; ++save)
		{
			if (!UpsertCelestial(characterId, save, character.celestial[save],
					error))
				return false;
		}
	}

	return true;
}

bool AccountRepository::ReplaceItems(const char *table, const char *ownerColumn,
	long long ownerId, const ItemSlot *slots, int count, std::string *error)
{
	std::vector<ItemRow> desired;

	for (int position = 0; position < count; ++position)
	{
		if (slots[position].index == 0)
			continue;

		ItemRow row;
		row.position = position;
		row.index = slots[position].index;
		row.effect0 = slots[position].effect[0];
		row.effect1 = slots[position].effect[1];
		row.effect2 = slots[position].effect[2];
		desired.push_back(row);
	}

	std::vector<ItemRow> current;

	if (!LoadItemRows(database_, table, ownerColumn, ownerId, &current, error))
		return false;

	bool same = current.size() == desired.size();

	for (std::size_t i = 0; same && i < current.size(); ++i)
	{
		same = current[i].position == desired[i].position
			&& current[i].index == desired[i].index
			&& current[i].effect0 == desired[i].effect0
			&& current[i].effect1 == desired[i].effect1
			&& current[i].effect2 == desired[i].effect2;
	}

	if (same)
		return true;

	Result deleted = database_.ExecParams(
		std::string("DELETE FROM ") + table + " WHERE " + ownerColumn + " = $1",
		{ParamInt64(ownerId)}, false);

	if (!deleted.Ok())
	{
		if (error != nullptr)
			*error = std::string("replace ") + table + ": " + deleted.Error();
		return false;
	}

	for (const ItemRow &row : desired)
	{
		Result inserted = database_.ExecParams(
			std::string("INSERT INTO ") + table + " (" + ownerColumn
				+ ", position, item_index, effect0, effect1, effect2) "
				"VALUES ($1,$2,$3,$4,$5,$6)",
			{
				ParamInt64(ownerId),
				ParamInt(row.position),
				ParamInt(row.index),
				ParamInt(row.effect0),
				ParamInt(row.effect1),
				ParamInt(row.effect2),
			}, false);

		if (!inserted.Ok())
		{
			if (error != nullptr)
				*error = std::string("replace ") + table + ": " + inserted.Error();
			return false;
		}
	}

	return true;
}

bool AccountRepository::ReplaceAffects(long long characterId,
	const CharacterRecord &character, std::string *error)
{
	std::vector<AffectRow> desired;

	for (int position = 0; position < MAX_AFFECT; ++position)
	{
		if (character.affects[position].type == 0)
			continue;

		AffectRow row;
		row.position = position;
		row.type = character.affects[position].type;
		row.value = character.affects[position].value;
		row.level = character.affects[position].level;
		row.time = character.affects[position].time;
		desired.push_back(row);
	}

	std::vector<AffectRow> current;

	if (!LoadAffectRows(database_, characterId, &current, error))
		return false;

	bool same = current.size() == desired.size();

	for (std::size_t i = 0; same && i < current.size(); ++i)
	{
		same = current[i].position == desired[i].position
			&& current[i].type == desired[i].type
			&& current[i].value == desired[i].value
			&& current[i].level == desired[i].level
			&& current[i].time == desired[i].time;
	}

	if (same)
		return true;

	Result deleted = database_.ExecParams(
		"DELETE FROM character_affects WHERE character_id = $1",
		{ParamInt64(characterId)}, false);

	if (!deleted.Ok())
	{
		if (error != nullptr)
			*error = "replace affects: " + deleted.Error();
		return false;
	}

	for (const AffectRow &row : desired)
	{
		Result inserted = database_.ExecParams(
			"INSERT INTO character_affects (character_id, position, type, value, level, time) "
			"VALUES ($1,$2,$3,$4,$5,$6)",
			{
				ParamInt64(characterId),
				ParamInt(row.position),
				ParamInt(row.type),
				ParamInt(row.value),
				ParamInt(row.level),
				ParamInt64(row.time),
			}, false);

		if (!inserted.Ok())
		{
			if (error != nullptr)
				*error = "replace affects: " + inserted.Error();
			return false;
		}
	}

	return true;
}

bool AccountRepository::UpsertExtra(long long characterId,
	const ExtraRecord &extra, std::string *error)
{
	const std::vector<std::string> &columns = ExtraColumns();

	std::string sql = "INSERT INTO character_extras (character_id, "
		+ Join(columns, ",") + ") VALUES ($1,"
		+ Placeholders(columns.size(), 2) + ") ON CONFLICT (character_id) DO UPDATE SET "
		+ ExcludedAssignments(columns);

	std::vector<Param> params;
	params.push_back(ParamInt64(characterId));
	AppendExtraParams(params, extra);

	Result result = database_.ExecParams(sql, params, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "save character extras: " + result.Error();
		return false;
	}

	return true;
}

bool AccountRepository::UpsertCelestial(long long characterId, int slot,
	const CelestialRecord &celestial, std::string *error)
{
	const std::vector<std::string> &columns = CelestialColumns();

	std::string sql = "INSERT INTO character_celestial_saves (character_id, save_slot, "
		+ Join(columns, ",") + ") VALUES ($1,$2,"
		+ Placeholders(columns.size(), 3)
		+ ") ON CONFLICT (character_id, save_slot) DO UPDATE SET "
		+ ExcludedAssignments(columns);

	std::vector<Param> params;
	params.push_back(ParamInt64(characterId));
	params.push_back(ParamInt(slot));
	AppendCelestialParams(params, celestial);

	Result result = database_.ExecParams(sql, params, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "save celestial save: " + result.Error();
		return false;
	}

	return true;
}

bool AccountRepository::CharacterNameAvailable(
	const std::vector<unsigned char> &name, bool *available, std::string *error)
{
	Result result = database_.ExecParams(
		"SELECT 1 FROM characters WHERE name_key = $1",
		{ParamBytes(NameKey(name))}, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "character name lookup: " + result.Error();
		return false;
	}

	if (available != nullptr)
		*available = result.Rows() == 0;

	return true;
}

bool AccountRepository::DeleteCharacterByName(long long accountId,
	const std::vector<unsigned char> &name, std::string *error)
{
	Result result = database_.ExecParams(
		"DELETE FROM characters WHERE account_id = $1 AND name_key = $2 "
		"AND account_id IS NOT NULL",
		{ParamInt64(accountId), ParamBytes(NameKey(name))}, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "delete character: " + result.Error();
		return false;
	}

	return true;
}

bool AccountRepository::FindAccountByCharacterName(
	const std::vector<unsigned char> &name, std::vector<unsigned char> *account,
	bool *found, std::string *error)
{
	Result result = database_.ExecParams(
		"SELECT a.name FROM characters c JOIN accounts a "
		"ON a.account_id = c.account_id "
		"WHERE c.name_key = $1 AND c.account_id IS NOT NULL",
		{ParamBytes(NameKey(name))}, true);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "character owner lookup: " + result.Error();
		return false;
	}

	if (found != nullptr)
		*found = result.Rows() > 0;

	if (account != nullptr)
		*account = result.Rows() > 0 ? result.Bytes(0, 0)
			: std::vector<unsigned char>();

	return true;
}

bool AccountRepository::PutInCapsule(long long accountId, int slot,
	int *capsuleId, std::string *error)
{
	Transaction transaction(database_);
	std::string transactionError;

	if (!transaction.Begin(&transactionError, "SERIALIZABLE"))
	{
		if (error != nullptr)
			*error = "put-in capsule: " + transactionError;
		return false;
	}

	Result lock = database_.ExecParams(
		"SELECT account_id FROM accounts WHERE account_id = $1 FOR UPDATE",
		{ParamInt64(accountId)}, false);

	if (!lock.Ok())
	{
		if (error != nullptr)
			*error = "put-in capsule: " + lock.Error();
		return false;
	}

	Result sequence = database_.Exec("SELECT nextval('capsule_id_seq')");

	if (!sequence.Ok())
	{
		if (error != nullptr)
			*error = "put-in capsule: capsule identifier space exhausted: "
				+ sequence.Error();
		return false;
	}

	const int id = static_cast<int>(sequence.Int64(0, 0));

	Result sealed = database_.ExecParams(
		"UPDATE characters SET account_id = NULL, slot = NULL, capsule_id = $1 "
		"WHERE account_id = $2 AND slot = $3 RETURNING character_id",
		{ParamInt(id), ParamInt64(accountId), ParamInt(slot)}, true);

	if (!sealed.Ok())
	{
		if (error != nullptr)
			*error = "put-in capsule: " + sealed.Error();
		return false;
	}

	if (sealed.Rows() == 0)
	{
		if (error != nullptr)
			*error = "put-in capsule: no character in that slot";
		return false;
	}

	const long long characterId = sealed.Int64(0, 0);

	Result extra = database_.ExecParams(
		"UPDATE character_extras SET arch_mortal_slot = -1 "
		"WHERE character_id = $1",
		{ParamInt64(characterId)}, false);

	if (!extra.Ok())
	{
		if (error != nullptr)
			*error = "put-in capsule: " + extra.Error();
		return false;
	}

	Result cleared = database_.ExecParams(
		"UPDATE accounts SET short_skill = overlay(short_skill placing $1::bytea "
		"from $2 for 16) WHERE account_id = $3",
		{
			ParamBytes(std::vector<unsigned char>(16, 0)),
			ParamInt(slot * 16 + 1),
			ParamInt64(accountId),
		}, false);

	if (!cleared.Ok())
	{
		if (error != nullptr)
			*error = "put-in capsule: " + cleared.Error();
		return false;
	}

	if (!transaction.Commit(&transactionError))
	{
		if (error != nullptr)
			*error = "put-in capsule: " + transactionError;
		return false;
	}

	if (capsuleId != nullptr)
		*capsuleId = id;

	return true;
}

bool AccountRepository::TakeOutCapsule(long long accountId, int capsuleId,
	int slot, const std::vector<unsigned char> &newName, std::string *error)
{
	Transaction transaction(database_);
	std::string transactionError;

	if (!transaction.Begin(&transactionError, "SERIALIZABLE"))
	{
		if (error != nullptr)
			*error = "take-out capsule: " + transactionError;
		return false;
	}

	Result lock = database_.ExecParams(
		"SELECT account_id FROM accounts WHERE account_id = $1 FOR UPDATE",
		{ParamInt64(accountId)}, false);

	if (!lock.Ok())
	{
		if (error != nullptr)
			*error = "take-out capsule: " + lock.Error();
		return false;
	}

	Result nameTaken = database_.ExecParams(
		"SELECT 1 FROM characters WHERE name_key = $1 "
		"AND (capsule_id IS NULL OR capsule_id <> $2)",
		{ParamBytes(NameKey(newName)), ParamInt(capsuleId)}, false);

	if (!nameTaken.Ok())
	{
		if (error != nullptr)
			*error = "take-out capsule: " + nameTaken.Error();
		return false;
	}

	if (nameTaken.Rows() > 0)
	{
		if (error != nullptr)
			*error = "take-out capsule: character name already in use";
		return false;
	}

	Result restored = database_.ExecParams(
		"UPDATE characters SET account_id = $1, slot = $2, capsule_id = NULL, "
		"name = $3, name_key = $4 WHERE capsule_id = $5 RETURNING character_id",
		{
			ParamInt64(accountId),
			ParamInt(slot),
			ParamBytes(newName),
			ParamBytes(NameKey(newName)),
			ParamInt(capsuleId),
		}, true);

	if (!restored.Ok())
	{
		if (error != nullptr)
			*error = "take-out capsule: " + restored.Error();
		return false;
	}

	if (restored.Rows() == 0)
	{
		if (error != nullptr)
			*error = "take-out capsule: capsule does not exist";
		return false;
	}

	const long long characterId = restored.Int64(0, 0);

	Result affects = database_.ExecParams(
		"DELETE FROM character_affects WHERE character_id = $1",
		{ParamInt64(characterId)}, false);

	if (!affects.Ok())
	{
		if (error != nullptr)
			*error = "take-out capsule: " + affects.Error();
		return false;
	}

	Result cleared = database_.ExecParams(
		"UPDATE accounts SET short_skill = overlay(short_skill placing $1::bytea "
		"from $2 for 16) WHERE account_id = $3",
		{
			ParamBytes(std::vector<unsigned char>(16, 0xFF)),
			ParamInt(slot * 16 + 1),
			ParamInt64(accountId),
		}, false);

	if (!cleared.Ok())
	{
		if (error != nullptr)
			*error = "take-out capsule: " + cleared.Error();
		return false;
	}

	if (!transaction.Commit(&transactionError))
	{
		if (error != nullptr)
			*error = "take-out capsule: " + transactionError;
		return false;
	}

	return true;
}

bool AccountRepository::LoadCharacterScalars(long long characterId,
	CharacterRecord *out, std::string *error)
{
	std::string sql = "SELECT character_id, slot, " + Join(CharacterColumns(), ",")
		+ " FROM characters WHERE character_id = $1";

	Result result = database_.ExecParams(sql, {ParamInt64(characterId)}, true);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "load character: " + result.Error();
		return false;
	}

	if (result.Rows() == 0)
		return false;

	ReadCharacterRow(result, 0, out);
	return true;
}

bool AccountRepository::ImportCapsule(int capsuleId,
	const CharacterRecord &character, std::string *error)
{
	const std::vector<std::string> &columns = CharacterColumns();

	Transaction transaction(database_);
	std::string transactionError;

	if (!transaction.Begin(&transactionError, "SERIALIZABLE"))
	{
		if (error != nullptr)
			*error = "import capsule: " + transactionError;
		return false;
	}

	Result insert = database_.ExecParams(
		"INSERT INTO characters (capsule_id, " + Join(columns, ",") + ") VALUES ($1,"
			+ Placeholders(columns.size(), 2) + ") RETURNING character_id",
		[&]()
		{
			std::vector<Param> params;
			params.push_back(ParamInt(capsuleId));
			AppendCharacterParams(params, character);
			return params;
		}(), true);

	if (!insert.Ok())
	{
		if (error != nullptr)
			*error = "import capsule " + std::to_string(capsuleId) + ": "
				+ insert.Error();
		return false;
	}

	const long long characterId = insert.Int64(0, 0);

	if (!ReplaceItems("character_equipment", "character_id", characterId,
			character.equipment, MAX_EQUIP, error)
		|| !ReplaceItems("character_inventory", "character_id", characterId,
			character.inventory, MAX_CARRY, error)
		|| !ReplaceAffects(characterId, character, error)
		|| !UpsertExtra(characterId, character.extra, error))
		return false;

	for (int slot = 0; slot < 2; ++slot)
	{
		if (!UpsertCelestial(characterId, slot, character.celestial[slot], error))
			return false;
	}

	if (!transaction.Commit(&transactionError))
	{
		if (error != nullptr)
			*error = "import capsule: " + transactionError;
		return false;
	}

	return true;
}

bool AccountRepository::SetCapsuleSequence(int lastId, std::string *error)
{
	Result result = database_.ExecParams(
		"SELECT setval('capsule_id_seq', GREATEST($1::int, 1), $1 > 0)",
		{ParamInt(lastId)}, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "set capsule sequence: " + result.Error();
		return false;
	}

	return true;
}

bool AccountRepository::LoadCapsule(int capsuleId, CharacterRecord *out,
	bool *found, std::string *error)
{
	if (out == nullptr)
		return false;

	*out = CharacterRecord();
	if (found != nullptr)
		*found = false;

	Result result = database_.ExecParams(
		"SELECT character_id FROM characters WHERE capsule_id = $1",
		{ParamInt(capsuleId)}, true);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "load capsule: " + result.Error();
		return false;
	}

	if (result.Rows() == 0)
		return true;

	if (!LoadCharacterScalars(result.Int64(0, 0), out, error))
		return false;

	out->live = false;
	out->capsule_id = capsuleId;

	if (!LoadCharacterChildren(out, error))
		return false;

	if (found != nullptr)
		*found = true;

	return true;
}

LoginStatus AccountRepository::AdmitLogin(
	const std::vector<unsigned char> &name, const std::string &password,
	const std::string &tempKey, AccountRecord *out, bool *tempKeyUsed,
	std::string *error)
{
	if (out == nullptr)
		return LoginStatus::Error;

	if (tempKeyUsed != nullptr)
		*tempKeyUsed = false;

	*out = AccountRecord();

	Transaction transaction(database_);
	std::string transactionError;

	if (!transaction.Begin(&transactionError, "SERIALIZABLE"))
	{
		if (error != nullptr)
			*error = "login: " + transactionError;
		return LoginStatus::Error;
	}

	Result account = database_.ExecParams(
		"SELECT account_id, name, password_hash, state, block_year, block_year_day, "
		"temp_key, coin, password_failures, "
		"(password_locked_until IS NOT NULL AND password_locked_until > now()), "
		"token_hash, token_failures "
		"FROM accounts WHERE name_key = $1 FOR UPDATE",
		{ParamBytes(NameKey(name))}, true);

	if (!account.Ok())
	{
		if (error != nullptr)
			*error = "login: " + account.Error();
		return LoginStatus::Error;
	}

	if (account.Rows() == 0)
		return LoginStatus::NotFound;

	const long long accountId = account.Int64(0, 0);
	const std::string passwordHash = account.Text(0, 2);
	const int state = account.Int16(0, 3);
	const int blockYear = account.Int32(0, 4);
	const int blockYearDay = account.Int32(0, 5);
	const std::vector<unsigned char> storedTempKey = account.Bytes(0, 6);
	const int coin = account.Int32(0, 7);
	int failures = account.Int32(0, 8);
	const bool passwordLocked = account.Bool(0, 9);

	std::time_t now = std::time(nullptr);
	std::tm local;
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	if (blockYear != 0 && blockYearDay != 0
		&& (blockYear > local.tm_year
			|| (blockYear == local.tm_year && blockYearDay >= local.tm_yday)))
		return LoginStatus::Blocked;

	if (state != 0)
		return LoginStatus::Blocked;

	int effectiveCoin = coin;

	if (effectiveCoin < 0)
	{
		effectiveCoin = 0;

		Result clamped = database_.ExecParams(
			"UPDATE accounts SET coin = 0, updated_at = now() WHERE account_id = $1",
			{ParamInt64(accountId)}, false);

		if (!clamped.Ok())
		{
			if (error != nullptr)
				*error = "login: " + clamped.Error();
			return LoginStatus::Error;
		}
	}

	bool hasStoredTempKey = false;
	for (unsigned char byte : storedTempKey)
	{
		if (byte != 0)
		{
			hasStoredTempKey = true;
			break;
		}
	}

	bool bypassPassword = false;

	if (hasStoredTempKey && !tempKey.empty())
	{
		std::string stored(storedTempKey.begin(), storedTempKey.end());
		std::string supplied = tempKey;
		supplied.resize(52, '\0');

		if (stored == supplied)
		{
			bypassPassword = true;
		}

		Result cleared = database_.ExecParams(
			"UPDATE accounts SET temp_key = $1, updated_at = now() "
			"WHERE account_id = $2",
			{ParamBytes(std::vector<unsigned char>(52, 0)), ParamInt64(accountId)},
			false);

		if (!cleared.Ok())
		{
			if (error != nullptr)
				*error = "login: " + cleared.Error();
			return LoginStatus::Error;
		}

		if (!bypassPassword)
		{
			// Legacy behavior: a supplied but non-matching server-change token
			// clears the stored token and ends the attempt without a reply.
			transaction.Commit(&transactionError);
			return LoginStatus::TempKeyMismatch;
		}
	}

	if (!bypassPassword)
	{
		if (passwordLocked)
			return LoginStatus::Locked;

		security::VerifyResult verification =
			security::PasswordHash::Verify(password, passwordHash);

		if (verification == security::VerifyResult::MalformedHash
			&& error != nullptr)
			*error = "login: stored password hash is malformed";

		if (verification != security::VerifyResult::Match)
		{
			if (error != nullptr)
				*error = "login credential mismatch";

			++failures;

			if (failures >= config_.auth_max_failures)
			{
				database_.ExecParams(
					"UPDATE accounts SET password_failures = $1, "
					"password_locked_until = now() + make_interval(secs => $2), "
					"updated_at = now() WHERE account_id = $3",
					{
						ParamInt(failures),
						ParamInt(config_.auth_lockout_seconds),
						ParamInt64(accountId),
					}, false);
			}
			else
			{
				database_.ExecParams(
					"UPDATE accounts SET password_failures = $1, updated_at = now() "
					"WHERE account_id = $2",
					{ParamInt(failures), ParamInt64(accountId)}, false);
			}

			RecordCredentialEvent(database_, accountId, 1, "login");
			transaction.Commit(&transactionError);
			return LoginStatus::WrongPassword;
		}

		Result reset = database_.ExecParams(
			"UPDATE accounts SET password_failures = 0, "
			"password_locked_until = NULL WHERE account_id = $1",
			{ParamInt64(accountId)}, false);

		if (!reset.Ok())
		{
			if (error != nullptr)
				*error = "login: " + reset.Error();
			return LoginStatus::Error;
		}

		if (security::PasswordHash::NeedsRehash(passwordHash, config_.bcrypt_cost))
		{
			std::string upgraded;

			if (!security::PasswordHash::Generate(password, config_.bcrypt_cost,
					&upgraded, error))
				return LoginStatus::Error;

			Result update = database_.ExecParams(
				"UPDATE accounts SET password_hash = $1 WHERE account_id = $2",
				{ParamText(upgraded), ParamInt64(accountId)}, false);

			if (!update.Ok())
			{
				if (error != nullptr)
					*error = "login: " + update.Error();
				return LoginStatus::Error;
			}
		}
	}

	out->account_id = accountId;
	out->name = account.Bytes(0, 1);
	out->name_key = NameKey(out->name);
	out->password_hash = passwordHash;
	out->state = state;
	out->block_year = blockYear;
	out->block_year_day = blockYearDay;
	out->coin = effectiveCoin;
	out->password_failures = 0;
	out->has_token = !account.IsNull(0, 10);
	out->token_hash = out->has_token ? account.Text(0, 10) : std::string();
	out->token_failures = account.Int32(0, 11);

	Result scalars = database_.ExecParams(
		"SELECT real_name, ssn1, ssn2, email, telephone, address, donate, "
		"short_skill, temp_key FROM accounts WHERE account_id = $1",
		{ParamInt64(accountId)}, true);

	if (!scalars.Ok())
	{
		if (error != nullptr)
			*error = "login: " + scalars.Error();
		return LoginStatus::Error;
	}

	out->real_name = scalars.Bytes(0, 0);
	out->ssn1 = scalars.Int32(0, 1);
	out->ssn2 = scalars.Int32(0, 2);
	out->email = scalars.Bytes(0, 3);
	out->telephone = scalars.Bytes(0, 4);
	out->address = scalars.Bytes(0, 5);
	out->donate = scalars.Int32(0, 6);

	std::vector<unsigned char> shortSkill = scalars.Bytes(0, 7);
	std::memcpy(out->short_skill, shortSkill.data(),
		std::min<std::size_t>(shortSkill.size(), sizeof(out->short_skill)));

	std::vector<unsigned char> tempKeyBytes = scalars.Bytes(0, 8);
	std::memcpy(out->temp_key, tempKeyBytes.data(),
		std::min<std::size_t>(tempKeyBytes.size(), sizeof(out->temp_key)));

	if (!LoadCargo(out->account_id, out, error)
		|| !LoadCharacters(out->account_id, out, error))
		return LoginStatus::Error;

	if (!transaction.Commit(&transactionError))
	{
		if (error != nullptr)
			*error = "login: " + transactionError;
		return LoginStatus::Error;
	}

	if (tempKeyUsed != nullptr)
		*tempKeyUsed = bypassPassword;

	return LoginStatus::Success;
}

bool AccountRepository::VerifyPassword(long long accountId,
	const std::string &password, bool *matched, std::string *error)
{
	if (matched != nullptr)
		*matched = false;

	Transaction transaction(database_);
	std::string transactionError;

	if (!transaction.Begin(&transactionError, "SERIALIZABLE"))
	{
		if (error != nullptr)
			*error = "verify password: " + transactionError;
		return false;
	}

	Result account = database_.ExecParams(
		"SELECT password_hash, password_failures, "
		"(password_locked_until IS NOT NULL AND password_locked_until > now()) "
		"FROM accounts WHERE account_id = $1 FOR UPDATE",
		{ParamInt64(accountId)}, true);

	if (!account.Ok())
	{
		if (error != nullptr)
			*error = "verify password: " + account.Error();
		return false;
	}

	if (account.Rows() == 0)
		return true;

	if (account.Bool(0, 2))
		return true; // locked: refused without hashing

	const std::string hash = account.Text(0, 0);
	int failures = account.Int32(0, 1);

	if (security::PasswordHash::Verify(password, hash)
		!= security::VerifyResult::Match)
	{
		++failures;

		if (failures >= config_.auth_max_failures)
		{
			database_.ExecParams(
				"UPDATE accounts SET password_failures = $1, "
				"password_locked_until = now() + make_interval(secs => $2), "
				"updated_at = now() WHERE account_id = $3",
				{
					ParamInt(failures),
					ParamInt(config_.auth_lockout_seconds),
					ParamInt64(accountId),
				}, false);
		}
		else
		{
			database_.ExecParams(
				"UPDATE accounts SET password_failures = $1, updated_at = now() "
				"WHERE account_id = $2",
				{ParamInt(failures), ParamInt64(accountId)}, false);
		}

		RecordCredentialEvent(database_, accountId, 1, "password-check");
		transaction.Commit(&transactionError);
		return true;
	}

	database_.ExecParams(
		"UPDATE accounts SET password_failures = 0, "
		"password_locked_until = NULL WHERE account_id = $1",
		{ParamInt64(accountId)}, false);

	if (security::PasswordHash::NeedsRehash(hash, config_.bcrypt_cost))
	{
		std::string upgraded;

		if (!security::PasswordHash::Generate(password, config_.bcrypt_cost,
				&upgraded, error))
			return false;

		database_.ExecParams(
			"UPDATE accounts SET password_hash = $1 WHERE account_id = $2",
			{ParamText(upgraded), ParamInt64(accountId)}, false);
	}

	if (!transaction.Commit(&transactionError))
	{
		if (error != nullptr)
			*error = "verify password: " + transactionError;
		return false;
	}

	if (matched != nullptr)
		*matched = true;

	return true;
}

bool AccountRepository::ChangePassword(long long accountId,
	const std::string &newPassword, std::string *error)
{
	std::string hash;

	if (!security::PasswordHash::Generate(newPassword, config_.bcrypt_cost,
			&hash, error))
		return false;

	Result result = database_.ExecParams(
		"UPDATE accounts SET password_hash = $1, password_failures = 0, "
		"password_locked_until = NULL, updated_at = now() WHERE account_id = $2",
		{ParamText(hash), ParamInt64(accountId)}, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "change password: " + result.Error();
		return false;
	}

	return true;
}

bool AccountRepository::SetState(long long accountId, int state,
	std::string *error)
{
	Result result = database_.ExecParams(
		"UPDATE accounts SET state = $1, updated_at = now() "
		"WHERE account_id = $2",
		{ParamInt(state), ParamInt64(accountId)}, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "set account state: " + result.Error();
		return false;
	}

	return true;
}

bool AccountRepository::BindToken(long long accountId, const std::string &token,
	bool change, std::string *error)
{
	std::string hash;

	if (!security::PasswordHash::Generate(token, config_.bcrypt_cost,
			&hash, error))
		return false;

	Result result = change
		? database_.ExecParams(
			"UPDATE accounts SET token_hash = $1, token_failures = 0, "
			"token_locked_until = NULL, updated_at = now() "
			"WHERE account_id = $2 RETURNING account_id",
			{ParamText(hash), ParamInt64(accountId)}, false)
		: database_.ExecParams(
			"UPDATE accounts SET token_hash = $1, token_failures = 0, "
			"token_locked_until = NULL, updated_at = now() "
			"WHERE account_id = $2 AND token_hash IS NULL "
			"RETURNING account_id",
			{ParamText(hash), ParamInt64(accountId)}, false);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "bind token: " + result.Error();
		return false;
	}

	return result.Rows() > 0;
}

bool AccountRepository::VerifyToken(long long accountId, const std::string &token,
	bool *matched, std::string *error)
{
	if (matched != nullptr)
		*matched = false;

	Transaction transaction(database_);
	std::string transactionError;

	if (!transaction.Begin(&transactionError, "SERIALIZABLE"))
	{
		if (error != nullptr)
			*error = "verify token: " + transactionError;
		return false;
	}

	Result account = database_.ExecParams(
		"SELECT token_hash, token_failures, "
		"(token_locked_until IS NOT NULL AND token_locked_until > now()) "
		"FROM accounts WHERE account_id = $1 FOR UPDATE",
		{ParamInt64(accountId)}, true);

	if (!account.Ok())
	{
		if (error != nullptr)
			*error = "verify token: " + account.Error();
		return false;
	}

	if (account.Rows() == 0 || account.IsNull(0, 0))
		return true; // unset token; first binding is allowed

	if (account.Bool(0, 2))
		return true; // locked: refused without hashing

	const std::string hash = account.Text(0, 0);
	int failures = account.Int32(0, 1);

	security::VerifyResult verification =
		security::PasswordHash::Verify(token, hash);

	if (verification != security::VerifyResult::Match)
	{
		++failures;

		if (failures >= config_.auth_max_failures)
		{
			database_.ExecParams(
				"UPDATE accounts SET token_failures = $1, "
				"token_locked_until = now() + make_interval(secs => $2) "
				"WHERE account_id = $3",
				{
					ParamInt(failures),
					ParamInt(config_.auth_lockout_seconds),
					ParamInt64(accountId),
				}, false);
		}
		else
		{
			database_.ExecParams(
				"UPDATE accounts SET token_failures = $1 WHERE account_id = $2",
				{ParamInt(failures), ParamInt64(accountId)}, false);
		}

		RecordCredentialEvent(database_, accountId, 2, "token");
		transaction.Commit(&transactionError);
		return true;
	}

	Result reset = database_.ExecParams(
		"UPDATE accounts SET token_failures = 0, token_locked_until = NULL "
		"WHERE account_id = $1",
		{ParamInt64(accountId)}, false);

	if (!reset.Ok())
	{
		if (error != nullptr)
			*error = "verify token: " + reset.Error();
		return false;
	}

	if (security::PasswordHash::NeedsRehash(hash, config_.bcrypt_cost))
	{
		std::string upgraded;

		if (!security::PasswordHash::Generate(token, config_.bcrypt_cost,
				&upgraded, error))
			return false;

		database_.ExecParams(
			"UPDATE accounts SET token_hash = $1 WHERE account_id = $2",
			{ParamText(upgraded), ParamInt64(accountId)}, false);
	}

	if (!transaction.Commit(&transactionError))
	{
		if (error != nullptr)
			*error = "verify token: " + transactionError;
		return false;
	}

	if (matched != nullptr)
		*matched = true;

	return true;
}

bool AccountRepository::DayLogEntries(int yearDay,
	std::vector<DayLogEntry> *out, std::string *error)
{
	if (out == nullptr)
		return false;

	out->clear();

	std::time_t now = std::time(nullptr);
	std::tm local;
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	const long long monthStart = LocalEpoch(local.tm_year + 1900, local.tm_mon, 1);
	const long long nextMonth = LocalEpoch(local.tm_year + 1900, local.tm_mon + 1, 1);

	Result result = database_.ExecParams(
		"SELECT a.name, c.name, e.daylog_exp "
		"FROM accounts a JOIN characters c ON c.account_id = a.account_id "
		"JOIN character_extras e ON e.character_id = c.character_id "
		"WHERE a.updated_at >= to_timestamp($1) "
		"AND a.updated_at < to_timestamp($2) AND e.daylog_year_day = $3 "
		"ORDER BY a.name_key, c.slot",
		{
			ParamInt64(monthStart),
			ParamInt64(nextMonth),
			ParamInt(yearDay),
		}, true);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "day-log query: " + result.Error();
		return false;
	}

	for (int row = 0; row < result.Rows(); ++row)
	{
		DayLogEntry entry;
		entry.account = result.Bytes(row, 0);
		entry.character = result.Bytes(row, 1);
		entry.exp = result.Int64(row, 2);
		out->push_back(entry);
	}

	return true;
}

bool AccountRepository::RankingEntries(std::vector<RankingEntry> *out,
	std::string *error)
{
	if (out == nullptr)
		return false;

	out->clear();

	std::time_t now = std::time(nullptr);
	std::tm local;
#ifdef _WIN32
	localtime_s(&local, &now);
#else
	localtime_r(&now, &local);
#endif

	long long threshold;

	if (local.tm_mon == 0)
	{
		// January keeps the legacy behavior: every account in the current year.
		threshold = LocalEpoch(local.tm_year + 1900, 0, 1);
	}
	else
	{
		threshold = LocalEpoch(local.tm_year + 1900, local.tm_mon - 1, 1);
	}

	Result result = database_.ExecParams(
		"SELECT c.name, c.exp, e.class_master, c.current_level, c.\"class\" "
		"FROM accounts a JOIN characters c ON c.account_id = a.account_id "
		"JOIN character_extras e ON e.character_id = c.character_id "
		"WHERE a.updated_at >= to_timestamp($1) "
		"AND length(c.name) > 0 AND c.base_level < 1000 "
		"ORDER BY a.name_key, c.slot",
		{ParamInt64(threshold)}, true);

	if (!result.Ok())
	{
		if (error != nullptr)
			*error = "ranking query: " + result.Error();
		return false;
	}

	for (int row = 0; row < result.Rows(); ++row)
	{
		RankingEntry entry;
		entry.name = result.Bytes(row, 0);
		entry.exp = result.Int64(row, 1);
		entry.class_master = result.Int16(row, 2);
		entry.level = result.Int32(row, 3);
		entry.clazz = result.Int16(row, 4);
		out->push_back(entry);
	}

	return true;
}

bool AccountRepository::ExportAccount(const std::vector<unsigned char> &name,
	STRUCT_ACCOUNTFILE *out, bool *found, std::string *error)
{
	AccountRecord record;

	if (!LoadAccount(name, &record, found, error))
		return false;

	if (out != nullptr)
	{
		if (found != nullptr && *found)
			AccountFileFromRecord(record, out);
		else
			std::memset(out, 0, sizeof(*out));
	}

	return true;
}

} // namespace storage
