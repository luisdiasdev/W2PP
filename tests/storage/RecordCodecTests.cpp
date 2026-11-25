/*
 * Record codec unit tests: struct <-> row mapping, sparse arrays, signed and
 * wide fields, normalization/checksum and the guarded record sizes.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <vector>

#include "AbiGuards.h"
#include "Basedef.h"
#include "storage/RecordCodec.h"

namespace
{

void FillScore(STRUCT_SCORE *score, int seed)
{
	score->Level = seed + 1;
	score->Ac = seed + 2;
	score->Damage = seed + 3;
	score->Merchant = static_cast<unsigned char>(seed + 4);
	score->AttackRun = static_cast<unsigned char>(seed + 5);
	score->Direction = static_cast<unsigned char>(seed + 6);
	score->ChaosRate = static_cast<unsigned char>(seed + 7);
	score->MaxHp = seed + 8;
	score->MaxMp = seed + 9;
	score->Hp = seed + 10;
	score->Mp = seed + 11;
	score->Str = static_cast<short>(seed + 12);
	score->Int = static_cast<short>(seed + 13);
	score->Dex = static_cast<short>(seed + 14);
	score->Con = static_cast<short>(seed + 15);
	score->Special[0] = static_cast<short>(seed + 16);
	score->Special[1] = static_cast<short>(seed + 17);
	score->Special[2] = static_cast<short>(seed + 18);
	score->Special[3] = static_cast<short>(seed + 19);
}

void FillItem(STRUCT_ITEM *item, int index, int effectSeed)
{
	item->sIndex = static_cast<short>(index);
	item->stEffect[0].cEffect = static_cast<unsigned char>(effectSeed);
	item->stEffect[0].cValue = static_cast<unsigned char>(effectSeed + 1);
	item->stEffect[1].cEffect = static_cast<unsigned char>(effectSeed + 2);
	item->stEffect[1].cValue = static_cast<unsigned char>(effectSeed + 3);
	item->stEffect[2].cEffect = static_cast<unsigned char>(effectSeed + 4);
	item->stEffect[2].cValue = static_cast<unsigned char>(effectSeed + 5);
}

void FillExtra(STRUCT_MOBEXTRA *extra, int seed)
{
	extra->ClassMaster = static_cast<short>(seed);
	extra->Citizen = static_cast<char>(-1);
	extra->Fame = seed * 10;
	extra->Soul = static_cast<char>(-2);
	extra->MortalFace = static_cast<short>(seed + 1);

	extra->QuestInfo.Mortal.Newbie = 1;
	extra->QuestInfo.Mortal.TerraMistica = 2;
	extra->QuestInfo.Mortal.MolarGargula = 3;
	extra->QuestInfo.Mortal.PilulaOrc = 4;

	extra->QuestInfo.Arch.MortalSlot = 5;
	extra->QuestInfo.Arch.MortalLevel = 6;
	extra->QuestInfo.Arch.Level355 = 7;
	extra->QuestInfo.Arch.Level370 = 8;
	extra->QuestInfo.Arch.Cristal = 9;

	extra->QuestInfo.Celestial.ArchLevel = static_cast<short>(seed + 10);
	extra->QuestInfo.Celestial.CelestialLevel = static_cast<short>(seed + 11);
	extra->QuestInfo.Celestial.SubCelestialLevel = static_cast<short>(seed + 12);
	extra->QuestInfo.Celestial.Lv40 = 10;
	extra->QuestInfo.Celestial.Lv90 = 11;
	extra->QuestInfo.Celestial.Add120 = 12;
	extra->QuestInfo.Celestial.Add150 = 13;
	extra->QuestInfo.Celestial.Add180 = 14;
	extra->QuestInfo.Celestial.Add200 = 15;
	extra->QuestInfo.Celestial.Arcana = 16;
	extra->QuestInfo.Celestial.Reset = 17;

	extra->QuestInfo.Circle = 18;

	extra->LastNT = static_cast<time_t>(0x1122334455667788LL);
	extra->NT = 123456;
	extra->KefraTicket = 7;
	extra->DivineEnd = static_cast<time_t>(0x0102030405060708LL);
	extra->Hold = 0xFEDCBA98u;

	extra->DayLog.Exp = 0x0011223344556677LL;
	extra->DayLog.YearDay = 321;

	extra->DonateInfo.LastTime = static_cast<time_t>(0x1020304050607080LL);
	extra->DonateInfo.Count = 42;

	for (int i = 0; i < 2; ++i)
	{
		auto &save = extra->SaveCelestial[i];
		save.Class = seed + i;
		save.Exp = 1000LL * (seed + i);
		save.SPX = static_cast<short>(seed + i);
		save.SPY = static_cast<short>(seed + i + 1);
		FillScore(&save.BaseScore, seed + i);
		save.LearnedSkill = 0x0A0B0C0D;
		save.ScoreBonus = static_cast<unsigned short>(300 + i);
		save.SpecialBonus = static_cast<unsigned short>(400 + i);
		save.SkillBonus = static_cast<unsigned short>(500 + i);

		for (int b = 0; b < 4; ++b)
			save.SkillBar1[b] = static_cast<unsigned char>(b + i);
		for (int b = 0; b < 16; ++b)
			save.SkillBar2[b] = static_cast<unsigned char>(b * 2 + i);

		save.Soul = static_cast<char>(-3);
	}
}

void FillCharacter(STRUCT_MOB *mob, STRUCT_MOBEXTRA *extra,
	STRUCT_AFFECT *affects, unsigned char *shortSkill, int slot)
{
	const char *names[4] = {"ALPHA", "BRAVO", "CHARLIE", "DELTA"};
	std::strncpy(mob->MobName, names[slot], NAME_LENGTH);

	mob->Clan = static_cast<char>(-1);
	mob->Merchant = 3;
	mob->Guild = 60000;
	mob->Class = static_cast<unsigned char>(slot + 1);
	mob->Rsv = 65535;
	mob->Quest = 9;
	mob->Coin = -1234;
	mob->Exp = 0x0011223344556677LL;
	mob->SPX = -500;
	mob->SPY = 500;
	FillScore(&mob->BaseScore, slot * 20);
	FillScore(&mob->CurrentScore, slot * 20 + 10);
	mob->LearnedSkill = 0x7F00FF00;
	mob->Magic = 0xFFFFFFFFu;
	mob->ScoreBonus = 60000;
	mob->SpecialBonus = 60001;
	mob->SkillBonus = 60002;
	mob->Critical = 200;
	mob->SaveMana = 201;
	mob->SkillBar[0] = 1;
	mob->SkillBar[1] = 2;
	mob->SkillBar[2] = 3;
	mob->SkillBar[3] = 4;
	mob->GuildLevel = 5;
	mob->RegenHP = 60000;
	mob->RegenMP = 60001;
	mob->Resist[0] = static_cast<char>(-128);
	mob->Resist[1] = static_cast<char>(-1);
	mob->Resist[2] = 1;
	mob->Resist[3] = 127;

	for (int i = 0; i < MAX_EQUIP; ++i)
		FillItem(&mob->Equip[i], 100 + slot * 100 + i, i * 3);

	for (int i = 0; i < MAX_CARRY; ++i)
		FillItem(&mob->Carry[i], 200 + slot * 100 + i, i * 5);

	for (int i = 0; i < MAX_AFFECT; ++i)
	{
		affects[i].Type = static_cast<unsigned char>(i + 1);
		affects[i].Value = static_cast<unsigned char>(i);
		affects[i].Level = static_cast<unsigned short>(i * 100);
		affects[i].Time = i == 0 ? 0xFFFFFFFFu : static_cast<unsigned int>(i * 1000);
	}

	for (int i = 0; i < 16; ++i)
		shortSkill[i] = static_cast<unsigned char>(slot * 16 + i);

	FillExtra(extra, slot + 1);
}

void PopulateAccount(STRUCT_ACCOUNTFILE *file)
{
	std::strncpy(file->Info.AccountName, "TESTACCOUNT", ACCOUNTNAME_LENGTH);
	// Credentials are deliberately empty at rest in the in-memory struct.
	file->Info.AccountPass[0] = '\0';
	file->Info.NumericToken[0] = -1; // unset token sentinel

	std::strncpy(file->Info.RealName, "Real Name", REALNAME_LENGTH);
	file->Info.SSN1 = 111222333;
	file->Info.SSN2 = 444555;
	std::strncpy(file->Info.Email, "user@example.test", EMAIL_LENGTH);
	std::strncpy(file->Info.Telephone, "555-0100", TELEPHONE_LENGTH);
	std::strncpy(file->Info.Address, "1 Example Street", ADDRESS_LENGTH);
	file->Info.Year = 124;
	file->Info.YearDay = 200;

	file->Coin = 0x7FFFFFFF;
	file->Donate = -5;

	for (int i = 0; i < 52; ++i)
		file->TempKey[i] = static_cast<char>(i);

	for (int slot = 0; slot < MOB_PER_ACCOUNT; ++slot)
		FillCharacter(&file->Char[slot], &file->mobExtra[slot],
			file->affect[slot], file->ShortSkill[slot], slot);

	for (int i = 0; i < MAX_CARGO; ++i)
		FillItem(&file->Cargo[i], 300 + i, i * 7);
}

} // namespace

TEST_CASE("account scalar and array round-trip", "[codec]")
{
	STRUCT_ACCOUNTFILE original;
	std::memset(&original, 0, sizeof(original));
	PopulateAccount(&original);

	storage::AccountRecord record;
	REQUIRE(storage::AccountRecordFromFile(original, &record));
	REQUIRE(record.characters.size() == MOB_PER_ACCOUNT);

	STRUCT_ACCOUNTFILE rebuilt;
	storage::AccountFileFromRecord(record, &rebuilt);

	if (std::memcmp(&original, &rebuilt, sizeof(original)) != 0)
	{
		const unsigned char *a = reinterpret_cast<const unsigned char *>(&original);
		const unsigned char *b = reinterpret_cast<const unsigned char *>(&rebuilt);
		for (std::size_t i = 0; i < sizeof(original); ++i)
		{
			if (a[i] != b[i])
			{
				WARN("first diff at offset " << i << ": " << static_cast<int>(a[i])
					<< " vs " << static_cast<int>(b[i]));
				break;
			}
		}
	}

	REQUIRE(std::memcmp(&original, &rebuilt, sizeof(original)) == 0);
}

TEST_CASE("all-zero account round-trip", "[codec]")
{
	STRUCT_ACCOUNTFILE original;
	std::memset(&original, 0, sizeof(original));
	std::strncpy(original.Info.AccountName, "ZERO", ACCOUNTNAME_LENGTH);
	original.Info.NumericToken[0] = -1;

	storage::AccountRecord record;
	REQUIRE(storage::AccountRecordFromFile(original, &record));

	STRUCT_ACCOUNTFILE rebuilt;
	storage::AccountFileFromRecord(record, &rebuilt);

	REQUIRE(std::memcmp(&original, &rebuilt, sizeof(original)) == 0);
}

TEST_CASE("sparse item arrays keep positions", "[codec]")
{
	STRUCT_ACCOUNTFILE original;
	std::memset(&original, 0, sizeof(original));
	std::strncpy(original.Info.AccountName, "SPARSE", ACCOUNTNAME_LENGTH);
	original.Info.NumericToken[0] = -1;

	// Equipment positions 3 and 9 stay empty; the rest are populated.
	for (int i = 0; i < MAX_EQUIP; ++i)
	{
		if (i == 3 || i == 9)
			continue;
		FillItem(&original.Char[0].Equip[i], 700 + i, i);
	}
	std::strncpy(original.Char[0].MobName, "SPARSE", NAME_LENGTH);

	// Cargo only at positions 0 and 127.
	FillItem(&original.Cargo[0], 401, 1);
	FillItem(&original.Cargo[127], 406, 2);

	storage::AccountRecord record;
	REQUIRE(storage::AccountRecordFromFile(original, &record));

	REQUIRE(record.cargo[0].index == 401);
	REQUIRE(record.cargo[127].index == 406);
	for (int i = 1; i < 127; ++i)
		REQUIRE(record.cargo[i].index == 0);

	STRUCT_ACCOUNTFILE rebuilt;
	storage::AccountFileFromRecord(record, &rebuilt);

	REQUIRE(std::memcmp(&original, &rebuilt, sizeof(original)) == 0);
}

TEST_CASE("duplicate item indices keep their own effects", "[codec]")
{
	STRUCT_ACCOUNTFILE original;
	std::memset(&original, 0, sizeof(original));
	std::strncpy(original.Info.AccountName, "DUPES", ACCOUNTNAME_LENGTH);
	original.Info.NumericToken[0] = -1;

	FillItem(&original.Char[0].Equip[0], 500, 10);
	FillItem(&original.Char[0].Equip[1], 500, 100);
	std::strncpy(original.Char[0].MobName, "DUPES", NAME_LENGTH);

	storage::AccountRecord record;
	REQUIRE(storage::AccountRecordFromFile(original, &record));
	REQUIRE(record.characters.size() == 1);
	REQUIRE(record.characters[0].equipment[0].index == 500);
	REQUIRE(record.characters[0].equipment[1].index == 500);
	REQUIRE(record.characters[0].equipment[0].effect[0]
		== original.Char[0].Equip[0].stEffect[0].sValue);
	REQUIRE(record.characters[0].equipment[1].effect[0]
		== original.Char[0].Equip[1].stEffect[0].sValue);
	REQUIRE(record.characters[0].equipment[0].effect[0]
		!= record.characters[0].equipment[1].effect[0]);
}

TEST_CASE("empty arrays reconstruct as zeroed entries", "[codec]")
{
	STRUCT_ACCOUNTFILE original;
	std::memset(&original, 0, sizeof(original));
	std::strncpy(original.Info.AccountName, "EMPTYARRAYS", ACCOUNTNAME_LENGTH);
	original.Info.NumericToken[0] = -1;
	std::strncpy(original.Char[0].MobName, "EMPTYARRAYS", NAME_LENGTH);

	storage::AccountRecord record;
	REQUIRE(storage::AccountRecordFromFile(original, &record));

	REQUIRE(record.characters.size() == 1);
	for (int i = 0; i < MAX_EQUIP; ++i)
		REQUIRE(record.characters[0].equipment[i].index == 0);
	for (int i = 0; i < MAX_CARRY; ++i)
		REQUIRE(record.characters[0].inventory[i].index == 0);
	for (int i = 0; i < MAX_AFFECT; ++i)
		REQUIRE(record.characters[0].affects[i].type == 0);
}

TEST_CASE("affects, extras and celestial saves round-trip", "[codec]")
{
	STRUCT_ACCOUNTFILE original;
	std::memset(&original, 0, sizeof(original));
	std::strncpy(original.Info.AccountName, "EXTRAS", ACCOUNTNAME_LENGTH);
	original.Info.NumericToken[0] = -1;
	FillCharacter(&original.Char[0], &original.mobExtra[0],
		original.affect[0], original.ShortSkill[0], 2);

	storage::AccountRecord record;
	REQUIRE(storage::AccountRecordFromFile(original, &record));
	REQUIRE(record.characters.size() == 1);

	const storage::CharacterRecord &character = record.characters[0];
	REQUIRE(character.resist[0] == -128);
	REQUIRE(character.resist[1] == -1);
	REQUIRE(character.resist[2] == 1);
	REQUIRE(character.resist[3] == 127);
	REQUIRE(character.affects[0].time == 0xFFFFFFFFLL);
	REQUIRE(character.affects[0].type == 1);
	REQUIRE(character.extra.last_nt == static_cast<long long>(0x1122334455667788LL));
	REQUIRE(character.extra.hold == 0xFEDCBA98LL);
	REQUIRE(character.celestial[0].clazz == 3);
	REQUIRE(character.celestial[1].clazz == 4);

	STRUCT_ACCOUNTFILE rebuilt;
	storage::AccountFileFromRecord(record, &rebuilt);
	REQUIRE(std::memcmp(&original, &rebuilt, sizeof(original)) == 0);
}

TEST_CASE("normalization zeroes padding and legacy unused bytes", "[codec]")
{
	STRUCT_ACCOUNTFILE garbage;
	std::memset(&garbage, 0xAA, sizeof(garbage));
	PopulateAccount(&garbage);

	STRUCT_ACCOUNTFILE clean;
	std::memset(&clean, 0, sizeof(clean));
	PopulateAccount(&clean);

	STRUCT_ACCOUNTFILE normalized;
	storage::NormalizeAccountFile(garbage, &normalized);

	REQUIRE(std::memcmp(&normalized, &clean, sizeof(clean)) == 0);

	// The named legacy EMPTY arrays are zero in the reconstruction.
	for (unsigned char byte : normalized.mobExtra[0].QuestInfo.Mortal.EMPTY)
		REQUIRE(byte == 0);
	for (unsigned char byte : normalized.mobExtra[0].QuestInfo.Arch.EMPTY)
		REQUIRE(byte == 0);
	for (unsigned char byte : normalized.mobExtra[0].QuestInfo.Celestial.EMPTY)
		REQUIRE(byte == 0);
	for (unsigned char byte : normalized.mobExtra[0].QuestInfo.EMPTY)
		REQUIRE(byte == 0);
	for (unsigned char byte : normalized.mobExtra[0].SaveCelestial[0].EMPTY)
		REQUIRE(byte == 0);
	for (int value : normalized.mobExtra[0].EMPTY)
		REQUIRE(value == 0);

	// Defined fields survive normalization unchanged.
	REQUIRE(std::memcmp(normalized.Char[0].MobName, clean.Char[0].MobName, NAME_LENGTH) == 0);
	REQUIRE(normalized.Char[0].Exp == clean.Char[0].Exp);
	REQUIRE(normalized.mobExtra[0].Fame == clean.mobExtra[0].Fame);
	REQUIRE(normalized.Cargo[127].sIndex == clean.Cargo[127].sIndex);
}

TEST_CASE("structural checksum ignores padding", "[codec]")
{
	STRUCT_ACCOUNTFILE clean;
	std::memset(&clean, 0, sizeof(clean));
	PopulateAccount(&clean);

	STRUCT_ACCOUNTFILE garbage;
	std::memset(&garbage, 0x5A, sizeof(garbage));
	PopulateAccount(&garbage);

	REQUIRE(storage::StructuralChecksum(clean) == storage::StructuralChecksum(garbage));

	clean.Cargo[0].sIndex = static_cast<short>(clean.Cargo[0].sIndex + 1);
	REQUIRE(storage::StructuralChecksum(clean) != storage::StructuralChecksum(garbage));
}

TEST_CASE("guarded sizes and offsets are unchanged", "[codec][abi]")
{
	STATIC_REQUIRE(sizeof(STRUCT_ACCOUNTFILE) == 7808);
	STATIC_REQUIRE(sizeof(STRUCT_MOB) == 816);
	STATIC_REQUIRE(sizeof(STRUCT_MOBEXTRA) == 536);
	STATIC_REQUIRE(sizeof(STRUCT_CAPSULE) == 1352);
	STATIC_REQUIRE(offsetof(STRUCT_ACCOUNTFILE, Char) == 224);
	STATIC_REQUIRE(offsetof(STRUCT_ACCOUNTFILE, Cargo) == 3488);
	STATIC_REQUIRE(offsetof(STRUCT_ACCOUNTFILE, mobExtra) == 5608);
	STATIC_REQUIRE(offsetof(STRUCT_CAPSULE, extra) == 816);

	STRUCT_ACCOUNTFILE record;
	std::memset(&record, 0, sizeof(record));
	std::strncpy(record.Info.AccountName, "ABI", ACCOUNTNAME_LENGTH);
	record.Info.NumericToken[0] = -1;

	STRUCT_ACCOUNTFILE rebuilt;
	storage::NormalizeAccountFile(record, &rebuilt);
	REQUIRE(sizeof(rebuilt) == 7808);
}
