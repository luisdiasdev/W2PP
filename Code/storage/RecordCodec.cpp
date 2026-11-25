#include "RecordCodec.h"

#include "../Basedef.h"

#include <cstring>

namespace storage
{
namespace
{

int SignedByte(int value)
{
	return static_cast<int>(static_cast<signed char>(value));
}

int UnsignedByte(int value)
{
	return static_cast<int>(static_cast<unsigned char>(value));
}

char ToSignedByte(int value)
{
	return static_cast<char>(static_cast<signed char>(value));
}

unsigned char ToUnsignedByte(int value)
{
	return static_cast<unsigned char>(value & 0xFF);
}

ScoreRecord ScoreFromStruct(const STRUCT_SCORE &score)
{
	ScoreRecord record;

	record.level = score.Level;
	record.ac = score.Ac;
	record.damage = score.Damage;
	record.merchant = UnsignedByte(score.Merchant);
	record.attack_run = UnsignedByte(score.AttackRun);
	record.direction = UnsignedByte(score.Direction);
	record.chaos_rate = UnsignedByte(score.ChaosRate);
	record.max_hp = score.MaxHp;
	record.max_mp = score.MaxMp;
	record.hp = score.Hp;
	record.mp = score.Mp;
	record.str = score.Str;
	record.int_stat = score.Int;
	record.dex = score.Dex;
	record.con = score.Con;

	for (int i = 0; i < 4; ++i)
		record.special[i] = score.Special[i];

	return record;
}

void ScoreToStruct(const ScoreRecord &record, STRUCT_SCORE *score)
{
	score->Level = record.level;
	score->Ac = record.ac;
	score->Damage = record.damage;
	score->Merchant = ToUnsignedByte(record.merchant);
	score->AttackRun = ToUnsignedByte(record.attack_run);
	score->Direction = ToUnsignedByte(record.direction);
	score->ChaosRate = ToUnsignedByte(record.chaos_rate);
	score->MaxHp = record.max_hp;
	score->MaxMp = record.max_mp;
	score->Hp = record.hp;
	score->Mp = record.mp;
	score->Str = static_cast<short>(record.str);
	score->Int = static_cast<short>(record.int_stat);
	score->Dex = static_cast<short>(record.dex);
	score->Con = static_cast<short>(record.con);

	for (int i = 0; i < 4; ++i)
		score->Special[i] = static_cast<short>(record.special[i]);
}

ItemSlot ItemFromStruct(const STRUCT_ITEM &item)
{
	ItemSlot slot;

	slot.index = item.sIndex;

	for (int i = 0; i < 3; ++i)
		slot.effect[i] = item.stEffect[i].sValue;

	return slot;
}

void ItemToStruct(const ItemSlot &slot, STRUCT_ITEM *item)
{
	item->sIndex = static_cast<short>(slot.index);

	for (int i = 0; i < 3; ++i)
		item->stEffect[i].sValue = static_cast<short>(slot.effect[i]);
}

CelestialRecord CelestialFromStruct(const STRUCT_MOBEXTRA &extra, int index)
{
	const auto &save = extra.SaveCelestial[index];
	CelestialRecord record;

	record.clazz = save.Class;
	record.exp = save.Exp;
	record.spx = save.SPX;
	record.spy = save.SPY;
	record.base_score = ScoreFromStruct(save.BaseScore);
	record.learned_skill = save.LearnedSkill;
	record.score_bonus = save.ScoreBonus;
	record.special_bonus = save.SpecialBonus;
	record.skill_bonus = save.SkillBonus;

	for (int i = 0; i < 4; ++i)
		record.skill_bar1[i] = save.SkillBar1[i];
	for (int i = 0; i < 16; ++i)
		record.skill_bar2[i] = save.SkillBar2[i];

	record.soul = SignedByte(save.Soul);

	return record;
}

void CelestialToStruct(const CelestialRecord &record, STRUCT_MOBEXTRA *extra,
	int index)
{
	auto &save = extra->SaveCelestial[index];

	save.Class = record.clazz;
	save.Exp = record.exp;
	save.SPX = static_cast<short>(record.spx);
	save.SPY = static_cast<short>(record.spy);
	ScoreToStruct(record.base_score, &save.BaseScore);
	save.LearnedSkill = static_cast<long>(record.learned_skill);
	save.ScoreBonus = static_cast<unsigned short>(record.score_bonus);
	save.SpecialBonus = static_cast<unsigned short>(record.special_bonus);
	save.SkillBonus = static_cast<unsigned short>(record.skill_bonus);

	for (int i = 0; i < 4; ++i)
		save.SkillBar1[i] = record.skill_bar1[i];
	for (int i = 0; i < 16; ++i)
		save.SkillBar2[i] = record.skill_bar2[i];

	save.Soul = ToSignedByte(record.soul);
}

ExtraRecord ExtraFromStruct(const STRUCT_MOBEXTRA &extra)
{
	ExtraRecord record;

	record.class_master = extra.ClassMaster;
	record.citizen = SignedByte(extra.Citizen);
	record.fame = extra.Fame;
	record.soul = SignedByte(extra.Soul);
	record.mortal_face = extra.MortalFace;

	record.quest_newbie = SignedByte(extra.QuestInfo.Mortal.Newbie);
	record.quest_terra_mistica = SignedByte(extra.QuestInfo.Mortal.TerraMistica);
	record.quest_molar_gargula = SignedByte(extra.QuestInfo.Mortal.MolarGargula);
	record.quest_pilula_orc = SignedByte(extra.QuestInfo.Mortal.PilulaOrc);

	record.arch_mortal_slot = SignedByte(extra.QuestInfo.Arch.MortalSlot);
	record.arch_mortal_level = SignedByte(extra.QuestInfo.Arch.MortalLevel);
	record.arch_level_355 = SignedByte(extra.QuestInfo.Arch.Level355);
	record.arch_level_370 = SignedByte(extra.QuestInfo.Arch.Level370);
	record.arch_cristal = SignedByte(extra.QuestInfo.Arch.Cristal);

	record.celestial_arch_level = extra.QuestInfo.Celestial.ArchLevel;
	record.celestial_celestial_level = extra.QuestInfo.Celestial.CelestialLevel;
	record.celestial_sub_celestial_level = extra.QuestInfo.Celestial.SubCelestialLevel;
	record.celestial_lv40 = SignedByte(extra.QuestInfo.Celestial.Lv40);
	record.celestial_lv90 = SignedByte(extra.QuestInfo.Celestial.Lv90);
	record.celestial_add120 = SignedByte(extra.QuestInfo.Celestial.Add120);
	record.celestial_add150 = SignedByte(extra.QuestInfo.Celestial.Add150);
	record.celestial_add180 = SignedByte(extra.QuestInfo.Celestial.Add180);
	record.celestial_add200 = SignedByte(extra.QuestInfo.Celestial.Add200);
	record.celestial_arcana = SignedByte(extra.QuestInfo.Celestial.Arcana);
	record.celestial_reset = SignedByte(extra.QuestInfo.Celestial.Reset);

	record.circle = SignedByte(extra.QuestInfo.Circle);

	record.last_nt = static_cast<long long>(extra.LastNT);
	record.nt = extra.NT;
	record.kefra_ticket = extra.KefraTicket;
	record.divine_end = static_cast<long long>(extra.DivineEnd);
	record.hold = static_cast<long long>(extra.Hold);

	record.daylog_exp = extra.DayLog.Exp;
	record.daylog_year_day = extra.DayLog.YearDay;

	record.donate_last_time = static_cast<long long>(extra.DonateInfo.LastTime);
	record.donate_count = extra.DonateInfo.Count;

	return record;
}

void ExtraToStruct(const ExtraRecord &record, STRUCT_MOBEXTRA *extra)
{
	extra->ClassMaster = static_cast<short>(record.class_master);
	extra->Citizen = ToSignedByte(record.citizen);
	extra->Fame = record.fame;
	extra->Soul = ToSignedByte(record.soul);
	extra->MortalFace = static_cast<short>(record.mortal_face);

	extra->QuestInfo.Mortal.Newbie = ToSignedByte(record.quest_newbie);
	extra->QuestInfo.Mortal.TerraMistica = ToSignedByte(record.quest_terra_mistica);
	extra->QuestInfo.Mortal.MolarGargula = ToSignedByte(record.quest_molar_gargula);
	extra->QuestInfo.Mortal.PilulaOrc = ToSignedByte(record.quest_pilula_orc);

	extra->QuestInfo.Arch.MortalSlot = ToSignedByte(record.arch_mortal_slot);
	extra->QuestInfo.Arch.MortalLevel = ToSignedByte(record.arch_mortal_level);
	extra->QuestInfo.Arch.Level355 = ToSignedByte(record.arch_level_355);
	extra->QuestInfo.Arch.Level370 = ToSignedByte(record.arch_level_370);
	extra->QuestInfo.Arch.Cristal = ToSignedByte(record.arch_cristal);

	extra->QuestInfo.Celestial.ArchLevel = static_cast<short>(record.celestial_arch_level);
	extra->QuestInfo.Celestial.CelestialLevel = static_cast<short>(record.celestial_celestial_level);
	extra->QuestInfo.Celestial.SubCelestialLevel = static_cast<short>(record.celestial_sub_celestial_level);
	extra->QuestInfo.Celestial.Lv40 = ToSignedByte(record.celestial_lv40);
	extra->QuestInfo.Celestial.Lv90 = ToSignedByte(record.celestial_lv90);
	extra->QuestInfo.Celestial.Add120 = ToSignedByte(record.celestial_add120);
	extra->QuestInfo.Celestial.Add150 = ToSignedByte(record.celestial_add150);
	extra->QuestInfo.Celestial.Add180 = ToSignedByte(record.celestial_add180);
	extra->QuestInfo.Celestial.Add200 = ToSignedByte(record.celestial_add200);
	extra->QuestInfo.Celestial.Arcana = ToSignedByte(record.celestial_arcana);
	extra->QuestInfo.Celestial.Reset = ToSignedByte(record.celestial_reset);

	extra->QuestInfo.Circle = ToSignedByte(record.circle);

	extra->LastNT = static_cast<time_t>(record.last_nt);
	extra->NT = record.nt;
	extra->KefraTicket = record.kefra_ticket;
	extra->DivineEnd = static_cast<time_t>(record.divine_end);
	extra->Hold = static_cast<unsigned int>(record.hold);

	extra->DayLog.Exp = record.daylog_exp;
	extra->DayLog.YearDay = record.daylog_year_day;

	extra->DonateInfo.LastTime = static_cast<time_t>(record.donate_last_time);
	extra->DonateInfo.Count = record.donate_count;
}

} // namespace

std::vector<unsigned char> NameBytes(const char *field, std::size_t capacity)
{
	std::vector<unsigned char> name;

	if (field == nullptr || capacity == 0)
		return name;

	std::size_t length = 0;
	while (length < capacity && field[length] != '\0')
		++length;

	// Keep one byte of room so the value can be written back terminated.
	if (length >= capacity)
		length = capacity - 1;

	name.assign(field, field + length);
	return name;
}

std::vector<unsigned char> NameKey(const std::vector<unsigned char> &name)
{
	std::vector<unsigned char> key = name;

	for (unsigned char &byte : key)
	{
		if (byte < 0x80)
			byte = static_cast<unsigned char>(std::toupper(byte));
	}

	return key;
}

void WriteName(char *field, std::size_t capacity,
	const std::vector<unsigned char> &name)
{
	if (field == nullptr || capacity == 0)
		return;

	std::memset(field, 0, capacity);

	std::size_t length = name.size();
	if (length >= capacity)
		length = capacity - 1;

	if (length > 0)
		std::memcpy(field, name.data(), length);
}

bool CharacterRecordFromMob(const STRUCT_MOB &mob, const STRUCT_MOBEXTRA &extra,
	const STRUCT_AFFECT *affects, int slot, CharacterRecord *out)
{
	if (out == nullptr)
		return false;

	CharacterRecord record;

	record.live = slot >= 0;
	record.slot = slot;
	record.name = NameBytes(mob.MobName, NAME_LENGTH);
	record.name_key = NameKey(record.name);

	record.clan = SignedByte(mob.Clan);
	record.merchant = UnsignedByte(mob.Merchant);
	record.guild = mob.Guild;
	record.clazz = UnsignedByte(mob.Class);
	record.rsv = mob.Rsv;
	record.quest = UnsignedByte(mob.Quest);
	record.coin = mob.Coin;
	record.exp = mob.Exp;
	record.spx = mob.SPX;
	record.spy = mob.SPY;
	record.learned_skill = mob.LearnedSkill;
	record.magic = static_cast<long long>(mob.Magic);
	record.score_bonus = mob.ScoreBonus;
	record.special_bonus = mob.SpecialBonus;
	record.skill_bonus = mob.SkillBonus;
	record.critical = UnsignedByte(mob.Critical);
	record.save_mana = UnsignedByte(mob.SaveMana);

	for (int i = 0; i < 4; ++i)
		record.skill_bar[i] = mob.SkillBar[i];

	record.guild_level = UnsignedByte(mob.GuildLevel);
	record.regen_hp = mob.RegenHP;
	record.regen_mp = mob.RegenMP;

	for (int i = 0; i < 4; ++i)
		record.resist[i] = SignedByte(mob.Resist[i]);

	record.base_score = ScoreFromStruct(mob.BaseScore);
	record.current_score = ScoreFromStruct(mob.CurrentScore);

	for (int i = 0; i < MAX_EQUIP; ++i)
	{
		if (mob.Equip[i].sIndex != 0)
			record.equipment[i] = ItemFromStruct(mob.Equip[i]);
	}

	for (int i = 0; i < MAX_CARRY; ++i)
	{
		if (mob.Carry[i].sIndex != 0)
			record.inventory[i] = ItemFromStruct(mob.Carry[i]);
	}

	if (affects != nullptr)
	{
		for (int i = 0; i < MAX_AFFECT; ++i)
		{
			if (affects[i].Type == 0)
				continue;

			record.affects[i].type = UnsignedByte(affects[i].Type);
			record.affects[i].value = UnsignedByte(affects[i].Value);
			record.affects[i].level = affects[i].Level;
			record.affects[i].time = static_cast<long long>(affects[i].Time);
		}
	}

	record.extra = ExtraFromStruct(extra);
	record.celestial[0] = CelestialFromStruct(extra, 0);
	record.celestial[1] = CelestialFromStruct(extra, 1);

	*out = record;
	return true;
}

void MobFromCharacterRecord(const CharacterRecord &record, STRUCT_MOB *mob,
	STRUCT_MOBEXTRA *extra, STRUCT_AFFECT *affects,
	unsigned char *short_skill)
{
	if (mob != nullptr)
	{
		STRUCT_MOB value;
		std::memset(&value, 0, sizeof(value));

		WriteName(value.MobName, NAME_LENGTH, record.name);

		value.Clan = ToSignedByte(record.clan);
		value.Merchant = ToUnsignedByte(record.merchant);
		value.Guild = static_cast<unsigned short>(record.guild);
		value.Class = ToUnsignedByte(record.clazz);
		value.Rsv = static_cast<unsigned short>(record.rsv);
		value.Quest = ToUnsignedByte(record.quest);
		value.Coin = record.coin;
		value.Exp = record.exp;
		value.SPX = static_cast<short>(record.spx);
		value.SPY = static_cast<short>(record.spy);
		value.LearnedSkill = static_cast<long>(record.learned_skill);
		value.Magic = static_cast<unsigned int>(record.magic);
		value.ScoreBonus = static_cast<unsigned short>(record.score_bonus);
		value.SpecialBonus = static_cast<unsigned short>(record.special_bonus);
		value.SkillBonus = static_cast<unsigned short>(record.skill_bonus);
		value.Critical = ToUnsignedByte(record.critical);
		value.SaveMana = ToUnsignedByte(record.save_mana);

		for (int i = 0; i < 4; ++i)
			value.SkillBar[i] = record.skill_bar[i];

		value.GuildLevel = ToUnsignedByte(record.guild_level);
		value.RegenHP = static_cast<unsigned short>(record.regen_hp);
		value.RegenMP = static_cast<unsigned short>(record.regen_mp);

		for (int i = 0; i < 4; ++i)
			value.Resist[i] = ToSignedByte(record.resist[i]);

		ScoreToStruct(record.base_score, &value.BaseScore);
		ScoreToStruct(record.current_score, &value.CurrentScore);

		for (int i = 0; i < MAX_EQUIP; ++i)
			ItemToStruct(record.equipment[i], &value.Equip[i]);

		for (int i = 0; i < MAX_CARRY; ++i)
			ItemToStruct(record.inventory[i], &value.Carry[i]);

		*mob = value;
	}

	if (extra != nullptr)
	{
		STRUCT_MOBEXTRA value;
		std::memset(&value, 0, sizeof(value));

		ExtraToStruct(record.extra, &value);
		CelestialToStruct(record.celestial[0], &value, 0);
		CelestialToStruct(record.celestial[1], &value, 1);

		*extra = value;
	}

	if (affects != nullptr)
	{
		std::memset(affects, 0, sizeof(STRUCT_AFFECT) * MAX_AFFECT);

		for (int i = 0; i < MAX_AFFECT; ++i)
		{
			affects[i].Type = ToUnsignedByte(record.affects[i].type);
			affects[i].Value = ToUnsignedByte(record.affects[i].value);
			affects[i].Level = static_cast<unsigned short>(record.affects[i].level);
			affects[i].Time = static_cast<unsigned int>(record.affects[i].time);
		}
	}

	if (short_skill != nullptr)
		std::memset(short_skill, 0, 16);
}

bool AccountRecordFromFile(const STRUCT_ACCOUNTFILE &file, AccountRecord *out)
{
	if (out == nullptr)
		return false;

	AccountRecord record;

	record.name = NameBytes(file.Info.AccountName, ACCOUNTNAME_LENGTH);
	record.name_key = NameKey(record.name);

	record.real_name = NameBytes(file.Info.RealName, REALNAME_LENGTH);
	record.ssn1 = file.Info.SSN1;
	record.ssn2 = file.Info.SSN2;
	record.email = NameBytes(file.Info.Email, EMAIL_LENGTH);
	record.telephone = NameBytes(file.Info.Telephone, TELEPHONE_LENGTH);
	record.address = NameBytes(file.Info.Address, ADDRESS_LENGTH);

	record.block_year = file.Info.Year;
	record.block_year_day = file.Info.YearDay;

	// The legacy sentinel is -1 in the first token byte. Any other value
	// (including all-zero) was a bound token.
	record.has_token = file.Info.NumericToken[0] != -1;

	record.coin = file.Coin;
	record.donate = file.Donate;

	std::memcpy(record.temp_key, file.TempKey, sizeof(record.temp_key));
	std::memcpy(record.short_skill, file.ShortSkill, sizeof(record.short_skill));

	for (int i = 0; i < MAX_CARGO; ++i)
	{
		if (file.Cargo[i].sIndex != 0)
			record.cargo[i] = ItemFromStruct(file.Cargo[i]);
	}

	for (int slot = 0; slot < MOB_PER_ACCOUNT; ++slot)
	{
		if (file.Char[slot].MobName[0] == '\0')
			continue;

		CharacterRecord character;

		if (!CharacterRecordFromMob(file.Char[slot], file.mobExtra[slot],
				file.affect[slot], slot, &character))
			return false;

		record.characters.push_back(character);
	}

	*out = record;
	return true;
}

void AccountFileFromRecord(const AccountRecord &record, STRUCT_ACCOUNTFILE *out)
{
	if (out == nullptr)
		return;

	std::memset(out, 0, sizeof(*out));

	WriteName(out->Info.AccountName, ACCOUNTNAME_LENGTH, record.name);
	WriteName(out->Info.RealName, REALNAME_LENGTH, record.real_name);
	WriteName(out->Info.Email, EMAIL_LENGTH, record.email);
	WriteName(out->Info.Telephone, TELEPHONE_LENGTH, record.telephone);
	WriteName(out->Info.Address, ADDRESS_LENGTH, record.address);

	out->Info.SSN1 = record.ssn1;
	out->Info.SSN2 = record.ssn2;
	out->Info.Year = record.block_year;
	out->Info.YearDay = record.block_year_day;

	// AccountPass stays zero: plaintext and hashes are never copied into the
	// in-memory/wire struct. NumericToken keeps the legacy sentinel so the
	// remaining code can tell "unset" from "bound".
	out->Info.NumericToken[0] = record.has_token ? 0 : static_cast<char>(-1);

	out->Coin = record.coin;
	out->Donate = record.donate;

	std::memcpy(out->TempKey, record.temp_key, sizeof(out->TempKey));
	std::memcpy(out->ShortSkill, record.short_skill, sizeof(out->ShortSkill));

	for (int i = 0; i < MAX_CARGO; ++i)
		ItemToStruct(record.cargo[i], &out->Cargo[i]);

	for (const CharacterRecord &character : record.characters)
	{
		if (!character.live || character.slot < 0 || character.slot >= MOB_PER_ACCOUNT)
			continue;

		STRUCT_MOB mob;
		STRUCT_MOBEXTRA extra;
		STRUCT_AFFECT affects[MAX_AFFECT];

		MobFromCharacterRecord(character, &mob, &extra, affects, nullptr);

		out->Char[character.slot] = mob;
		out->mobExtra[character.slot] = extra;
		std::memcpy(out->affect[character.slot], affects, sizeof(affects));
	}
}

void NormalizeAccountFile(const STRUCT_ACCOUNTFILE &source,
	STRUCT_ACCOUNTFILE *normalized)
{
	AccountRecord record;

	if (!AccountRecordFromFile(source, &record))
	{
		std::memset(normalized, 0, sizeof(*normalized));
		return;
	}

	AccountFileFromRecord(record, normalized);
}

unsigned long long StructuralChecksum(const STRUCT_ACCOUNTFILE &file)
{
	STRUCT_ACCOUNTFILE normalized;

	NormalizeAccountFile(file, &normalized);

	const unsigned char *bytes =
		reinterpret_cast<const unsigned char *>(&normalized);

	unsigned long long hash = 1469598103934665603ULL; // FNV offset basis

	for (std::size_t i = 0; i < sizeof(normalized); ++i)
	{
		hash ^= bytes[i];
		hash *= 1099511628211ULL; // FNV prime
	}

	return hash;
}

} // namespace storage
