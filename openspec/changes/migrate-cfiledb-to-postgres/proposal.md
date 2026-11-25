# Proposal

## Why

DBSrv persists every account, character, cargo, affect and soul capsule as a fixed-size binary blob rewritten in place (`./account/<bucket>/<NAME>`, `./char/<bucket>/<NAME>`, `./capsule/<n>`). That gives the authoritative player store no transactions, no atomic replace, no integrity checks and no query surface: a crash mid-write leaves a truncated record, the ranking and day-log features walk the whole account tree, and character-name uniqueness depends on creating a marker file. The same files hold account passwords in plaintext and compare them with `strcmp`, and the 4–6 digit secondary token is stored verbatim, so anyone with file access owns every credential.

PostgreSQL 19 is now available and the project already resolves third-party dependencies through a pinned vcpkg manifest, so the store can move to a real database without adding a machine-global requirement. Moving now also makes the credential storage fix possible: a BCrypt hash does not fit the 16-byte password field, so hashing and the database migration are one change.

## What Changes

- **PostgreSQL-backed persistence**: add a DBSrv storage layer using PostgreSQL 19 over libpq. Accounts, characters, cargo, equipment, inventory, affects, quick-skill bytes, character-extension blocks, soul capsules and the capsule counter become relational rows. Every multi-row mutation (login admission, character create/delete, save, capsule put-in/take-out) runs in one transaction.
- **Normalized arrays**: equipment (16), carried inventory (64), account cargo (128) and affects (32 per character) become child tables keyed by account/character and position. A shared serializer maps them to and from the existing in-memory `STRUCT_MOB`/`STRUCT_ITEM`/`STRUCT_AFFECT` view, preserving the exact byte semantics the client and TMSrv expect.
- **Legacy data importer**: add a one-time, re-runnable importer that reads the existing `./account`, `./char` and `./capsule` trees into the schema, reports skipped/corrupt records, verifies record counts and field checksums, and only then allows the servers to switch. After cutover the legacy trees are read-only archives.
- **Query replacements for file scans**: the exp day-log report and the EXP ranking loader become SQL queries. The `./char` name-index files are replaced by a unique constraint on character name. `DBExportAccount` re-encodes a `STRUCT_ACCOUNTFILE` from database rows instead of copying a file.
- **BCrypt credential storage**: account passwords and the numeric secure token are stored as BCrypt hashes with per-credential salts and a configurable cost factor (default 12, minimum 10). All verification paths (game login, delete-character confirmation, NP admin login) verify against the hash, successful verifications transparently upgrade weaker hashes, and consecutive failures are throttled per account because a 4–6 digit token is low entropy. The legacy password-prefix state markers (`@`, `_`, `#`) become an explicit account-state column. Plaintext passwords and tokens are no longer written to logs, exports or the NP protocol.
- **Credential migration**: the importer hashes the plaintext (or prefix-marked) legacy values, skips already-hashed rows, and records a migration state so a re-run is safe. Accounts whose stored value cannot be hashed are imported as locked with an explicit state instead of being silently dropped.
- **Configuration and failure behavior**: DB connection settings come from DBSrv `Config.txt` with environment-variable overrides. DBSrv refuses to start when the database is unreachable or the schema version is older than the binary expects, instead of continuing against stale data.
- **Build and test integration**: add the PostgreSQL client and a vendored BCrypt implementation to the vcpkg/CMake build, and add Docker-Compose-based PostgreSQL 19 integration tests for the SQL layer that run under CTest with a throwaway database.
- **BREAKING - operational requirement**: a running PostgreSQL 19 instance and a completed import become prerequisites for DBSrv. Existing account/character/capsule files are no longer read or written on the game path, and the NP admin tool can no longer display stored plaintext passwords.

## Capabilities

### New Capabilities

- `storage/account-persistence`: the relational account/character/capsule store, its schema and transaction guarantees, the legacy importer, the replacement of directory scans with queries, and DBSrv configuration/startup behavior for the database.
- `security/credential-hashing`: BCrypt storage and verification of account passwords and numeric secure tokens, credential migration, account-state modeling, and the removal of plaintext credential exposure.

### Modified Capabilities

- `build/cmake-vcpkg`: the manifest gains the PostgreSQL client and BCrypt dependencies, and the test command gains a database-backed integration target that provisions its own PostgreSQL instance.

## Impact

- **Code**: new `Code/storage/` module (connection, schema/migrations, statements, record codec); rework of `Code/DBSrv/CFileDB.cpp`, `CFileDB.h`, `CReadFiles.cpp`, `CRanking.cpp`, `Server.cpp`, `App.cpp`; new importer executable/target; new BCrypt wrapper; `Code/Basedef.h` only for named constants/state, never for record layout; `tools/` or `tests/tools/` for the importer entry point.
- **Build**: `vcpkg.json` and `CMakeLists.txt` add libpq and a vendored BCrypt source target; `CMakePresets.json` unchanged unless a dependency needs a feature; new CTest test with a Docker Compose file pinned to PostgreSQL 19.
- **Runtime**: DBSrv now needs network access to PostgreSQL, schema creation privileges at first start (or a migration step), and the importer run before first production start. `Config.txt` gains connection settings; the password cache and `pAccountList` slot addressing are unchanged.
- **Compatibility**: wire format, packet sizes, `STRUCT_ACCOUNTFILE`/`STRUCT_MOB`/`STRUCT_CAPSULE` layouts and the NP protocol layout stay unchanged. Stored credential semantics change (case-sensitive BCrypt verification, no plaintext recovery), and character-name uniqueness moves from marker files to a database constraint.
- **Out of scope**: migrating guild state (`Guilds.txt`), content files (`ItemEffect.h`, CSV imports other than the account-affecting ones), log-file formats, asynchronous/non-blocking database access, read replicas, connection pooling beyond a bounded per-process pool, and any change to the client or TMSrv authentication packets.
