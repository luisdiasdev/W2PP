# Spec Delta

## MODIFIED Requirements

### Requirement: Pinned dependency manifest

Third-party dependencies SHALL be declared in a vcpkg manifest with a pinned baseline. A clean checkout SHALL resolve the same dependency versions without relying on machine-global installs or floating SDK selections. The manifest SHALL include the PostgreSQL client library and the BCrypt implementation used by the DB server, and every dependency SHALL remain available for the supported 32-bit Windows and Linux targets.

#### Scenario: Clean machine build

- **GIVEN** a checkout with no prior package cache
- **WHEN** the documented configure command runs with vcpkg available
- **THEN** dependencies resolve from the manifest at the pinned versions and the build proceeds

#### Scenario: Dependency version drift

- **WHEN** the manifest baseline is unchanged
- **THEN** two machines configure the same third-party versions instead of picking up whatever is installed globally

#### Scenario: No Boost dependency

- **WHEN** the transport dependency is configured
- **THEN** it builds in standalone mode and the build does not require or link Boost

#### Scenario: Database and hashing dependencies are declared

- **WHEN** the manifest is inspected
- **THEN** the PostgreSQL client and BCrypt dependencies are listed with the shared baseline and no dependency is resolved from a machine-global install

#### Scenario: Dependency unavailable for a supported target

- **WHEN** a declared dependency cannot be built for a supported triplet
- **THEN** configure fails with a message naming the dependency and the triplet instead of producing a binary with a substituted or missing library

### Requirement: Automated test execution

The project SHALL expose a single test command per supported target that runs the transport codec, platform, host and SQL-layer tests without requiring live game clients, external servers or content files. SQL-layer integration tests SHALL provision their own PostgreSQL instance from a repository-managed Compose definition and SHALL use a throwaway database. Windows SHALL retain its integration harnesses and Linux SHALL provide an equivalent POSIX smoke test.

#### Scenario: Codec regression

- **WHEN** the test suite runs
- **THEN** handshake, framing, obfuscation round-trip, checksum and size-window cases are exercised and any mismatch fails the run

#### Scenario: Loopback transport

- **WHEN** the integration test runs
- **THEN** a listener and client exchange a valid frame over the loopback interface and the decoded payload matches

#### Scenario: Linux test command

- **WHEN** the Linux test preset runs
- **THEN** the unit tests and the Linux server smoke test execute and any failure fails the run

#### Scenario: Missing test dependency

- **WHEN** the test target is configured but the test framework is unavailable
- **THEN** configure fails clearly instead of producing a build that silently omits tests

#### Scenario: SQL layer integration test

- **WHEN** the database integration test runs
- **THEN** a PostgreSQL 19 instance is started from the repository Compose definition, the schema is applied to a throwaway database, the SQL-layer cases run, and the instance is torn down afterwards

#### Scenario: Database test isolation

- **GIVEN** a database container already used by another run
- **WHEN** the integration test starts
- **THEN** it uses its own database or schema and leaves pre-existing data unmodified

#### Scenario: Container runtime unavailable

- **WHEN** the database integration test cannot start its container
- **THEN** the test fails with a message naming the unavailable runtime instead of silently passing without exercising the SQL layer
