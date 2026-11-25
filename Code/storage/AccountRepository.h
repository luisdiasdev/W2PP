/*
 * Account/character/capsule repository for DBSrv.
 *
 * Every externally triggered mutation runs in one transaction. Credential
 * columns are only written through the credential operations; a normal
 * account save never overwrites a hash, the token or the account state.
 */

#ifndef W2PP_STORAGE_ACCOUNTREPOSITORY_H
#define W2PP_STORAGE_ACCOUNTREPOSITORY_H

#include <string>
#include <vector>

#include "Database.h"
#include "Records.h"
#include "StorageConfig.h"

struct STRUCT_ACCOUNTFILE;

namespace storage
{

enum class LoginStatus
{
	Success,
	NotFound,
	WrongPassword,
	Blocked,
	Locked,
	TempKeyMismatch,
	Error
};

class AccountRepository
{
public:
	AccountRepository(Database &database, const StorageConfig &config);

	// --- Account load/save -------------------------------------------------

	// Uppercase-insensitive lookup by raw name bytes. `out` is fully populated
	// (live characters only; capsules are loaded separately).
	bool LoadAccount(const std::vector<unsigned char> &name,
		AccountRecord *out, bool *found, std::string *error);

	// Legacy-shaped load by the name in file->Info.AccountName.
	bool LoadAccountFile(const STRUCT_ACCOUNTFILE &key, STRUCT_ACCOUNTFILE *out,
		bool *found, std::string *error);

	/*
	 * Persist account scalars, every live character, cargo and child arrays.
	 * Existing credential columns are preserved. When the account row does not
	 * exist and `allow_insert` is true, file.Info.AccountPass is hashed with the
	 * configured cost; otherwise the call fails.
	 */
	bool SaveAccountFile(const STRUCT_ACCOUNTFILE &file, bool allow_insert,
		bool *inserted, std::string *error);

	// --- Character primitives ---------------------------------------------

	bool CharacterNameAvailable(const std::vector<unsigned char> &name,
		bool *available, std::string *error);

	bool DeleteCharacterByName(long long account_id,
		const std::vector<unsigned char> &name, std::string *error);

	bool FindAccountByCharacterName(const std::vector<unsigned char> &name,
		std::vector<unsigned char> *account, bool *found, std::string *error);

	// --- Soul capsules -----------------------------------------------------

	// Seal the live character in `slot`. Returns the allocated capsule id.
	bool PutInCapsule(long long account_id, int slot, int *capsule_id,
		std::string *error);

	// Re-link a sealed character into `slot` under `new_name`.
	bool TakeOutCapsule(long long account_id, int capsule_id, int slot,
		const std::vector<unsigned char> &new_name, std::string *error);

	bool LoadCapsule(int capsule_id, CharacterRecord *out, bool *found,
		std::string *error);

	// Import one legacy capsule as a sealed character row (no account link).
	bool ImportCapsule(int capsule_id, const CharacterRecord &character,
		std::string *error);

	// Seed capsule_id_seq so the next allocated id is above `last_id`.
	bool SetCapsuleSequence(int last_id, std::string *error);

	// --- Credentials / login ----------------------------------------------

	/*
	 * One transaction: account row lock, date/state checks, temp-key bypass and
	 * clear, coin clamp persistence, password verification with throttling and
	 * transparent hash upgrade, then the full account load.
	 */
	LoginStatus AdmitLogin(const std::vector<unsigned char> &name,
		const std::string &password, const std::string &temp_key,
		AccountRecord *out, bool *temp_key_used, std::string *error);

	// Verify a password for an already identified account (character deletion,
	// admin login). Applies the same throttling and hash upgrade as a login.
	bool VerifyPassword(long long account_id, const std::string &password,
		bool *matched, std::string *error);

	bool ChangePassword(long long account_id, const std::string &new_password,
		std::string *error);

	// Explicit account state (0 normal, 1 blocked, 2 deleted, 3 disabled).
	bool SetState(long long account_id, int state, std::string *error);

	bool BindToken(long long account_id, const std::string &token, bool change,
		std::string *error);

	bool VerifyToken(long long account_id, const std::string &token,
		bool *matched, std::string *error);

	// --- Reports and export ------------------------------------------------

	struct DayLogEntry
	{
		std::vector<unsigned char> account;
		std::vector<unsigned char> character;
		long long exp = 0;
	};

	// Characters whose day-log year-day equals `year_day` on accounts updated
	// in the current local month.
	bool DayLogEntries(int year_day, std::vector<DayLogEntry> *out,
		std::string *error);

	struct RankingEntry
	{
		std::vector<unsigned char> name;
		long long exp = 0;
		int class_master = 0;
		int level = 0;
		int clazz = 0;
	};

	// Live characters of accounts updated in the legacy ranking window.
	bool RankingEntries(std::vector<RankingEntry> *out, std::string *error);

	// Same as LoadAccountFile but always zeroes the credential fields.
	bool ExportAccount(const std::vector<unsigned char> &name,
		STRUCT_ACCOUNTFILE *out, bool *found, std::string *error);

private:
	bool LoadCharacters(long long account_id, AccountRecord *out,
		std::string *error);
	bool LoadCharacterChildren(CharacterRecord *character,
		std::string *error);
	bool LoadCargo(long long account_id, AccountRecord *out,
		std::string *error);

	bool SaveCharacters(long long account_id, const AccountRecord &record,
		std::string *error);
	bool ReplaceItems(const char *table, const char *ownerColumn,
		long long ownerId, const ItemSlot *slots, int count,
		std::string *error);
	bool ReplaceAffects(long long characterId, const CharacterRecord &record,
		std::string *error);
	bool UpsertExtra(long long characterId, const ExtraRecord &extra,
		std::string *error);
	bool UpsertCelestial(long long characterId, int slot,
		const CelestialRecord &celestial, std::string *error);

	bool LoadCharacterScalars(long long characterId, CharacterRecord *out,
		std::string *error);

	std::string NameKeyFor(const std::vector<unsigned char> &name) const;

	Database &database_;
	StorageConfig config_;
};

}

#endif
