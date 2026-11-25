/*
 * Row-shaped records exchanged between the repository and the codec.
 *
 * They are plain data: no libpq, no legacy structs. The codec converts
 * between these records and STRUCT_ACCOUNTFILE/STRUCT_MOB, which stay the
 * in-memory and wire representation.
 */

#ifndef W2PP_STORAGE_RECORDS_H
#define W2PP_STORAGE_RECORDS_H

#include <string>
#include <vector>

namespace storage
{

struct ItemSlot
{
	int index = 0;
	int effect[3] = {0, 0, 0};
};

struct ScoreRecord
{
	int level = 0;
	int ac = 0;
	int damage = 0;
	int merchant = 0;
	int attack_run = 0;
	int direction = 0;
	int chaos_rate = 0;
	int max_hp = 0;
	int max_mp = 0;
	int hp = 0;
	int mp = 0;
	int str = 0;
	int int_stat = 0;
	int dex = 0;
	int con = 0;
	int special[4] = {0, 0, 0, 0};
};

struct AffectRecord
{
	int type = 0;
	int value = 0;
	int level = 0;
	long long time = 0;
};

struct ExtraRecord
{
	int class_master = 0;
	int citizen = 0;
	int fame = 0;
	int soul = 0;
	int mortal_face = 0;

	int quest_newbie = 0;
	int quest_terra_mistica = 0;
	int quest_molar_gargula = 0;
	int quest_pilula_orc = 0;

	int arch_mortal_slot = 0;
	int arch_mortal_level = 0;
	int arch_level_355 = 0;
	int arch_level_370 = 0;
	int arch_cristal = 0;

	int celestial_arch_level = 0;
	int celestial_celestial_level = 0;
	int celestial_sub_celestial_level = 0;
	int celestial_lv40 = 0;
	int celestial_lv90 = 0;
	int celestial_add120 = 0;
	int celestial_add150 = 0;
	int celestial_add180 = 0;
	int celestial_add200 = 0;
	int celestial_arcana = 0;
	int celestial_reset = 0;

	int circle = 0;

	long long last_nt = 0;
	int nt = 0;
	int kefra_ticket = 0;
	long long divine_end = 0;
	long long hold = 0;

	long long daylog_exp = 0;
	int daylog_year_day = 0;

	long long donate_last_time = 0;
	int donate_count = 0;
};

struct CelestialRecord
{
	int clazz = 0;
	long long exp = 0;
	int spx = 0;
	int spy = 0;
	ScoreRecord base_score;
	long long learned_skill = 0;
	int score_bonus = 0;
	int special_bonus = 0;
	int skill_bonus = 0;
	unsigned char skill_bar1[4] = {0, 0, 0, 0};
	unsigned char skill_bar2[16] = {0};
	int soul = 0;
};

struct CharacterRecord
{
	long long character_id = 0;
	bool live = false;       // owns an account slot (vs sealed capsule)
	int slot = -1;
	int capsule_id = 0;

	std::vector<unsigned char> name;
	std::vector<unsigned char> name_key;

	int clan = 0;
	int merchant = 0;
	int guild = 0;
	int clazz = 0;
	int rsv = 0;
	int quest = 0;
	int coin = 0;
	long long exp = 0;
	int spx = 0;
	int spy = 0;
	long long learned_skill = 0;
	long long magic = 0;
	int score_bonus = 0;
	int special_bonus = 0;
	int skill_bonus = 0;
	int critical = 0;
	int save_mana = 0;
	unsigned char skill_bar[4] = {0, 0, 0, 0};
	int guild_level = 0;
	int regen_hp = 0;
	int regen_mp = 0;
	int resist[4] = {0, 0, 0, 0};

	ScoreRecord base_score;
	ScoreRecord current_score;

	ItemSlot equipment[16];
	ItemSlot inventory[64];
	AffectRecord affects[32];
	ExtraRecord extra;
	CelestialRecord celestial[2];
};

struct AccountRecord
{
	long long account_id = 0;
	std::vector<unsigned char> name;
	std::vector<unsigned char> name_key;
	std::string password_hash;
	std::string token_hash;
	bool has_token = false;
	int state = 0;

	std::vector<unsigned char> real_name;
	int ssn1 = 0;
	int ssn2 = 0;
	std::vector<unsigned char> email;
	std::vector<unsigned char> telephone;
	std::vector<unsigned char> address;

	int block_year = 0;
	int block_year_day = 0;
	unsigned char temp_key[52] = {0};
	int coin = 0;
	int donate = 0;
	unsigned char short_skill[64] = {0};

	std::vector<CharacterRecord> characters;
	ItemSlot cargo[128];

	int password_failures = 0;
	int token_failures = 0;
};

}

#endif
