# Implementation notes

Running log of verification results and environment facts recorded while
applying this change.

## 1. Dependency and build foundation

### 1.1 libpq (task 1.1)

- Manifest: `vcpkg.json` adds `libpq` at the existing baseline
  (`ec48c11eae9448c8c6e7739d93e06fa2433aa95f`).
- Probe target: `libpq_probe` (`tests/tools/libpq_probe.cpp`) links
  `PostgreSQL::PostgreSQL` and prints `PQlibVersion()`.
- Windows x86 Debug: configure and build succeeded with
  `libpq[core,lz4,openssl,zlib]:x86-windows@18.4#2`; the probe reports
  `libpq 180004`. Result: **verified**.
- Linux i386 (`x86-linux`): **not verified in this environment**. No Linux
  host or 32-bit toolchain is available here (the session runs on Windows).
  The same port resolves the same baseline for `x86-linux`; verify on the
  Linux host during task 8.2. If the port cannot build there, the design's
  fallback (overlay port building libpq from source) applies.

### 1.2 Vendored BCrypt (task 1.2)

- Source: OpenBSD `lib/libc/crypt/bcrypt.c` and `blowfish.c`, vendored under
  `third_party/bcrypt` with both licenses in `LICENSE` and the adaptations
  documented in `third_party/bcrypt/README.md`.
- Target: `w2pp_bcrypt` (static). Exports `w2pp_bcrypt_hash` and
  `w2pp_bcrypt_verify`; new hashes use the `$2b$` prefix, verification accepts
  `$2a$`/`$2b$`.
- Windows x86 Debug: `bcrypt_probe` produced
  `$2b$04$P05jbz6uvmuFhrs/gmjFY.q/unYWY13ek7FZ5HUoHTmAL0IE4ChZW`, verified the
  correct credential, rejected a wrong one, and exited 0. Result: **verified**.

### 1.3 `w2pp_storage` target (task 1.3)

- `CMakeLists.txt` adds `w2pp_storage` (static) carrying `Code/storage` and,
  as the change proceeds, `Code/security`; it links `PostgreSQL::PostgreSQL`
  and `w2pp_bcrypt` and is linked into `dbsrv`.
- Initial contents: `StorageConfig` (Config.txt + `W2PP_*` overrides) and the
  RAII libpq `Database`/`Result`/`Transaction` wrapper.
- Windows x86 Debug: `dbsrv` rebuilt and linked with no behavior change.
  Result: **verified**. Linux i386 is pending the same host limitation as 1.1.

### 1.4 Compose test stack and runners (task 1.4)

- `tests/db/docker-compose.yml` pins `postgres:19.0`, stores data on tmpfs,
  exposes `${W2PP_DB_PORT:-55432}` and has a `pg_isready` healthcheck.
- `tests/db/run_db_tests.ps1` and `tests/db/run_db_tests.sh` start the stack
  under a per-run project name, wait for health, export `W2PP_TEST_DSN` (and
  the `W2PP_PG_*` variables), run the supplied command and always tear down
  with `down --volumes --remove-orphans`.
- Both runners verified with a placeholder command: the stack reported
  healthy, the command saw the DSN, exit status was 0, and afterwards no
  `w2pp-dbtest` container or volume remained. The Bash runner was exercised
  through the available login shell because plain `bash` on this host does not
  inherit the Docker CLI path.

### 1.5 CTest database wiring (task 1.5)

- `W2PP_DB_TESTS` option (default `ON`), `w2pp_db_tests` Catch2 binary, and
  one `db-integration` CTest entry that runs the whole binary through the
  runner. `SKIP_RETURN_CODE 77` and the DSN check in `DbTests.cpp` make a
  direct run without a container explicit.
- `ctest -N` lists `db-integration` when enabled (48 tests) and omits it with
  `-DW2PP_DB_TESTS=OFF` (47 tests). `ctest -R db-integration` passed against
  the Compose database in 7.7 s. Result: **verified**.

### 1.6 README (task 1.6)
`README.md` now lists `libpq` and the vendored BCrypt target, documents the
Docker prerequisite, the `W2PP_DB_TESTS` opt-out, both runner commands, the
`W2PP_DB_PORT` override and the skip-code 77 behavior, and notes that
`db-integration` is excluded from CI by the `integration` label.

## 2. Schema and migrations

### 2.1 - 2.3 Migration runner and constraints

- `Code/storage/Schema.{h,cpp}`: advisory-locked runner (`pg_advisory_lock`,
  key `0x77327070`), bootstrap of `schema_migration` + `store_metadata`,
  contract check, and version 1 DDL for all tables in the design.
- Each step runs in its own `SERIALIZABLE` transaction and records its
  version; a failure rolls back and reports `migration N (name) failed at
  step:`.
- Tests in `tests/db/SchemaTests.cpp` (run through `db-integration`) cover the
  fresh migration, every uniqueness/range/exclusivity/width constraint, and a
  deliberately broken step 2 that leaves version 1 intact.
- Fixed during this step: text-format libpq parameters must be NUL-terminated
  (`ParamText` appends the terminator); the initial build exposed this as an
  invalid UTF-8 byte in `pg_advisory_lock`.
- Result: `db-integration` passed with 10 test cases / all constraints
  rejected with the expected SQLSTATE. **verified**.

## 3. Record codec

- `Code/storage/RecordCodec.{h,cpp}` maps every defined `STRUCT_ACCOUNTFILE`,
  `STRUCT_MOB`, `STRUCT_MOBEXTRA`, `STRUCT_ITEM` and `STRUCT_AFFECT` field to
  `AccountRecord`/`CharacterRecord` rows and back. Reconstruction starts from a
  zeroed struct, so padding and the legacy `EMPTY` arrays never round-trip.
- Equipment/inventory/cargo/affects are stored only for non-empty positions;
  empty positions reconstruct as zero. Duplicate item indices stay independent.
- Signed resist bytes, unsigned affect time (`0xFFFFFFFF`), 64-bit
  `time_t` fields, `Magic`/`Hold` unsigned values and all celestial saves are
  covered.
- `NormalizeAccountFile` + `StructuralChecksum` (FNV-1a over the normalized
  7808-byte record) are padding-independent.
- `tests/storage/RecordCodecTests.cpp`: 19 cases / 13020 assertions, all
  passing, including the ABI size/offset static checks. **verified**.

## 4. Storage repository

- `Code/storage/AccountRepository.{h,cpp}` implements account load/save,
  character reconcile/delete/lookup, child-array replacement, capsules, login
  admission, credential operations, day-log/ranking queries and export.
- Save is one `SERIALIZABLE` transaction: account scalars updated without
  touching credential columns, live characters upserted by slot, slots removed
  when empty, child arrays rewritten only when their rows differ.
- Capsule put-in allocates `capsule_id_seq` (MAXVALUE 65535), re-links the
  character row and clears the account quick-skill block; take-out checks the
  name and slot, renames and clears affects. Sequence and data survive a new
  connection.
- Login admission locks the account row and commits coin clamp, temp-key clear,
  state/date checks, verification/throttling and hash upgrade together.
- Repo tests (`tests/db/RepositoryTests.cpp`, `ConnectionTests.cpp`):
  25 DB test cases / 300 assertions passing, covering full round-trip, no-op
  child rows (xmin unchanged), duplicate/freed names, lookup by character name,
  capsule restart/monotonic ids/rollback (trigger-induced failure), coin clamp,
  blocked state, temp-key bypass/clear, lockout + expiry + restart, hash
  upgrade, token bind/verify, export size/empty credentials, day-log/ranking
  parity, connect failure, explicit rollback and statement timeout.
- Fixed while testing: `EXCLUDED` upsert assignments, NULL vs empty bytea
  parameters, and the coin check constraint (removed so legacy negative values
  can be clamped at login).

## 5. Credential security

- `Code/security/PasswordHash` validates cost 10-14, generates `$2b$` hashes,
  verifies `$2a$`/`$2b$`, detects malformed hashes and needs-rehash.
- `Code/security/CredentialBytes` implements the 16-byte/NUL password and
  6-byte token semantics, case sensitivity, the `@`/`_`/`#` prefix states and
  `LooksHashed` for import re-runs.
- Repository integration: creation stores a hash (never plaintext), password
  change invalidates the old credential, token binding is hashed and one-shot,
  failures create `credential_events` rows with no material, lockout resets on
  success and survives reconnects.
- `docs/operations.md` documents the policy, cost/limit keys and the residual
  low-entropy token risk.
- Unit tests (`tests/security/SecurityTests.cpp`): 8 cases / 43 assertions
  passing, including the standard `$2a$` known vector. **verified**.

## 6. DBSrv integration

Implemented:

- `CFileDB` owns a `storage::Database` + `AccountRepository`; `InitStorage`
  connects, migrates, refuses `import_state != complete`. `DBReadAccount`,
  `DBWriteAccount`, `DBExportAccount`, `CreateCharacter`, `DeleteCharacter`
  and `GetAccountByChar` now use the repository. `STRUCT_ACCOUNTFILE` stays
  the in-memory shape.
- Login uses `AdmitLogin` (date/state, coin clamp, temp-key bypass/clear,
  password throttling + upgrade) and keeps the duplicate-session, dual-sword
  and MAC-primary flows. Token binding/verification and character-deletion
  password checks use hashes.
- Capsule put-in/take-out use the repository and the capped sequence; the
  item 3443 import line uses the allocated id; `LastCapsule` is no longer read
  or written by DBSrv. Take-out keeps the legacy equipment/carry/coin/guild
  resets.
- NP login verifies the hash; NP account view reports the explicit state
  0-3 and an empty password; admin save ignores the submitted password.
- `CReadFiles` update logging no longer includes the password.
- Ranking load and the exp day-log use repository queries (local month-year
  windows preserved, including the January ranking behavior).
- Startup: storage config from `Config.txt` plus `W2PP_*` overrides; a missing
  database, newer schema contract or pending import aborts with a non-zero
  exit.
- `db_login_probe` sends `_MSG_DBAccountLogin` directly to DBSrv.
- `tests/integration/dbsrv_db_smoke.ps1` writes a known fixture with
  `legacy_fixture`, imports it with `dbimport`, starts DBSrv against the
  compose database, and checks the three login signals (admitted / wrong
  credential / missing account).
- `tests/integration/dbsrv_loopback.ps1` now requires the `W2PP_PG_*` env and
  runs `dbimport --empty`; without a database it exits 77 (CTest skip).

Verified:

- Build and code search: no `./account`, `./char` or `./capsule` game-path
  access remains in DBSrv (the day-log signature no longer takes a path).
- Repository behavior tests for load/save, create/delete, ranking/day-log.
- Live DBSrv: the loopback test passed with PostgreSQL (`game accept`,
  `admin accept`, redirect and console commands), and the login smoke produced
  `admitted=0 wrong=2 missing=2`.
- Startup failure modes: unreachable database and `import_state = pending`
  both exited 1.

Still pending verification (left unchecked): character create/delete/arch/
transfer packet paths (6.4), capsule put-in/take-out with the item 3443 flow
(6.5), NP admin login/save/disable/enable (6.6), and the `CReadFiles`
bulk import/update/donate fixtures (6.7). The code paths are wired; these need
protocol-level fixtures.

## 7. Legacy importer

- `Code/storage/LegacyImporter` + `Code/tools/dbimport`: `--source`, `--dsn`,
  `--dry-run`, `--replace`, `--report`, `--legacy-export`, `--empty`, advisory
  lock, schema check, record-size window, name normalization, credential
  hashing (prefix states, unusable values imported disabled), cargo/child/
  extras/celestial import via the codec, marker verification, capsule import
  and sequence seeding, per-account checksum verification and
  `import_state` transitions.
- `--legacy-export` writes 7808-byte account files, 1352-byte capsules and a
  `Config.txt` with the capsule maximum.
- Verified: `--help`, clean failure against an unreachable DSN, first import,
  idempotent re-run, truncated-record rejection + `failed` state, `--replace`,
  dry-run (no writes), marker/capsule reporting, monotonic capsule ids, and a
  complete export -> marker-completed restore -> fresh import round-trip with
  byte-identical gameplay exports (`tests/db/ImportTests.cpp`, 6 cases / 85
  assertions).
- Runbooks are in `docs/operations.md`.

## 8. Integration verification

### 8.1 Windows x86 Debug suite

- `ctest --preset x86-debug` component runs: 62 non-integration tests passed
  and `db-integration` passed in 16 s. The DB runner created its throwaway
  database, and no `w2pp-dbtest` container or volume remained afterwards.
  Result: **verified**.

### 8.2 Linux i386

- Not available in this environment (Windows host only). The Linux-specific
  runner and vcpkg triplet still need a run on a supported Linux host; this is
  recorded here per the task. DBSrv/unit code uses no Windows-only API in the
  new modules, and `w2pp_bcrypt`/libpq resolve through the same manifest.

### 8.3 - 8.5

- 8.3 is partially exercised: the import -> DBSrv -> DB login sequence runs in
  `dbsrv_db_smoke.ps1`. The full TMSrv login, item move, save/quit, restart and
  inventory/coin survival scenario is still pending.
- 8.4: a no-op save does not rewrite child arrays (xmin assertions in
  `RepositoryTests`), but login/save latency measurements are not recorded yet.
- 8.5: the importer only reads the source tree and the export test writes a
  restorable tree, but the "start the previous binary on the exported tree"
  check is still pending.
