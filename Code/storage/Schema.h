/*
 * Schema versioning and forward migrations for the PostgreSQL store.
 *
 * Migrations are compile-time constants. At startup the runner takes an
 * advisory lock, ensures the schema exists, compares the recorded contract
 * version with the binary, and applies each pending step in its own
 * transaction. A failing step rolls back and leaves the previous version
 * recorded.
 */

#ifndef W2PP_STORAGE_SCHEMA_H
#define W2PP_STORAGE_SCHEMA_H

#include <string>
#include <vector>

#include "Database.h"

namespace storage
{

struct MigrationStep
{
	int version;
	std::string name;
	std::string sql;
};

// The steps this binary knows, in ascending version order.
const std::vector<MigrationStep> &DefaultMigrations();

// Advisory lock key used to serialize migration runs (ASCII "w2pp").
constexpr long long kMigrationLockKey = 0x77327070;

// Separate lock so an import cannot race another importer.
constexpr long long kImportLockKey = 0x77327071;

/*
 * Create the schema objects (if absent), apply pending steps and record each
 * version. On success `appliedVersion` is the highest applied version.
 */
bool ApplyMigrations(Database &database,
	const std::vector<MigrationStep> &steps,
	std::string *error, int *appliedVersion);

// Resume with the shipped steps.
bool Migrate(Database &database, std::string *error, int *appliedVersion);

// Highest applied version, 0 when the version table is absent.
int CurrentVersion(Database &database, std::string *error);

/*
 * Verify that the database's schema contract is not newer than this binary.
 * Returns false with a message telling the operator to upgrade the binary.
 */
bool CheckSchemaContract(Database &database, std::string *error);

// Read/write store_metadata values (import_state, schema_contract).
bool GetMetadata(Database &database, const std::string &key,
	std::string *value, std::string *error);
bool SetMetadata(Database &database, const std::string &key,
	const std::string &value, std::string *error);

}

#endif
