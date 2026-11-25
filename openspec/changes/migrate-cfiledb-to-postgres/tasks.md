# Tasks

## 1. Dependency and build foundation

- [ ] 1.1 Add `libpq` to `vcpkg.json` at the existing baseline and link it into a probe target; verify configure and build succeed for the available supported triplet(s) and record the result for the other triplet in the change notes.
- [ ] 1.2 Vendor the BCrypt implementation under `third_party/bcrypt` with its license, add a `w2pp_bcrypt` static target, and verify a smoke executable produces a `$2b$` hash and validates it.
- [ ] 1.3 Add the `w2pp_storage` static library (storage, security, codec) to `CMakeLists.txt` and link it into `dbsrv`; verify the build still succeeds with no behavior change.
- [ ] 1.4 Add `tests/db/docker-compose.yml` pinned to PostgreSQL 19 with a healthcheck, plus `tests/db/run_db_tests.ps1` and `tests/db/run_db_tests.sh` that start the stack, wait for health, run a placeholder command and always tear down; verify both scripts up and down the stack and that no container or volume remains.
- [ ] 1.5 Register the database test wiring in CTest (`W2PP_DB_TESTS` option, skip code 77, configure status message) and verify `ctest -N` shows the test when enabled and omits it with `-DW2PP_DB_TESTS=OFF`.
- [ ] 1.6 Document the new dependency, opt-out switch and container prerequisite in `README.md`; verify the documented commands match the scripts and configure options.

## 2. Schema and migrations

- [ ] 2.1 Implement the schema version table, `store_metadata`, advisory-locked migration runner and version 1 DDL for accounts, characters, child item/affect tables, extras, celestial saves and credential events; verify a fresh database migrates to version 1 and records the version.
- [ ] 2.2 Add the uniqueness, range and exclusivity constraints (account name key, character name key, live slot, capsule link, position ranges, byte widths); verify each violation is rejected by an integration test.
- [ ] 2.3 Verify a failing migration leaves the previous version intact and reports the failing step.
- [ ] 2.4 Document the schema, table purposes and key invariants in `docs/storage-schema.md`; verify every table and constraint in the document exists in the migration DDL.

## 3. Record codec

- [ ] 3.1 Implement account scalar mapping between `STRUCT_ACCOUNTFILE` and repository rows (identity, registration fields, block date, coin, donate, temp key, quick-skill bytes); verify unit round-trips for fully populated and all-zero accounts.
- [ ] 3.2 Implement item-array mapping for equipment, carried inventory and account cargo with explicit positions; verify unit tests cover all positions populated, sparse positions, duplicate item indices with different effects, and empty arrays.
- [ ] 3.3 Implement affect, character-extra and celestial-save mapping; verify unit round-trips for populated, sparse and all-zero values, including signed resist bytes and unsigned affect time.
- [ ] 3.4 Implement the structural checksum/normalization step that zeroes padding and unused legacy bytes; verify a record with garbage padding imports to a zero-padded reconstruction while every defined field is unchanged.
- [ ] 3.5 Verify codec output keeps the guarded sizes and offsets (7808/816/536/1352 bytes, documented offsets) through the existing ABI guards and a unit test.

## 4. Storage repository

- [ ] 4.1 Implement connection/config resolution (Config.txt plus environment overrides), prepared statements, transaction guard and timeout settings; verify integration tests cover connect failure, explicit rollback and statement timeout.
- [ ] 4.2 Implement account load and save (identity lookup by name key, insert, update, updated_at maintenance); verify an integration test writes and reads back every account field.
- [ ] 4.3 Implement character create, update, delete and lookup by name with slot/name conflicts surfaced as the existing failure results; verify integration tests for duplicate names, freed names, out-of-range slots and lookup by character name.
- [ ] 4.4 Implement child-array replacement keyed by `character_id`/`account_id`; verify an integration test confirms a full and a sparse array round-trip and that re-saving an unchanged array does not rewrite its rows.
- [ ] 4.5 Implement capsule put-in, take-out and the capped capsule sequence; verify integration tests for restart persistence, monotonically increasing identifiers and rollback when clearing the slot fails.
- [ ] 4.6 Implement login-admission persistence (coin clamp, temp-key clear, date/state checks, hash upgrade) as one transaction; verify an integration test interrupts a step and confirms no partial admission.
- [ ] 4.7 Implement the day-log, ranking and character-name queries with legacy selection semantics (local month boundaries, stale-account rules, level filter); verify parity tests against expected seeded results.
- [ ] 4.8 Implement the 7808-byte export projection with empty credential fields; verify an integration test exports an imported account and asserts size, layout and non-credential field equality.

## 5. Credential security

- [ ] 5.1 Implement `PasswordHash` (generate, verify, needs-rehash, cost validation) over the vendored BCrypt; verify known-vector, malformed-hash, cost-range and salt-uniqueness unit tests.
- [ ] 5.2 Implement credential byte extraction with the legacy 16-byte/NUL semantics; verify unit tests for 15-byte, 16-byte unterminated, case sensitivity and embedded NUL input.
- [ ] 5.3 Implement credential persistence: password/token hash columns, unset-token state, account state column and prefix-marker conversion; verify integration tests for create, change, verify, wrong credential and each legacy prefix.
- [ ] 5.4 Implement failed-verification counting, lockout windows and reset-on-success; verify integration tests for lockout, expiry, restart persistence and audit rows containing no credential material.
- [ ] 5.5 Implement transparent hash upgrade on successful verification; verify an integration test that a below-cost hash is replaced and a current-cost hash is untouched.
- [ ] 5.6 Wire the security module into account creation, password update and account export; verify no code path writes plaintext and the export test shows empty credential fields.
- [ ] 5.7 Document the hashing policy, cost/limit configuration and the residual low-entropy token risk in `docs/operations.md`; verify the documented setting names match `Config.txt` and environment handling.

## 6. DBSrv integration

- [ ] 6.1 Replace the file primitives inside `CFileDB` (`DBReadAccount`, `DBWriteAccount`) with repository calls while preserving method signatures; verify the server builds and an integration test loads, saves and reloads an account.
- [ ] 6.2 Remove the remaining game-path `./account`, `./char` and `./capsule` reads/writes and replace character index marker operations with database constraints; verify a code search finds no game-path file store usage and the delete/create tests pass.
- [ ] 6.3 Rewire login and session handling (coin clamp, dated block, temp-key bypass, duplicate session, dual-sword swap, MAC primary broadcast) onto the repository; verify parity scenarios through repository tests and a DBSrv login probe.
- [ ] 6.4 Rewire character create, delete, arch-character create and transfer paths; verify the reply signals and persisted state match the specification scenarios.
- [ ] 6.5 Rewire capsule put-in/take-out, including the item 3443 import line and `LastCapsule` removal; verify an integration test seals, restarts DBSrv and restores a character.
- [ ] 6.6 Rewire admin paths: NP login verification, account request, account save, disable and enable, plus state reporting 0–3; verify an admin integration test sees no password material and correct state values.
- [ ] 6.7 Rewire `CReadFiles` bulk user import, update, donate import and item import account access; verify fixture tests import and update accounts without logging credentials.
- [ ] 6.8 Replace ranking and day-log directory scans with repository queries; verify output parity against fixtures with current-month, previous-month, stale, admin-level and missing-name characters.
- [ ] 6.9 Implement startup validation: config parsing, connect, schema contract/version check, pending migrations, `import_state = complete`; verify each failure mode exits non-zero with an actionable message and no fallback to files.
- [ ] 6.10 Add a database-backed login probe tool that speaks the DB protocol to DBSrv; verify it reports success for a valid account and the existing failure signals for wrong password, blocked account and missing account.
- [ ] 6.11 Document the new DBSrv prerequisites, `Config.txt` keys, environment overrides and operator-visible admin changes in `docs/operations.md`; verify the runbook steps reproduce a working start from an empty database.

## 7. Legacy importer

- [ ] 7.1 Create the `dbimport` executable with `--source`, `--dsn`, `--dry-run`, `--replace`, `--report`, `--legacy-export` and `--empty`, plus the advisory lock and schema check; verify `--help` output and a clean failure against an unreachable DSN.
- [ ] 7.2 Implement account import from the legacy tree, including the record-size window, name normalization, credential hashing, prefix states and quick-skill bytes; verify a fixture tree imports with the expected rows and hashes.
- [ ] 7.3 Implement character, equipment, inventory, cargo, affect, extra and celestial-save import; verify fixture records with all positions populated and with sparse arrays import losslessly.
- [ ] 7.4 Implement `./char` marker verification, capsule import and `LastCapsule` sequence seeding; verify fixtures report missing/extra markers and that capsule identifiers continue above the legacy maximum.
- [ ] 7.5 Implement verification reporting: per-account structural checksums, counts, rejected-file details and `import_state` transitions; verify a clean run sets `complete`, a run with a truncated record sets `failed` and exits non-zero, and a re-run makes no changes.
- [ ] 7.6 Implement `--legacy-export` and verify a full import → export → import cycle reproduces the same database state and a byte-comparable legacy tree for defined fields.
- [ ] 7.7 Write the cutover runbook in `docs/operations.md` (dry run, import, verification, smoke test, cutover, rollback); verify each command in the runbook against the test compose instance.

## 8. Integration verification

- [ ] 8.1 Run the full unit and database integration suite on Windows x86 Debug with Docker available; verify all tests pass, the database test creates and drops its throwaway database, and no containers remain.
- [ ] 8.2 Run the full suite on Linux i386 when a supported Linux host is available; verify the same tests pass and record the result in the change notes if the host is unavailable.
- [ ] 8.3 Run an end-to-end scenario against the compose database: import fixtures, start DBSrv and TMSrv, log in, move an item, save and quit, restart, and verify inventory and coin survive; attach the log excerpt to the change notes.
- [ ] 8.4 Verify persistence performance sanity: measure login and save latency and assert a no-op save does not rewrite unchanged child arrays; record the measurements in the change notes.
- [ ] 8.5 Verify the legacy trees are untouched after the end-to-end run and that a rollback export can be started by the previous binary.

## Workflow follow-up

- Archive the change after review and after the `linux-x64-port` change has been archived (it modifies the same build and host capabilities).
- Re-run the documented test command after archiving to confirm the specs and implementation agree.
