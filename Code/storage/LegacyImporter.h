/*
 * One-time legacy tree importer: ./account, ./char and ./capsule into the
 * relational store. Re-runnable; refuses to report success with rejected
 * records and records import_state accordingly.
 */

#ifndef W2PP_STORAGE_LEGACYIMPORTER_H
#define W2PP_STORAGE_LEGACYIMPORTER_H

#include <string>
#include <vector>

#include "AccountRepository.h"
#include "Database.h"

namespace storage
{

struct ImportOptions
{
	std::string source;          // directory containing account/, char/, capsule/
	bool dry_run = false;
	bool replace = false;
	std::string report_path;
	std::string legacy_export;   // empty = no export
	bool empty_store = false;
};

struct ImportReport
{
	int accounts = 0;
	int characters = 0;
	int capsules = 0;
	int skipped = 0;
	int rejected = 0;
	bool success = false;
	std::vector<std::string> lines;

	std::string Summary() const;
};

class LegacyImporter
{
public:
	LegacyImporter(Database &database, AccountRepository &repository);

	ImportReport Run(const ImportOptions &options, std::string *error);

private:
	bool ExportLegacyTree(const ImportOptions &options, ImportReport *report,
		std::string *error);

	Database &database_;
	AccountRepository &repository_;
};

}

#endif
