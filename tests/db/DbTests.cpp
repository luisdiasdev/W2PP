/*
 * PostgreSQL integration tests for the storage layer.
 *
 * CTest runs this binary through tests/db/run_db_tests.{ps1,sh}, which starts
 * a throwaway PostgreSQL 19 container and exports W2PP_TEST_DSN. Direct
 * invocation without that variable exits with CTest's skip code (77) instead
 * of silently passing without exercising SQL.
 */

#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "storage/Database.h"
#include "storage/StorageConfig.h"

int main(int argc, char *argv[])
{
	const char *dsn = std::getenv("W2PP_TEST_DSN");

	if (dsn == nullptr || dsn[0] == '\0')
	{
		std::fprintf(stderr,
			"w2pp_db_tests: W2PP_TEST_DSN is not set; run through "
			"tests/db/run_db_tests.ps1 or tests/db/run_db_tests.sh\n");
		return 77;
	}

	return Catch::Session().run(argc, argv);
}

TEST_CASE("compose database is reachable", "[db]")
{
	std::string error;
	storage::StorageConfig config;

	REQUIRE(storage::LoadStorageConfig(nullptr, &config, &error));

	storage::Database database;
	REQUIRE(database.Connect(config, &error));

	storage::Result result = database.Exec("SELECT 1");
	REQUIRE(result.Ok());
	REQUIRE(result.Rows() == 1);
	REQUIRE(result.Int32(0, 0) == 1);

	database.Close();
}
