/*
 * Dependency probe for the PostgreSQL client library.
 *
 * The DBSrv storage layer links libpq through vcpkg. This target exists so a
 * configure/build of the project proves that the pinned manifest resolves and
 * links the client library for the supported triplet, and so an operator can
 * confirm the runtime library version on a deployment host.
 *
 * It never connects to a database.
 */

#include <cstdio>

#include <libpq-fe.h>

int main()
{
	std::printf("libpq %d (compiled against headers, linked runtime)\n",
		PQlibVersion());
	return 0;
}
