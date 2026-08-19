# Component Deep Analysis Report — CFileDB

**Project:** W2PP (legacy C/C++ MMO game server)
**Component analyzed:** CFileDB (account/character file persistence layer)
**Scope root:** `/home/luisdias/dev/github/luisdiasdev/w2pp/legacy`
**Folders ignored:** `.git`, `.opencode`
**Date:** 2026-08-19 17:13:23
**Analysis mode:** Read-only. No project files were modified.

---

## 1. Executive Summary

**CFileDB** is the account and character file-persistence layer of **DBSrv** (the "Database Server" of the W2PP private-server release). It is implemented as a single C++ class split across two files — `legacy/Code/DBSrv/CFileDB.h` (89 lines) and `legacy/Code/DBSrv/CFileDB.cpp` (2,688 lines) — and is instantiated once as the global `CFileDB cFileDB` (`DBSrv/Server.cpp:89`, declared `extern` in `DBSrv/Server.h:113`).

Its role in the system is central: every game server (TMSrv) connects to DBSrv over TCP, and DBSrv forwards account/character/transfer requests to `cFileDB.ProcessMessage()` (`DBSrv/Server.cpp:1356`), which is the single entry point that mutates and persists account data. The component also provides the lower-level persistence primitives (`DBReadAccount`, `DBWriteAccount`, `DBExportAccount`, `CreateCharacter`, `DeleteCharacter`) used directly by the DBSrv admin-protocol handlers in `Server.cpp` (`DisableAccount`, `EnableAccount`, import/export, redirect/transfer flows).

Persistence is **file-based, not relational**: one binary `STRUCT_ACCOUNTFILE` blob per account stored under `./account/<first>/<uppercased-name>`, plus per-character stub files under `./char/<first>/<name>`, capsule blobs under `./capsule/<id>`, an export mirror under `S:/export/account<ServerIndex>/`, and log/import text files under `../../Common/record<ServerIndex>/` and `../../Common/ImportItem/`.

**Key findings:**

- **Broad message-handling surface.** `ProcessMessage` dispatches 29 distinct `_MSG_*` protocol cases covering account login, account creation, character creation (mortal and arch), character login, character deletion, periodic character saves (`_MSG_DBSaveMob`), logout saves (`_MSG_SavingQuit`), the secure-PIN (`NumericToken`) flow, server-transfer, capsule store/restore, guild war/ally/info propagation, and ranking updates.
- **Heavy reuse of a reserved-name guard.** The `COM`/`LPT` prefix check is duplicated in at least ten code paths (`AddAccount`, `UpdateAccount`, `CreateCharacter`, `DeleteCharacter`, `DBWriteAccount`, `DBReadAccount`, `DBExportAccount`, `GetAccountByChar`, and the `_MSG_DBNewAccount`/`_MSG_DBAccountLogin`/`_MSG_DBCreateCharacter`/`_MSG_DBCreateArchCharacter` handlers).
- **No transactional integrity.** Writes are direct, unbuffered `_write()` calls with no locking, checksum, backup, or journal; a crash mid-write can corrupt an account file. This matches the "file persistence, no ACID" debt flagged in the architectural report.
- **Plaintext credentials.** `STRUCT_ACCOUNTINFO.AccountPass` is stored and compared raw (`strcmp` at `CFileDB.cpp:677`), never hashed.
- **Zero automated tests.** No test/spec files exist anywhere in the repository; the class has no self-contained test harness and depends on a Win32 socket/global-state environment.
- **Single-threaded coupling to global mutable state.** CFileDB reads and mutates process-wide globals (`pUser`, `pAdmin`, `AdminClient`, `g_pBaseSet`, `ChargedGuildList`, `ItemDayLog`, `Sapphire`, `TransperCharacter`, `rankingSystem`) and is invoked from the single-threaded DBSrv message pump.

---

## 2. Data Flow Analysis

The account lifecycle flows through CFileDB as follows:

```
1. TMSrv sends a GAME2DB packet over TCP to DBSrv.
2. DBSrv Server.cpp reads the packet into a raw char* buffer.
3. ProcessClientMessage(conn, msg) validates FLAG_GAME2DB + ID bounds  (Server.cpp:1339-1357).
4. cFileDB.ProcessMessage(msg, conn) is invoked (Server.cpp:1356).
5. ProcessMessage casts the buffer to the expected MSG_* struct and switches on std->Type.
6. For read flows (e.g. _MSG_DBAccountLogin):
     - GetIndex(conn, m->ID) resolves the account-list slot.
     - DBReadAccount(&file) opens ./account/<first>/<name>, _read()s sizeof(STRUCT_ACCOUNTFILE).
     - Validation (reserved names, block date, password, TempKey transfer) runs.
     - State is copied into pAccountList[Idx].File and added via AddAccountList(Idx).
     - A reply (MSG_DBCNFAccountLogin / MSG_CNFCharacterLogin) is sent back via pUser[conn].cSock.SendOneMessage().
7. For write flows (e.g. _MSG_DBSaveMob / _MSG_SavingQuit):
     - The handler mutates pAccountList[Idx].File in memory.
     - DBWriteAccount() re-opens the same path and _write()s the full STRUCT_ACCOUNTFILE.
     - DBExportAccount() mirrors the blob to S:/export/account<ServerIndex>/<name>.
     - RemoveAccountList(Idx) frees the in-memory slot on logout.
8. Side effects may be emitted to other servers (SendDBSignal* / SendGuildInfo) or the filesystem
   (ProcessRecord log files, capsule blobs, ImportItem files).
9. Ranking updates are pushed to rankingSystem for character login / exp updates.
```

In-memory account state is held in the public array `STRUCT_ACCOUNTLIST pAccountList[MAX_DBACCOUNT]` (`CFileDB.h:40`), where `MAX_DBACCOUNT = MAX_USER * MAX_SERVER = 1000 * 10 = 10,000` slots (`Basedef.h:60`). The account list is the cache/working-set that all handlers mutate before flushing to disk.

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Reject accounts/chars with reserved `COM`/`LPT`+digit names | CFileDB.cpp:86-89, 153-156, 520-522, 619-625, 906-914, 1470-1478, 2244-2256, 2311-2314, 2400-2403, 2531-2534, 2641-2644 |
| Validation | Reject command-like character names (`KING`, `KINGDOM`, `GRITAR`, `RELO`) | CFileDB.cpp:897-904, 1461-1468 |
| Validation | Reject character names containing a double `í` sequence | CFileDB.cpp:940-950, 1504-1514, 1864-1874 |
| Validation | Account must not already exist on creation | CFileDB.cpp:99-106 |
| Business Logic | New-account starter cargo (100 items: 50x id 401, 50x id 406) when `bonus != 0` | CFileDB.cpp:126-135 |
| Business Logic | Default new-character stat/scoring state via `g_pBaseSet[class]`, `ClassMaster = MORTAL` | CFileDB.cpp:960-997 |
| Business Logic | Arch character: `ClassMaster = ARCH`, `Ac = 230`, face `MortalFace+5+cls`, links to mortal slot | CFileDB.cpp:1527-1557 |
| Business Logic | Login block-date check (`Year`/`YearDay`) rejects expired accounts | CFileDB.cpp:648-660 |
| Business Logic | Password must match (`strcmp`) on login; else `_MSG_DBAccountLoginFail_Pass` | CFileDB.cpp:677-682 |
| Business Logic | Dual-login handling: already/still playing via `_MSG_DBAlreadyPlaying`/`_MSG_DBStillPlaying` | CFileDB.cpp:685-703 |
| Business Logic | Anti-duplication name-swap (Equip[13] 774/775 swap logic) on login | CFileDB.cpp:707-743 |
| Business Logic | Server transfer via `TempKey` (server-change credential handoff) | CFileDB.cpp:662-675, 1929-1987 |
| Business Logic | Character deletion requires `ClassMaster` in {MORTAL, ARCH} and `Level < 219` | CFileDB.cpp:1315-1328 |
| Business Logic | Secure PIN (`NumericToken`) setup/change/verify state machine | CFileDB.cpp:1357-1421 |
| Business Logic | Capsule store (`_MSG_DBPutInCapsule`) / restore (`_MSG_DBOutCapsule`) | CFileDB.cpp:1684-1927 |
| Business Logic | `Sapphire` rate clamp to [1, 64] | CFileDB.cpp:492-495 |
| Business Logic | Guild war/ally/info propagation to all online servers | CFileDB.cpp:388-481 |
| Validation | Slot bounds `[0, MOB_PER_ACCOUNT)` and class bounds `[0,4)` on creation | CFileDB.cpp:865-880, 1036-1041, 1434-1453 |
| Business Logic | In-memory account-list login/slot bookkeeping | CFileDB.cpp:2202-2234 |

### Detailed breakdown of the business rules

---

### Business Rule: Reserved account/character name validation (COM/LPT prefix)

**Overview:** DBSrv refuses to create, read, write, export, or otherwise process accounts and characters whose (case-insensitive) name starts with the DOS reserved prefixes `COM` or `LPT` followed by a digit and then a null terminator. This guard is the most widely duplicated rule in the component.

**Detailed description:** The rule exists to prevent collisions with reserved device names on the Windows filesystem where these account/character blobs are persisted. Because names are mapped directly to file paths (`./account/<first>/<name>`), a name like `COM1` or `LPT3` would otherwise map to a reserved DOS device and cause `_open`/`_write` to behave unexpectedly. The check is performed on the upper-cased name (via `_strupr`), and requires the exact pattern `'C','O','M'` (or `'L','P','T'`) at offsets 0-2, a digit at offset 3, and a null terminator at offset 4. Because the guard is a 5-character window, names that merely start with these prefixes but continue with more characters (e.g. `COMMAND`) are not rejected — only the exact reserved-device form.

The check is re-implemented (not factored into a helper) at ten locations in `CFileDB.cpp`: `AddAccount` (86-89), `UpdateAccount` (153-156), the `_MSG_DBNewAccount` handler (520-522), the `_MSG_DBAccountLogin` handler (619-625), the `_MSG_DBCreateCharacter` handler (906-914), the `_MSG_DBCreateArchCharacter` handler (1470-1478), `CreateCharacter` (2244-2256), `DeleteCharacter` (2311-2314), `DBWriteAccount` (2400-2403), `DBReadAccount` (2531-2534), `DBExportAccount` (2481-2484), and `GetAccountByChar` (2641-2644). This duplication is a maintainability risk: the rule is enforced at the persistence layer and the protocol layer independently, so a change must be made in each location.

**Rule workflow:**
1. Caller receives an account/character name in a request or file structure.
2. The name is copied into a fixed buffer and converted to upper case with `_strupr`.
3. The first five bytes are compared against the `C-O-M-[0-9]-0` or `L-P-T-[0-9]-0` pattern.
4. If matched, the operation returns `FALSE`/`FAIL` immediately (no file I/O occurs, and for protocol handlers a `_MSG_*Fail` signal is sent).
5. Otherwise the operation proceeds.

---

### Business Rule: Command-like character name rejection

**Overview:** When creating a character, the name (upper-cased) must not be exactly one of the reserved command tokens `KING`, `KINGDOM`, `GRITAR`, or `RELO`.

**Detailed description:** These four names correspond to administrator/command identifiers or reserved NPC/game-system names in the W2 world. Allowing a player to create a character with one of these names would permit impersonation of, or collision with, system entities and in-game commands. The check is applied in both character-creation handlers: `_MSG_DBCreateCharacter` (CFileDB.cpp:897-904) and `_MSG_DBCreateArchCharacter` (CFileDB.cpp:1461-1468). It uses an exact `strcmp` against the uppercased name, so case is normalized before comparison (e.g. `king` is rejected). On violation, the handler logs an error (`err,newchar - cmd name`) and returns `_MSG_DBNewCharacterFail` to the requesting game server, so the creation is not persisted.

**Rule workflow:**
1. The proposed character name is copied and upper-cased into a `NAME_LENGTH` buffer.
2. The string is compared against each of the four reserved tokens.
3. On any exact match, the handler logs and replies with `_MSG_DBNewCharacterFail`; creation aborts.
4. Otherwise creation continues.

---

### Business Rule: Character name double-`í` rejection

**Overview:** A character name must not contain two consecutive `í` (i-with-acute, 0xED in CP-936/latin) characters.

**Detailed description:** The W2 client/server protocol and the `STRUCT_MOB.MobName` field are `char` arrays interpreted under the legacy MultiByte character set (project is built with `MultiByte` per `DBSrv.vcxproj:27`). The specific byte sequence `íí` (two 0xED bytes) is a legacy corruption/invalid marker that the server treats as malformed — likely because it collides with the double-byte encoding used for Korean Hangul names elsewhere in the base system (`BASE_GetFirstKey` handles high-byte names). The rule is implemented as a linear scan over the name length in `_MSG_DBCreateCharacter` (CFileDB.cpp:940-950), `_MSG_DBCreateArchCharacter` (CFileDB.cpp:1504-1514), and `_MSG_DBOutCapsule` (CFileDB.cpp:1864-1874). On detection the handler replies `_MSG_DBNewCharacterFail` and aborts. Note the scan reads `m->MobName[i+1]` for `i` up to `len-1`, relying on the fact that the buffers are NUL-terminated (guaranteed by the preceding `MobName[NAME_LENGTH-1]=0` / `[NAME_LENGTH-2]=0` assignments).

**Rule workflow:**
1. The name is NUL-terminated at the last two buffer positions.
2. The loop walks each byte `i` up to `strlen(name)`.
3. If `name[i]==0xED && name[i+1]==0xED`, the request is rejected with `_MSG_DBNewCharacterFail`.
4. Otherwise creation continues.

---

### Business Rule: Account uniqueness / no-overwrite on creation

**Overview:** `AddAccount` (CFileDB.cpp:78-143) refuses to create an account if the target account file already exists on disk.

**Detailed description:** Before writing a new account, `AddAccount` builds the path `./account/<first>/<upper-name>` via `BASE_GetFirstKey` and attempts to open it read-only (`_open(..., O_RDONLY | O_BINARY)`). If the open succeeds (handle != -1), the file exists, so the account already exists and the function returns `FALSE` without writing. This is the primary uniqueness guard for the file-based store. It is complemented at the protocol layer by the `_MSG_DBNewAccount` handler (CFileDB.cpp:510-561), which validates the reserved-name rule, copies `m->AccountInfo` into `pAccountList[Idx].File`, zeroes character/cargo/coin state, and calls `DBWriteAccount`; on `ret == 0` it replies `_MSG_DBNewAccountFail`. `DBWriteAccount` itself opens with `O_RDWR | O_CREAT | O_BINARY` and does not itself check for pre-existence — it overwrites whatever is at the path, which is why the existence check in `AddAccount` is the guard that prevents clobbering an existing account.

**Rule workflow:**
1. Reserved-name check runs first.
2. Path is derived with `BASE_GetFirstKey` and the uppercased name.
3. A read-only open probes existence; if the handle is valid the file exists and creation fails.
4. A zeroed `STRUCT_ACCOUNTFILE` is populated (name, password, real name, email, phone, address, SSN, `NumericToken[0]=-1`, `Donate=0`, `ShortSkill` set to `-1`).
5. If `bonus != 0`, starter cargo (50x item 401 + 50x item 406) is inserted.
6. `DBWriteAccount` persists the blob; non-zero return yields `TRUE`.

---

### Business Rule: New-account starter cargo bonus

**Overview:** When an account is created with a non-zero `bonus` flag, its cargo is pre-loaded with 100 starter items: slots 0-49 = item id 401, slots 50-99 = item id 406.

**Detailed description:** This implements a promotional/new-player starter bundle. The `bonus` parameter to `AddAccount` (CFileDB.cpp:126-135) is a boolean-like flag: when non-zero, the loop writes `file.Cargo[i].sIndex = 401` for `i < 50` and `file.Cargo[i].sIndex = 406` for the remaining 50 slots, filling all 100 usable cargo slots of `MAX_CARGO` (128). Item ids 401 and 406 are static item definitions from the shared item tables. This rule is only triggered through the `AddAccount` path; the protocol-level `_MSG_DBNewAccount` handler (CFileDB.cpp:510-561) zeroes the entire cargo instead, so bonus-granting is specific to the direct/admin `AddAccount` flow. Confidence: the exact meaning of items 401/406 (e.g. currency/consumable quantities) is defined in item data tables outside this component, so only the slot-fill rule is certain here.

**Rule workflow:**
1. Account file is zeroed and base fields populated.
2. If `bonus != 0`, iterate `i` from 0 to 99; `sIndex=401` for `i<50`, `sIndex=406` otherwise.
3. `DBWriteAccount` persists the filled cargo.

---

### Business Rule: Account login block-date check

**Overview:** On login (`_MSG_DBAccountLogin`, CFileDB.cpp:648-660), if the account carries a block date (`Year` and `YearDay` both non-zero) and that date has not yet passed, the login is refused with `_MSG_DBAccountLoginFail_Block`.

**Detailed description:** `STRUCT_ACCOUNTINFO` stores `Year` and `YearDay` (`Basedef.h:793-794`). The admin `DisableAccount` (`Server.cpp:1359-1417`) writes these fields to permanently/temporarily ban an account. The login handler reads the current local time and compares: it rejects when `Year >= currentYear` OR (`Year >= currentYear` AND `YearDay >= currentYday`). In effect the account is blocked while the stored date is in the future or today; once the stored date is strictly in the past the account may log in again (i.e. the block is date-based and self-expiring rather than an indefinite flag). The comparison is not strictly "future only" — it allows the block to remain effective for the entire stored day. On rejection it sends `_MSG_DBAccountLoginFail_Block` with parameter 0 and returns without logging in.

**Rule workflow:**
1. `DBReadAccount` loads the account; if missing, `_MSG_DBAccountLoginFail_Account` is sent.
2. If `Coin < 0`, it is normalized to 0.
3. If both `Year` and `YearDay` are non-zero, compute current `tm_year`/`tm_yday`.
4. Reject with `_MSG_DBAccountLoginFail_Block` if the stored date is not yet passed.
5. Otherwise continue to password/tempkey/dual-login handling.

---

### Business Rule: Password authentication on login

**Overview:** Successful account login requires the supplied password to match the stored account password byte-for-byte.

**Detailed description:** After the block-date check, the login handler compares `file.Info.AccountPass` against the client-supplied `m->AccountPassword` using `strcmp` (CFileDB.cpp:677-682). On mismatch it sends `_MSG_DBAccountLoginFail_Pass` and returns without establishing a session. Passwords are stored in plaintext in `STRUCT_ACCOUNTINFO.AccountPass` (12 bytes, `Basedef.h:783`) and are transmitted cleartext over the network — there is no hashing, salting, or key derivation anywhere in this component. The comparison also feeds the broader failure-counting/anti-abuse mechanisms maintained on the TMSrv side (per the architectural report), but CFileDB itself performs only the direct comparison. This is a security-relevant weakness (plaintext credential handling).

**Rule workflow:**
1. Reserved-name guard for the account name.
2. `DBReadAccount`; block-date and coin checks.
3. If a `TempKey` transfer is pending and matches, it is consumed and login proceeds via `goto lb_sucess` (server-change path).
4. Otherwise `strcmp(storedPass, suppliedPass)`; mismatch sends `_MSG_DBAccountLoginFail_Pass`.

---

### Business Rule: Dual-login / "already playing" detection

**Overview:** An account already logged into the DB server is not silently re-admitted; depending on the `DBNeedSave` flag the DB either refuses (`_MSG_DBAlreadyPlaying`) or force-disconnects the prior session (`_MSG_DBStillPlaying` + `SendDBSavingQuit`).

**Detailed description:** At CFileDB.cpp:685-703, after successful authentication, the handler resolves `IdxName = GetIndex(m->AccountName)` (the index of any slot already holding a session for this account). If `IdxName == Idx` the request is a no-op. If another slot already holds the session: when `m->DBNeedSave == 0`, the handler logs "desconectado. conexão anterior finalizada." and sends `_MSG_DBAlreadyPlaying` (refusing the new login). Otherwise it sends `_MSG_DBStillPlaying` and calls `SendDBSavingQuit(IdxName, 0)` to force the previous session to save and quit, freeing the slot. This enforces single-session-per-account semantics across the fleet of game servers funneling into DBSrv.

**Rule workflow:**
1. After auth, resolve the existing-session index by account name.
2. If same as the requesting slot, return (no-op).
3. If a session exists elsewhere and `DBNeedSave==0`, send `_MSG_DBAlreadyPlaying` and abort.
4. Else send `_MSG_DBStillPlaying` and trigger `SendDBSavingQuit` on the old slot, then proceed.

---

### Business Rule: Character name de-duplication swap on login

**Overview:** On login, if two characters on the account wear items `Equip[13].sIndex == 774` and `775` respectively, their names are swapped and the marker items zeroed.

**Detailed description:** At CFileDB.cpp:707-743, the login handler scans all four character slots. A slot whose `Equip[13].sIndex == 774` is tagged as `left`; one with `775` is tagged as `right`. If both exist, the two `MobName` values are swapped, both `Equip[13].sIndex` fields are set to 0, an `etc,name swap` log line is written, and `DBWriteAccount` persists the result. This is an anti-duplication/repair rule: item ids 774/775 are evidently duplicate-character markers, and swapping names reconciles ownership before the player proceeds. The swap is applied only when both markers are present, and it runs before the account state is copied into `pAccountList[Idx].File`.

**Rule workflow:**
1. Iterate the four character slots.
2. Mark `left` where `Equip[13].sIndex==774`, `right` where `==775`.
3. If both marked: swap names, clear both markers, log, `DBWriteAccount`.
4. Continue with normal login state load.

---

### Business Rule: Character creation (mortal)

**Overview:** `_MSG_DBCreateCharacter` (CFileDB.cpp:856-1026) creates a new mortal character in a validated empty slot, copying the base class template and setting `ClassMaster = MORTAL`.

**Detailed description:** The handler validates: `Slot` in `[0, MOB_PER_ACCOUNT)`; class `cls` in `[0,4)`; `SecurePass` already set (i.e. the account completed the secure-PIN step); the proposed name passes the command-token, `COM`/`LPT`, and double-`í` checks; and the target slot is empty (`Char[Slot].MobName[0] == 0` — otherwise `_MSG_DBNewCharacterFail` with "already charged"). It then calls `CreateCharacter(account, name)` to create the `./char/` stub file; on failure it aborts. The in-memory `STRUCT_MOB` is cleared with `BASE_ClearMob`, `mobExtra` with `BASE_ClearMobExtra`, affects zeroed, `ShortSkill` set to `-1`, and `ClassMaster = MORTAL`. The class template is copied from `g_pBaseSet[cls]` (the four class base-mob blobs loaded at DBSrv startup from `./BaseMob/TK|FM|BM|HT`, `Server.cpp:450-471`), `MortalFace` is captured from `Equip[0].sIndex`, the chosen name is written, and `DBWriteAccount` persists. On success `DBGetSelChar` builds a `STRUCT_SELCHAR` snapshot and `_MSG_DBCNFNewCharacter` is returned.

**Rule workflow:**
1. Validate slot, class, secure state, and name rules.
2. Confirm target slot empty.
3. `CreateCharacter` stub file; abort on failure.
4. Clear mob/extra; copy class template from `g_pBaseSet[cls]`.
5. Write name, set `ClassMaster=MORTAL`, persist, reply `_MSG_DBCNFNewCharacter`.

---

### Business Rule: Arch character creation

**Overview:** `_MSG_DBCreateArchCharacter` (CFileDB.cpp:1423-1589) creates an "Arch" (ascended) character into the first free slot with a distinct stat/face profile.

**Detailed description:** Unlike mortal creation (which uses an explicit slot), the arch handler searches for the first slot whose `MobName[0] == 0` (CFileDB.cpp:1434-1436). It validates class in `[0, MAX_CLASS)` and the same reserved-name rules. It clears the mob/extra, sets `extra->ClassMaster = ARCH`, records the source mortal slot via `extra->QuestInfo.Arch.MortalSlot = m->MortalSlot`, copies the class template from `g_pBaseSet[cls]`, then overrides: `mob->Equip[0].sIndex = MortalFace + 5 + cls` (arch face derived from the mortal face and class), `mob->BaseScore.Ac = 230`, `Equip[1..]` and `Carry[0..MAX_CARRY-4]` cleared, and `extra->MortalFace = MortalFace`. It persists via `DBWriteAccount` and, on success, replies `_MSG_DBCNFArchCharacterSucess` (with the chosen slot) and `_MSG_DBCNFNewCharacter`. The arch class represents the transcendent path in the game's class-master ladder (MORTAL -> ARCH -> CELESTIAL).

**Rule workflow:**
1. Locate first free slot; validate slot/class and name rules.
2. Clear mob/extra; set `ClassMaster=ARCH` and link `MortalSlot`.
3. Copy class template; apply arch face, `Ac=230`, clear equip/carry.
4. Persist, reply `_MSG_DBCNFArchCharacterSucess` + `_MSG_DBCNFNewCharacter`.

---

### Business Rule: Character deletion eligibility

**Overview:** A character may be deleted only if the account password matches, the class-master is mortal or arch, and the level is below 219.

**Detailed description:** `_MSG_DBDeleteCharacter` (CFileDB.cpp:1276-1355) enforces: slot bounds; `SecurePass` set; the supplied password matches `File.Info.AccountPass` via `strncmp(..., ACCOUNTPASS_LENGTH)` (CFileDB.cpp:1302); the character's `ClassMaster` is `MORTAL` or `ARCH` (CFileDB.cpp:1315) — celestial/higher characters cannot be deleted this way; and `mob->BaseScore.Level < 219` (CFileDB.cpp:1321), i.e. near-max-level characters are protected from deletion (the "deletechar level 219" guard). On success the handler zeroes `ShortSkill[Slot]`, calls `DeleteCharacter(name, account)` to remove the `./char/` stub, clears the mob/extra with `BASE_ClearMob`/`BASE_ClearMobExtra`, persists via `DBWriteAccount`, and replies `_MSG_DBCNFDeleteCharacter` with the refreshed `STRUCT_SELCHAR`.

**Rule workflow:**
1. Validate slot, secure state, password, class-master, and level.
2. On any violation send `_MSG_DBDeleteCharacterFail`.
3. Clear skills, delete the char stub file.
4. Clear mob/extra, persist, reply `_MSG_DBCNFDeleteCharacter`.

---

### Business Rule: Secure PIN (NumericToken) state machine

**Overview:** The account secure-PIN flow (`_MSG_AccountSecure`, CFileDB.cpp:1357-1421) manages a 6-character `NumericToken` through setup, verify, and change transitions, tracked by the in-memory `SecurePass` state.

**Detailed description:** Each account slot carries `SecurePass` (-1 = not yet set/unlocked, 1 = set/verified) and the persisted `NumericToken[6]`. On first setup (token currently `-1`), the token is stored and persisted, `SecurePass=1`, and `_MSG_AccountSecure` confirms. To verify without changing (`Change==0`), if `SecurePass==-1` and the supplied token matches, the account is unlocked (`SecurePass=1`) and confirmed. To change (`Change==1`) while `SecurePass==1`, the new token is stored/persisted and confirmed. Any path not matching one of these transitions resets `SecurePass=-1` and sends `_MSG_AccountSecureFail`. Because `SecurePass` gates character creation, deletion, and login, this rule is effectively a mandatory pre-login PIN gate.

**Rule workflow:**
1. Read `SecurePass` and current token state.
2. Setup: token==-1 → store, persist, unlock, confirm.
3. Verify: `Change==0 && SecurePass==-1 && token match` → unlock, confirm.
4. Change: `Change==1 && SecurePass==1` → store new token, persist, unlock, confirm.
5. Else reset `SecurePass=-1` and send `_MSG_AccountSecureFail`.

---

### Business Rule: Server-transfer credential handoff (TempKey)

**Overview:** Character transfer between game servers is mediated by a one-time `TempKey` stored in the account file and consumed on the next login.

**Detailed description:** On `_MSG_DBServerChange` (CFileDB.cpp:1929-1987), after bounds checks on `ID` and `NewServerID`, the handler builds an 8-value `Enc` array (via `GetEncPassword`, which in this build fills it with `rand() % 900 + 100` and always returns `FALSE`), formats a transfer string `"*%d %d ... %d %d"` (server id + 8 encoded values + slot), copies it into `File.TempKey[52]`, and replies `_MSG_DBCNFServerChange`. On the subsequent login (`_MSG_DBAccountLogin`, CFileDB.cpp:662-675), if `TempKey[0] != 0` and the client-supplied `m->Zero[0] != 0` and they `memcmp` equal, the key is consumed (zeroed), `ChangeServer=1`, and login proceeds via `goto lb_sucess`; on mismatch the key is zeroed and persisted and login returns. When `ChangeServer==1`, the handler parses `m->Zero` back into server/slot/Enc, sets `pAccountList[Idx].Slot`, and emits `_MSG_DBCNFCharacterLogin` directly (CFileDB.cpp:802-853). Note `SetEncPassword` (CFileDB.cpp:2685) is a stub body, and `GetEncPassword`'s return of `FALSE` means the `EncResult` branch is effectively never taken, so the transfer always writes the TempKey.

**Rule workflow:**
1. `_MSG_DBServerChange`: validate IDs, build Enc array, write TempKey string, reply.
2. Login: compare stored TempKey to client token; on match consume and proceed to character login; on mismatch clear and persist, then return.
3. If `ChangeServer==1`, restore server/slot, load the character, and emit `_MSG_DBCNFCharacterLogin`.

---

### Business Rule: Capsule store and restore

**Overview:** A character can be archived into a capsule blob file (`./capsule/<id>`) and later restored into a free slot; each capsule store also emits an import item file.

**Detailed description:** `_MSG_DBPutInCapsule` (CFileDB.cpp:1684-1766) validates the slot, increments the global `LastCapsule`, writes a `STRUCT_CAPSULE` (the character's `STRUCT_MOB` + `STRUCT_MOBEXTRA`) to `./capsule/<LastCapsule>` with `extra.QuestInfo.Arch.MortalSlot = -1`, clears the in-memory character, persists the account, replies `_MSG_DBCNFCapsuleSucess`, and appends an import line to `../../Common/ImportItem/<LastCapsule><rand>...` encoding account + item 3443 (a capsule-claim token). `_MSG_DBOutCapsule` (CFileDB.cpp:1768-1927) resolves the capsule index from the item's `stEffect[0].cValue*256 + stEffect[1].cValue`, reads `./capsule/<index>`, verifies the target slot is free and the name passes validation, renames the restored mob, resets coin/guild/guild-level, clears equip/carry, persists the account, copies the mob/extra into `pAccountList[Idx].File`, deletes the capsule file (`DeleteFileA`), and replies `_MSG_DBCNFNewCharacter`. `_MSG_DBCapsuleInfo` (CFileDB.cpp:1624-1682) reads a capsule and returns a `STRUCT_SEALOFSOUL` summary.

**Rule workflow:**
1. Store: validate slot, `LastCapsule++`, write capsule blob, clear character, persist, reply, emit import file.
2. Info: open capsule by index, read, reply `STRUCT_SEALOFSOUL`.
3. Restore: resolve index from item effects, read capsule, validate slot/name, reset fields, persist, copy state, delete capsule, reply.

---

### Business Rule: Sapphire rate clamp

**Overview:** The `Sapphire` global (a drop-rate multiplier) is halved or doubled by `_MSG_DBUpdateSapphire` and clamped to `[1, 64]`.

**Detailed description:** At CFileDB.cpp:483-508, `_MSG_DBUpdateSapphire` reads a `MSG_STANDARDPARM`; if `Parm==1` the rate doubles, otherwise it halves. It is clamped so `Sapphire < 1 → 1` and `> 64 → 64`. The new value is broadcast to every online game server via `SendDBSignalParm3(... _MSG_DBSetIndex, -1, Sapphire, i)`, and `WriteConfig()` persists it to `Config.txt` (`Server.cpp:412-425`). This is an operational tuning rule exposed through the DB protocol.

**Rule workflow:**
1. Read `Parm`; double or halve `Sapphire`.
2. Clamp to `[1, 64]`.
3. Broadcast to all online servers; persist via `WriteConfig`.

---

### Business Rule: Guild war / ally / info propagation

**Overview:** Guild war (`_MSG_War`), ally (`_MSG_GuildAlly`), and guild-info (`_MSG_GuildInfo`) messages update in-memory tables and fan out to every connected game server.

**Detailed description:** The `_MSG_War` handler (CFileDB.cpp:388-418) validates the guild id in `(0, 65536)`, stores the opponent into `g_pGuildWar[myguild]`, and broadcasts `_MSG_War` to each online server. `_MSG_GuildAlly` (420-452) does the same for `g_pGuildAlly`. `_MSG_GuildInfo` (454-481) stores the `STRUCT_GUILDINFO` into the global `GuildInfo[65536]`, pushes it to each online server via `SendGuildInfo`, and calls `CReadFiles::WriteGuildInfo()` to persist the guild table. `_MSG_GuildZoneReport` (357-386) updates `ChargedGuildList[srv][5]` and re-broadcasts `_MSG_GuildReport`. These globals (`GuildInfo`, `g_pGuildWar`, `g_pGuildAlly`, `ChargedGuildList`, `LastCapsule`) are declared in `CFileDB.h:85-88` and `CFileDB.cpp:48-54`.

**Rule workflow:**
1. Validate guild id bounds.
2. Update the in-memory war/ally/info table.
3. Broadcast the relevant message to every online game server.
4. For guild info, persist via `CReadFiles::WriteGuildInfo`.

---

## 4. Component Structure

```
legacy/Code/DBSrv/
├── CFileDB.h                     # Class declaration, STRUCT_ACCOUNTLIST, extern guild globals
├── CFileDB.cpp                   # Full implementation (2,688 lines)
│   ├── CFileDB::CFileDB()        # Constructor: init pAccountList[MAX_DBACCOUNT]
│   ├── CFileDB::~CFileDB()       # Destructor (empty)
│   ├── Account primitives:
│   │   ├── AddAccount()          # Create account file + optional starter cargo
│   │   ├── UpdateAccount()       # Change account password
│   │   ├── DBReadAccount()       # Read STRUCT_ACCOUNTFILE blob from ./account/...
│   │   ├── DBWriteAccount()      # Write STRUCT_ACCOUNTFILE blob to ./account/...
│   │   ├── DBExportAccount()     # Mirror blob to S:/export/account<idx>/...
│   │   └── GetAccountByChar()    # Look up owning account by char stub file
│   ├── Character primitives:
│   │   ├── CreateCharacter()     # Create ./char/ stub file
│   │   └── DeleteCharacter()     # Delete ./char/ stub file
│   ├── Account-list management:
│   │   ├── AddAccountList()      # Mark slot logged-in, bump server user count
│   │   ├── RemoveAccountList()   # Clear slot, decrement count
│   │   ├── GetIndex(server,id)   # Linear-index mapping
│   │   ├── GetIndex(account)     # Search by account name
│   │   ├── GetAccountsByMac()    # Count sessions sharing a MAC
│   │   ├── InitAccountList()     # Zero a slot's file
│   │   └── SendDBSavingQuit()    # Request a save-and-quit from a session
│   ├── Protocol dispatcher:
│   │   └── ProcessMessage()      # 29-case _MSG_* switch (main entry point)
│   ├── Outbound messaging helpers:
│   │   ├── SendDBSignal / SendDBSignalParm / Parm2 / Parm3
│   │   ├── SendDBMessage()
│   │   └── SendGuildInfo()
│   ├── Misc:
│   │   ├── DBCheckImpleName()    # Chat-filter word scan (unused in this file)
│   │   ├── DBGetSelChar()        # Build STRUCT_SELCHAR snapshot
│   │   ├── GetEncPassword()      # rand-based 8-value code (returns FALSE)
│   │   └── SetEncPassword()      # Stub body
│   └── ProcessRecord()           # Free function: write ../Common/record log file
└── (integrated with) Server.cpp  # Owns the global cFileDB instance and dispatches to it
```

The component exposes a public in-memory state array `STRUCT_ACCOUNTLIST pAccountList[MAX_DBACCOUNT]` (`CFileDB.h:40`), which other DBSrv modules (`Server.cpp`, `CReadFiles.cpp`) read directly (e.g. `Server.cpp:718` clears `pAccountList[IdxName].File.Char[slot]`).

---

## 5. Dependency Analysis

**Internal Dependencies (within DBSrv / shared Code):**
```
Server.cpp (ProcessClientMessage/ProcessAdminMessage)
   └─► CFileDB::ProcessMessage / DBReadAccount / DBWriteAccount / GetIndex / ...
         │
         ├─► Basedef.h / Basedef.cpp      (STRUCT_* models, BASE_GetFirstKey,
         │                                 BASE_ClearMob, BASE_ClearMobExtra, GetItemPointer,
         │                                 _MSG_* packet structs, Log-free helpers)
         ├─► CUser.h (DBSrv)              (pUser[MAX_SERVER], pAdmin[MAX_ADMIN], cSock.SendOneMessage)
         ├─► CRanking.h / CRanking.cpp    (rankingSystem: grindRanking, sendUpdate*Rank)
         ├─► CReadFiles.h                 (CReadFiles::WriteGuildInfo)
         └─► Server.h                     (Log, DisableAccount, WriteConfig, ServerIndex, globals)
```

**External / platform dependencies:**
| Dependency | Type | Purpose |
|------------|------|---------|
| Windows CRT / `io.h` | Platform | `_open`, `_read`, `_write`, `_close`, `_lseek`, `_filelength` |
| `stdio.h` / `time.h` | Platform | `fopen`/`fprintf`, `time`/`localtime` |
| Win32 `DeleteFileA` (`windows.h`) | Platform | Capsule file deletion |
| Filesystem `./account/`, `./char/`, `./capsule/` | Local store | Binary persistence |
| Filesystem `S:/export/`, `../../Common/record`, `../../Common/ImportItem` | Local store | Export/backup and import/record files |
| `rand()` (`stdlib.h`) | Platform | Enc-code / import filename generation |

**Related files referencing CFileDB (fan-in):**
- `DBSrv/Server.cpp:89` — global `CFileDB cFileDB` instantiation; ~30 call sites across admin and dispatch handlers.
- `DBSrv/Server.h:23,113` — includes header and declares `extern CFileDB cFileDB`.
- `DBSrv/CReadFiles.cpp:33` — includes header and uses `rankingSystem` / account file helpers.
- `DBSrv/DBSrv.vcxproj:95,106` and `DBSrv.vcxproj.filters:36,59` — build membership.

---

## 6. Afferent and Efferent Coupling

Coupling is assessed at the C++ class level (the programming paradigm is class-based C++).

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|-------------------|
| CFileDB (class) | High — ~30 call sites from `Server.cpp`; used by `CReadFiles.cpp`; global `extern` in `Server.h` | High — depends on `CUser` (pUser/pAdmin + socket), `CRanking` (rankingSystem), `CReadFiles`, `Basedef`, `CPSock` (via CUser), plus filesystem and Win32/CRT | High |

**Afferent:** The single global instance is the persistence hub for all DBSrv account/character work; `Server.cpp` is the dominant consumer (login, admin disable/enable, import/export, redirect, transfer). The class is therefore a stability point — any interface change ripples across `Server.cpp`.

**Efferent:** CFileDB reaches into many external structures and globals: `pUser`/`pAdmin`/`AdminClient` (CUser/CPSock), `g_pBaseSet` (Basedef), `ChargedGuildList`, `ItemDayLog`, `Sapphire`, `TransperCharacter`, `ServerIndex` (Server globals), `rankingSystem` (CRanking), and `CReadFiles::WriteGuildInfo`. This high fan-out means changes to any of those subsystems can affect CFileDB compilation and behavior.

---

## 7. Endpoints

CFileDB does not expose network endpoints of its own. It is a server-internal persistence layer invoked by DBSrv's socket dispatcher (`Server.cpp:1356`). The component's "interface" is the `ProcessMessage(char* Msg, int conn)` protocol handler, which consumes the following DBSrv wire messages (all `FLAG_GAME2DB` inbound from TMSrv). These are internal protocol message types, not REST/GraphQL/gRPC endpoints.

| Message Type (const) | Handling Location | Description |
|----------------------|-------------------|-------------|
| `_MSG_ReqTransper` | CFileDB.cpp:243 | Character transfer to transfer-server |
| `_MSG_GuildZoneReport` | CFileDB.cpp:357 | Guild zone charge report |
| `_MSG_War` | CFileDB.cpp:388 | Guild war declaration |
| `_MSG_GuildAlly` | CFileDB.cpp:420 | Guild alliance declaration |
| `_MSG_GuildInfo` | CFileDB.cpp:454 | Guild info update |
| `_MSG_DBUpdateSapphire` | CFileDB.cpp:483 | Change Sapphire drop rate |
| `_MSG_DBNewAccount` | CFileDB.cpp:510 | Create new account |
| `_MSG_MessageDBRecord` | CFileDB.cpp:563 | Write a record/log file |
| `_MSG_NPAppeal` | CFileDB.cpp:579 | Forward appeal to admins |
| `_MSG_MessageDBImple` | CFileDB.cpp:592 | Broadcast DB implement message |
| `_MSG_DBAccountLogin` | CFileDB.cpp:611 | Account login |
| `_MSG_DBCreateCharacter` | CFileDB.cpp:856 | Create mortal character |
| `_MSG_DBCharacterLogin` | CFileDB.cpp:1028 | Character login |
| `_MSG_DBNoNeedSave` | CFileDB.cpp:1096 | Release slot without save |
| `_MSG_DBSaveMob` | CFileDB.cpp:1124 | Periodic character save |
| `_MSG_SavingQuit` | CFileDB.cpp:1191 | Logout save + release |
| `_MSG_DBDeleteCharacter` | CFileDB.cpp:1276 | Delete character |
| `_MSG_AccountSecure` | CFileDB.cpp:1357 | Secure-PIN setup/verify/change |
| `_MSG_DBCreateArchCharacter` | CFileDB.cpp:1423 | Create arch character |
| `_MSG_MagicTrumpet` | CFileDB.cpp:1591 | Broadcast magic trumpet |
| `_MSG_DBNotice` | CFileDB.cpp:1612 | Broadcast DB notice |
| `_MSG_DBCapsuleInfo` | CFileDB.cpp:1624 | Query capsule summary |
| `_MSG_DBPutInCapsule` | CFileDB.cpp:1684 | Store character into capsule |
| `_MSG_DBOutCapsule` | CFileDB.cpp:1768 | Restore character from capsule |
| `_MSG_DBServerChange` | CFileDB.cpp:1929 | Initiate server transfer |
| `_MSG_UpdateExpRanking` | CFileDB.cpp:1990 | Update exp ranking |
| `_MSG_DBItemDayLog` | CFileDB.cpp:2044 | Increment item day log |
| `_MSG_DBActivatePinCode` | CFileDB.cpp:2060 | PIN activation (empty handler) |
| `_MSG_DBPrimaryAccount` | CFileDB.cpp:2068 | Mark account as primary by MAC |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| DBSrv message dispatcher (`Server.cpp`) | Internal invocation | Dispatch inbound TMSrv account/char packets | In-process call (`ProcessMessage`) | `_MSG_*` packed structs over `char*` | Header validation in `Server.cpp:1343`; handlers send `_MSG_*Fail` signals |
| Game servers (`pUser[].cSock`) | Internal socket sink | Send confirm/fail/replies and broadcasts | TCP via `CPSock.SendOneMessage` | `_MSG_*` packed structs | `if (Sock)` guard before send; no retry |
| Admin client (`AdminClient`) | Internal socket sink | Forward transfer/redirect (`_MSG_NPCreateCharacter`) | TCP via `CPSock` | Packed struct | `if (AdminClient.Sock == 0)` guard |
| Filesystem `./account/<first>/<name>` | Local storage | Account blob read/write | Raw binary `_read`/`_write` | `STRUCT_ACCOUNTFILE` (fixed layout) | errno logging; `FALSE` on open/seek/write fail |
| Filesystem `./char/<first>/<name>` | Local storage | Character stub create/delete/lookup | Raw binary | 16-byte account name | `fopen`/`_open` probe; errno logging |
| Filesystem `./capsule/<id>` | Local storage | Capsule store/restore | Raw binary | `STRUCT_CAPSULE` | errno logging on open |
| Filesystem `S:/export/account<idx>/` | Local storage | Account export mirror | Raw binary `_write` | `STRUCT_ACCOUNTFILE` | `FALSE` on failure; not logged |
| Filesystem `../../Common/record<idx>/` | Local storage | Request/record logging (`ProcessRecord`) | Text `fprintf` | Text | `FALSE` if `fopen` fails |
| Filesystem `../../Common/ImportItem/` | Local storage | Capsule claim item import | Text append | Text line | Ignored on failure |
| `CReadFiles::WriteGuildInfo` | Internal call | Persist guild info table | In-process | `STRUCT_GUILDINFO[65536]` | None (void) |
| `rankingSystem` (CRanking) | Internal call | Rank insert/update/broadcast on login/exp | In-process | `STRUCT_RANKING` | OUT_OF_RANK checks |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Global singleton instance | `CFileDB cFileDB` global + `extern` | Server.cpp:89, Server.h:113 | Single shared persistence service |
| Command/Message dispatcher | `switch(std->Type)` over 29 `_MSG_*` cases | CFileDB.cpp:236-2106 | Route wire messages to per-type logic |
| Raw-binary repository / DAO | `DBReadAccount`/`DBWriteAccount`/`DBExportAccount` | CFileDB.cpp:2390-2577 | Read/write account blobs to/from filesystem |
| In-memory cache + write-through | `pAccountList[]` mutated then flushed via `DBWriteAccount` | CFileDB.h:40; handlers | Working-set of logged-in accounts |
| Data structure as wire format | `STRUCT_ACCOUNTFILE`/`STRUCT_MOB` persisted raw | Basedef.h:840 | No serialization layer; layout == file/network format |
| Index mapping | `GetIndex(server,id) = server*MAX_USER+id` | CFileDB.cpp:2328-2333 | Linear slot addressing for `pAccountList` |
| "Active record"-style inline validation | Guard checks inline in each handler | throughout | Enforce reserved names/slots before mutation |
| Server-push fan-out | Loop over `pUser[]` broadcasting state | CFileDB.cpp:388-481, 2068 | Propagate guild/notice/primary-account state |

**Architectural decisions:** (1) Persistence is deliberately file-based with a fixed-struct binary format rather than a relational DB; (2) all account state is cached in a fixed-capacity global array and written through to disk on save; (3) the shared `Basedef.h` header is the compile-time contract between DBSrv and TMSrv for these messages and structures; (4) everything runs on the single-threaded Win32 message pump, so no locks are used.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| Critical | `DBWriteAccount` (CFileDB.cpp:2390) | No transactions, locking, checksum, backup, or journal; raw `_write` of a large fixed struct | A crash mid-write corrupts the account file; no recovery path |
| Critical | All write paths | File created with `O_CREAT` without pre-existence check except `AddAccount` | Overwrite/clobber risk if path assumptions change |
| High | Credential handling (CFileDB.cpp:677, 1302) | Passwords stored and compared in plaintext | Account compromise on disk and on the wire |
| High | `ProcessMessage` (236-2106) | 29-case monolith, ~1,900 lines in one method | High complexity, high blast radius, hard to review/test |
| High | Reserved-name guard (10 duplicated blocks) | Same rule re-implemented in many paths | Inconsistent enforcement if one site is missed |
| High | Out-of-bounds risks | `DBGetSelChar` reads `Equip[13]`/`Equip[0]` on 16-slot arrays (fine) but `SelChar.Equip[i][16]` index handling depends on `MOB_PER_ACCOUNT` (4) matching `STRUCT_SELCHAR` sizing | Layout drift between `Basedef.h` and persisted/wire formats corrupts data |
| Medium | `GetEncPassword`/`SetEncPassword` (2671-2688) | `GetEncPassword` always returns `FALSE`; `SetEncPassword` is an empty stub | Server-transfer branch is effectively dead code / incomplete |
| Medium | `_MSG_DBActivatePinCode` (2060-2066) | Handler body is empty (commented-out call) | Feature not implemented; silently accepts the message |
| Medium | `DBCheckImpleName` (2579) | Method defined but never called within CFileDB; unused logic | Dead code |
| Medium | `DBSrv.vcxproj` links `odbc32.lib;odbccp32.lib` | Dead dependency (no SQL usage) | Misleading linkage, per dependency audit |
| Medium | `DBReadAccount` (2558-2574) | Reads exactly `sizeof(STRUCT_ACCOUNTFILE)` regardless of actual file length; if file longer, ignores remainder; if shorter, only patches `ShortSkill` | Silent data truncation / stale data if struct grows |
| Medium | Capsule store (1684-1766) | `LastCapsule++` with no persistence of the counter except via `WriteConfig`; `rand()`-suffixed import filenames | Capsule id reuse / import-file collisions after restart |
| Low | `DBGetSelChar` (2624) | `sel->SPX[i]` assigned twice (SPX then SPY) | SPX value overwritten; likely a bug |
| Low | `ProcessRecord` (219) | `sprintf` format string `"%d2.2d_%2d"` typo (stray `2`) | Malformed record filename |

---

## 11. Test Coverage Analysis

**Finding: No automated tests exist for CFileDB (or any component) in the repository.**

A repository-wide search for test/spec files (case-insensitive `*test*` / `*spec*`, excluding `.git`, `.opencode`, `.codegraph`) returned **zero matches**. The architectural report independently confirms "no automated testing (CodeGraph reports no covering tests for core handlers)." The DBSrv project (`DBSrv.vcxproj`) contains no test project, and the solution (`W2PP Code Project.sln`) contains only the two application projects (`DBSrv`, `TMSrv`) plus a solution-items folder.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| CFileDB (`CFileDB.cpp`, 2,688 lines) | 0 | 0 | 0% (no harness) | N/A — no assertions, fixtures, or mocks |
| CFileDB helpers (`DBReadAccount`/`DBWriteAccount`/`DBExportAccount`) | 0 | 0 | 0% | N/A |
| `ProcessMessage` dispatcher (29 cases) | 0 | 0 | 0% | N/A |

**Implications:** Because CFileDB is a Win32-dependent class that reads/writes process-wide globals (`pUser`, `pAdmin`, `AdminClient`, `g_pBaseSet`) and the filesystem, and is driven through `_MSG_*` wire structs, there is no test seam currently: no test files, no mocked sockets, and no self-contained fixtures. The business rules extracted in Section 3 (reserved-name guards, block dates, password checks, dual-login, capsule store/restore, deletion eligibility, PIN state machine) are entirely unverified by automated tests. This is a significant coverage risk given the component's role as the sole persistence authority for accounts and characters.

---

## 12. Report Metadata

- **Component name:** CFileDB
- **Analyzed files:**
  - `legacy/Code/DBSrv/CFileDB.h`
  - `legacy/Code/DBSrv/CFileDB.cpp`
  - `legacy/Code/DBSrv/Server.cpp` / `Server.h` (integration and call sites)
  - `legacy/Code/DBSrv/CReadFiles.cpp` (integration)
  - `legacy/Code/Basedef.h` / `Basedef.cpp` (data structures and helpers)
  - `legacy/Code/DBSrv/CRanking.h` (ranking integration)
  - `legacy/Code/DBSrv/DBSrv.vcxproj` / `.filters` (build membership)
  - `legacy/W2PP Code Project.sln` (solution/build configuration)
- **Referenced prior reports:** `docs/reports/architectural-analyzer/architectural-report-2026-08-19 17:13:23.md`, `docs/reports/dependency-auditor/dependencies-report-2026-08-19 17:13:23.md`
- **Folders ignored:** `.git`, `.opencode`
- **Known ambiguities:** The exact semantics of starter items 401/406 and of the `_MSG_DBServerChange` Enc/TempKey encoding are defined outside CFileDB; the component's `SetEncPassword` is a stub and `_MSG_DBActivatePinCode` is empty, so those flows are documented as incomplete rather than fully specified.
