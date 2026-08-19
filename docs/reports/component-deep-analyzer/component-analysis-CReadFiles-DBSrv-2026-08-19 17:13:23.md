# Component Deep Analysis Report

**Component:** CReadFiles (DBSrv)
**Project:** W2PP (legacy C/C++ game server)
**Scope analyzed:** `/home/luisdias/dev/github/luisdiasdev/w2pp/legacy/Code/DBSrv`
**Folders ignored:** `.git`, `.opencode`
**Date:** 2026-08-19 17:13

---

## 1. Executive Summary

`CReadFiles` is a **static-method-only utility class** inside the DBSrv (accounts/characters database server) of the W2PP legacy MMO game-server codebase. Its role is **file-based data import and export**: it consumes external "drop-box" files produced by operators/admin tools and integrates them into the persistent account store managed by `CFileDB`, and it exports runtime state (connection counts, ranking, guild info) to flat files for external consumption (web dashboards, ranking pages, server-transfer tooling).

The class is defined in `Code/DBSrv/CReadFiles.h` (56 lines) and implemented in `Code/DBSrv/CReadFiles.cpp` (1,066 lines). It contains nine public static methods:

| Method | Kind | Purpose |
|--------|------|---------|
| `UpdateConnection` | Export | Writes live per-server connection counts to a WAMP web file |
| `UpdateConnectionData` | Export | Appends timestamped connection-history rows to a CSV file |
| `ImportItem` | Import | Applies operator-requested item grants to a player's cargo |
| `ImportUser` | Import | Creates new player accounts from plaintext registration files |
| `UpdateUser` | Import | Applies account password changes from drop-box files |
| `ImportDonate` | Import | Credits donation (cash-shop) points to an account |
| `ReadGuildInfo` | Import | Loads the persisted guild table at boot |
| `WriteGuildInfo` | Export | Persists the in-memory guild table to disk |
| `WriteRanking` | Export | Writes the top-N exp ranking to a plaintext file |

The class is a **pure static service** — it holds no instance state; every member is `static` and all methods operate on shared globals (`cFileDB`, `pUser`, `UserConnection`, `GuildInfo`, `rankingSystem`, `ServerIndex`) defined elsewhere in the DBSrv translation unit.

**Key findings:**

- **Scheduling-driven component.** None of the import/export methods are invoked directly from gameplay. They are driven by the DBSrv boot sequence (`Server.cpp` `WinMain`) and by `ProcessSecTimer()` timers (`Server.cpp:2013-2084`) on fixed cadences (every 1 s, 30 s, 600 s, and 900 s), plus event-driven calls from `CFileDB::ProcessMessage` and `CRanking`.
- **Shared drop-box contract.** All import formats are plaintext, space/line-delimited files dropped into a shared `../../Common/...` folder that both servers and external operator/admin tools write to. There is no schema, no handshake, and no atomicity beyond simple file-move/delete.
- **Runtime "live" path.** For accounts currently logged in, `ImportItem`/`ImportDonate` send the grant directly to the connected TMSrv over the socket (`MSG_DBSendItem` / `MSG_DBSendDonate`) **and** also persist to disk; for offline accounts they only persist to disk.
- **File-system persistence with a fragile recover/cleanup model.** Successful imports delete the source file; failed imports `MoveFile` the file into an `Error/` folder. Deletion failures are logged as warnings rather than retried.
- **Hard-coded environment coupling.** `UpdateConnection` writes to the absolute Windows path `C:/wamp/www/serv%2.2d.htm`, coupling the DBSrv to a specific WAMP web-server layout.
- **No automated tests exist** anywhere in the project (verified across the whole repository), so this component — like the rest of the codebase — has zero automated test coverage.

---

## 2. Data Flow Analysis

The component has two primary flow directions: **inbound import** (files → account store / live sockets) and **outbound export** (runtime state → files). Flows are driven by the boot sequence and the per-second timer.

### Import flows

```
1. Boot: WinMain (Server.cpp:583) calls CReadFiles::ImportItem()
2. Operator/admin tool writes plaintext file into ../../Common/ImportItem/<name>
3. ProcessSecTimer (Server.cpp:2015-2023) periodically calls:
     ImportUser()  (every 1 s)
     UpdateUser()  (every 1 s)
     ImportItem()  (every 30 s)
     ImportDonate()(every 30 s)
4. CReadFiles scans the drop folder with _findfirst/_findnext
5. For each file:
   a. Parse fields with fgets/sscanf
   b. Validate ranges and lengths
   c. Look up account (cFileDB.GetIndex / cFileDB.DBReadAccount)
   d. If account ONLINE (Slot 0..3):
        Send runtime packet MSG_DBSendItem / MSG_DBSendDonate via pUser[svr].cSock.SendOneMessage
   e. Mutate STRUCT_ACCOUNTFILE (cargo slot or Donate field)
   f. Persist via cFileDB.DBWriteAccount (or cFileDB.AddAccount / UpdateAccount)
   g. On success DeleteFile(source); on failure MoveFile(source → Error folder)
```

### Export flows

```
1. ProcessSecTimer cadence:
     UpdateConnection()      every 30 s  -> C:/wamp/www/serv%2.2d.htm
     UpdateConnectionData()  every 900 s -> ../../Common/data%2.2d.csv (append)
     WriteRanking()          every 600 s -> ../../Common/Ranking.txt
2. Weekly (Sun 00:00, Server.cpp:2061-2082):
     guild Fame reset loop -> CReadFiles::WriteGuildInfo() -> ../../Common/GuildInfo
3. Event-driven:
     CFileDB::ProcessMessage _MSG_GuildInfo (CFileDB.cpp:480) -> WriteGuildInfo()
     CRanking::RankingSystem::loadRanking (CRanking.cpp:110) -> WriteRanking()
4. Boot: WinMain (Server.cpp:615) calls CReadFiles::ReadGuildInfo() to load guild table
```

---

## 3. Business Rules & Logic

## Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Item file: reject if item `Index` out of `[0, MAX_ITEMLIST)` or any effect outside `[0, 255]` | CReadFiles.cpp:209 |
| Validation | Item/Donate file: reject if file has no contents (empty `fgets`) | CReadFiles.cpp:185-199, 770-786 |
| Validation | Item/Donate: reject import for account not present in the account DB | CReadFiles.cpp:267-282, 843-858 |
| Validation | User file: account/pass/name/email/tel/addr length must be nonzero and below the field limits | CReadFiles.cpp:445-664 |
| Validation | User file: SSN1 and SSN2 must be nonzero | CReadFiles.cpp:531-565 |
| Validation | Donate file: reject negative donation amount | CReadFiles.cpp:796 |
| Validation | Import folder scan: skip dotfiles, cap at 10 files per pass | CReadFiles.cpp:155-163, 150-153, 746-749 |
| Business Logic | Online account item/donate grant also sent live via `MSG_DBSendItem`/`MSG_DBSendDonate` | CReadFiles.cpp:233-257, 810-833 |
| Business Logic | Item cargo placement tries 126 slots by `BASE_CanCargo`, then first empty slot | CReadFiles.cpp:284-329 |
| Business Logic | Donate is accumulated (`file.Donate += Donate`), not replaced | CReadFiles.cpp:862 |
| Business Logic | ImportUser bonus grants 100 starter items (sIndex 401/406) via CFileDB::AddAccount | CReadFiles.cpp:677 (→ CFileDB.cpp:126-135) |
| Business Logic | Item/user/donate import files are deleted on success; moved to `Error/` on failure | CReadFiles.cpp:358-368, 888-898, 680 |
| Business Logic | UpdateUser deletes the file regardless of update success/failure | CReadFiles.cpp:1017-1030 |
| Business Logic | UpdateConnection fakes connection count as `(4*Count/3)+rand()%4`, capped by `UserConnection` | CReadFiles.cpp:79-82 |
| Business Logic | UpdateConnectionData resets `UserConnection[]` to 0 after each export and appends a total | CReadFiles.cpp:115-125 |
| Business Logic | Ranking export writes entries with `Level < 1000` in descending rank order, excluding index 0 | CReadFiles.cpp:1055-1064 |
| Business Logic | Ranking class labels mapped from `ClassMaster`/`Class` via fixed arrays | CReadFiles.cpp:1052-1053 |
| Business Logic | GuildInfo read/write is a raw binary block copy of `GuildInfo[65536]` | CReadFiles.cpp:692-724 |
| Error Handling | Log/MessageBox on missing GuildInfo file at boot; silent return on most other failures | CReadFiles.cpp:698-703, 715-720 |

## Detailed breakdown of the business rules

---

### Business Rule: Item import (operator item grants)

**Overview**

`ImportItem()` reads plaintext files placed in `../../Common/ImportItem/` and grants a single item (identified by item index and up to three effect/value pairs) to a specified player account's cargo. It is invoked at boot (`Server.cpp:583`) and every 30 seconds (`Server.cpp:2022`). It is the "GM/operator grant" channel of the DBSrv — the mechanism by which an external tool or web panel requests that a specific item be placed into a specific player's persistent storage.

**Detailed description**

Each file in the drop folder contains a single line of the form `ACCOUNT INDEX EFF1 VAL1 EFF2 VAL2 EFF3 VAL3`. The account name is uppercased, and the item index plus the three effect codes are validated against hard ranges: the index must satisfy `0 <= Index < MAX_ITEMLIST` (6500) and each effect code must be in `[0, 255]`. The account is resolved first against the live session table via `cFileDB.GetIndex(ids)` — which returns a nonzero index only if the account is currently logged in on some TMSrv. If the account is online and has a character selected (`Slot` in 0..3), the DBSrv builds a `MSG_DBSendItem` packet and pushes it to the owning TMSrv through `pUser[svr].cSock.SendOneMessage`, so the grant takes effect on the live character immediately. Independently of the live path, the item is written to the persistent account file. The account file is read with `cFileDB.DBReadAccount`; if the account does not exist in the DB, the file is moved to the error folder (unless it was already sent live). The code then searches the cargo array for a valid placement: first it iterates slots `0..125` (the first 126 of `MAX_CARGO`=128) calling `BASE_CanCargo` to respect item-grid collisions, and if none of those fit it falls back to the first empty slot scanning backward from index 127. If no slot is found, the file is moved to the error folder. When a slot is found, the `STRUCT_ITEM` is copied in via two 4-byte integer copies and the account is written back with `cFileDB.DBWriteAccount`. On a successful write the source file is deleted; if deletion fails the file is moved to the error folder and a warning is logged. Every file processed is limited to a maximum of 10 per pass, and dotfiles (`.`/`..`) are skipped.

**Rule workflow**

1. Scan `../../Common/ImportItem/*.*` with `_findfirst`.
2. Skip dotfiles; stop after 10 files.
3. Open file, read one line; if empty → move to `Error/`, continue.
4. Parse `account index eff1 val1 eff2 val2 eff3 val3`.
5. Validate index/effect ranges; invalid → move to `Error/`, continue.
6. Resolve account via `GetIndex`; if online with a selected slot, send `MSG_DBSendItem` live.
7. Read account file via `DBReadAccount`; if missing → move to `Error/` (unless already sent), continue.
8. Find a cargo slot via `BASE_CanCargo` (slots 0..125), else first empty slot (127..0).
9. No slot → move to `Error/`, continue.
10. Write item into `file.Cargo[Pos]`; `DBWriteAccount`.
11. On success `DeleteFile`; on delete failure `MoveFile` to `Error/` + warning log.

---

### Business Rule: User account registration import

**Overview**

`ImportUser()` processes plaintext multi-line registration files placed in `../../Common/ImportUser/` to create new player accounts in bulk. It is invoked every second by `ProcessSecTimer` (`Server.cpp:2015`). Each file is a fixed sequence of nine lines describing one new account: id, password, real name, SSN1, SSN2, email, telephone, address, and an optional bonus flag.

**Detailed description**

The file layout is positional and newline-delimited, parsed with successive `fgets`/`sscanf` calls. The account id is uppercased and forcibly truncated to `ACCOUNTNAME_LENGTH` (16). Each textual field is validated for nonzero length and against its field ceiling: password `< ACCOUNTPASS_LENGTH` (12), name `< REALNAME_LENGTH` (24), email `< EMAIL_LENGTH` (48), telephone `< TELEPHONE_LENGTH` (16), and address `<= ADDRESS_LENGTH` (78). The two SSN integers must both be nonzero. If any read or validation fails, the file is skipped (not moved to an error folder — the file is simply closed and iteration continues) and processing moves to the next file. On successful parsing, the data is handed to `cFileDB.AddAccount(id, pass, name, ssn1, ssn2, email, tel, addr, bonus)`. `AddAccount` (CFileDB.cpp:78) itself enforces reserved-name rejection (`COM*`/`LPT*`), rejects duplicate accounts, and — when `bonus != 0` — seeds the new account's cargo with 100 starter items (`sIndex` 401 for the first 50, 406 for the next 50). When `AddAccount` returns nonzero (success), the source file is deleted.

**Rule workflow**

1. Scan `../../Common/ImportUser/*.*`; skip dotfiles.
2. For each file read 9 lines: id, pass, name, ssn1, ssn2, email, tel, addr, bonus.
3. Validate each field length; abort file (skip) on any failure.
4. Validate SSN1/SSN2 nonzero.
5. Uppercase/truncate id.
6. Call `cFileDB.AddAccount(...)` with the bonus flag.
7. On success `DeleteFile`; else leave the file for the next pass.

---

### Business Rule: Account password update import

**Overview**

`UpdateUser()` processes two-line files in the per-server update folder `../../Common/serv%2.2d/update/` (where `%2.2d` is `ServerIndex`) to change an existing account's password. It runs every second (`Server.cpp:2016`).

**Detailed description**

Each file contains two lines: the account name and the new password. The account is uppercased/truncated and length-checked against `ACCOUNTNAME_LENGTH` and `ACCOUNTPASS_LENGTH`. The update is delegated to `cFileDB.UpdateAccount(id, pass)` (CFileDB.cpp:145), which re-reads the account file, sets the new password, and writes it back. Unlike the item and donate flows, `UpdateUser` **always deletes the source file** after the attempt — regardless of whether `UpdateAccount` succeeded or failed — and logs a "SUCCESS" or "FAIL" entry accordingly. This means a failed update is silently dropped (the request file is consumed either way), which is a notable data-loss risk if the delete happens before a transient failure is resolved.

**Rule workflow**

1. Scan `../../Common/serv<ServerIndex>/update/*.*`; skip dotfiles.
2. Read account line and password line.
3. Validate lengths; skip file on failure.
4. Call `cFileDB.UpdateAccount(id, pass)`.
5. Delete the source file unconditionally.
6. Log success/failure with account, password, and filename.

---

### Business Rule: Donation (cash-shop) point credit

**Overview**

`ImportDonate()` credits donation points to a player account from files in `../../Common/ImportDonate/`. It runs every 30 seconds (`Server.cpp:2023`). This is the DBSrv channel that turns cash-shop purchase records (written by the admin tool into the drop folder) into persistent `Donate` points on the account.

**Detailed description**

Each file is a single line `ACCOUNT AMOUNT`. The amount must be non-negative (`Donate < 0` → move to error folder). If the account is currently online with a selected slot, a `MSG_DBSendDonate` packet is sent live to the owning TMSrv so the balance updates immediately on the connected character. Independently, the persistent account file is read and the amount is **accumulated** (`file.Donate += Donate`), so repeated imports add to the stored total rather than overwriting it. The account is written back via `DBWriteAccount`; on success the source file is deleted, and on failure it is moved to the error folder (unless already sent live). This mirrors the item-import flow's dual live/persistent model and its delete-on-success/move-on-fail cleanup.

**Rule workflow**

1. Scan `../../Common/ImportDonate/*.*`; stop after 10 files.
2. Read `account amount`; reject negative amounts (move to `Error`).
3. Resolve account via `GetIndex`; if online with slot, send `MSG_DBSendDonate` live.
4. Read account file via `DBReadAccount`; if missing → move to `Error` (unless live), continue.
5. Accumulate `file.Donate += Donate`.
6. `DBWriteAccount`.
7. On success `DeleteFile`; on failure move to `Error`.

---

### Business Rule: Connection-count reporting (web dashboard)

**Overview**

`UpdateConnection()` exports a per-server online-count report to a web-readable file, and `UpdateConnectionData()` appends a timestamped history row to a CSV. They run every 30 s and 900 s respectively (`Server.cpp:2030, 2036`).

**Detailed description**

`UpdateConnection` writes one line per server (up to `MAX_SERVER`=10) into the absolute path `C:/wamp/www/serv%2.2d.htm`. For an empty server (`pUser[i].Mode == USER_EMPTY`) it writes `-1`; otherwise it emits a **deliberately fuzzed** count computed as `(4 * pUser[i].Count / 3) + rand() % 4`, and it first raises the running `UserConnection[i]` high-water mark to the current `pUser[i].Count` if the current count is larger. This rounding/fuzzing is a legacy anti-leak/heuristic measure that intentionally does not report the exact live count. `UpdateConnectionData` appends a row to `../../Common/data%2.2d.csv` containing a timestamp (`YYYY_MM_DD_HH`) and each server's `UserConnection[i]`, followed by the grand total; it then **resets all `UserConnection[]` entries to zero**, making it a periodic high-water-mark sampler. The two methods together feed a web page (WAMP) and a CSV history used for operator/server-status monitoring.

**Rule workflow**

1. `UpdateConnection`: build path with `ServerIndex`; open `wt`; per server write `-1` or `(4*Count/3)+rand()%4`, updating the high-water mark; close.
2. `UpdateConnectionData`: build CSV path; open `a+`; write timestamp + each `UserConnection[i]` + total; reset all counters to 0; close.

---

### Business Rule: Ranking export

**Overview**

`WriteRanking()` exports the top-N experience ranking held in `rankingSystem.grindRanking` to `../../Common/Ranking.txt`. It is invoked every 600 s (`Server.cpp:2044`) and once at ranking load (`CRanking.cpp:110`).

**Detailed description**

The method writes one line per ranked player of the form `POSITION NAME CLASSMASTER-LABEL CLASS-LABEL`. It iterates the `GrindRanking` table from `RankPos::FIRST` (index `MAX_RANK_INDEX-1` = 499) down to just above `RankPos::LAST` (index 0 is excluded because the loop condition is `i > RankPos::LAST`, i.e. `i > 0`). Only entries whose `Name[0] != 0` and `Level < 1000` are emitted (the `< 1000` filter excludes test/placeholder accounts with inflated levels). The `ClassMaster` value indexes a six-element label array (`SEM CLASSMASTER`, `M`, `A`, `C`, `C`, `SC`) and `Class` indexes a four-element label array (`TK`, `FM`, `BM`, `HT`). This produces a web-facing ranking file consumed by external ranking pages.

**Rule workflow**

1. Open `../../Common/Ranking.txt` for write.
2. For rank index 499 down to 1: if name present and `Level < 1000`, write `pos name classlabel classlabel`; increment `pos`.
3. Close file.

---

### Business Rule: Guild info persistence

**Overview**

`ReadGuildInfo()` / `WriteGuildInfo()` load and store the in-memory guild table `GuildInfo[65536]` (a `STRUCT_GUILDINFO` array) to/from the binary file `../../Common/GuildInfo`. `ReadGuildInfo` runs at boot (`Server.cpp:615`); `WriteGuildInfo` is triggered weekly on Sunday at 00:00 after the guild-fame reset (`Server.cpp:2082`) and whenever a `_MSG_GuildInfo` update arrives (`CFileDB.cpp:480`).

**Detailed description**

Both methods operate on the raw binary block using `_open`/`_read`/`_write` (POSIX-style low-level I/O). `ReadGuildInfo` opens the file `_O_RDONLY | _O_BINARY` and reads `sizeof(GuildInfo)` bytes directly into the global array; if the file is missing at boot it raises a modal `MessageBoxA` ("no GuildInfo file", "BOOTING ERROR") and returns. `WriteGuildInfo` opens with `O_RDWR | O_CREAT | O_BINARY` and writes the whole array back. This is a wholesale binary snapshot: the entire 65536-entry guild table (fame, clan, citizen, and reserved bytes per entry) is persisted as one contiguous structure, giving no partial-write or per-guild granularity. The file is the shared persistence point for guild fame, which is used by the game servers through `cFileDB.SendGuildInfo`.

**Rule workflow**

1. `ReadGuildInfo` (boot): open `../../Common/GuildInfo` binary read-only; if missing → MessageBox + return; else read `sizeof(GuildInfo)` bytes; close.
2. `WriteGuildInfo` (weekly / on update): open with create; write `sizeof(GuildInfo)` bytes; close.

---

## 4. Component Structure

```
legacy/Code/DBSrv/
├── CReadFiles.h          # Class declaration (static constants + 9 static methods)
├── CReadFiles.cpp        # Implementation (1,066 lines)
├── Server.cpp            # CALLER: boot + ProcessSecTimer scheduling, Log()
├── Server.h              # CALLER CONTEXT: globals (ServerIndex, UserConnection, hWndMain, pUser)
├── CFileDB.cpp           # DEPENDENCY + CALLER (cFileDB; also calls WriteGuildInfo)
├── CFileDB.h             # DEPENDENCY: cFileDB, GuildInfo, STRUCT_ACCOUNTLIST
├── CUser.h               # DEPENDENCY: pUser structure (Mode, Count, cSock)
├── CRanking.cpp          # DEPENDENCY + CALLER (rankingSystem; calls WriteRanking)
├── CRanking.h            # DEPENDENCY: rankingSystem, RankPos, GrindRanking
└── (shared) Code/Basedef.h / Basedef.cpp   # DEPENDENCY: STRUCT_*, MSG_*, constants, BASE_CanCargo
```

Method-to-line map (`CReadFiles.cpp`):

```
UpdateConnection()       :60-86
UpdateConnectionData()   :88-128
ImportItem()             :130-378
ImportUser()             :380-690
ReadGuildInfo()          :692-707
WriteGuildInfo()         :709-724
ImportDonate()           :726-908
UpdateUser()             :910-1041
WriteRanking()           :1043-1065
```

Path constants (`CReadFiles.cpp:39-58`):

| Constant | Value | Use |
|----------|-------|-----|
| `UPDATE_CONNECTION_PATH` | `C:/wamp/www/serv%2.2d.htm` | Export web connection report |
| `UPDATE_CONNECTION_DATA_PATH` | `../../Common/data%2.2d.csv` | Export CSV history |
| `IMPORT_ITEM_PATH` / `IMPORT_ITEM2_PATH` | `../../Common/ImportItem/*.*` / `%s` | Item grant drop-box |
| `IMPORT_ITEM_ERROR_PATH` | `../../Common/Error/%s` | Item grant failure quarantine |
| `IMPORT_USER_PATH` / `IMPORT_USER2_PATH` | `../../Common/ImportUser/*.*` / `%s` | Account registration drop-box |
| `GUILD_INFO_PATH` | `../../Common/GuildInfo` | Guild table binary snapshot |
| `IMPORT_DONATE_PATH` / `IMPORT_DONATE2_PATH` | `../../Common/ImportDonate/*.*` / `%s` | Donation drop-box |
| `IMPORT_DONATE_ERROR_PATH` | `../../Common/ImportDonateError/%s` | Donation failure quarantine |
| `UPDATE_USER_PATH` / `UPDATE_USER2_PATH` | `../../Common/serv%2.2d/update/*.*` / `%s` | Password-change drop-box |
| `RANKING_PATH` | `../../Common/Ranking.txt` | Ranking export |

---

## 5. Dependency Analysis

### Internal Dependencies

```
CReadFiles → cFileDB (CFileDB global): GetIndex(char*), DBReadAccount, DBWriteAccount,
              AddAccount, UpdateAccount, pAccountList[].Slot
CReadFiles → pUser (CUser[] global): [svr].Mode, [svr].Count, [svr].cSock.SendOneMessage
CReadFiles → CPSock: SendOneMessage(char*, int)
CReadFiles → Server globals: ServerIndex, UserConnection[], hWndMain, Log(...)
CReadFiles → CRanking global: rankingSystem.grindRanking.getElement(i)->{Name,Level,ClassMaster,Class}
CReadFiles → Basedef: STRUCT_ITEM, STRUCT_ACCOUNTFILE, STRUCT_RANKING,
              MSG_DBSendItem, MSG_DBSendDonate, BASE_CanCargo, all size constants
```

### External Dependencies

| Dependency | Type | Notes |
|------------|------|-------|
| Windows CRT (`windows.h`, `io.h`, `fcntl.h`, `sys/stat.h`, `sys/timeb.h`) | System | `_findfirst/_findnext/_findclose`, `_open/_read/_write/_close`, `MoveFile`, `DeleteFile`, `MessageBoxA` |
| Filesystem (drop folders + exports) | External data | Plaintext/binary files under `../../Common/...` and `C:/wamp/www/...` |
| Winsock (via CPSock) | System | Indirect, only for live grant packets |
| WAMP web server layout | External env | Hard-coded `C:/wamp/www/serv%2.2d.htm` |

No third-party frameworks are used; the class is Windows-specific (uses Windows file API and CRT POSIX shims).

---

## 6. Afferent and Efferent Coupling

Coupling is measured at the **source-file/translation-unit** level (the natural "component" boundary for this C-style codebase). Afferent (Ca) = distinct callers; Efferent (Ce) = distinct dependencies.

| Component | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Critical |
|-----------|------------------------|------------------------|----------|
| CReadFiles (DBSrv) | 3 | 6 | High |
| — Server.cpp | caller (uses 9 methods) | — | — |
| — CFileDB.cpp | caller (WriteGuildInfo, :480) | — | — |
| — CRanking.cpp | caller (WriteRanking, :110) | — | — |

Efferent dependencies of `CReadFiles` (Ce = 6): `CFileDB`, `CUser`+`CPSock`, `Server` (globals + `Log`), `CRanking`, `Basedef` (structs/messages/`BASE_CanCargo`), and the Windows CRT/filesystem.

Notes: The class has low afferent coupling (only three translation units invoke it) but high efferent reach into nearly every other DBSrv subsystem and the shared `Basedef` contract, plus the filesystem. It is therefore a **leaf "consumer" of global state** — it depends broadly on the rest of the server rather than being depended upon broadly. Because all coupling flows through global objects (`cFileDB`, `pUser`, `rankingSystem`, `GuildInfo`) rather than constructor-injected dependencies, the class is implicitly coupled to initialization order and shared mutable state, making it hard to test or reuse in isolation.

---

## 7. Endpoints

The DBSrv `CReadFiles` component does **not** expose any network endpoints (REST/GraphQL/gRPC/socket protocol handlers) of its own. It is invoked internally by the DBSrv's `ProcessSecTimer` and boot sequence, and it participates only indirectly in the DBSrv's socket protocol through the `MSG_DBSendItem` / `MSG_DBSendDonate` packets it constructs to forward grants to connected TMSrv instances. The authoritative socket endpoints for DBSrv are defined in `Server.cpp` (`DB_PORT` 7514, `ADMIN_PORT` 8895) and are outside this component's boundary. Consequently, this section is intentionally omitted.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Item drop-box (`../../Common/ImportItem`) | Filesystem (shared) | Operator item grants | File drop + scan | Plaintext `ACCOUNT INDEX EFF VAL...` | Move to `Error/` + log |
| Donation drop-box (`../../Common/ImportDonate`) | Filesystem (shared) | Cash-shop point credits | File drop + scan | Plaintext `ACCOUNT AMOUNT` | Move to `ImportDonateError/` + log |
| Registration drop-box (`../../Common/ImportUser`) | Filesystem (shared) | Bulk account creation | File drop + scan | 9-line plaintext record | Skip file, log nothing |
| Password update drop-box (`../../Common/serv<idx>/update`) | Filesystem (shared) | Password changes | File drop + scan | 2-line plaintext record | Delete file unconditionally |
| TMSrv live grants | Inter-process (socket) | Live item/donate credit | Custom binary (`MSG_DBSendItem`/`MSG_DBSendDonate`) | Binary packet via `CPSock::SendOneMessage` | None (fire-and-forget) |
| Account persistence (`cFileDB`) | Internal service | Read/write account records | In-process method calls | `STRUCT_ACCOUNTFILE` binary | Return-code check → move to `Error/` |
| Web connection report (`C:/wamp/www/serv%2.2d.htm`) | External (WAMP web) | Online-status dashboard | File write | Plaintext lines | Silent return on open failure |
| Connection history CSV (`../../Common/data%2.2d.csv`) | Filesystem | Connection sampling | File append | CSV | Silent return on open failure |
| Ranking export (`../../Common/Ranking.txt`) | Filesystem | Web ranking page | File write | Plaintext lines | Silent return on open failure |
| Guild snapshot (`../../Common/GuildInfo`) | Filesystem | Guild persistence | Binary read/write | Raw `STRUCT_GUILDINFO[65536]` | MessageBox at boot if missing |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Static Utility / Service Class | All 9 methods and 14 path constants are `static`; no instance state | CReadFiles.h:26-54 | Group file I/O operations without object lifecycle |
| Drop-Box / Mailbox pattern | Operators drop plaintext files into a shared folder; the component polls and consumes them | CReadFiles.cpp:130, 380, 726, 910 | Loose async coupling between admin tooling and the DB server |
| Polling scheduler (timer-driven) | `ProcessSecTimer` invokes methods on fixed cadences | Server.cpp:2013-2084 | Periodic import/export without threads |
| Fire-and-forget messaging | Live grants sent via `SendOneMessage` with no ack/retry | CReadFiles.cpp:253, 829 | Push real-time grants to connected game servers |
| Command/data parser | `fgets`/`sscanf` positional parsing of fixed-format records | CReadFiles.cpp:201, 443-664, 788 | Decode flat-file payloads |
| Raw block I/O | `_read`/`_write` of whole `STRUCT_ACCOUNTFILE`/`GuildInfo` arrays | CReadFiles.cpp:705, 722 | Binary persistence of fixed-layout structs |
| Quarantine/cleanup pattern | Success → `DeleteFile`; failure → `MoveFile` to `Error/` folder | CReadFiles.cpp:189, 211, 358-368, 888-898 | Recoverable failure handling with audit trail |

Architectural note: the class is a thin **file-persistence facade** layered directly over global DBSrv state. It intentionally separates "external file ingestion/export" from the account-persistence logic in `CFileDB`, but because it reaches through globals rather than an interface, the two layers are coupled by shared mutable state and init order.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | ImportItem/ImportDonate (CReadFiles.cpp:358-368, 888-898) | File deleted only on write success, but a deletion failure simply moves the file to `Error/`; no retry/transaction | Possible duplicate grants if a "successful" import file is reprocessed after a crash |
| High | UpdateUser (CReadFiles.cpp:1017-1030) | Source file deleted unconditionally even when `UpdateAccount` fails | Failed password changes are silently lost (no retry, no quarantine) |
| High | Data integrity | Import mutates and persists account files with no locking; a crash mid-write can corrupt the account | Account file corruption risk (consistent with whole-codebase file-based persistence) |
| High | Live-vs-persistent split (ImportItem) | When the account is online, the item is both sent live and persisted; if the account DB write then fails, only the live send occurred | Live and persistent state can diverge |
| Medium | UpdateConnection (CReadFiles.cpp:82) | Connection count is fuzzed (`(4*Count/3)+rand()%4`) | Non-accurate monitoring data by design (legacy anti-leak heuristic) |
| Medium | Hard-coded absolute path (CReadFiles.cpp:39) | `C:/wamp/www/serv%2.2d.htm` is environment-specific | Deployment is coupled to a specific WAMP folder layout |
| Medium | Import folder scan (CReadFiles.cpp:150-153, 746-749) | Hard-coded cap of 10 files per pass | Backlog if more than 10 files accumulate between polls |
| Medium | No input validation on numeric parse | `sscanf` results not checked for every field; malformed files may leave uninitialized buffer data in parsed structs | Possible undefined behavior on malformed input |
| Medium | Security | Account passwords read/written in plaintext and logged (UpdateUser logs the password, CReadFiles.cpp:1021) | Credential exposure in logs |
| Medium | Security | Import drop-folders accept any file with no authentication/authorization on the filesystem | Anyone with write access to `../../Common/...` can grant items, donate points, or create accounts |
| Low | Unused parameter `Pos` in ImportDonate | `int Pos` declared (CReadFiles.cpp:860) but unused in donate flow | Dead code / minor noise |
| Low | ImportUser error handling | Failed files are silently skipped (no move to error folder, no log) | Operator gets no visibility into rejected registrations |
| Low | Ranking export off-by-one | Loop `i > RankPos::LAST` (i > 0) skips rank index 0 | Top-of-table index 0 is never emitted |
| Low | Redundant null check | `UpdateConnectionData` re-checks `fp == NULL` after already returning if null (CReadFiles.cpp:108) | Dead/redundant code |

---

## 11. Test Coverage Analysis

**No automated tests exist for this component (or anywhere in the project).** A repository-wide search for test files (`*test*`, `*spec*`) across `/home/luisdias/dev/github/luisdiasdev/w2pp` (excluding `.git`, `.opencode`) returned zero results. There are no unit tests, integration tests, or test fixtures covering `CReadFiles` or any other module.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| CReadFiles (DBSrv) | 0 | 0 | 0% (estimated) | N/A — no test infrastructure |

Consequences and risks of the missing coverage:

- The nine import/export methods (all parsing, validation ranges, and file-cleanup logic) are entirely unverified.
- The file-cleanup branches (delete-on-success, move-on-failure, unconditional-delete in `UpdateUser`) are exercised only in live production.
- The live-vs-persistent dual path in `ImportItem`/`ImportDonate` and the accumulation logic in `ImportDonate` (`file.Donate += Donate`) have no regression protection.
- The `UpdateConnection` fuzzing arithmetic and high-water-mark reset in `UpdateConnectionData` have no assertions.
- Because the class is coupled to Windows file APIs, globals, and the filesystem, any future tests would require refactoring toward dependency injection or interface-based file access — but per scope, no refactoring is recommended here, only the risk is documented.

---

## Limitations

- Coupling metrics (Section 6) are measured at the translation-unit level and are approximate; they reflect the actual call sites identified in the source rather than a tool-computed dependency graph.
- Test coverage is estimated as 0% because no test files exist in the repository; this is a stated finding, not a measured value.
- Business rules that are implicit (e.g., the purpose of the fuzzed connection count, the exact producer of the item drop-box files) are documented with the evidence present in the code; where the external producer is not defined in this codebase, it is noted as an external tooling contract.
- This analysis is read-only; no project files were modified.

---

## References

- Component source: `legacy/Code/DBSrv/CReadFiles.cpp`, `legacy/Code/DBSrv/CReadFiles.h`
- Callers: `legacy/Code/DBSrv/Server.cpp` (WinMain :583, :615; ProcessSecTimer :2013-2084), `legacy/Code/DBSrv/CFileDB.cpp:480`, `legacy/Code/DBSrv/CRanking.cpp:110`
- Dependencies: `legacy/Code/DBSrv/CFileDB.h/.cpp`, `legacy/Code/DBSrv/CUser.h`, `legacy/Code/DBSrv/CRanking.h/.cpp`, `legacy/Code/Basedef.h/.cpp`, `legacy/Code/CPSock.h/.cpp`
- Context: `docs/reports/architectural-analyzer/architectural-report-2026-08-19 17:13:23.md`
