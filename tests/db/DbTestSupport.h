/*
 * Test-only helper: a throwaway database inside the Compose instance.
 *
 * Every test case gets its own database created by the container superuser and
 * dropped with FORCE on teardown, so cases cannot see each other's rows and a
 * pre-existing database is never modified.
 */

#ifndef W2PP_TESTS_DB_SUPPORT_H
#define W2PP_TESTS_DB_SUPPORT_H

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "storage/Database.h"
#include "storage/Schema.h"
#include "storage/StorageConfig.h"

class TestDatabase
{
public:
	TestDatabase()
	{
		std::string error;

		if (!storage::LoadStorageConfig(nullptr, &config_, &error))
			throw std::runtime_error("config: " + error);

		maintenance_ = config_;
		maintenance_.database = config_.database;

		if (!maintenanceConnection_.Connect(maintenance_, &error))
			throw std::runtime_error("maintenance connect: " + error);

		static unsigned int counter = 0;
		char name[64];
		std::snprintf(name, sizeof(name), "w2pp_test_%u_%u",
			static_cast<unsigned int>(currentProcessId()), ++counter);
		name_ = name;

		DropDatabase();

		std::string sql = "CREATE DATABASE \"" + name_ + "\" TEMPLATE template0";
		storage::Result created = maintenanceConnection_.Exec(sql);

		if (!created.Ok())
			throw std::runtime_error("create database: " + created.Error());

		config_.database = name_;

		if (!database_.Connect(config_, &error))
			throw std::runtime_error("connect: " + error);

		if (!storage::Migrate(database_, &error, &version_))
			throw std::runtime_error("migrate: " + error);
	}

	~TestDatabase()
	{
		Close();

		try
		{
			std::string error;
			maintenanceConnection_.Connect(maintenance_, &error);
			DropDatabase();
		}
		catch (...)
		{
		}
	}

	TestDatabase(const TestDatabase &) = delete;
	TestDatabase &operator=(const TestDatabase &) = delete;

	storage::Database &Db() { return database_; }
	const storage::StorageConfig &Config() const { return config_; }
	const std::string &Name() const { return name_; }
	int Version() const { return version_; }

	void Close()
	{
		database_.Close();
	}

private:
	static unsigned long currentProcessId()
	{
#ifdef _WIN32
		return static_cast<unsigned long>(_getpid());
#else
		return static_cast<unsigned long>(getpid());
#endif
	}

	void DropDatabase()
	{
		if (name_.empty())
			return;

		std::string sql = "DROP DATABASE IF EXISTS \"" + name_ + "\" WITH (FORCE)";
		maintenanceConnection_.Exec(sql);
	}

	storage::StorageConfig config_;
	storage::StorageConfig maintenance_;
	storage::Database maintenanceConnection_;
	storage::Database database_;
	std::string name_;
	int version_ = 0;
};

#endif
