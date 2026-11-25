/*
 * DBSrv storage configuration: Config.txt keys with environment overrides.
 *
 * Config.txt keeps its legacy first two lines (`Sapphire`, `LastCapsule`) and
 * gains appended `Key Value` lines for the database and credential policy.
 * The password is never stored in Config.txt; it comes from the environment,
 * PGPASSWORD or a libpq password file.
 */

#ifndef W2PP_STORAGE_CONFIG_H
#define W2PP_STORAGE_CONFIG_H

#include <string>

namespace storage
{

struct StorageConfig
{
	// Connection
	std::string host = "127.0.0.1";
	int port = 5432;
	std::string database = "w2pp";
	std::string user = "w2pp";
	std::string password;
	std::string sslmode = "prefer";

	// Credential policy
	int bcrypt_cost = 12;
	int auth_max_failures = 5;
	int auth_lockout_seconds = 900;

	// Timeouts
	int connect_timeout_seconds = 10;
	int statement_timeout_ms = 5000;
	int lock_timeout_ms = 2000;

	// Schema version this binary understands.
	static const int kSchemaContract = 1;
	static const int kMinBcryptCost = 10;
	static const int kMaxBcryptCost = 14;

	// "host:port/database" without credentials, safe for logs and errors.
	std::string Endpoint() const;
};

/*
 * Load Config.txt and then apply W2PP_* environment overrides. Unknown keys
 * are ignored so the file can keep legacy lines. On failure `error` explains
 * the problem without echoing any secret.
 */
bool LoadStorageConfig(const char *path, StorageConfig *out, std::string *error);

// Exposed for tests: apply only the environment overrides.
bool ApplyEnvironmentOverrides(StorageConfig *config, std::string *error);

}

#endif
