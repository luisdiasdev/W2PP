#include "StorageConfig.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace storage
{
namespace
{

std::string Trim(const std::string &value)
{
	std::size_t begin = 0;
	std::size_t end = value.size();

	while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
		++begin;
	while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
		--end;

	return value.substr(begin, end - begin);
}

bool ParseInt(const std::string &text, int minimum, int maximum, int *out)
{
	if (text.empty())
		return false;

	errno = 0;
	char *end = nullptr;
	long value = std::strtol(text.c_str(), &end, 10);

	if (errno != 0 || end == text.c_str() || *end != '\0')
		return false;
	if (value < minimum || value > maximum)
		return false;

	*out = static_cast<int>(value);
	return true;
}

const char *EnvironmentValue(const char *name)
{
	const char *value = std::getenv(name);
	return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

bool ApplyKey(StorageConfig *config, const std::string &key,
	const std::string &value, std::string *error)
{
	if (key == "PgHost")
		config->host = value;
	else if (key == "PgPort")
	{
		if (!ParseInt(value, 1, 65535, &config->port))
		{
			*error = "Config.txt: PgPort must be a number between 1 and 65535";
			return false;
		}
	}
	else if (key == "PgDatabase")
		config->database = value;
	else if (key == "PgUser")
		config->user = value;
	else if (key == "PgSslMode")
		config->sslmode = value;
	else if (key == "BcryptCost")
	{
		if (!ParseInt(value, StorageConfig::kMinBcryptCost,
				StorageConfig::kMaxBcryptCost, &config->bcrypt_cost))
		{
			*error = "Config.txt: BcryptCost must be between 10 and 14";
			return false;
		}
	}
	else if (key == "AuthMaxFailures")
	{
		if (!ParseInt(value, 1, 1000, &config->auth_max_failures))
		{
			*error = "Config.txt: AuthMaxFailures must be a positive number";
			return false;
		}
	}
	else if (key == "AuthLockoutSeconds")
	{
		if (!ParseInt(value, 0, 86400 * 365, &config->auth_lockout_seconds))
		{
			*error = "Config.txt: AuthLockoutSeconds must be a non-negative number";
			return false;
		}
	}
	else if (key == "PgConnectTimeout")
	{
		if (!ParseInt(value, 1, 3600, &config->connect_timeout_seconds))
		{
			*error = "Config.txt: PgConnectTimeout must be between 1 and 3600";
			return false;
		}
	}
	else if (key == "PgStatementTimeoutMs")
	{
		if (!ParseInt(value, 1, 3600000, &config->statement_timeout_ms))
		{
			*error = "Config.txt: PgStatementTimeoutMs must be between 1 and 3600000";
			return false;
		}
	}
	else if (key == "PgLockTimeoutMs")
	{
		if (!ParseInt(value, 1, 3600000, &config->lock_timeout_ms))
		{
			*error = "Config.txt: PgLockTimeoutMs must be between 1 and 3600000";
			return false;
		}
	}

	// Legacy keys (Sapphire, LastCapsule) and anything unrecognized are left
	// to their existing readers.
	return true;
}

} // namespace

std::string StorageConfig::Endpoint() const
{
	return host + ":" + std::to_string(port) + "/" + database;
}

bool LoadStorageConfig(const char *path, StorageConfig *out, std::string *error)
{
	if (out == nullptr || error == nullptr)
		return false;

	StorageConfig config;

	if (path != nullptr)
	{
		FILE *file = std::fopen(path, "rb");

		if (file == nullptr)
		{
			*error = std::string("cannot open ") + path;
			return false;
		}

		char line[512];
		bool first = true;

		while (std::fgets(line, sizeof(line), file) != nullptr)
		{
			std::string text = Trim(line);

			// Preserve the legacy positional header: the first two lines are
			// `Sapphire <n>` and `LastCapsule <n>`; they are owned by the
			// server, not by storage config.
			if (first)
			{
				first = false;
				if (text.rfind("Sapphire", 0) == 0)
					continue;
			}

			if (text.empty() || text[0] == '#')
				continue;

			std::size_t split = text.find_first_of(" \t");
			std::string key = (split == std::string::npos)
				? text : text.substr(0, split);
			std::string value = (split == std::string::npos)
				? std::string() : Trim(text.substr(split + 1));

			if (!ApplyKey(&config, key, value, error))
			{
				std::fclose(file);
				return false;
			}
		}

		std::fclose(file);
	}

	if (!ApplyEnvironmentOverrides(&config, error))
		return false;

	*out = config;
	return true;
}

bool ApplyEnvironmentOverrides(StorageConfig *config, std::string *error)
{
	if (config == nullptr || error == nullptr)
		return false;

	if (const char *value = EnvironmentValue("W2PP_PG_HOST"))
		config->host = value;

	if (const char *value = EnvironmentValue("W2PP_PG_PORT"))
	{
		if (!ParseInt(value, 1, 65535, &config->port))
		{
			*error = "W2PP_PG_PORT must be a number between 1 and 65535";
			return false;
		}
	}

	if (const char *value = EnvironmentValue("W2PP_PG_DATABASE"))
		config->database = value;

	if (const char *value = EnvironmentValue("W2PP_PG_USER"))
		config->user = value;

	// The password is only ever taken from the environment (or libpq's own
	// PGPASSWORD/.pgpass handling when both are absent).
	if (const char *value = EnvironmentValue("W2PP_PG_PASSWORD"))
		config->password = value;
	else if (const char *value = EnvironmentValue("PGPASSWORD"))
		config->password = value;

	if (const char *value = EnvironmentValue("W2PP_PG_SSLMODE"))
		config->sslmode = value;

	if (const char *value = EnvironmentValue("W2PP_BCRYPT_COST"))
	{
		if (!ParseInt(value, StorageConfig::kMinBcryptCost,
				StorageConfig::kMaxBcryptCost, &config->bcrypt_cost))
		{
			*error = "W2PP_BCRYPT_COST must be between 10 and 14";
			return false;
		}
	}

	if (const char *value = EnvironmentValue("W2PP_AUTH_MAX_FAILURES"))
	{
		if (!ParseInt(value, 1, 1000, &config->auth_max_failures))
		{
			*error = "W2PP_AUTH_MAX_FAILURES must be a positive number";
			return false;
		}
	}

	if (const char *value = EnvironmentValue("W2PP_AUTH_LOCKOUT_SECONDS"))
	{
		if (!ParseInt(value, 0, 86400 * 365, &config->auth_lockout_seconds))
		{
			*error = "W2PP_AUTH_LOCKOUT_SECONDS must be a non-negative number";
			return false;
		}
	}

	if (const char *value = EnvironmentValue("W2PP_PG_CONNECT_TIMEOUT"))
	{
		if (!ParseInt(value, 1, 3600, &config->connect_timeout_seconds))
		{
			*error = "W2PP_PG_CONNECT_TIMEOUT must be between 1 and 3600";
			return false;
		}
	}

	if (const char *value = EnvironmentValue("W2PP_PG_STATEMENT_TIMEOUT_MS"))
	{
		if (!ParseInt(value, 1, 3600000, &config->statement_timeout_ms))
		{
			*error = "W2PP_PG_STATEMENT_TIMEOUT_MS must be between 1 and 3600000";
			return false;
		}
	}

	if (const char *value = EnvironmentValue("W2PP_PG_LOCK_TIMEOUT_MS"))
	{
		if (!ParseInt(value, 1, 3600000, &config->lock_timeout_ms))
		{
			*error = "W2PP_PG_LOCK_TIMEOUT_MS must be between 1 and 3600000";
			return false;
		}
	}

	return true;
}

} // namespace storage
