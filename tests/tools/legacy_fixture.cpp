/*
 * Write a minimal legacy account/char tree for importer and login smoke tests.
 *
 * usage: legacy_fixture <root> [account] [password] [character]
 *
 * Produces:
 *   <root>/account/<bucket>/<ACCOUNT>  (7808-byte STRUCT_ACCOUNTFILE)
 *   <root>/char/<bucket>/<CHARACTER>   (16-byte owning account marker)
 *   <root>/Config.txt                  (Sapphire + LastCapsule 0)
 */

#include <filesystem>
#include <fstream>
#include <string>

#include <cstdio>
#include <cstring>

#include "../../Code/Basedef.h"

namespace
{

namespace fs = std::filesystem;

std::string Bucket(const char *name)
{
	char source[NAME_LENGTH + 1];
	char key[16] = {0};

	std::strncpy(source, name, NAME_LENGTH);
	source[NAME_LENGTH] = '\0';
	BASE_GetFirstKey(source, key);

	return std::string(key);
}

}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: legacy_fixture <root> [account] [password] [character]\n");
		return 2;
	}

	const fs::path root = argv[1];
	const std::string account = argc > 2 ? argv[2] : "E2E";
	const std::string password = argc > 3 ? argv[3] : "hunter2";
	const std::string character = argc > 4 ? argv[4] : "E2ECHAR";

	STRUCT_ACCOUNTFILE file;
	std::memset(&file, 0, sizeof(file));

	std::strncpy(file.Info.AccountName, account.c_str(), ACCOUNTNAME_LENGTH - 1);
	std::strncpy(file.Info.AccountPass, password.c_str(), ACCOUNTPASS_LENGTH - 1);
	file.Info.NumericToken[0] = -1;
	std::strncpy(file.Info.RealName, "E2E Fixture", REALNAME_LENGTH - 1);
	std::strncpy(file.Info.Email, "e2e@example.test", EMAIL_LENGTH - 1);
	file.Info.SSN1 = 1;
	file.Info.SSN2 = 2;
	file.Coin = 1000;
	file.Donate = 0;

	std::strncpy(file.Char[0].MobName, character.c_str(), NAME_LENGTH - 1);
	file.Char[0].Class = 0;
	file.Char[0].BaseScore.Level = 20;
	file.Char[0].CurrentScore.Level = 20;
	file.Char[0].CurrentScore.Hp = 100;
	file.Char[0].Exp = 5000;
	file.Char[0].Equip[0].sIndex = 100;
	file.Char[0].Carry[0].sIndex = 200;
	file.Cargo[0].sIndex = 401;

	file.mobExtra[0].ClassMaster = MORTAL;
	file.mobExtra[0].Fame = 5;

	std::error_code error;
	fs::create_directories(root / "account" / Bucket(account.c_str()), error);
	fs::create_directories(root / "char" / Bucket(character.c_str()), error);

	{
		std::ofstream stream(root / "account" / Bucket(account.c_str()) / account,
			std::ios::binary | std::ios::trunc);
		stream.write(reinterpret_cast<const char *>(&file), sizeof(file));
	}

	{
		char marker[ACCOUNTNAME_LENGTH] = {0};
		std::strncpy(marker, account.c_str(), sizeof(marker) - 1);

		std::ofstream stream(root / "char" / Bucket(character.c_str()) / character,
			std::ios::binary | std::ios::trunc);
		stream.write(marker, sizeof(marker));
	}

	{
		std::ofstream stream(root / "Config.txt", std::ios::trunc);
		stream << "Sapphire 8\nLastCapsule 0\n";
	}

	std::printf("fixture written to %s\n", root.string().c_str());
	return 0;
}
