# Spec Delta

## Purpose

Makes PostgreSQL the authoritative store for player accounts, characters, cargo and soul capsules, with transactional writes, a verified legacy import and database-backed replacements for the account-tree scans.

## ADDED Requirements

### Requirement: Authoritative database storage

DBSrv SHALL treat PostgreSQL as the only writable store for accounts, characters, cargo, affects and soul capsules. DBSrv SHALL NOT read or write `./account`, `./char` or `./capsule` while serving game or admin traffic.

#### Scenario: Login reads from the database

- **WHEN** a game server requests an account login
- **THEN** the account, its four character slots, cargo, quick-skill bytes, affects and character-extension data are read from PostgreSQL

#### Scenario: Save persists to the database

- **WHEN** a game server sends a character save or a save-and-quit request
- **THEN** the changed character, cargo, affects and account fields are written to PostgreSQL

#### Scenario: Legacy trees untouched

- **GIVEN** legacy `./account`, `./char` and `./capsule` trees exist
- **WHEN** a player logs in, saves and logs out
- **THEN** no file under those trees is created, modified, renamed or deleted

### Requirement: Transactional account mutations

Every account mutation triggered by one game or admin request SHALL commit as a single database transaction. If any step fails, the transaction SHALL roll back and the reply SHALL report failure, leaving the previously committed account state intact.

#### Scenario: Failed save leaves prior state

- **GIVEN** a logged-in character with a committed snapshot
- **WHEN** a save fails partway through writing the character, cargo and affects
- **THEN** the transaction rolls back and a subsequent login returns the previously committed snapshot

#### Scenario: Partial login admission

- **WHEN** an account is admitted to the session cache but persisting the server-change token fails
- **THEN** the cache entry is not admitted and the client receives the login failure signal

#### Scenario: Process death during save

- **WHEN** DBSrv is terminated while a save transaction is open
- **THEN** the database contains either the complete new snapshot or the complete previous snapshot, never a mixture

### Requirement: Normalized character arrays

Equipment, carried inventory, account cargo and affects SHALL be stored as rows that preserve each element's position. Reconstructing a character or account from the database SHALL reproduce the same array contents, including empty positions, as the stored record.

#### Scenario: Full equipment and inventory round-trip

- **GIVEN** a character with all 16 equipment positions, all 64 carried positions and all 32 affect positions populated with distinct values
- **WHEN** the record is stored and read back
- **THEN** every position holds the same item index, effect bytes and affect fields as before the store

#### Scenario: Sparse arrays

- **GIVEN** a character whose equipment positions 3 and 9 are empty and whose cargo contains items at positions 0 and 127 only
- **WHEN** the record is stored and read back
- **THEN** the empty positions remain empty, the populated positions keep their exact indices, and no position shifts

#### Scenario: Cross-position item values

- **GIVEN** two items with identical item index but different effect bytes at different positions
- **WHEN** the record is stored and read back
- **THEN** each position keeps its own effect bytes and the items are not merged or deduplicated

### Requirement: Record reconstruction fidelity

Rebuilding an account record from database rows SHALL produce the same values for every field defined by `STRUCT_ACCOUNTFILE`, `STRUCT_MOB`, `STRUCT_MOBEXTRA`, `STRUCT_ITEM` and `STRUCT_AFFECT`. Padding and unused legacy bytes SHALL be zero-filled rather than round-tripped.

#### Scenario: Structural checksum matches

- **GIVEN** a seeded account record with characters, cargo, quick skills, affects, extension data and a server-change token
- **WHEN** it is imported, read back and re-encoded
- **THEN** the re-encoded record matches the source for all defined fields and the guarded record size stays 7808 bytes

#### Scenario: Unknown legacy padding

- **GIVEN** a legacy record whose unused bytes contain non-zero garbage
- **WHEN** the record is imported and read back
- **THEN** the reconstruction has zero-filled unused bytes and gameplay-visible fields are unchanged

### Requirement: Character name uniqueness

Character names SHALL be unique across all accounts under the same uppercase normalization used today. Creating a character with an existing name SHALL fail through the existing failure signal, and deleting a character SHALL release the name immediately.

#### Scenario: Duplicate character name

- **GIVEN** account A owns a character named `KNIGHT`
- **WHEN** account B creates a character named `knight`
- **THEN** the create request fails with the existing new-character failure signal and no character row is created

#### Scenario: Name released on delete

- **GIVEN** a character named `KNIGHT` was deleted
- **WHEN** another account creates `KNIGHT`
- **THEN** the creation succeeds

#### Scenario: Account lookup by character name

- **WHEN** the admin path requests the account for a character name
- **THEN** the owning account name is returned from the database

### Requirement: Soul capsule persistence

Soul capsules SHALL be stored in the database under a stable numeric identifier, and the capsule counter SHALL be durable and monotonic across restarts. Put-in and take-out SHALL commit together with the affected character slot and cargo changes.

#### Scenario: Capsule survives restart

- **GIVEN** a character sealed into a capsule with identifier N
- **WHEN** DBSrv restarts and the capsule is taken out
- **THEN** the same character data is restored and the capsule row no longer exists

#### Scenario: Counter never reuses an identifier

- **GIVEN** the last allocated capsule identifier was N
- **WHEN** DBSrv restarts and a new capsule is created
- **THEN** the new identifier is greater than N

#### Scenario: Put-in failure is atomic

- **WHEN** writing the capsule row succeeds but clearing the character slot fails
- **THEN** the transaction rolls back, the character remains in its slot and the capsule is not available

### Requirement: Legacy data import

A separate import operation SHALL read the legacy `./account`, `./char` and `./capsule` trees into the schema. It SHALL be re-runnable without duplicating rows, SHALL report every rejected record with its path and reason, and SHALL verify imported counts and record checksums before reporting success.

#### Scenario: First import

- **GIVEN** a populated legacy tree and an empty schema
- **WHEN** the import runs
- **THEN** every valid account, character index, capsule and capsule counter value is present in the database and the verification report matches the source counts

#### Scenario: Re-run is idempotent

- **GIVEN** a completed import
- **WHEN** the same import runs again
- **THEN** no account, character or capsule is duplicated and the report states that no changes were needed

#### Scenario: Corrupt record is isolated

- **GIVEN** one truncated account record among valid records
- **WHEN** the import runs
- **THEN** the remaining records are imported, the truncated record is listed as rejected with its path and reason, and the import exits with a non-zero status

#### Scenario: Verification mismatch blocks cutover

- **WHEN** the imported row counts or checksums do not match the source totals
- **THEN** the import reports failure and marks the schema as not ready for serving

### Requirement: Database configuration and startup validation

DBSrv SHALL read connection settings from `Config.txt` and SHALL allow environment variables to override every setting. DBSrv SHALL refuse to start when the database is unreachable, the credentials are rejected or the schema version is newer than the binary supports, and SHALL NOT fall back to legacy files.

#### Scenario: Database unreachable

- **WHEN** DBSrv starts and cannot connect within the configured timeout
- **THEN** startup fails with a non-zero exit code and a message naming the host, port and database

#### Scenario: Environment override

- **GIVEN** `Config.txt` names one database host
- **WHEN** the corresponding environment variable names another reachable host
- **THEN** DBSrv connects to the environment-specified host

#### Scenario: Newer schema

- **WHEN** the schema version in the database is newer than the version supported by the binary
- **THEN** DBSrv refuses to start and instructs the operator to upgrade the binary

### Requirement: Schema versioning

The store SHALL track its schema version in the database. DBSrv SHALL apply pending forward migrations at startup inside a transaction, record the applied version, and leave the schema unchanged when a migration fails.

#### Scenario: Fresh database

- **WHEN** DBSrv starts against an empty database with migration privileges
- **THEN** all tables, indexes and constraints are created and the schema version is recorded

#### Scenario: Failed migration

- **GIVEN** a database at version N
- **WHEN** the migration to version N+1 fails
- **THEN** the schema stays at version N and DBSrv refuses to serve traffic

#### Scenario: Insufficient privileges

- **WHEN** the configured role cannot create or alter objects and migrations are pending
- **THEN** startup fails with a message that names the missing privilege

### Requirement: Scan replacement for reports and ranking

The exp day-log report and the EXP ranking loader SHALL use database queries with the same selection rules as the previous directory scans. Generated report lines and ranking entries SHALL contain the same accounts, characters and values for equivalent data.

#### Scenario: Day-log selection parity

- **GIVEN** accounts last modified in the current month and accounts modified earlier
- **WHEN** the exp day-log is generated
- **THEN** only current-month accounts are included and only characters whose day-log year-day equals yesterday appear

#### Scenario: Ranking parity

- **GIVEN** a set of accounts with experience values
- **WHEN** the ranking is loaded
- **THEN** the same ordered entries are produced as the legacy scan for the same data

### Requirement: Export projection

The account export SHALL build a `STRUCT_ACCOUNTFILE` from database rows rather than copying a file. The exported record SHALL keep the guarded 7808-byte layout, SHALL contain all non-credential fields, and SHALL leave credential fields empty.

#### Scenario: Export from database

- **GIVEN** an imported account
- **WHEN** the export path is invoked
- **THEN** a 7808-byte record is written to the configured export location with all gameplay fields preserved and the password and numeric-token fields empty

### Requirement: Login and session behavior parity

Moving the store SHALL preserve existing login and session semantics: dated account blocks, server-change token acceptance, duplicate-session resolution, negative-coin clamping, dual-sword name swapping and the in-memory slot addressing used by game servers.

#### Scenario: Dated block

- **GIVEN** an account whose block date is in the future
- **WHEN** it logs in with correct credentials
- **THEN** the login is refused with the existing block failure signal

#### Scenario: Server-change token

- **GIVEN** an account with a committed server-change token matching the login token
- **WHEN** it logs in
- **THEN** the password check is bypassed, the token is cleared and the character login reply is produced

#### Scenario: Duplicate session

- **GIVEN** an account already admitted on another game-server slot
- **WHEN** the same account logs in again
- **THEN** the existing already-playing or still-playing sequence runs unchanged

#### Scenario: Cache slots unchanged

- **WHEN** a game server connects and a player logs in
- **THEN** the cache index remains `connection * MAX_USER + player id`
