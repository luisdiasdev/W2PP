/*
 * dbimport: one-time, re-runnable import of the legacy account/char/capsule
 * trees into PostgreSQL.
 *
 * usage:
 *   dbimport --source <legacy root> [--dsn "host=... port=... dbname=...
 *            user=... password=... sslmode=..."] [--dry-run] [--replace]
 *            [--report <file>] [--legacy-export <dir>] [--empty]
 */

#include "../storage/AccountRepository.h"
#include "../storage/Database.h"
#include "../storage/LegacyImporter.h"
#include "../storage/Schema.h"
#include "../storage/StorageConfig.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{

void PrintUsage()
{
	std::printf(
		"usage: dbimport --source <legacy root> [options]\n"
		"\n"
		"  --source <dir>         directory containing account/, char/, capsule/\n"
		"  --dsn <key=value ...>  libpq keyword/value overrides (host, port, dbname,\n"
		"                         user, password, sslmode)\n"
		"  --dry-run              validate and report without writing\n"
		"  --replace              overwrite rows that differ from the source\n"
		"  --report <file>        write the per-record report to <file>\n"
		"  --legacy-export <dir>  write a STRUCT_ACCOUNTFILE/capsule tree from the DB\n"
		"  --empty                initialize an intentionally empty production store\n"
		"  --help                 show this text\n");
}

bool ApplyDsn(storage::StorageConfig *config, const std::string &dsn,
	std::string *error)
{
	std::size_t position = 0;

	while (position < dsn.size())
	{
		while (position < dsn.size() && std::isspace(static_cast<unsigned char>(dsn[position])))
			++position;

		std::size_t end = position;
		while (end < dsn.size() && !std::isspace(static_cast<unsigned char>(dsn[end])))
			++end;

		if (end == position)
			break;

		const std::string token = dsn.substr(position, end - position);
		position = end;

		const std::size_t equals = token.find('=');
		if (equals == std::string::npos)
		{
			if (error != nullptr)
				*error = "invalid --dsn token: " + token;
			return false;
		}

		const std::string key = token.substr(0, equals);
		const std::string value = token.substr(equals + 1);

		if (key == "host")
			config->host = value;
		else if (key == "port")
			config->port = std::atoi(value.c_str());
		else if (key == "dbname" || key == "database")
			config->database = value;
		else if (key == "user")
			config->user = value;
		else if (key == "password")
			config->password = value;
		else if (key == "sslmode")
			config->sslmode = value;
	}

	return true;
}

} // namespace

int main(int argc, char **argv)
{
	storage::ImportOptions options;
	std::string dsn;

	for (int i = 1; i < argc; ++i)
	{
		const std::string argument = argv[i];

		if (argument == "--help" || argument == "-h")
		{
			PrintUsage();
			return 0;
		}
		if (argument == "--source" && i + 1 < argc)
			options.source = argv[++i];
		else if (argument == "--dsn" && i + 1 < argc)
			dsn = argv[++i];
		else if (argument == "--dry-run")
			options.dry_run = true;
		else if (argument == "--replace")
			options.replace = true;
		else if (argument == "--report" && i + 1 < argc)
			options.report_path = argv[++i];
		else if (argument == "--legacy-export" && i + 1 < argc)
			options.legacy_export = argv[++i];
		else if (argument == "--empty")
			options.empty_store = true;
		else
		{
			std::fprintf(stderr, "dbimport: unknown argument %s\n", argument.c_str());
			PrintUsage();
			return 2;
		}
	}

	if (options.source.empty() && !options.empty_store)
	{
		PrintUsage();
		return 2;
	}

	storage::StorageConfig config;
	std::string error;

	if (!options.source.empty()
		&& std::filesystem::exists(std::filesystem::path(options.source) / "Config.txt"))
	{
		const std::string configPath =
			(std::filesystem::path(options.source) / "Config.txt").string();

		if (!storage::LoadStorageConfig(configPath.c_str(), &config, &error))
		{
			std::fprintf(stderr, "dbimport: %s\n", error.c_str());
			return 1;
		}
	}
	else if (!storage::LoadStorageConfig(nullptr, &config, &error))
	{
		std::fprintf(stderr, "dbimport: %s\n", error.c_str());
		return 1;
	}

	if (!dsn.empty() && !ApplyDsn(&config, dsn, &error))
	{
		std::fprintf(stderr, "dbimport: %s\n", error.c_str());
		return 1;
	}

	storage::Database database;

	if (!database.Connect(config, &error))
	{
		std::fprintf(stderr, "dbimport: %s\n", error.c_str());
		return 1;
	}

	int version = 0;

	if (!storage::Migrate(database, &error, &version))
	{
		std::fprintf(stderr, "dbimport: %s\n", error.c_str());
		return 1;
	}

	if (!storage::CheckSchemaContract(database, &error))
	{
		std::fprintf(stderr, "dbimport: %s\n", error.c_str());
		return 1;
	}

	storage::AccountRepository repository(database, config);
	storage::LegacyImporter importer(database, repository);
	storage::ImportReport report = importer.Run(options, &error);

	for (const std::string &line : report.lines)
		std::printf("%s\n", line.c_str());

	if (!report.success)
	{
		if (!error.empty())
			std::fprintf(stderr, "dbimport: %s\n", error.c_str());
		return 1;
	}

	return 0;
}
