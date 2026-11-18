# Component Deep Analysis Report

**Component:** CFileDB Account and Character Store
**Primary location:** `Code/DBSrv/CFileDB.cpp`, `Code/DBSrv/CFileDB.h`
**Secondary in-scope evidence:** `Code/Basedef.h` (record layouts, message contracts), `Code/DBSrv/Server.cpp` (callers/host), `Code/DBSrv/CReadFiles.cpp` (bulk import/export callers), `Code/DBSrv/CRanking.cpp` (bulk account-file reader)
**Host subsystem:** DBSrv (`Code/DBSrv/DBSrv.vcxproj`, Win32/x86, MSVC v143, `/W3`, `_CRT_SECURE_NO_WARNINGS`)
**Analysis date:** 2026-11-15 22:55:21
**Ignored folders:** `.agents`, `.codegraph`, `.opencode`, `openspec`

---

## 1. Executive Summary

`CFileDB` is the persistence and session-authority core of DBSrv. It owns three responsibilities that are tightly welded into one class:

1. **Raw binary file storage** for accounts. One fixed-size, memory-mapped-by-convention `STRUCT_ACCOUNTFILE` record is written per account at `./account/<first-key>/<UPPERCASED-ACCOUNT-NAME>` (`Code/DBSrv/CFileDB.cpp:2411`, `Code/DBSrv/CFileDB.cpp:2542`). The record embeds account metadata, four character slots, a 128-slot cargo, per-character quick-skill/affect/extra blocks and a server-change token.
2. **An in-memory login cache** — `STRUCT_ACCOUNTLIST pAccountList[MAX_DBACCOUNT]` (`Code/DBSrv/CFileDB.h:40`), where `MAX_DBACCOUNT = MAX_USER * MAX_SERVER = 1000 * 10 = 10000` (`Code/Basedef.h:60`). The array is indexed directly by the caller's connection slot (`server * MAX_USER + id`, `Code/DBSrv/CFileDB.cpp:2328-2333`), so the "cache" is simultaneously an address space and a login registry.
3. **A protocol endpoint** — `ProcessMessage(char *Msg, int conn)` (`Code/DBSrv/CFileDB.cpp:236`) is the single dispatcher for every game-server-to-DB message that touches accounts, characters, capsules, guild war/ally state, Sapphire configuration and the exp ranking feed.

Architectural role in the system: it is the authoritative writer for player data. TMSrv holds the live `pMob`/`pUser` state and ships whole `STRUCT_MOB` snapshots down on save/quit; DBSrv (this component) is the only process that touches the account files on the game path. The NP administration tool and the capsule/import subsystem reach the same files through the same class (`Code/DBSrv/CReadFiles.cpp:235-334`, `Code/DBSrv/CReadFiles.cpp:677`, `Code/DBSrv/CReadFiles.cpp:1015`).

Key findings (details and evidence in later sections):

- **No CRUD transaction story.** Every persist is a single `_write` of ~7.7 KB at offset 0 into an already-existing file, with no temp-file + atomic rename, no backup, no checksum and no `FlushFileBuffers`. A partial write or a crash mid-write leaves a truncated/zero-tailed record that `DBReadAccount` will happily accept because it only checks `_read` for `-1` (`Code/DBSrv/CFileDB.cpp:2562-2576`).
- **The 0 index is overloaded.** `GetIndex(char *account)` returns `0` both for "server 0 / player 0" and for "not found" (`Code/DBSrv/CFileDB.cpp:2348`), and many handlers use its result directly as an array index (`Code/DBSrv/CFileDB.cpp:283`, `:322`, `:867`, `:1038`, `:1104`, `:1135`, `:1204`, `:1288`, `:1368`).
- **The cache has no eviction on connection loss.** `Server.cpp` clears `pUser[User]` on socket teardown but never walks `pAccountList` (`Code/DBSrv/Server.cpp:890-900`), so a TMSrv crash leaves accounts permanently "logged in" until the same slot logs in again.
- **Business logic is largely implicit and defensive-by-accident.** Reserved names (`COM0-9`, `LPT0-9`, `KING`, `KINGDOM`, `GRITAR`, `RELO`), level-219 deletion caps, dual-sword name swapping, dated account blocks and capsule index encoding are all inline conditionals with no named constants, no configuration and no documentation.
- **Dead and stubbed code is present and reachable in the call graph.** `SetEncPassword` is an empty function (`Code/DBSrv/CFileDB.cpp:2685`), `GetEncPassword` ignores its `idx` argument and always returns `FALSE` (`Code/DBSrv/CFileDB.cpp:2671`), `DBCheckImpleName` and `InitAccountList` and `SendDBMessage` have no callers.
- **There are no tests.** No test project exists in `W2PP Code Project.sln` (only `DBSrv` and `TMSrv`), no test files, mocks or fixtures exist anywhere in the workspace, and no test framework headers are referenced. See section 12.

---

## 2. Data Flow Analysis

### 2.1 Inbound network entry (game server to DB)

```
1. CPSock completes WSAAsyncSelect read for a TMSrv connection          (Code/DBSrv/Server.cpp:877)
2. GetUserFromSocket(wParam) maps socket -> pUser[conn]                 (Code/DBSrv/Server.cpp:879)
3. CPSock::ReadMessage de-frames and de-obfuscates the packet           (Code/CPSock.cpp:353)
4. ProcessClientMessage(conn, msg)                                      (Code/DBSrv/Server.cpp:1339)
5. Guard: msg->Type must carry FLAG_GAME2DB, ID in [0..MAX_USER]        (Code/DBSrv/Server.cpp:1343)
6. cFileDB.ProcessMessage(msg, conn)                                    (Code/DBSrv/Server.cpp:1356)
7. switch(std->Type) selects the account/character/capsule handler      (Code/DBSrv/CFileDB.cpp:240)
8. Idx = GetIndex(conn, m->ID)  ->  conn*MAX_USER + m->ID               (Code/DBSrv/CFileDB.cpp:2328)
9. Handler mutates pAccountList[Idx].File (cache) and/or the disk file
10. Handler emits MSG_* reply through CPSock::SendOneMessage            (Code/DBSrv/CFileDB.cpp:2108-2183)
11. pUser[conn].cSock flushes the send buffer to the TMSrv socket       (Code/CPSock.cpp:686)
```

### 2.2 Login / authentication flow (`_MSG_DBAccountLogin`, `Code/DBSrv/CFileDB.cpp:611-854`)

```
1. _strupr(AccountName)                                             (615)
2. Reserved pattern test COM[0-9] / LPT[0-9]                        (619-625) -> _MSG_DBAccountLoginFail_Account, return FALSE
3. Idx     = GetIndex(conn, m->ID)          # cache address          (627)
   IdxName = GetIndex(m->AccountName)       # linear scan by name    (628)
4. memset file; copy AccountName; DBReadAccount(&file)              (630-636) -> on failure _MSG_DBAccountLoginFail_Account
5. Clamp negative Coin to 0                                         (645-646)
6. Date block test on Info.Year / Info.YearDay                      (648-660) -> _MSG_DBAccountLoginFail_Block
7. Server-change token test on file.TempKey vs m->Zero              (664-675)
     match    -> clear TempKey, ChangeServer = 1, goto lb_sucess
     mismatch -> clear TempKey, DBWriteAccount (persist wipe), return silently
8. strcmp(Info.AccountPass, m->AccountPassword)                     (677-682) -> _MSG_DBAccountLoginFail_Pass
9. lb_sucess:
     IdxName == Idx            -> return TRUE (no reply emitted)    (685-686)
     IdxName != 0              -> AlreadyPlaying / StillPlaying +
                                  SendDBSavingQuit(IdxName, 0)       (688-703)
10. Dual-sword 774/775 name swap, DBWriteAccount                     (710-743)
11. memcpy cache <- file; store Mac[4]; AddAccountList(Idx)          (745-756)
12. SecurePass = -1; build MSG_DBCNFAccountLogin (selchar, cargo,    (758-780)
    coin); send to pUser[conn]
13. If GetAccountsByMac(m->AdapterName) <= 1 broadcast               (782-801)
    _MSG_DBCheckPrimaryAccount to every TMSrv
14. If ChangeServer == 1: DBWriteAccount, SecurePass = 1,            (802-853)
    sscanf(m->Zero, "*%d %d...%d", Server, Enc[8], Slot),
    cache Slot, send _MSG_DBCNFCharacterLogin, feed ranking
```

### 2.3 Save / quit durability flow (`_MSG_SavingQuit`, `Code/DBSrv/CFileDB.cpp:1191-1274`)

```
1. Idx  = GetIndex(conn, m->ID)
2. Slot = pAccountList[Idx].Slot                     # slot chosen at character login
3. Truncate cached MobName (last 2 bytes zeroed)
4. strncmp(m->AccountName, cached AccountName) != 0 -> log, evict, return
5. Slot out of range or Slot != m->Slot             -> log, evict, return
6. Login == 0                                       -> log, return (no write)
7. Overwrite cached Char[Slot], Cargo[128], ShortSkill[Slot], mobExtra[Slot], affect[Slot], Coin, Donate
8. DBWriteAccount(&pAccountList[Idx].File)          # in-place rewrite of the whole record
9. DBExportAccount(&pAccountList[Idx].File)         # mirror to S:/export/account{ServerIndex}/
10. RemoveAccountList(Idx)                          # Login = FALSE, Slot = -1, zero the cache record
11. SendDBSignal(conn, m->ID, _MSG_DBCNFAccountLogOut)
12. Apply any pending admin disable for this slot    (pAdmin[i].DisableID == Idx)
```

### 2.4 Character create flow (`_MSG_DBCreateCharacter`, `Code/DBSrv/CFileDB.cpp:856-1026`)

```
1. Slot = m->Slot, cls = m->MobClass, Idx = GetIndex(conn, m->ID)
2. Slot range 0..3                                 -> _MSG_DBNewCharacterFail
3. Class range 0..3                                -> _MSG_DBNewCharacterFail
4. SecurePass == -1 (numeric token not yet verified) -> break (silent, no reply)
5. Reserved command names KING/KINGDOM/GRITAR/RELO -> _MSG_DBNewCharacterFail, return FALSE
6. Reserved COM/LPT pattern                        -> _MSG_DBNewCharacterFail, return FALSE
7. Slot already occupied (MobName[0] != 0)         -> _MSG_DBNewCharacterFail
8. Reject 'í' followed by 'í' in name              -> _MSG_DBNewCharacterFail
9. CreateCharacter(account, name) -> create ./char/<key>/<NAME> holding the 16-byte account name
10. BASE_ClearMob / BASE_ClearMobExtra / affect = 0 / ShortSkill = -1
11. ClassMaster = MORTAL; mob template = g_pBaseSet[cls] (loaded from ./BaseMob/{TK,FM,BM,HT})
12. extra->MortalFace = template Equip[0].sIndex; copy name into mob
13. DBWriteAccount(&pAccountList[Idx].File)        -> on failure _MSG_DBNewCharacterFail
14. Reply MSG_CNFNewCharacter with DBGetSelChar projection
```

### 2.5 Capsule put-in / take-out flow (`Code/DBSrv/CFileDB.cpp:1684-1927`)

```
PUT IN
1. Slot = m->Parm range check 0..3
2. LastCapsule++  (process-global counter, persisted only via WriteConfig)
3. Create ./capsule/<LastCapsule> and write STRUCT_CAPSULE {MOB, extra}
4. extra.QuestInfo.Arch.MortalSlot = -1 (sever the mortal link before sealing)
5. ShortSkill[Slot] = 0; WriteConfig(); clear cache mob + mobextra; DBWriteAccount
6. Reply _MSG_DBCNFCapsuleSucess + MSG_CNFDeleteCharacter
7. Append line to ../../Common/ImportItem/<LastCapsule><4 random bytes>:
   "<account> 3443 59 <hi> 59 <lo> 0 0"  (item 3443 carries the capsule id in two effect bytes)

TAKE OUT
1. Slot = m->Slot range check; find first free NovoSlot 0..3
2. CreateCharacter(account, m->MobName)  (duplicate name protection)
3. index = item.stEffect[0].cValue*256 + item.stEffect[1].cValue
4. Read ./capsule/<index>; rename mob; wipe Equip[1..], Carry[0..59], Coin, Guild, GuildLevel
5. Zero the source cargo item; DBWriteAccount; copy mob/mobextra into new slot
6. DeleteFileA(./capsule/<index>)
7. Reply MSG_CNFNewCharacter
```

### 2.6 Persistence primitive flow (`DBWriteAccount`, `Code/DBSrv/CFileDB.cpp:2390-2469`)

```
account->Info.AccountName
  -> copy 16 bytes, _strupr, reject COM/LPT patterns
  -> BASE_GetFirstKey(name, First)        # 'A'-'Z' bucket or 'etc'
  -> sprintf("./account/%s/%s", First, NAME)
  -> _open(O_RDWR|O_CREAT|O_BINARY, _S_IREAD|_S_IWRITE)
  -> _lseek(0, SEEK_SET)
  -> _write(sizeof(STRUCT_ACCOUNTFILE))   # truncation-free overwrite at offset 0
  -> _close
  -> return TRUE / FALSE
```
No locking, no rename, no flush, no directory creation, no backup copy, and the caller's return value is ignored in the majority of the save/progress paths (for example `Code/DBSrv/CFileDB.cpp:1184`, `:1255`, `:1343`, `:1747`, `:1897`).

---

## 3. Business Rules & Logic

### Overview of the business rules

| # | Rule Type | Rule Description | Location |
|---|-----------|------------------|----------|
| R1 | Data format | Account record is a fixed-layout binary blob; readers must tolerate legacy/short files | `Code/DBSrv/CFileDB.cpp:2518-2577`, `Code/Basedef.h:840-856` |
| R2 | Data format | Account file path = `./account/<first-key>/<UPPERCASE name>`; first key is A-Z or `etc` | `Code/DBSrv/CFileDB.cpp:2411`, `Code/Basedef.cpp:4114` |
| R3 | Validation | Account names matching `COM[0-9]` or `LPT[0-9]` (case-insensitive) are forbidden everywhere | `Code/DBSrv/CFileDB.cpp:86-89`, `:153-156`, `:2244-2256`, `:2400-2403`, `:2531-2534` |
| R4 | Validation | Duplicate account creation is rejected by probing the target path with `_open` | `Code/DBSrv/CFileDB.cpp:99-106` |
| R5 | Business logic | New account defaults: `NumericToken[0] = -1`, `Donate = 0`, `ShortSkill = -1`, optional 100x bonus cargo | `Code/DBSrv/CFileDB.cpp:119-135` |
| R6 | Validation | Login password comparison is exact `strcmp` on 16 bytes; no hashing, no rate limit, no lockout | `Code/DBSrv/CFileDB.cpp:677-682` |
| R7 | Business logic | Negative coin is clamped to 0 on every login | `Code/DBSrv/CFileDB.cpp:645-646` |
| R8 | Business logic | Dated account block: `Info.Year/YearDay` in the future blocks login | `Code/DBSrv/CFileDB.cpp:648-660` |
| R9 | Business logic | Server-change token (`TempKey`) short-circuits password authentication | `Code/DBSrv/CFileDB.cpp:662-675`, `:1929-1988` |
| R10 | Business logic | Duplicate session resolution: AlreadyPlaying / StillPlaying + forced save-and-quit of the older session | `Code/DBSrv/CFileDB.cpp:685-703` |
| R11 | Business logic | Dual-sword pairing (equip index 774/775) triggers a character-name swap and a rewrite | `Code/DBSrv/CFileDB.cpp:710-743` |
| R12 | Business logic | Cache admission/eviction is refcounted per connection; there is no LRU, TTL or disconnect sweep | `Code/DBSrv/CFileDB.cpp:2202-2234`, `Code/DBSrv/Server.cpp:890-900` |
| R13 | Business logic | Cache address = `conn * MAX_USER + id`; name lookup is a linear scan that returns 0 on miss | `Code/DBSrv/CFileDB.cpp:2328-2349` |
| R14 | Business logic | MAC (adapter) tuple per account; "primary account" broadcast when a MAC has <= 1 account | `Code/DBSrv/CFileDB.cpp:751-754`, `:782-801`, `:2351-2367` |
| R15 | Validation | Character name must not be `KING`, `KINGDOM`, `GRITAR`, `RELO` or match `COM/LPT[0-9]`, and must not contain `íí` | `Code/DBSrv/CFileDB.cpp:897-914`, `:1461-1478` |
| R16 | Validation | Character slot must be 0..3 and class 0..3 | `Code/DBSrv/CFileDB.cpp:865-880` |
| R17 | Business logic | Character creation requires a verified numeric token (`SecurePass`) | `Code/DBSrv/CFileDB.cpp:882-889`, `:1043-1050`, `:1293-1300` |
| R18 | Business logic | Character name uniqueness is enforced by creating `./char/<key>/<NAME>` holding the account name | `Code/DBSrv/CFileDB.cpp:2236-2301` |
| R19 | Business logic | New character is a copy of `g_pBaseSet[class]` with `ClassMaster=MORTAL` and face taken from Equip[0] | `Code/DBSrv/CFileDB.cpp:964-997` |
| R20 | Business logic | Arch character: first free slot, `ClassMaster=ARCH`, `MortalSlot` recorded, AC forced to 230, gear stripped | `Code/DBSrv/CFileDB.cpp:1423-1589` |
| R21 | Validation | Character deletion requires the account password, `ClassMaster` in {MORTAL, ARCH} and level < 219 | `Code/DBSrv/CFileDB.cpp:1302-1328` |
| R22 | Business logic | SaveMob writes only when the cached slot matches, the account is logged in; export is optional | `Code/DBSrv/CFileDB.cpp:1124-1189` |
| R23 | Business logic | Save-and-quit requires account-name and slot agreement, then writes, exports, evicts and signals logout | `Code/DBSrv/CFileDB.cpp:1191-1274` |
| R24 | Business logic | `_MSG_DBNoNeedSave` evicts the cache entry and applies a pending admin disable | `Code/DBSrv/CFileDB.cpp:1096-1122` |
| R25 | Resilience | Persist is an in-place whole-record overwrite with no atomicity, backup or flush | `Code/DBSrv/CFileDB.cpp:2390-2469` |
| R26 | Resilience | Every write is mirrored to `S:/export/account{ServerIndex}/<NAME>` (hard-coded drive) | `Code/DBSrv/CFileDB.cpp:2471-2516` |
| R27 | Business logic | Capsule index allocation is a process-global monotonic counter persisted in `Config.txt` | `Code/DBSrv/CFileDB.cpp:54`, `:1698-1703`, `Code/DBSrv/Server.cpp:405`, `:420` |
| R28 | Business logic | Capsule put-in severs the arch/mortal link, clears the cache character and appends an import-job line for item 3443 | `Code/DBSrv/CFileDB.cpp:1723-1765` |
| R29 | Business logic | Capsule take-out restores the record into the first free slot, strips gear/guild/coin and deletes the capsule file | `Code/DBSrv/CFileDB.cpp:1783-1926` |
| R30 | Business logic | Numeric token (secondary password) binding and change semantics | `Code/DBSrv/CFileDB.cpp:1357-1421` |
| R31 | Business logic | Password change via `UpdateAccount` (NP path) rewrites the file and patches the live cache | `Code/DBSrv/CFileDB.cpp:145-208` |
| R32 | Business logic | Character transfer (`_MSG_ReqTransper`) renames, disables and forwards the mob to NPTool | `Code/DBSrv/CFileDB.cpp:243-355` |
| R33 | Business logic | Guild war / ally / guild info are in-memory 65536-entry arrays with fan-out and file persistence for guild info | `Code/DBSrv/CFileDB.cpp:357-481`, `:51-53` |
| R34 | Business logic | Sapphire global is doubled/halved per request and clamped to 1..64, then persisted | `Code/DBSrv/CFileDB.cpp:483-508`, `Code/DBSrv/Server.cpp:412-425` |
| R35 | Business logic | Ranking index is computed on login/character login and updated by `_MSG_UpdateExpRanking` | `Code/DBSrv/CFileDB.cpp:842-852`, `:1082-1092`, `:1990-2042` |
| R36 | Business logic | Item day-log increments a per-item counter with range validation | `Code/DBSrv/CFileDB.cpp:2044-2058` |
| R37 | Business logic | Record text submitted by a server is archived to `../../Common/record{ServerIndex}/RC....txt` | `Code/DBSrv/CFileDB.cpp:210-234`, `:563-577` |
| R38 | Data format | Character-select projection remaps certain faces and copies position/score/coin/exp | `Code/DBSrv/CFileDB.cpp:2611-2631` |
| R39 | Business logic | Character index lookup by name returns the account name from the 16-byte head of the file | `Code/DBSrv/CFileDB.cpp:2633-2664` |

---

### Detailed breakdown of the business rules

---

### Business Rule: R1 — Account record binary layout and version tolerance

**Overview**:
The account store is a raw C struct dump. `STRUCT_ACCOUNTFILE` (`Code/Basedef.h:840-856`) is written and read with `_write`/`_read` of `sizeof(STRUCT_ACCOUNTFILE)` bytes, with no header, magic, version field or checksum. The struct's own comments document the intended offsets: `Info` 0-216, `Char[4]` 216-3480, `Cargo[128]` 3480-4504, `Coin` 4504-4508, `ShortSkill[4][16]` 4508-4572, then `affect[4][32]`, `mobExtra[4]`, `Donate` and `TempKey[52]`.

**Detailed description**:
The record is the unit of durability for an entire account: authentication data (`AccountName`, `AccountPass`, real name, SSN1/SSN2, e-mail, telephone, address, 6-byte numeric token, block year/year-day), four `STRUCT_MOB` character snapshots of 816 bytes each, a 128-entry cargo grid shared across characters, the account-wide coin balance, per-character quick-skill bytes, per-character affect timers, and the much larger `STRUCT_MOBEXTRA` block that holds class-master state, quest progress, two celestial save slots, NT/Kefra/divine timers and donation counters. Because the whole struct is dumped, any change to any nested struct silently changes the file format and the on-disk offset of every later field. The only guard against drift is implicit: all readers and writers in the same build agree on `sizeof`.

Version tolerance is handled by a single heuristic. `DBReadAccount` measures the file with `_filelength` and, if `length < sizeof(STRUCT_ACCOUNTFILE)`, clears exactly 8 bytes of `ShortSkill` (`Code/DBSrv/CFileDB.cpp:2558-2574`). Eight bytes is inconsistent with the 64-byte `ShortSkill[4][16]` array that the current layout defines, which indicates the shim was written for a much older layout and never revisited. Critically, the function does not zero the destination buffer; it reads into whatever the caller passed. Callers that `memset` first (login at `Code/DBSrv/CFileDB.cpp:632`, `CReadFiles` at `:261`) are safe; callers that only copy the account name into an uninitialised stack struct (`DisableAccount`/`EnableAccount` in `Code/DBSrv/Server.cpp:1366-1369` and `:1426-1429`) leave the tail of the record as indeterminate stack content, and a subsequent `DBWriteAccount` persists that content into the account file.

The ranking loader and the day-log scanner both hard-code size acceptance instead: `CRanking.cpp:175` accepts `fsize >= 7500 && fsize <= 7600` or an exact `sizeof(STRUCT_ACCOUNTFILE)`, and treats anything else as an unusable file. This tolerance window is the only explicit documentation that account records of different vintages coexist on disk, and it also confirms the nominal record size is in the 7.5-7.7 KB range (derived layout size is approximately 7.7 KB with 32-bit `time_t`, matching the Win32/x86 build in `Code/DBSrv/DBSrv.vcxproj:4-8`).

**Rule workflow**:
```
writer: sizeof(STRUCT_ACCOUNTFILE) bytes at offset 0
reader: _filelength -> _read(sizeof) -> if short: clear 8 bytes of ShortSkill (partial shim)
bulk readers: accept 7500..7600 bytes or exact sizeof, else skip file
```

---

### Business Rule: R2 — Account file path derivation and first-key sharding

**Overview**:
An account's location on disk is a pure function of its name: `./account/<First>/<UPPERCASED_NAME>`, where `First` comes from `BASE_GetFirstKey` (`Code/Basedef.cpp:4114`). Latin A-Z maps to a single-character directory; anything else falls through to a Hangul bucket computation and, when out of range, to `"etc"`.

**Detailed description**:
Every entry point applies the same two-step transform. First the name is copied into a local buffer and uppercased with `_strupr`, then `BASE_GetFirstKey` derives the shard. AddAccount (`Code/DBSrv/CFileDB.cpp:91-97`), UpdateAccount (`:158-164`), CreateCharacter and DeleteCharacter (`:2258-2263`, `:2316-2321`), DBWriteAccount (`:2405-2411`), DBReadAccount (`:2536-2542`) and GetAccountByChar (`:2646-2653`) all follow the pattern, so the sharding is consistent across reads and writes. Case-insensitive identity therefore comes from normalising for the path, not from normalising the stored name — several writers store the name in its original case (`Code/DBSrv/CFileDB.cpp:112`), and the login response re-applies `_strupr` later (`:705`).

The consequence is that account identity is a filesystem convention, not an application invariant. `AddAccount` checks existence by opening the uppercase path and returns FALSE if the handle is valid, which correctly rejects a differently-cased duplicate. But an account created by an external tool with lowercase directory names, or an account whose name begins with a multi-byte character, can live outside the reachable namespace. `BASE_GetFirstKey` also has a subtle property: it writes an empty second byte for Latin letters and never null-terminates a caller buffer for the multi-byte branch beyond `strcpy(dest, "etc")`, so callers always pre-initialise or rely on the two-character limit.

Path buffers are fixed 128 bytes and the format is `"./account/%s/%s"`, which bounds a shard to 1-3 characters and a name to 16. There is no directory creation anywhere: `DBWriteAccount` uses `O_CREAT` but not `O_DIRECT`, and `ENOENT` is merely logged (`Code/DBSrv/CFileDB.cpp:2425-2426`). A missing shard directory therefore turns every write for that bucket into a silent failure, and the caller (for example the save-and-quit path) ignores the return code.

**Rule workflow**:
```
name -> copy -> _strupr -> BASE_GetFirstKey
     -> 'A'..'Z'  => First = <letter>
     -> multi-byte => Hangul index bucket
     -> otherwise => "etc"
     -> "./account/<First>/<NAME>"
```

---

### Business Rule: R3 — Reserved account and character name patterns

**Overview**:
Names that collide with DOS device names are refused: `COM0`-`COM9` and `LPT0`-`LPT9`, matched case-insensitively on exactly five characters with the fifth byte required to be the terminator. The identical check is duplicated eight times.

**Detailed description**:
The pattern test appears in AddAccount (`Code/DBSrv/CFileDB.cpp:86-89`), UpdateAccount (`:153-156`), `_MSG_DBNewAccount` (`:520-521`), `DBWriteAccount` (`:2400-2403`), DBReadAccount (`:2531-2534`), CreateCharacter for both the account and the character name (`:2244-2256`), DeleteCharacter (`:2311-2314`), GetAccountByChar (`:2641-2644`) and DBExportAccount (`:2481-2484`). The intent is clearly defensive: an account literally named `COM1` would make `_open("./account/C/COM1")` ambiguous on Windows, where `COM1` is a device name and is opened as a serial port regardless of the preceding path component. The check is a workaround for the operating system rather than a domain rule.

Behaviour on violation depends on the caller, and this inconsistency matters. The low-level primitives (`DBWriteAccount`, `DBReadAccount`, `DBExportAccount`) return `FALSE`, which callers treat as an I/O error. `AddAccount` returns `FALSE`, which the bulk importer interprets as "cannot create, keep the source file". `_MSG_DBNewAccount` returns `FALSE` from the dispatcher after already having assigned `file->Info` into the cache — so a rejected account name still clobbers the cached record before the request is abandoned (`Code/DBSrv/CFileDB.cpp:528-536`). `_MSG_DBAccountLogin` sends `_MSG_DBAccountLoginFail_Account` before returning `FALSE`. `_MSG_DBCreateCharacter` returns `FALSE` after sending the failure signal, whereas the ordinary failure paths return `TRUE`.

Note the matcher only covers `COM`/`LPT` plus one digit. `CON`, `PRN`, `AUX` and `NUL` are not covered, and neither is `COM10`. Whether the missing names are exploitable is not documented, and the check is not extended anywhere in the component.

**Rule workflow**:
```
_strupr(name)
if name[0..2] == "COM" and '0' <= name[3] <= '9' and name[4] == 0 -> reject
if name[0..2] == "LPT" and '0' <= name[3] <= '9' and name[4] == 0 -> reject
```

---

### Business Rule: R4 — Duplicate account detection (creation is exclusive)

**Overview**:
`AddAccount` refuses to create an account whose file already exists. Detection is an `_open(O_RDONLY)` on the derived path; a non-`-1` handle means the name is taken.

**Detailed description**:
The probe is deliberately different from the write path: it opens read-only in binary, immediately closes, and returns `FALSE` (`Code/DBSrv/CFileDB.cpp:99-106`). Because the path is built from the uppercased name, the probe is case-insensitive on Windows' default filesystem. The check is also racy: between `_open(O_RDONLY)` and the later `_open(O_RDWR|O_CREAT)` inside `DBWriteAccount`, another caller could create the same file; nothing serialises the two. In practice the only concurrency source is the single-threaded Win32 message loop plus the `CReadFiles` import loop, so the window is narrow but real for concurrent NP-tool operations.

`AddAccount` is not the only account-creation route. `_MSG_DBNewAccount` creates accounts directly through `DBWriteAccount` without any existence probe (`Code/DBSrv/CFileDB.cpp:510-561`), so a game-server-issued account creation overwrites an existing account record. The two paths therefore disagree about whether account creation is exclusive. `_MSG_DBNewAccount` compensates partially by clearing everything after `Info` in the cached record before writing:

```
file->Info = m->AccountInfo;
memset(file->Char, 0, sizeof(STRUCT_ACCOUNTFILE) - sizeof(STRUCT_ACCOUNTINFO));
memset(file->Cargo, 0, sizeof(STRUCT_ITEM) * MAX_CARGO);
file->Coin = 0;
```
(`Code/DBSrv/CFileDB.cpp:528-533`) — note that the explicit cargo memset is redundant with the preceding range memset, and that the memset length `sizeof(STRUCT_ACCOUNTFILE) - sizeof(STRUCT_ACCOUNTINFO)` starts at `file->Char`, which is correct only because `Char` immediately follows `Info` in the layout.

On success the handler registers the account in the cache with `AddAccountList(Idx)` and replies with `MSG_DBCNFAccountLogin`, i.e. an implicit login, with `memset(sm.Cargo, 0, ...)` and a freshly computed select-char projection (`Code/DBSrv/CFileDB.cpp:544-559`).

**Rule workflow**:
```
add path:      probe _open(path, O_RDONLY) -> exists ? FALSE : build+write
network path:  no probe -> overwrite existing record with zeroed body
```

---

### Business Rule: R5 — New-account defaults and the bonus cargo grant

**Overview**:
A freshly written account starts with a sentinel numeric token (`-1`), zero donation balance, all quick-skill bytes set to `-1`, and, when the `bonus` parameter is non-zero, one hundred cargo items: fifty of index 401 and fifty of index 406.

**Detailed description**:
The `-1` token is the "unset" marker that drives the numeric-token state machine in R30: `_MSG_AccountSecure` tests `NumericToken[0] == -1` to decide whether it is binding a token for the first time (`Code/DBSrv/CFileDB.cpp:1373`). The `memset(file.ShortSkill, -1, 8)` at `Code/DBSrv/CFileDB.cpp:124` writes only eight bytes even though `ShortSkill` is `unsigned char[4][16]`; the remaining 56 bytes stay zero because the whole struct was `memset` to 0 three lines earlier (`:110`), so the effective result is that slot 0's first eight quick-skill bytes are 0xFF while everything else is 0. That asymmetry is visible to the client when the character-select projection is built.

The bonus cargo loop writes `sIndex` only, leaving `stEffect` zeroed, and fills indices 0-49 with 401 and 50-99 with 406, leaving 100-127 empty. `bonus` is sourced from the last line of the import file processed by `CReadFiles::ImportItems` (`Code/DBSrv/CReadFiles.cpp:666-677`), i.e. it is an operator-controlled flag on the bulk account import rather than a game rule. Because no `sValue`/effect is set, these are plain items with no refinement or quantity, and their meaning is entirely defined by the item table in `ItemEffect.h`/the client.

Nothing normalises `bonus` beyond `!= 0`; negative values behave the same as positive, and the value is not stored in the record, so a re-import cannot distinguish a bonus account from a normal one afterwards. The rule therefore only affects the initial file content.

**Rule workflow**:
```
memset(record, 0)
NumericToken[0] = -1; Donate = 0; SSN1/SSN2 = params; ShortSkill[0..7] = 0xFF
if bonus != 0: Cargo[0..49].sIndex = 401; Cargo[50..99].sIndex = 406
DBWriteAccount -> success ? TRUE : FALSE
```

---

### Business Rule: R6 — Login authentication

**Overview**:
Authentication is a single `strcmp` between the 16-byte stored `AccountPass` and the 16-byte `AccountPassword` supplied by the game server, which obtained it from the client. There is no hashing, no salt, no attempt counter, no lockout and no timing protection.

**Detailed description**:
Order of operations matters because several checks precede the password test and change what an attacker can observe. The name is uppercased and pattern-checked (R3), which can reveal whether a name is structurally valid without touching disk. The record is then read; a read failure produces `_MSG_DBAccountLoginFail_Account`, which is indistinguishable from "no such account" and from "record rejected as reserved". Only after the coin clamp (R7) and the date block (R8) and the server-change token test (R9) does the password comparison run, and a mismatch produces a distinct signal `_MSG_DBAccountLoginFail_Pass`. The separation of `fail_account` and `fail_pass` gives an enumeration oracle: a valid account name with a wrong password is distinguishable from an invalid name.

The stored password is plaintext in the file and is compared byte-for-byte with `strcmp`, which stops at the first NUL. A password shorter than 16 bytes therefore matches any stored value with the same prefix and a terminator at the same position; a password of exactly 16 non-NUL bytes relies on `strncpy`'s behaviour at write time. The record is written with `strncpy(file.Info.AccountPass, pass, ACCOUNTPASS_LENGTH)` (`Code/DBSrv/CFileDB.cpp:113`, `:191`, `:176`), which does not terminate when the source is 16 bytes or longer, so a maximum-length password leaves no terminator and `strcmp` reads into the following field until it meets a zero byte in `RealName`.

Passwords beginning with `_` or `@` are treated as banned in the NP authentication path (`Code/DBSrv/Server.cpp:1701-1704`), which is a separate credential surface that re-reads the same file and additionally requires the account's highest character level to be `>= 1000`, deriving an admin level of `level - 1000` (`Code/DBSrv/Server.cpp:1713-1726`).

**Rule workflow**:
```
read record -> coin clamp -> date block -> token test -> strcmp(pass)
match    => proceed to cache admission (R10/R12)
mismatch => _MSG_DBAccountLoginFail_Pass, return TRUE
unreadable => _MSG_DBAccountLoginFail_Account, return TRUE
reserved name => _MSG_DBAccountLoginFail_Account, return FALSE
```

---

### Business Rule: R7 — Negative coin clamp

**Overview**:
After reading an account record, `if (file.Coin < 0) file.Coin = 0;` repairs a corrupt or legacy negative balance before it is exposed or persisted.

**Detailed description**:
The clamp sits between the read and every subsequent use (`Code/DBSrv/CFileDB.cpp:645-646`), so it protects the login response (`sm.Coin`, `:776`) and the cached record. It is a one-way repair: the clamped value is not written back at that moment, so the negative value survives on disk until the next save writes the whole struct from the cache, at which point 0 is persisted. This is the only field-level sanitation in the whole read path; level, experience, item indices and upgrade values are copied verbatim from disk into the client projection.

Because the clamp only runs on login, other readers bypass it. The NP account-info handler (`Code/DBSrv/Server.cpp:1760-1800`) and the ranking scan (`Code/DBSrv/CRanking.cpp:175-196`) read raw records and would show or use a negative balance. The rule is therefore not an invariant of the storage layer; it is a local repair in one of several read paths.

**Rule workflow**:
```
DBReadAccount -> if Coin < 0 then Coin = 0 (memory only, persisted on next full write)
```

---

### Business Rule: R8 — Dated account block

**Overview**:
If `Info.Year` and `Info.YearDay` are both non-zero, the account is blocked while the stored date is not in the past, and login is refused with `_MSG_DBAccountLoginFail_Block`.

**Detailed description**:
The comparison is `if (file.Info.Year >= when.tm_year || file.Info.Year >= when.tm_year && file.Info.YearDay >= when.tm_yday)` (`Code/DBSrv/CFileDB.cpp:655`). The second disjunct is unreachable whenever the first is false, so the effective rule reduces to `Year >= current_year`, meaning any block with the current year or a later year always blocks, regardless of day-of-year. A block set to a previous year never blocks even if the day-of-year is in the future. The `when.tm_year` field is years since 1900, while the admin tool writes `pAdmin[i].Year`, whose origin is not established in this component; `DisableAccount` copies it verbatim (`Code/DBSrv/Server.cpp:1393-1394`) and `EnableAccount` clears both fields to 0 (`:1452-1453`).

The block is applied to the account, not to the character or session. `SendDBSignalParm(conn, m->ID, _MSG_DBAccountLoginFail_Block, 0)` is sent and the handler returns `TRUE` without admitting the account to the cache, so the client is expected to display a block message. Because the check runs before the password test, a blocked account cannot be probed for password correctness — a deliberate but undocumented ordering choice.

Blocks are also applied reactively: `_MSG_DBNoNeedSave` and `_MSG_SavingQuit` inspect `pAdmin[i].DisableID` for the evicted cache index and call `DisableAccount` with the admin's stored `Year`/`YearDay` (`Code/DBSrv/CFileDB.cpp:1108-1120`, `:1261-1272`). This means a GM can queue a ban that only takes effect when the target logs out, and the ban date is taken from the GM's session state at that later moment.

**Rule workflow**:
```
if Year != 0 && YearDay != 0:
    if Year >= current_year: block, send _MSG_DBAccountLoginFail_Block, return
    else: allow
```

---

### Business Rule: R9 — Server-change token bypasses the password

**Overview**:
When both the stored `TempKey` and the client-supplied `Zero[52]` are non-zero and their first `sizeof(TempKey)` bytes are equal, the password check is skipped, the stored token is cleared in memory, and the login is treated as a character-resume on the new server.

**Detailed description**:
The mechanism exists to support cross-server character movement without re-asking the client for a password. `_MSG_DBServerChange` generates eight random values in the range 100-999 (`Code/DBSrv/CFileDB.cpp:2671-2683`), formats them together with the target server id and slot into `"*<server> <e0> <e1> ... <e7> <slot>"`, copies that string into `sm.Enc`, and copies it into the *in-memory* `pAccountList[Idx].File.TempKey` (`Code/DBSrv/CFileDB.cpp:1981-1984`). The reply `_MSG_DBCNFServerChange` carries the same string to the game server, which forwards it to the client; the client reconnects to the new server and passes it back in `MSG_AccountLogin.Zero`.

`SetEncPassword(idx, newsrv, Enc, Unk)` is an empty function body (`Code/DBSrv/CFileDB.cpp:2685-2688`) and `GetEncPassword(idx, Enc)` ignores `idx` and unconditionally returns `FALSE` (`:2671-2683`). The result is that the `if (EncResult)` branch of `_MSG_DBServerChange` — the "reuse an existing token" path — is dead, and every request generates a fresh token. More importantly, `_MSG_DBServerChange` performs **no** `DBWriteAccount` call: the token exists only in the RAM record addressed by the *old* connection index, while the validating login arrives on a *different* index (`newserver * MAX_USER + newid`) and re-reads the token from disk. Therefore the on-disk `TempKey` remains whatever the last full write left there, which is normally all zeros, and the `file.TempKey[0] != 0` precondition cannot be satisfied by a fresh server change.

The remaining live behaviour of the branch is the destructive one on mismatch: `memset(file.TempKey, 0, sizeof(file.TempKey)); DBWriteAccount(&file); return TRUE;` (`Code/DBSrv/CFileDB.cpp:672-674`). Any stale token on disk is wiped and the record rewritten, and the client receives no reply at all — a client waiting on a login response after presenting a stale token gets silence rather than a failure signal. Confidence: high that no persistence occurs inside the handler (verified by reading the complete case body); medium on the end-to-end consequence, since the exact client-side retry behaviour lives outside this component.

**Rule workflow**:
```
store: TempKey(disk=unchanged, RAM=Enc string); reply Enc to game server
verify: if TempKey[0] != 0 && Zero[0] != 0:
            equal   -> clear RAM token, ChangeServer = 1, resume character login
            unequal -> clear disk token + rewrite record, return with no reply
        else -> normal password path
```

---

### Business Rule: R10 — Duplicate session resolution

**Overview**:
A login is refused, or the older session is force-saved, based on whether the account name is already present in the cache at a different index. `DBNeedSave == 0` yields `_MSG_DBAlreadyPlaying`; otherwise `_MSG_DBStillPlaying` plus a `_MSG_DBSavingQuit` push to the older session.

**Detailed description**:
The decision point is `IdxName = GetIndex(m->AccountName)` (`Code/DBSrv/CFileDB.cpp:628`). If it equals the requesting index, the handler returns `TRUE` immediately with no reply at all (`:685-686`) — this is the "same slot re-sent the login" case, and the game server is expected to be retrying. If the name is found at another index, the handler logs a Portuguese message, then consults `m->DBNeedSave`: zero sends `_MSG_DBAlreadyPlaying` and drops the new request; non-zero sends `_MSG_DBStillPlaying` and calls `SendDBSavingQuit(IdxName, 0)` (`:688-703`), then breaks out of the switch, leaving the new login unanswered for the moment.

`SendDBSavingQuit` targets the *older* connection (`Idx / MAX_USER`) and sends `MSG_DBSavingQuit` with the account name and mode 0; the TMSrv side handles it by showing "your account is being used from elsewhere" to the older player and closing that session (`Code/TMSrv/ProcessDBMessage.cpp:1136-1168`). In that handler, if the older user is not in `USER_PLAY`/`USER_SAVING4QUIT`, the TMSrv replies with `_MSG_DBNoNeedSave`, which triggers the eviction path in R24 and frees the slot. Only then can the new login succeed on a retry.

The design therefore implements a request/response dance with no correlation id: the new login is neither accepted nor rejected in this invocation, and its acceptance depends on the client or game server retrying after the older session drains. There is no timeout, no queue and no record of the pending request — a client that does not retry simply sees no login. `GetIndex(char*)` skips entries with `Login == 0` (`Code/DBSrv/CFileDB.cpp:2341-2342`), so an evicted entry is invisible to this scan, and, since index 0 doubles as the not-found sentinel, an account that lives at index 0 can never be detected as a duplicate session.

**Rule workflow**:
```
IdxName == Idx       -> silent return
IdxName != 0:
   DBNeedSave == 0   -> _MSG_DBAlreadyPlaying, return
   DBNeedSave != 0   -> _MSG_DBStillPlaying + DBSavingQuit(IdxName,0), break
else                 -> admit to cache and reply
```

---

### Business Rule: R11 — Dual-sword character name swap on login

**Overview**:
At account login, if any two characters wear equip slot 13 with item index 774 and 775 respectively, the two characters swap names and both equip items are cleared, then the record is rewritten.

**Detailed description**:
The scan walks the four slots and records `left` for a character whose `Equip[13].sIndex == 774` and `right` for one whose value is 775 (`Code/DBSrv/CFileDB.cpp:710-723`). If both are found the handler exchanges the 16-byte `MobName` fields and sets both equip indices to 0, logs `etc,name swap <left> <right>`, and calls `DBWriteAccount` mid-login before the cache is populated (`:725-743`). The effect is a one-time repair of a data condition — two characters whose names are bound to the wrong body — driven entirely by the presence of special marker items rather than by a name/body identity field.

The rule has no idempotency guard beyond the marker items: after the swap both markers are cleared, so a second login will not re-trigger. Items 774/775 are permanently consumed by the operation, which means an account holding these items for another purpose loses them. The swap only exchanges names, not `mobExtra`, `affect`, `ShortSkill`, or the character index files under `./char/`, so after the swap the name-to-account index files still map the old names to the same account (which is harmless for lookups but means any external index built from those files is now stale in the name-to-slot sense). `left` and `right` are checked only for `!= -1`; if a single character somehow satisfied both conditions, the first assignment wins and the character would swap its name with itself.

Since the swap happens on the record read from disk and the rewrite occurs before `memcpy(pFile, &file, ...)`, the cache receives the post-swap state in the same request. If the subsequent `DBWriteAccount` returned `FALSE`, the handler would still proceed with the swapped in-memory data and the login would succeed with data that was never persisted — the return value is not checked.

**Rule workflow**:
```
for q in 0..3: if MobName non-empty and Equip[13] == 774 -> left = q
               if MobName non-empty and Equip[13] == 775 -> right = q
if left != -1 && right != -1:
    swap MobName(left, right); Equip[13](left) = Equip[13](right) = 0; DBWriteAccount
```

---

### Business Rule: R12 — Cache lifecycle: admission, eviction, refcount, and the absence of a sweep

**Overview**:
`AddAccountList` marks a cache entry as logged in, resets its character slot to `-1` and increments the owning connection's `Count`; `RemoveAccountList` reverses that, decrements `Count` and zeroes the entire cached record. There is no capacity pressure handling, no LRU, no timeout and no eviction when a game server disconnects.

**Detailed description**:
Admission is idempotent by guard: a second `AddAccountList` on a live entry logs `err,addAccountlist - already added` and returns without touching `Count` (`Code/DBSrv/CFileDB.cpp:2204-2209`). Eviction is also guarded: `RemoveAccountList` on an entry with `Login == FALSE` returns without decrementing (`:2223-2224`). The pairing keeps `pUser[conn].Count` (displayed in the DBSrv window, `Code/DBSrv/Server.cpp:148`) approximately equal to the number of admitted accounts for that connection. Because `GetIndex(char*)` returns 0 for both "not found" and "found at 0", the guard logic inherits the ambiguity: a call on an index-0 record either double-increments or fails to decrement.

The eviction triggers are exactly two message types, `_MSG_SavingQuit` and `_MSG_DBNoNeedSave`, plus the failure paths inside `_MSG_SavingQuit` itself (account mismatch, slot mismatch) which evict without writing. There is no periodic sweep, and `Server.cpp` never walks the array. When a TMSrv socket closes, the host sets `pUser[User].cSock.CloseSocket(); pUser[User].Mode = USER_EMPTY;` (`Code/DBSrv/Server.cpp:890-900`) and nothing else. Consequently, after a game-server crash or a network partition, every account that was logged in remains `Login == TRUE` with a stale `File` and a stale `Slot`. On the next login attempt, `IdxName != 0` becomes true and the R10 branch fires; with `DBNeedSave == 0` the player is told the account is already playing and can never get in until the process restarts or someone evicts the entry.

Memory behaviour reinforces the design: the array is a fixed 10000-element member of the single global `CFileDB cFileDB` (`Code/DBSrv/Server.cpp:89`), sized at roughly 7.7 KB per element plus 28 bytes of bookkeeping, i.e. on the order of 77 MB of static storage that is never freed. `InitAccountList` (`Code/DBSrv/CFileDB.cpp:2666-2669`) would zero one record but has no callers.

**Rule workflow**:
```
add:    if Login == TRUE -> log+return; Count++ (conn); Login = TRUE; Slot = -1
remove: if Login == FALSE -> return; Count-- (conn); Login = FALSE; Slot = -1; memset(File)
triggers: SavingQuit, NoNeedSave, SavingQuit validation failures
absent: disconnect sweep, TTL, capacity eviction
```

---

### Business Rule: R13 — Address space and index resolution

**Overview**:
The cache index is `conn * MAX_USER + id`, where `conn` is the TMSrv connection slot (0..9) and `id` is the player id (0..999). Name lookup is a linear scan over 10000 entries that returns 0 for a miss.

**Detailed description**:
The arithmetic mapping (`Code/DBSrv/CFileDB.cpp:2328-2333`) is total and collision-free for valid inputs, which is why so many handlers index `pAccountList` without bounds checks. The guarantee depends entirely on two upstream validations: `ProcessClientMessage` checks `std->ID < 0 || std->ID > MAX_USER` (`Code/DBSrv/Server.cpp:1343`) — note `>` rather than `>=` — and `conn` is validated because it comes from `GetUserFromSocket`. With `ID == MAX_USER` the computed index is `conn * 1000 + 1000`, which is the first entry of the *next* connection's range: a message from server 0 player 1000 aliases server 1 player 0. `_MSG_ReqTransper` applies a stricter local test (`m->ID <= 0 || m->ID >= MAX_USER`, `Code/DBSrv/CFileDB.cpp:259`), showing that the correct form was known but applied inconsistently.

The name lookup returns 0 on failure (`:2348`). Every caller that does `Idx = GetIndex(account)` and then dereferences has a latent aliasing bug when the account is absent: the operation lands on the record for server 0, player 0. In analyzable cases the outcome is benign-but-wrong (an eviction of an unrelated record, a log line with the wrong account name); in `_MSG_DBNoNeedSave` it means `RemoveAccountList(0)` executes when the account cannot be found, silently evicting whoever occupies slot 0. Several handlers avoid the issue by asserting `IdxName > 0 && IdxName < MAX_DBACCOUNT` (`Code/DBSrv/CReadFiles.cpp:237`, `:814`, and `Code/DBSrv/CFileDB.cpp:204`).

A second consequence of the direct addressing is that the "cache" is not keyed by identity: the same account can occupy two entries simultaneously if a second connection logs in before the first is evicted, which is exactly the state R10 exists to arbitrate. There is no reverse index and no hash, so every name lookup is a 10000-iteration scan with a string compare, executed at least once per login and per admin query.

**Rule workflow**:
```
key  = conn * MAX_USER + id            (validated only by the dispatcher, with an off-by-one)
miss = 0                               (indistinguishable from the (0,0) record)
```

---

### Business Rule: R14 — MAC recording and primary-account detection

**Overview**:
The four 32-bit adapter identifiers supplied at login are stored in the cache entry, and if the number of cached accounts sharing that MAC is `<= 1` the component broadcasts a `_MSG_DBCheckPrimaryAccount` notice to all game servers.

**Detailed description**:
`pAccountList[Idx].Mac[0..3]` is filled from `m->AdapterName` on the success path (`Code/DBSrv/CFileDB.cpp:751-754`) and reset to zeros by the constructor (`:65-68`). `GetAccountsByMac` counts live entries whose `memcmp` of the four ints matches exactly (`:2351-2367`). The count includes the entry just inserted, so `<= 1` means "this is the only account currently online from this machine", which is the definition of a primary account in this codebase. The notice goes to every connected TMSrv, not to a specific one, and the local game server is then expected to tell the client (`Code/DBSrv/CFileDB.cpp:782-801`).

Because the population is the *live cache* rather than the disk, the result depends on session timing and on the eviction defects in R12. After a TMSrv crash the stale entries remain and the count stays high, so a returning player is permanently classified as secondary. Conversely, an account that has just been evicted is not counted, so two clients on the same machine logging in on the same tick can both be judged primary. The MAC value is also client-supplied and unvalidated; it is used only for this heuristic and for the `_MSG_DBPrimaryAccount` broadcast (`Code/DBSrv/CFileDB.cpp:2068-2101`), not for authentication.

The stored MAC is never persisted: `STRUCT_ACCOUNTFILE` has no MAC field, so `Mac[4]` lives only in `STRUCT_ACCOUNTLIST`. Restarting DBSrv therefore erases all MAC history and resets every player to primary.

**Rule workflow**:
```
on login success: Mac[4] = AdapterName[4]; if count(entries with same Mac) <= 1:
    broadcast MSG_DBCheckPrimaryAccount{Mac, AccountName} to all live TMSrv
```

---

### Business Rule: R15 — Character name restrictions

**Overview**:
Character creation rejects four literal command names (`KING`, `KINGDOM`, `GRITAR`, `RELO`, uppercased before comparison), the `COM[0-9]`/`LPT[0-9]` patterns, and any name containing two consecutive `í` characters (the first byte of the CP949 encoding of certain syllables, used as an obfuscation filler).

**Detailed description**:
The literal list assumes `_strupr` on an ASCII name; the comparison is a `strcmp` against uppercase literals (`Code/DBSrv/CFileDB.cpp:897`) after uppercasing the input copy, so lowercase variants are rejected too. The `íí` loop is more fragile: it iterates `for (i = 0; i < len; i++)` and reads `m->MobName[i + 1]`, so at `i == len - 1` it reads one byte past the string terminator, and `len` is measured *after* the request buffer was force-terminated at `NAME_LENGTH - 1` and `NAME_LENGTH - 2` (`:937-940`). The check is a duplicate-lookup safeguard for multi-byte names in the character index files, not a profanity filter.

The same block is duplicated almost verbatim in the arch-character handler (`:1461-1478`), including the identical `return FALSE` after `SendDBSignal(_MSG_DBNewCharacterFail)`. Note that in the arch handler a class-range failure sends `_MSG_DBNewCharacterFail` (`:1450`) while the slot-range failure sends `_MSG_DBCNFArchCharacterFail` (`:1442`), so failure signalling is not consistent between adjacent checks in the same function.

`DBCheckImpleName(char **ilchat, char *Str)` (`Code/DBSrv/CFileDB.cpp:2579-2609`) looks like the intended home for a configurable banned-word list (it matches 4-, 6- and 8-character entries from a caller-supplied table) but has no callers anywhere in the project, and its inner loop indexes `ilchat[j][i + 4]`/`[i + 6]` with `i` as the string offset, which is incorrect as a bounds test. The configured-list path is therefore not wired in.

**Rule workflow**:
```
_strupr(copy)
if copy in {KING, KINGDOM, GRITAR, RELO} -> fail
if copy matches COM[0-9]|LPT[0-9] (len 5) -> fail
if name contains "íí"                    -> fail
```

---

### Business Rule: R16 — Slot and class ranges for character creation

**Overview**:
The requested slot must be 0..3 (`MOB_PER_ACCOUNT - 1`) and the requested class 0..3 (`MAX_CLASS - 1`); violations produce `_MSG_DBNewCharacterFail`.

**Detailed description**:
Both tests use signed comparisons on attacker-controlled ints and log with the account name taken from the cache (`Code/DBSrv/CFileDB.cpp:865-880`). The class value is used later as an index into `g_pBaseSet[MAX_CLASS]` after a second, redundant if-else ladder that repeats the same range test (`:973-992`), whose `else` branch sends a failure signal and returns — unreachable given the earlier check, but retained as defence in depth. The template arrays are loaded once at boot from `./BaseMob/{TK,FM,BM,HT}` (`Code/DBSrv/Server.cpp:450-508`); a missing file aborts DBSrv startup with a message box, so a running process always has all four templates.

The arch handler uses `cls < 0 || cls >= MAX_CLASS` for its class check (`:1446`) which is equivalent to 0..3, and computes its slot itself rather than taking it from the message (R20). Slot ownership is decided purely by `MobName[0] == 0`; there is no separate occupancy bitmap, so the empty-name convention is the single source of truth for "slot free" and is relied on by character creation, deletion, capsule restore and the select-char projection alike.

Failure signalling diverges from the general pattern: the slot failure at `:869` and class failure at `:877` send the signal and `return TRUE`, while the reserved-name failures at `:903` and `:913` send the same signal and `return FALSE`. Both are "requested action failed", so `FALSE` here propagates to `Server.cpp`, which discards it (`Code/DBSrv/Server.cpp:1356`) — the value has no effect on protocol behaviour.

**Rule workflow**:
```
if Slot < 0 || Slot >= 4      -> log, _MSG_DBNewCharacterFail, return TRUE
if cls  < 0 || cls  >= 4      -> log, _MSG_DBNewCharacterFail, return TRUE
```

---

### Business Rule: R17 — Numeric-token gate on character operations

**Overview**:
Character creation, character login and character deletion refuse to proceed when `pAccountList[Idx].SecurePass == -1`. The state is set to 1 only by `_MSG_AccountSecure` success or by the server-change resume path, and is reset to -1 on every account login.

**Detailed description**:
`SecurePass` is initialised to `-1` by the constructor (`Code/DBSrv/CFileDB.cpp:63`) and forced to `-1` immediately after the login response is prepared (`:762`), which means an account is "unverified" as soon as it authenticates. The client is then expected to send `_MSG_AccountSecure` with a 6-byte numeric token; on success the handler sets `SecurePass = 1` and echoes `_MSG_AccountSecure` (`:1386`, `:1394`, `:1413`). The three character operations then read the flag and either proceed or log and abandon the request — note they `break` out of the switch rather than sending a failure signal, so the client sees silence, presumably because the UI is expected to be gated client-side.

The flag is per session, not per account: it lives in `STRUCT_ACCOUNTLIST`, is not persisted, and is re-armed to -1 on the next login. It also gates only these three operations; `_MSG_DBSaveMob`, `_MSG_SavingQuit`, `_MSG_DBCapsuleInfo`, `_MSG_DBPutInCapsule` and `_MSG_DBOutCapsule` do not consult it. Since the token is a second factor checked against the same file the password came from, and since the login has already succeeded by the time it is requested, the mechanism protects against a client-side UI bypass only — not against an attacker who speaks the game protocol directly.

The arch-character handler does **not** consult `SecurePass` at all (`:1423-1589`), which is an asymmetry with the mortal creation path in the same class.

**Rule workflow**:
```
login success -> SecurePass = -1
_MSG_AccountSecure ok -> SecurePass = 1  (or on server-change resume)
create/char-login/delete: SecurePass == -1 -> log + abandon
save/capsule/token-independent ops: no check
```

---

### Business Rule: R18 — Character name uniqueness through index files

**Overview**:
A character name is reserved by creating the file `./char/<first-key>/<UPPERCASED_NAME>` (opened `r` first to detect existence, then created) and writing the 16-byte account name into it. Deletion removes that file.

**Detailed description**:
`CreateCharacter` (`Code/DBSrv/CFileDB.cpp:2236-2301`) applies the reserved-pattern checks to both the account and the character name, derives the shard from the *character* name, probes with `fopen(temp, "r")`, and returns `FALSE` if the probe succeeds. It then opens with `O_RDWR|O_CREAT|O_BINARY` and writes exactly `ACCOUNTNAME_LENGTH` bytes (`_write(Handle, ac, ACCOUNTNAME_LENGTH)`). The file is therefore a 16-byte record whose sole payload is the owning account, and the filename is the character name. There is no slot number in the record and no level or timestamp, so the file is a pure uniqueness token plus reverse mapping.

The probe-then-create sequence is not atomic, and the create step will happily truncate an existing file if the probe raced. Note also that `fopen("r")` and `_open(O_RDWR|O_CREAT|O_BINARY)` can disagree in edge cases (for example, a path that exists as a directory, where `fopen` fails but `_open` also fails — producing an `EACCES`/`EINVAL` log and `FALSE`). Failure reasons are logged by `errno` (`EEXIST`, `EACCES`, `EINVAL`, `EMFILE`, `ENOENT`) with an `else` attached to the last `if`, so `EACCES` and `EINVAL` produce both their specific line and the `UNKNOWN` line.

The account record is the authoritative copy of the character; the index file is the global name registry. The two can diverge: `DeleteCharacter` removes the index file by name (`:2303-2326`) but the caller separately clears the slot in the record and rewrites it (`:1331-1343`), and if the account write fails after the index file is gone, the character still exists in the record but its name is no longer reserved. The capsule take-out path creates an index file for the restored name and then deletes a *different* file (`./capsule/<index>`), so a failure between those steps leaves a reserved name with no body.

**Rule workflow**:
```
reject reserved patterns -> derive shard from the character name
fopen(path, "r") success -> EEXIST, return FALSE
_open(O_RDWR|O_CREAT) -> _write(accountName, 16) -> close -> TRUE
delete: DeleteFileA(path) -> return Win32 BOOL
```

---

### Business Rule: R19 — New mortal character template

**Overview**:
A newly created mortal character is `memcpy` of `g_pBaseSet[class]` after clearing the destination mob and its extra block, with `ClassMaster = MORTAL`, `MortalFace = mob->Equip[0].sIndex`, quick-skills set to `-1`, affects zeroed, and the requested name copied in last.

**Detailed description**:
The sequence is order-sensitive. `BASE_ClearMob(mob)` and `BASE_ClearMobExtra(extra)` wipe the target slot; `affect[Slot]` is zeroed and `ShortSkill[Slot]` is filled with `0xFF` for 16 bytes (`Code/DBSrv/CFileDB.cpp:964-969`). `extra->ClassMaster = MORTAL` is then set, and only after that is the template copied over the mob. `extra->MortalFace = mob->Equip[0].sIndex` records the template's face item so that later projections can rebuild the portrait, then `memcpy(mob->MobName, m->MobName, NAME_LENGTH)` stamps the identity. Because `mob` is a pointer into the cached record, the whole edit is applied in RAM and flushed by a single `DBWriteAccount(&pAccountList[Idx].File)` three statements later (`:999`).

The template copy also overwrites any `Coin`, `Exp`, `SPX/SPY`, `Guild` and `Carry` content that happened to be in the template file, so the base mob files are effectively the new-character defaults for everything except name, extra block, affects and quick skills. `BASE_ClearMob` is called before the copy, which is redundant for the mob itself but necessary for the fields the template does not set.

If the account write fails, the handler sends `_MSG_DBNewCharacterFail` and returns, leaving the in-memory slot populated with the new character while the disk record still has an empty slot. The next successful save from that session would persist the character; a disconnect before that would leave the character only in RAM and the reserved name on disk — a divergence that no code path reconciles.

**Rule workflow**:
```
clear mob/extra/affect -> extra.ClassMaster = MORTAL -> CopyMob(g_pBaseSet[cls])
MortalFace = Equip[0].sIndex -> MobName = requested -> DBWriteAccount
reply MSG_CNFNewCharacter{DBGetSelChar}
```

---

### Business Rule: R20 — Arch character creation

**Overview**:
`_MSG_DBCreateArchCharacter` picks the first slot whose `MobName` is empty, requires `cls` in 0..3 and a non-reserved name, sets `ClassMaster = ARCH`, stores `MortalSlot`, forces the face item to `MortalFace + 5 + cls` and `BaseScore.Ac` to 230, and strips equipment and the first 60 carry slots.

**Detailed description**:
The slot is discovered rather than supplied (`Code/DBSrv/CFileDB.cpp:1434-1437`), and the "out of range" test that follows can only fire when all four slots are occupied — in which case the loop exits with `Slot == 4` and the condition `Slot >= MOB_PER_ACCOUNT` triggers, sending `_MSG_DBCNFArchCharacterFail` (`:1438-1445`). Functional but encoded as a sentinel value rather than a found/not-found result. The `MortalSlot` from the message is stored into `extra->QuestInfo.Arch.MortalSlot` and later cleared to `-1` when the character is sealed into a capsule (R28), which is how the arch-mortal link is tracked.

After copying the class template, the handler overrides the appearance and defence: `mob->Equip[0].sIndex = MortalFace + 5 + cls` and `mob->BaseScore.Ac = 230` (`:1551-1552`). It then zeroes `Equip[1..15]` (`sizeof(STRUCT_ITEM) * (MAX_EQUIP - 1)`) and `Carry[0..59]` (`sizeof(STRUCT_ITEM) * (MAX_CARRY - 4)`), leaving the last four carry slots untouched from the template — an asymmetry that is preserved from the original client behaviour and not explained in code. `extra->MortalFace = MortalFace` records the chosen face.

On success the handler emits two replies: `SendDBSignalParm(conn, m->ID, _MSG_DBCNFArchCharacterSucess, Slot)` with the discovered slot, then `MSG_CNFNewCharacter` with the projection (`:1586-1587`). Unlike mortal creation it performs no `CreateCharacter` index-file reservation, so an arch character's name is never entered into the `./char/` registry and can therefore duplicate an existing mortal name at the record level. Confidence: high that no index file is written on this path (the case body contains no `CreateCharacter` call).

**Rule workflow**:
```
Slot = first i with Char[i].MobName[0] == 0 (4 => fail)
cls in 0..3, name not reserved
extra.ClassMaster = ARCH; extra.QuestInfo.Arch.MortalSlot = MortalSlot
Equip[0] = MortalFace + 5 + cls; BaseScore.Ac = 230; clear Equip[1..], Carry[0..59]
DBWriteAccount -> signal success + MSG_CNFNewCharacter
```

---

### Business Rule: R21 — Character deletion preconditions

**Overview**:
Deletion requires a valid slot, a verified numeric token, the account password, a `ClassMaster` of `MORTAL` or `ARCH`, and a character level strictly below 219. Every violation sends `_MSG_DBDeleteCharacterFail` except the token gate, which silently abandons the request.

**Detailed description**:
The password test is `strncmp(m->Password, pAccountList[Idx].File.Info.AccountPass, ACCOUNTPASS_LENGTH)` compared `!= 0` → fail (`Code/DBSrv/CFileDB.cpp:1302`). Because both operands are 16-byte fields and `strncmp` stops at a mismatch or a NUL, a `Password` field padded with zeros matches a stored password of any length up to 16 — the client must therefore send an exact prefix. The class-master test rejects any character whose `ClassMaster` is neither `MORTAL` (0) nor `ARCH`, which excludes celestial states and any future value; the level cap `BaseScore.Level >= 219` blocks deletion of high-level characters (`:1321`). `MAX_LEVEL` is 399 and `MAX_CLEVEL` 199 in `Code/Basedef.h:117-118`, so 219 is a bespoke threshold that appears nowhere else in the component.

The destructive sequence is: zero `ShortSkill[Slot]`, call `DeleteCharacter(mob->MobName, account)` to remove the index file, log, `BASE_ClearMob(mob)`, `BASE_ClearMobExtra(&mobExtra[Slot])`, then `DBWriteAccount` (`:1331-1343`). The index file is removed *before* the record is rewritten, and neither return value is checked, so a failed record write leaves a live character with no name reservation. Note `DeleteCharacter`'s second parameter `account` is unused inside the function (`:2303-2326`).

The capsule take-out path reuses `BASE_ClearMob` semantics (`:1744-1745`) but not this rule set, so deletion-by-sealing ignores the level and class-master restrictions and the password, requiring only a verified token. That difference is significant: level-219 protection applies to deletion but not to encapsulation.

**Rule workflow**:
```
Slot range -> SecurePass != -1 -> strncmp(password) == 0 -> ClassMaster in {MORTAL, ARCH}
-> Level < 219 -> clear ShortSkill -> delete ./char index file -> clear mob+extra -> rewrite record
-> reply MSG_CNFDeleteCharacter{DBGetSelChar}
```

---

### Business Rule: R22 — SaveMob persistence (`_MSG_DBSaveMob`)

**Overview**:
A periodic/at-will save replaces the cached `Char[Slot]`, `mobExtra[Slot]`, `affect[Slot]`, `ShortSkill[Slot]`, `Cargo[128]`, `Coin` and `Donate` from the message, writes the record, and optionally writes the export mirror.

**Detailed description**:
The slot used is **the cached** `pAccountList[Idx].Slot`, not the slot in the message. If the two disagree the handler logs `err,savemob2` and continues with the cached slot (`Code/DBSrv/CFileDB.cpp:1153-1160`); if the cached slot is out of range it logs `err,savemob1` and returns without writing (`:1142-1151`). This ordering protects against a client that reports a different slot than the one it logged in with, at the cost of silently writing to a slot the sender did not intend. A cache entry with `Login == 0` is refused with `err,savemob3` (`:1162-1171`), which is the only guard against writing into an evicted record.

The write is unconditional on success of those checks: the entire character snapshot is copied into the cache and then `DBWriteAccount` is called with the return value ignored (`:1173-1184`). Because `Cargo` is account-level (shared by all four characters) and the message carries all 128 entries, every save from any character rewrites the whole account cargo — matching the client model where cargo is a shared storage, but meaning a save from an out-of-date session can revert cargo changes made by another character of the same account.

The `Export` field in the message triggers `DBExportAccount`, a second full record write to `S:/export/account{ServerIndex}/<NAME>` (`:1186-1187`). Since both writes are independent and unchecked, an export failure is invisible and the mirror silently drifts from the primary.

**Rule workflow**:
```
Idx = conn*MAX_USER+id; Slot = cached Slot
Slot invalid -> log, return
Slot != message Slot -> log, continue anyway
Login == 0 -> log, return
copy fields -> DBWriteAccount (unchecked) -> if Export: DBExportAccount (unchecked)
```

---

### Business Rule: R23 — Save-and-quit durability

**Overview**:
`_MSG_SavingQuit` is the full durable logout: it validates that the message belongs to the cached session (account name and slot), refuses to write for a non-logged-in entry, persists the whole record, writes the export mirror, evicts the cache entry, signals `_MSG_DBCNFAccountLogOut`, and finally applies any pending admin disable.

**Detailed description**:
Two independent identity checks precede the write. `strncmp(m->AccountName, cachedName, ACCOUNTNAME_LENGTH)` must be zero (`Code/DBSrv/CFileDB.cpp:1209`), and `Slot` must be in range and equal to `m->Slot` (`:1221`). Both failures log, call `RemoveAccountList(Idx)`, and return without writing — i.e. a mismatched logout destroys the cached state instead of persisting it, which is the correct conservative choice for a forged packet but also means a legitimate logout with a corrupted account-name field silently discards the session's progress. The `Login == 0` case logs and returns *without* evicting (`:1233-1241`), unlike the two mismatch paths.

The persistence itself mirrors SaveMob: `Char[Slot]`, `Cargo[128]` (`sizeof(STRUCT_ITEM)*MAX_CARGO`), `ShortSkill[Slot]` (16), `mobExtra[Slot]`, `affect[Slot]`, `Coin`, `Donate`, then `DBWriteAccount`, then `DBExportAccount`, then `RemoveAccountList` (`:1243-1257`). Both write results are ignored. `SendDBSignal(conn, m->ID, _MSG_DBCNFAccountLogOut)` is then sent; because `RemoveAccountList` has already zeroed the cached record, any failure signal cannot be accompanied by account data and the game server will close the session regardless.

Ordering creates the crash window: the record is written, the export is written, the cache is zeroed, and only then is the logout acknowledged. A crash between the record write and the acknowledgement leaves the disk correct and the client waiting — recoverable. A crash *during* the record write leaves a partially written ~7.7 KB record that DBReadAccount will accept without verification (R25). A crash after the record write but before the export leaves the mirror stale, with no way to detect it.

**Rule workflow**:
```
name match -> slot match -> Login != 0 -> copy fields -> DBWriteAccount
-> DBExportAccount -> RemoveAccountList -> _MSG_DBCNFAccountLogOut -> apply pending disable
```

---

### Business Rule: R24 — No-save eviction and deferred disable

**Overview**:
`_MSG_DBNoNeedSave` evicts a cache entry without persistence and applies a pending ban queued by an admin for that cache index.

**Detailed description**:
The handler copies the account name out of the cache before eviction (since eviction zeroes the record), calls `RemoveAccountList(Idx)`, then scans `pAdmin[0..9]` for an entry with `Level > 2` and a matching `DisableID`, and calls `DisableAccount(i, acc, pAdmin[i].Year, pAdmin[i].YearDay)` (`Code/DBSrv/CFileDB.cpp:1100-1120`). The name copy is essential because `RemoveAccountList` memsets `File`, including `Info.AccountName`; without it the ban would be applied to an empty string. This is the only place in the component where the ordering dependency is handled correctly.

`DisableAccount` itself is a host function (`Code/DBSrv/Server.cpp:1359-1417`) that re-reads the record from disk, refuses if it is already dated, sets `Info.Year`/`Info.YearDay`, and rewrites. Because it re-reads from disk, any unsaved RAM state for that account is discarded by the subsequent write — the function writes the disk copy it just read, so a ban applied at logout in the `_MSG_DBNoNeedSave` path does not lose the last save (there was none), but a ban applied from the NP tool concurrently with an active session will be reverted by that session's next save.

The `Level <= 2` filter means only admins above level 2 can carry a pending disable, and `DisableID == 0` is treated as "no pending ban", which again collides with the index-0 ambiguity: a ban targeted at cache index 0 is indistinguishable from no ban.

**Rule workflow**:
```
copy account name -> RemoveAccountList(Idx) -> for each pAdmin: Level>2 && DisableID==Idx
-> DisableAccount(i, name, Year, YearDay)
```

---

### Business Rule: R25 — Persist primitive: in-place overwrite, no atomicity, no durability barrier

**Overview**:
`DBWriteAccount` opens the record with `O_RDWR|O_CREAT`, seeks to 0, writes `sizeof(STRUCT_ACCOUNTFILE)` bytes and closes. There is no temporary file, no rename, no backup, no checksum, no `fsync`/`FlushFileBuffers`, no write-ahead log and no file locking.

**Detailed description**:
The failure handling covers open, seek and write (`Code/DBSrv/CFileDB.cpp:2415-2464`) and logs the `errno` class for each, but the `if/else` chain again attaches `else` only to the final `if`, so `EACCES`, `EINVAL`, `EMFILE` and `ENOENT` each produce two log lines. A short write is not detected at all: the code tests `ret == -1`, not `ret != sizeof(...)`, so a partially written record is reported as success. After the write the handle is closed without flushing, relying on the CRT's close-time flush to the OS cache, which does not protect against power loss or an OS crash.

The recovery characteristics follow directly. Because the overwrite starts at offset 0 and never truncates, a crash mid-write yields a record whose head is the new content and whose tail is the old content — a *torn* record, internally inconsistent between the account block and the character blocks but still `sizeof` bytes long, so `DBReadAccount` returns `TRUE` and the login succeeds with a hybrid. If the file did not exist and the write failed early, a zero-length or truncated file results; `DBReadAccount` then reads short and only clears 8 bytes of `ShortSkill` (R1), leaving the rest of the caller-provided buffer as-is.

Callers compound the risk by ignoring the return value on the hot paths: SaveMob (`:1184`), SavingQuit (`:1255`), DeleteCharacter (`:1343`), capsule put-in (`:1747`), capsule take-out (`:1897`), the login name-swap (`:742`), and the server-change token wipe (`:673`). Only account creation (`:535`), token binding (`:1377`, `:1404`), `_MSG_DBCreateCharacter` (`:999`), `_MSG_DBCreateArchCharacter` (`:1561`), the save/quit validation errors, and `AddAccount`/`UpdateAccount` check the result. A failed save therefore produces no client-visible error anywhere on the normal gameplay path.

**Rule workflow**:
```
_open(O_RDWR|O_CREAT) -> _lseek(0) -> _write(sizeof record) -> _close
short write undetected; no temp+rename; no flush; no lock; return ignored by most callers
```

---

### Business Rule: R26 — Export mirror

**Overview**:
`DBExportAccount` writes a second copy of the same record to an absolute path on drive `S:` — `S:/export/account{ServerIndex}/<UPPERCASED_NAME>` — and returns `FALSE` silently on any failure.

**Detailed description**:
The mirror is unconditional on the save-and-quit path (`Code/DBSrv/CFileDB.cpp:1256`) and conditional on the message's `Export` flag for SaveMob (`:1186-1187`). The path is built from `ServerIndex`, the DBSrv process's own server-group index resolved at boot from the local IP (`Code/DBSrv/Server.cpp:555-581`), so all ten possible DB instances would write into ten sibling directories. The drive letter is absolute and hard-coded, meaning the mirror is unavailable on any host without an `S:` volume; in that case `_open` fails, the function returns `FALSE`, and no caller notices.

Note the name copy: `strncpy(check, accname, 16)` uses the literal 16 rather than `ACCOUNTNAME_LENGTH` (`:2477`). The two are equal today, so behaviour is correct, but the constant is duplicated. The function applies the reserved-name test again before opening, which means an account that somehow exists with a reserved name will be saved primarily but never mirrored — a permanent, silent divergence.

The mirror's purpose is not documented in code. The components that could consume it (`CReadFiles::ImportItem`, `readAccountsInDir`) read `./account/...`, not `S:/export/...`, so within this workspace the export is a dead-end copy. Its only plausible use is offline backup or an external tool not present in the repository. Confidence: high that no in-repo consumer reads `S:/export` (verified by searching the workspace for the literal).

**Rule workflow**:
```
_strupr(name) -> reserved pattern check -> "S:/export/account<ServerIndex>/<NAME>"
_open(O_RDWR|O_CREAT) -> _lseek(0) -> _write(sizeof) -> _close
any failure -> FALSE (never surfaced)
```

---

### Business Rule: R27 — Capsule index allocation is a global monotonic counter

**Overview**:
Capsule identifiers come from `unsigned short LastCapsule`, a process-global that starts at 0, is loaded from `Config.txt` at boot, incremented per capsule put-in, and persisted only when `WriteConfig()` is called (on put-in, and on Sapphire change, and on window close).

**Detailed description**:
The counter is declared and defined in this component (`Code/DBSrv/CFileDB.cpp:54`, `extern` in `Code/DBSrv/CFileDB.h:88`) but owned and persisted by the host: `ReadConfig` reads `LastCapsule %hu` from `Config.txt` (`Code/DBSrv/Server.cpp:405`) and `WriteConfig` writes `LastCapsule %d` (`:420`). Put-in increments first and then uses the new value as the filename (`:1698-1703`), so ids used on disk are 1-based and id 0 is never a valid capsule. Because it is 16-bit, the counter wraps after 65535 capsules to 0, at which point the next allocation collides with... nothing, since 0 was never used, but the following allocations would overwrite older capsule files if those still exist — `O_CREAT` without `O_EXCL` truncates.

`WriteConfig` rewrites the whole `Config.txt` with `fopen(..., "wt")` (`Code/DBSrv/Server.cpp:414-422`), so the file is truncated and rewritten each time; a crash mid-write loses both the Sapphire setting and the capsule counter, and the counter regresses to 0. On restart, `LastCapsule = 0` means the next put-in overwrites `./capsule/1`, destroying a previously sealed character. The window is small (the write happens immediately after the capsule file is closed, before the account is cleared) but the consequence is a lost capsule rather than a lost account.

Two operators manipulate the same counter outside the config: the capsule *item* written by the import subsystem encodes the id as two bytes, `LastCapsule/256` and `LastCapsule - (LastCapsule/256)*256`, in effect bytes 0 and 1 of item 3443 (`:1764`), and take-out reconstructs it as `stEffect[0].cValue * 256 + stEffect[1].cValue` (`:1808`). The 16-bit range is therefore load-bearing for the item encoding.

**Rule workflow**:
```
boot: LastCapsule = ReadConfig()
put-in: LastCapsule++ -> ./capsule/<n> -> WriteConfig()
import job: item 3443 with hi/lo of LastCapsule
take-out: index = eff[0]*256 + eff[1] -> ./capsule/<index>
```

---

### Business Rule: R28 — Capsule put-in (sealing a character)

**Overview**:
Sealing copies the slot's `STRUCT_MOB` and `STRUCT_MOBEXTRA` into `./capsule/<n>`, severs the arch/mortal link, clears the cached character and its quick skills, persists, and appends a job line to `../../Common/ImportItem/` that grants item 3443 carrying the capsule id.

**Detailed description**:
The message is `MSG_STANDARDPARM` whose `Parm` is the slot (`Code/DBSrv/CFileDB.cpp:1686-1690`). After the range check the handler increments `LastCapsule`, opens the capsule file, fills a local `STRUCT_CAPSULE` from the cache slot, sets `extra.QuestInfo.Arch.MortalSlot = -1`, writes `sizeof(STRUCT_CAPSULE)` bytes and closes (`:1723-1731`). The mortal link is severed before the write so that a restored character cannot point at an arch character that has since changed; the arch side is not modified at all, meaning the arch character keeps its `MortalSlot` pointing at a now-empty slot.

Then `WriteConfig()` persists the counter, `memset(ShortSkill[Slot], 0, 16)` clears quick skills, the log line `putchar in capsule [name]` is written, `BASE_ClearMob` + `BASE_ClearMobExtra` clear the slot, and `DBWriteAccount` persists the now-empty slot (`:1733-1747`). The order matters: the capsule file is closed before the account is cleared, so a crash in between leaves the character both sealed and still on the account — a duplication, not a loss. A failure of the account write leaves the character live in the cache and on disk while the capsule file also exists, and the success signal `_MSG_DBCNFCapsuleSucess` plus `MSG_CNFDeleteCharacter` are sent regardless (`:1749-1759`).

The final step writes an import job: a randomly-named file under `../../Common/ImportItem/` containing `"<account> 3443 59 <hi> 59 <lo> 0 0"` (`:1761-1765`). The `fopen(..., "a+")` result is never checked before `fprintf`, so a missing directory produces a null-pointer write. The job is how the sealed character's capsule item is delivered back to the player by the import loop in `CReadFiles`.

**Rule workflow**:
```
slot check -> LastCapsule++ -> write ./capsule/<n> {MOB, extra with MortalSlot = -1}
-> WriteConfig -> ShortSkill = 0 -> clear cache slot -> DBWriteAccount
-> _MSG_DBCNFCapsuleSucess + MSG_CNFDeleteCharacter -> append ImportItem job (item 3443)
```

---

### Business Rule: R29 — Capsule take-out (restoring a sealed character)

**Overview**:
Restore finds the first free slot, reserves the new name via `CreateCharacter`, reads the capsule indicated by the item's two effect bytes, renames the mob, strips equipment beyond slot 0, carry beyond slot 3, coin, guild and guild level, consumes the source item, persists twice, and deletes the capsule file.

**Detailed description**:
The source item is located with `GetItemPointer(&Char[Slot], Cargo, m->SourType, m->SourPos)` (`Code/DBSrv/CFileDB.cpp:1806`), and the capsule id is decoded from its first two effect values. Note the pointer is used before any validation that the item actually is a capsule: `item->stEffect[0].cValue * 256 + item->stEffect[1].cValue` is computed and the resulting path is opened; an ordinary item yields an arbitrary id and therefore an open failure, which returns `FALSE` (`:1815-1831`). The handler therefore trusts the item's effect bytes completely and only survives invalid input because the file does not exist.

The restore sequence has a structural defect worth recording precisely. The local pointers `mob` and `extra` are first pointed at the destination slot's `mobExtra` and then immediately reassigned to `&file.MOB` and `&file.extra` — the capsule copy (`:1876-1881`). Everything after that mutates the *local* `file` (clear equipment, carry, coin, guild, guild level, set the new name), then `DBWriteAccount(&pAccountList[Idx].File)` is called **before** the local mob and extra are copied into the cache (`:1897-1908`). The result is that the first write persists the cache as it was at that moment — the destination slot was still empty and the source slot was not yet cleared — and the restored character is only present in RAM afterwards. It is persisted later only if some subsequent save writes the whole record. The source item is zeroed before that write (`:1883`), so the item consumption *is* persisted.

`DeleteFileA("./capsule/<index>")` runs after the second write (`:1914-1915`), and its return value is ignored, so a locked capsule file leaves an orphan that can be restored again by replaying the item — except the item has already been zeroed. `memset(&mob->Equip[1], 0, sizeof(STRUCT_ITEM)*(MAX_EQUIP - 2))` clears 14 of the 15 equipment slots (leaving slots 14 and 15 untouched), a different range than the arch-creation path's `MAX_EQUIP - 1`; both look like preserved client quirks rather than intentional rules.

**Rule workflow**:
```
slot range -> first free NovoSlot -> CreateCharacter(name)
item = GetItemPointer(...); index = eff[0]*256 + eff[1]; read ./capsule/<index>
rename -> zero source item -> strip gear/carry/coin/guild -> DBWriteAccount
-> copy mob+extra into NovSlot -> log -> DeleteFileA(capsule) -> msg MSG_CNFNewCharacter
```

---

### Business Rule: R30 — Numeric token binding and change

**Overview**:
`_MSG_AccountSecure` implements a 6-byte secondary secret. Unset (`NumericToken[0] == -1`) binds a new token; an existing token is only accepted on a matching value; a change request requires the session to already be verified. Success sets `SecurePass = 1` and echoes `_MSG_AccountSecure`; any other path sets `SecurePass = -1` and sends `_MSG_AccountSecureFail`.

**Detailed description**:
The state machine has four branches (`Code/DBSrv/CFileDB.cpp:1357-1421`). `Change == 1 && Secure == -1` logs and breaks silently, so a change cannot be initiated before verification. If the stored token's first byte is `-1` (the "unset" marker written at account creation, R5) the supplied token is copied and persisted, `SecurePass = 1`, and the success echo is sent — meaning the very first token presented becomes the account's token, with no confirmation step. If `Change == 0 && Secure == -1` and `strncmp` of six bytes matches, the session is marked verified without any write. Otherwise, if `Change == 1 && Secure == 1`, the token is replaced and persisted.

The comparison `strncmp(..., 6)` over a field that is exactly 6 bytes and may contain no NUL means the check is a strict 6-byte prefix comparison, and `strncpy(..., m->NumericToken, 6)` again may leave the field unterminated. Unverified sessions fall through to `SecurePass = -1` plus `_MSG_AccountSecureFail`, which is also what a legitimate "change without verification" produces — the client cannot distinguish "wrong token" from "not allowed to change".

Because the token is stored in the same unencrypted record as the password and is only reachable after a password login, it is a convenience lock, not an independent factor. Its protection is also asymmetric: it gates character creation, character login and deletion (R17) but not save, capsule operations or arch-character creation.

**Rule workflow**:
```
Change==1 && Secure==-1            -> log, break
NumericToken[0] == -1              -> bind, write, Secure=1, success
Change==0 && Secure==-1 && match   -> Secure=1, success
Change==1 && Secure==1             -> replace, write, Secure=1, success
otherwise                          -> Secure=-1, _MSG_AccountSecureFail
```

---

### Business Rule: R31 — Password change (`UpdateAccount`, NP path)

**Overview**:
`UpdateAccount(id, pass)` verifies the account file exists, optimistically patches the live cache entry's password, reads the full record, replaces the password, writes the record back, and re-reads the index — with two dead/no-op tail statements.

**Detailed description**:
After the reserved-pattern check and an existence probe (`Code/DBSrv/CFileDB.cpp:153-171`), the handler does `int Idx = GetIndex(id); if (Idx != 0) strncpy(pAccountList[Idx].File.Info.AccountPass, pass, ACCOUNTPASS_LENGTH);` (`:173-176`). This is the only place the cache is patched ahead of the disk, and it inherits the index-0 ambiguity: an account at index 0 is never patched, and an account that is not in the cache (lookup returns 0) is also never patched — indistinguishable. The password is copied without a terminator when it is 16 bytes long, the same defect as R6.

The disk half then reads the record with `memcpy(file.Info.AccountName, id, ACCOUNTNAME_LENGTH)` into an **uninitialised** stack struct (`:178-182`). `DBReadAccount` fills `sizeof(STRUCT_ACCOUNTFILE)` bytes when the file is full-size, which is the normal case, so the struct is fully written before use; for a short legacy file, the tail stays indeterminate and is then persisted by the write below. The password is replaced and `DBWriteAccount` is called, with proper logging of both failure modes (`:184-200`) — this is one of the few paths that checks its result.

The trailing block is dead: `int IdxName = GetIndex(file.Info.AccountName); if (IdxName > 0 && IdxName < MAX_DBACCOUNT) strncpy(file.Info.AccountPass, pass, ...)` (`:202-205`) writes to the local `file` after it has already been persisted and the function returns `TRUE` immediately afterwards. Nothing observes the copy. The intended effect was presumably a second cache refresh, which the block does not perform.

**Rule workflow**:
```
reserved check -> _open(O_RDONLY) exists? -> Idx=cache index (if != 0) patch cache password
-> DBReadAccount -> replace password -> DBWriteAccount -> dead re-patch -> TRUE
```

---

### Business Rule: R32 — Character transfer to NPTool (`_MSG_ReqTransper`)

**Overview**:
A transfer request renames a cached character, verifies the caller's expected old name against the cached name, disables the account record, and forwards the whole mob to the external NP tool over `AdminClient`, which later replies and triggers a slot wipe.

**Detailed description**:
Validation is layered: `m->ID` must be 1..999 and the slot 0..3 (`Code/DBSrv/CFileDB.cpp:259-277`); `TransperCharacter != 0` and `AdminClient.Sock != 0` must hold (`:281-297`); and the uppercased cached `MobName` must equal the uppercased `m->OldName` (`:301-336`). Every failure sends `_MSG_ReqTransper` with `Result = 4` to the originating game server (guarded by `IdxName != 0`, so a request from server 0 / player 0 gets no reply). The rename is applied to the cache *before* the name comparison (`:309-312`), so a failed comparison leaves the cache holding the new name while the request is rejected — an in-memory mutation that the next save would persist.

On success `DisableAccount(-1, accountName, 0, 0)` is called first (`:338`), writing `Year = 0`/`YearDay = 0` (i.e. *un*blocking) to the record, and then a `MSG_NPCreateCharacter` carrying the whole `STRUCT_MOB` is sent to `AdminClient` (`:340-352`). The reply is handled in the host (`Code/DBSrv/Server.cpp:626-741`), which re-enables the account, and on `Result == 0` re-reads the record, zeroes the transferred slot with `memset(&file.Char[slot], 0, sizeof(STRUCT_MOB))`, rewrites the record, and — if the account is cached — zeroes the cached slot too (`:714-719`). Note the host zeroes only `STRUCT_MOB`, not `mobExtra[slot]`, so a transferred character leaves its extra block (fame, quests, celestial saves) behind in the record.

The transfer path never consults `SecurePass`, never checks the character level and never touches the `./char/` index file, so the old name remains reserved permanently after a successful transfer. The rename also makes the ranking entry (R35) point at a name that no longer matches any `MobName` in the record.

**Rule workflow**:
```
id/slot check -> transper enabled && AdminClient connected -> name match
-> rename cached mob -> DisableAccount(unblock) -> AdminClient: MSG_NPCreateCharacter
-> reply: EnableAccount -> on success: zero disk slot + zero cached slot
```

---

### Business Rule: R33 — Guild war, ally and guild-info fan-out

**Overview**:
Three message types maintain process-global 65536-entry arrays: `g_pGuildWar`, `g_pGuildAlly` and `STRUCT_GUILDINFO GuildInfo[65536]`. War/ally changes are broadcast as signals to every live game server; guild-info changes are additionally persisted through `CReadFiles::WriteGuildInfo()`.

**Detailed description**:
`_MSG_War` stores `g_pGuildWar[Parm1] = Parm2` after rejecting guild ids outside 1..65535, then sends `SendDBSignalParm2(i, 0, _MSG_War, Parm1, Parm2)` to each connection with a live socket (`Code/DBSrv/CFileDB.cpp:388-418`). `_MSG_GuildAlly` is structurally identical for the ally array (`:420-452`). Both write only one direction of the relation: setting `A -> B` does not set `B -> A`, and there is no consistency check that a guild exists. The arrays are zero-initialised at definition and at boot (`Code/DBSrv/Server.cpp:515-516`), and are never loaded from disk — so war and ally state is volatile and resets on every DBSrv restart, which is a deliberate-looking asymmetry with guild info.

`_MSG_GuildInfo` copies the incoming `STRUCT_GUILDINFO` into the array (a 7-byte struct: `Fame`, `Clan`, `Citizen`, `Sub1..Sub3`), then for every live connection calls `SendGuildInfo(i, Guild)` which re-packs and sends the array entry (`:454-481`, `:2185-2200`), and finally calls `CReadFiles::WriteGuildInfo()` to persist the entire 65536-entry array to the guild-info path. Because the persistence writes the whole array on every single guild change, each change is a full array dump; there is no dirty tracking.

`_MSG_GuildZoneReport` maintains `ChargedGuildList[MAX_SERVER][MAX_GUILDZONE]` and broadcasts the whole matrix to all servers, ignoring the incoming guild values beyond copying five ints (`:357-386`). It validates only `m->ID` as the server index, not the array contents. All three guild handlers operate on socket liveness (`pUser[i].cSock.Sock != 0`) and `Mode != USER_EMPTY`, so a connection object that is closed but not reset is skipped by the socket test while a reset one is skipped by the mode test.

**Rule workflow**:
```
War:   guild in (0,65535) -> g_pGuildWar[g] = parm2 -> fan-out signal to live servers
Ally:  guild in (0,65535) -> g_pGuildAlly[g] = ally  -> fan-out signal to live servers
Info:  guild in (0,65535) -> GuildInfo[g] = payload  -> SendGuildInfo to each -> WriteGuildInfo()
Zone:  srv in [0,MAX_SERVER) -> ChargedGuildList[srv][0..4] -> broadcast matrix
```

---

### Business Rule: R34 — Sapphire global scaling

**Overview**:
`_MSG_DBUpdateSapphire` doubles `Sapphire` when `Parm == 1` and halves it otherwise, clamps the result to the inclusive range 1..64, broadcasts `_MSG_DBSetIndex` with the new value to every live server, and persists it to `Config.txt`.

**Detailed description**:
`Sapphire` is a host global initialised to 8 (`Code/DBSrv/Server.cpp:50`) and loaded from `Config.txt` at boot (`:404`). The handler mutates it with `*=` or `/=` (`Code/DBSrv/CFileDB.cpp:487-495`) and clamps with `if (< 1) = 1; else if (> 64) = 64;` — a two-sided clamp that is correct for both directions. Integer division truncates, so repeated halving from 3 reaches 1 and stays there rather than reaching 0. Non-1 `Parm` values (including negative) all halve, which makes the "increase" action dependent on the exact value 1 rather than on a boolean.

The broadcast is `SendDBSignalParm3(i, 0, _MSG_DBSetIndex, -1, Sapphire, i)` — note `parm1 = -1` and `parm3 = i` (`:503`), a signature that differs from the boot-time broadcast, which uses `SendDBSignalParm3(User, 0, _MSG_DBSetIndex, ServerIndex, Sapphire, User)` (`Code/DBSrv/Server.cpp:1159`). The receiver must therefore interpret the triple contextually, and the `-1` sentinel in the live path versus a real server index at boot is an undocumented asymmetry.

Persistence uses `WriteConfig()` which rewrites the entire file (Sapphire plus LastCapsule). Because it truncates and rewrites, a crash during the write loses the capsule counter as described in R27; the two unrelated settings share one destructive persistence primitive.

**Rule workflow**:
```
Parm == 1 ? Sapphire *= 2 : Sapphire /= 2
clamp [1..64] -> broadcast _MSG_DBSetIndex(-1, Sapphire, i) to live servers -> WriteConfig()
```

---

### Business Rule: R35 — Ranking feed

**Overview**:
Two paths feed the in-memory `RankingSystem`: login/character-login publishes the character's current rank position and binds its connection id, and `_MSG_UpdateExpRanking` inserts or raises the player in the grind ranking and notifies affected players.

**Detailed description**:
On both the account-login resume branch and the explicit character-login branch, the handler computes `rankId = rankingSystem.grindRanking.getElementIndex(MobName)`; if the id is in `[LAST..FIRST]` (0..499) it stores `GrindRankingConnId(conn, sm.ID)` against that rank, then sends `sendUpdateIndividualRank(rankId, conn, sm.ID, new STRUCT_RANKING(name, Exp, ClassMaster, CurrentScore.Level, Class))` (`Code/DBSrv/CFileDB.cpp:842-852`, `:1082-1092`). The `new` allocation is never deleted here, which is a leak per login; whether `sendUpdateIndividualRank` takes ownership is not determinable from this component (`Code/DBSrv/CRanking.cpp` would have to be inspected for the ownership contract).

`_MSG_UpdateExpRanking` handles three states. `OUT_OF_RANK` attempts an insert and, if successful, notifies the evicted last-place player (when their conn id is valid) and then broadcasts the affected range. In-rank entries are raised only when `m->RankInfo.Value > thisRank->Value && m->RankInfo.ClassMaster <= thisRank->ClassMaster || m->RankInfo.ClassMaster > thisRank->ClassMaster` (`:2033`) — due to precedence this is `(value greater && classmaster not greater) || (classmaster greater)`, i.e. a higher class master can raise a rank with a lower value, and a lower class master cannot raise a rank even with a higher value. Whether that matches the intended "same-or-better class master" rule is not documented.

The ranking store is populated at boot by scanning the account directory tree and reading every file whose size is in the accepted window (`Code/DBSrv/CRanking.cpp:140-215`), skipping characters with `BaseScore.Level >= 1000` (admin characters) and characters with empty names. Level 1000 is the same threshold the NP login uses to derive admin level (R6), so the two subsystems share the convention documented nowhere in this component.

**Rule workflow**:
```
login: rankId = index(name) -> if in [0,499] bind connId -> send individual rank
update: OUT_OF_RANK -> try insert -> notify evicted + range
        in rank     -> if value/classmaster condition -> raise -> notify range
```

---

### Business Rule: R36 — Item day-log counter

**Overview**:
`_MSG_DBItemDayLog` increments `ItemDayLog[ItemID].Num` after validating `1 <= ItemID < 6500` (`MAX_ITEMLIST`).

**Detailed description**:
The message carries the item id in `MSG_STANDARDPARM.Parm`; the handler rejects `<= 0` and `>= MAX_ITEMLIST` with a log and returns `TRUE` (`Code/DBSrv/CFileDB.cpp:2044-2058`). Index 0 is deliberately excluded, which makes the valid range 1..6499 — one short of the array, so the last slot is unused. `ItemDayLog` is `STRUCT_ITEMLOG[MAX_ITEMLIST]` declared in the host (`Code/DBSrv/Server.cpp:92`) and defined `extern` in this component's header scope; the counter is a 32-bit int with no overflow protection.

The array is only ever incremented here and only ever read by `DayLog_ItemLog()`, which is called from the window-close handler and from the minute timer (`Code/DBSrv/Server.cpp:1317`, `:2058`). It is never reset by this component, so accumulation across a long uptime is expected and the "day log" name refers to the file it is dumped into, not to a daily reset performed here.

**Rule workflow**:
```
ItemID in [1, MAX_ITEMLIST) -> ItemDayLog[ItemID].Num++
otherwise -> log "err,itemid invalid", return TRUE
```

---

### Business Rule: R37 — Server record archiving

**Overview**:
`_MSG_MessageDBRecord` writes the submitted text into `../../Common/record{ServerIndex}/RC<yy>_<mm>_<dd>_<hh>_<mi>_<ss>_<conn>2.2d_<conn>.txt` and logs the same text to the server log.

**Detailed description**:
`ProcessRecord(int conn, char *str)` (`Code/DBSrv/CFileDB.cpp:210-234`) builds the filename from `localtime` fields. The format string `"RC%4.4d_%2.2d_%2.2d_%2.2d_%2.2d_%d2.2d_%2d.txt"` contains a defect: `%d2.2d` applies `%d` to `when.tm_sec` and then emits the literal `2.2d`, and the following `%2d` takes `conn`. The practical filename is therefore `RC<year-100>_<mm>_<dd>_<hh>_<mi>_<sec>2.2d_<conn>.txt`, which still yields a unique-enough name per second per connection but is not the intended timestamp. The buffer is 256 bytes, and the target path buffer is 236 bytes — a divergence that only matters if `ServerIndex` were ever multi-digit.

The caller truncates `Record[254]` and `Record[255]` before dispatch (`Code/DBSrv/CFileDB.cpp:567-568`), so the payload is at most 254 characters. `fopen(..., "wt")` truncates; if the same second and connection collide (two records in one second), the second overwrites the first. The function returns `FALSE` when `fopen` fails, and the caller logs `DBRecord Fail`. The `conn` used in the filename is the game-server connection index, not the player id.

**Rule workflow**:
```
time() -> localtime -> build name -> "../../Common/record<ServerIndex>/<name>"
fopen("wt") -> fprintf(text) -> fclose -> TRUE / FALSE
```

---

### Business Rule: R38 — Character-select projection

**Overview**:
`DBGetSelChar` copies, for all four slots, the name, the raw 16-item equip array, guild, score, coin and exp into `STRUCT_SELCHAR`, and rewrites the face item when it is one of 22, 23, 24, 25 or 32.

**Detailed description**:
The face remap rule is `if (Equip[i][0].sIndex is one of {22,23,24,25,32}) Equip[i][0].sIndex = (ClassMaster == MORTAL) ? 21 : mobExtra[i].MortalFace + 7` (`Code/DBSrv/CFileDB.cpp:2618-2619`). This is the display-side counterpart of the arch-creation rule that stores `MortalFace + 5 + cls` (R20) and the login-side projection that uses `MortalFace + 7` for non-mortal characters. The five hard-coded item indices are legacy default faces that must be replaced by the computed one so the character-select screen shows the right portrait for arch/celestial bodies.

The position copy contains a defect: `sel->SPX[i] = file->Char[i].SPX; sel->SPX[i] = file->Char[i].SPY;` (`:2623-2624`) assigns to `SPX` twice, so `SPY` is never populated and the outgoing `SPX` holds the Y coordinate. The client therefore receives the Y value in both position fields for every slot. `Coin[i]` and `Exp[i]` are copied from the mob (not from the account-level `Coin`, which is a separate field sent alongside), so the per-character coin shown on the select screen can differ from the account coin.

`sel->Equip` receives the full `STRUCT_ITEM[16]` array by `memcpy` before the single field is rewritten, so the projection leaks the character's full equipment (including effect bytes) to the select screen. There is no filtering of invalid indices, no zeroing of unused slots, and no bound check on `MOB_PER_ACCOUNT` (which equals the 4 hard-coded array sizes of `STRUCT_SELCHAR`, so it is safe by construction).

**Rule workflow**:
```
for i in 0..3: copy Name, Equip[16], Guild, SPX(<- SPX then SPY), Score, Coin, Exp
               if Equip[0] in {22,23,24,25,32} -> Equip[0] = mortal ? 21 : MortalFace + 7
```

---

### Business Rule: R39 — Account lookup by character name

**Overview**:
`GetAccountByChar(acc, cha)` maps a character name to its owning account by reading the first 16 bytes of `./char/<key>/<NAME>`; on any failure the output buffer is left untouched.

**Detailed description**:
The function uppercases the name, applies the reserved-pattern check, derives the shard from the character name, opens the index file read-only and copies `ACCOUNTNAME_LENGTH` bytes into `acc` (`Code/DBSrv/CFileDB.cpp:2633-2664`). It never verifies the read length: a zero-length or truncated index file leaves `acc` as whatever the caller passed in. The NP handler that calls it pre-uppercases both fields and then tests `m->Account[0] == 0` to detect failure (`Code/DBSrv/Server.cpp:1745-1758`), so it relies on the caller having zeroed the buffer — which it does via `_strupr` on a message field, not explicitly.

The function never writes, never logs failure and never distinguishes "file missing" from "read error". Because the index file content is the account name as it existed at creation time, an account subsequently renamed (R31 changes only the password; nothing renames accounts) would still resolve. The reverse mapping is used by the NP tool's "find account by character" operation and by the transfer reply path in the host.

**Rule workflow**:
```
_strupr(name) -> reserved check -> "./char/<first-key>/<NAME>"
_open(O_RDONLY) fail -> return (acc unchanged)
_read(acc, 16) -> _close -> return
```

---

## 4. Component Structure

```
Code/DBSrv/
├── CFileDB.h                       # STRUCT_ACCOUNTLIST + CFileDB class contract (89 lines)
├── CFileDB.cpp                     # 2688 lines: store, cache, dispatcher, capsule, ranking feed
├── Server.cpp                      # Host: Win32 loop, socket dispatch, NP tool ops, Disable/EnableAccount,
│                                   #   ReadConfig/WriteConfig (LastCapsule + Sapphire), DayLog scanners
├── Server.h                        # Host declarations incl. extern CFileDB cFileDB
├── CUser.h / CUser.cpp             # Per-connection slot (Mode, Count, Level, DisableID, Year/YearDay)
├── CReadFiles.h / .cpp             # Bulk import/export: ImportItem, ImportAccounts (calls AddAccount),
│                                   #   WriteGuildInfo (called from _MSG_GuildInfo), capsule item delivery
├── CRanking.h / .cpp               # RankingSystem/GrindRanking; scans ./account for boot-time ranking
├── DBSrv.vcxproj                   # Win32/x86, v143, /W3, _CRT_SECURE_NO_WARNINGS
└── stdafx.h, resource.h, DBSrv.rc  # Win32 shell

Code/
├── Basedef.h                       # STRUCT_ACCOUNTFILE / ACCOUNTINFO / MOB / MOBEXTRA / CAPSULE /
│                                   #   SELCHAR layouts; all MSG_* contracts; MAX_DBACCOUNT, MOB_PER_ACCOUNT
├── Basedef.cpp                     # BASE_GetFirstKey (sharding), BASE_ClearMob, BASE_ClearMobExtra,
│                                   #   BASE_CanCargo, GetItemPointer
└── CPSock.cpp / .h                 # Framing, obfuscation, buffered send used by every reply
```

Internal organisation of `CFileDB.cpp` (by line range):

| Range | Contents |
|-------|----------|
| 1-70 | Includes, externs, global guild arrays, `LastCapsule` |
| 57-76 | Constructor (cache init), destructor |
| 78-208 | `AddAccount`, `UpdateAccount` (NP-facing account provisioning) |
| 210-234 | `ProcessRecord` (text archive) |
| 236-2106 | `ProcessMessage` — 24 message cases |
| 2108-2200 | Reply helpers (`SendDBSignal` x3 variants, `SendDBMessage`, `SendGuildInfo`) |
| 2202-2234 | Cache add/remove |
| 2236-2326 | `CreateCharacter` / `DeleteCharacter` (index files) |
| 2328-2367 | Index arithmetic, name lookup, MAC counting |
| 2369-2388 | `SendDBSavingQuit` |
| 2390-2577 | `DBWriteAccount`, `DBExportAccount`, `DBReadAccount` |
| 2579-2669 | `DBCheckImpleName` (unused), `DBGetSelChar`, `GetAccountByChar`, `InitAccountList` (unused) |
| 2671-2688 | `GetEncPassword` (degenerate), `SetEncPassword` (empty) |

Message cases handled by `ProcessMessage` (in source order):

| Line | Message | Role |
|------|---------|------|
| 243 | `_MSG_ReqTransper` | Character transfer to NPTool |
| 357 | `_MSG_GuildZoneReport` | Guild zone ownership matrix fan-out |
| 388 | `_MSG_War` | Guild war state |
| 420 | `_MSG_GuildAlly` | Guild ally state |
| 454 | `_MSG_GuildInfo` | Guild info fan-out + persistence |
| 483 | `_MSG_DBUpdateSapphire` | Sapphire scaling + persistence |
| 510 | `_MSG_DBNewAccount` | Account creation (overwrite semantics) |
| 563 | `_MSG_MessageDBRecord` | Text archive |
| 579 | `_MSG_NPAppeal` | Appeal relay to admins |
| 592 | `_MSG_MessageDBImple` | System message relay |
| 611 | `_MSG_DBAccountLogin` | Account login (core) |
| 856 | `_MSG_DBCreateCharacter` | Mortal character creation |
| 1028 | `_MSG_DBCharacterLogin` | Character login |
| 1096 | `_MSG_DBNoNeedSave` | Eviction without save |
| 1124 | `_MSG_DBSaveMob` | Snapshot save |
| 1191 | `_MSG_SavingQuit` | Durable logout |
| 1276 | `_MSG_DBDeleteCharacter` | Character deletion |
| 1357 | `_MSG_AccountSecure` | Numeric token |
| 1423 | `_MSG_DBCreateArchCharacter` | Arch character creation |
| 1591 | `_MSG_MagicTrumpet` | Broadcast relay |
| 1612 | `_MSG_DBNotice` | Notice relay |
| 1624 | `_MSG_DBCapsuleInfo` | Capsule inspection |
| 1684 | `_MSG_DBPutInCapsule` | Seal character |
| 1768 | `_MSG_DBOutCapsule` | Restore character |
| 1929 | `_MSG_DBServerChange` | Cross-server token issuance |
| 1990 | `_MSG_UpdateExpRanking` | Ranking insert/raise |
| 2044 | `_MSG_DBItemDayLog` | Item counter |
| 2060 | `_MSG_DBActivatePinCode` | No-op (body commented out) |
| 2068 | `_MSG_DBPrimaryAccount` | Primary-account broadcast + client message |

On-disk artefacts owned by the component:

```
<DBSrv working dir>/
├── account/<A-Z|Hangul|etc>/<UPPERCASED_NAME>   fixed-size STRUCT_ACCOUNTFILE (~7.7 KB)
├── char/<A-Z|Hangul|etc>/<UPPERCASED_NAME>      16-byte account-name token
├── capsule/<index>                              STRUCT_CAPSULE (MOB + MOBEXTRA)
├── Config.txt                                   "Sapphire <n>" + "LastCapsule <n>"
└── ../../Common/
    ├── ImportItem/<random>                      "<account> 3443 59 <hi> 59 <lo> 0 0"
    └── record<ServerIndex>/RC*.txt              server text archives
S:/export/account<ServerIndex>/<NAME>            mirror of STRUCT_ACCOUNTFILE
```

---

## 5. Dependency Analysis

### Internal dependencies (within the project)

```
ProcessClientMessage (Server.cpp:1339)
   └─> CFileDB::ProcessMessage (CFileDB.cpp:236)
         ├─> CFileDB::GetIndex x2 (2328, 2335)
         ├─> CFileDB::DBReadAccount (2518)  ──> BASE_GetFirstKey (Basedef.cpp:4114)
         ├─> CFileDB::DBWriteAccount (2390) ──> BASE_GetFirstKey
         ├─> CFileDB::DBExportAccount (2471)
         ├─> CFileDB::AddAccountList / RemoveAccountList (2202, 2221) ──> CUser::Count
         ├─> CFileDB::CreateCharacter / DeleteCharacter (2236, 2303)
         ├─> CFileDB::DBGetSelChar (2611)
         ├─> CFileDB::GetAccountsByMac (2351)
         ├─> CFileDB::SendDBSavingQuit (2369) ──> CPSock::SendOneMessage
         ├─> CFileDB::SendDBSignal* (2108-2168) ──> CPSock::SendOneMessage
         ├─> BASE_ClearMob / BASE_ClearMobExtra / GetItemPointer (Basedef)
         ├─> DisableAccount (Server.cpp:1359)  [host callback, same module]
         ├─> CReadFiles::WriteGuildInfo (CReadFiles.cpp) [static]
         ├─> rankingSystem.grindRanking / sendUpdateIndividualRank / tryInsertInRanking
         └─> Log, ProcessRecord, WriteConfig [host]
CReadFiles::ImportItems (CReadFiles.cpp:235-334)
   ├─> CFileDB::GetIndex / DBReadAccount / DBWriteAccount / pAccountList
   └─> MSG_DBSendItem to pUser[svr]
CReadFiles::ImportAccounts (CReadFiles.cpp:677-680)
   └─> CFileDB::AddAccount
CReadFiles (@ ~1015)
   └─> CFileDB::UpdateAccount
DayLog_ReadAccountInDir (Server.cpp:2202) ──> reads ./account/** directly (bypasses the class)
RankingSystem::readAccountsInDir (CRanking.cpp:140-215) ──> reads ./account/** directly
```

### External dependencies (libraries and OS services)

| Dependency | Kind | Use in this component | Evidence |
|---|---|---|---|
| MSVCRT/UCRT (`io.h`, `fcntl.h`, `sys/stat.h`) | C runtime | `_open`, `_read`, `_write`, `_lseek`, `_close`, `_filelength` | `Code/DBSrv/CFileDB.cpp:99`, `:2444`, `:2558` |
| Win32 file API (`fileapi.h`) | OS | `DeleteFileA` for index and capsule removal | `:1915`, `:2323` |
| Win32 `FindFirstFile`/`FindNextFile` | OS | Bulk account scanning (host side) | `Code/DBSrv/Server.cpp:2210`, `Code/DBSrv/CRanking.cpp:132` |
| CRT string/format | C runtime | `strncpy`, `_strupr`, `sprintf`, `sscanf`, `strcmp`, `memcmp` | throughout |
| CRT time | C runtime | `time`, `localtime` for records | `:214-219` |
| CRT rand | C runtime | Token generation, import job filenames | `:2673-2680`, `:1761` |
| `CPSock` | Internal transport | `SendOneMessage` for every reply | `Code/CPSock.cpp:686` |
| Winsock | OS | Reached indirectly through `CPSock` | `Code/DBSrv/Server.cpp:520` |
| `rankingSystem` | Internal service | Login feed and exp updates | `Code/DBSrv/CRanking.h:88` |
| `CReadFiles` | Internal utility | Guild info persistence, import jobs | `Code/DBSrv/CReadFiles.cpp` |

No third-party libraries, no database driver, no serialization framework and no crypto library are used by this component.

---

## 6. Afferent and Efferent Coupling

Counting basis: static textual references in the workspace (`Code/**/*.cpp|*.h`, excluding the ignored folders), treating each distinct `cFileDB.<member>` or `pAccountList` access site as one coupling edge. Counts are approximate but reproducible.

| Component (C++ class / struct / module) | Afferent (Ca) | Efferent (Ce) | Instability I = Ce/(Ca+Ce) | Critical |
|---|---|---|---|---|
| `CFileDB` (class) | 43 call sites (Server.cpp 31, CReadFiles.cpp 11, headers 1) | ~20 external symbols + 13 internal collaborators | 0.32 | High |
| `STRUCT_ACCOUNTFILE` (record) | 9 direct file readers/writers (CFileDB x3, Server x3, CReadFiles x2, CRanking x1) | 6 nested types | 0.40 | High |
| `STRUCT_ACCOUNTLIST` (cache element) | 2 (`CFileDB`, `CReadFiles` via `pAccountList`) | 1 (`STRUCT_ACCOUNTFILE`) | 0.33 | High |
| `STRUCT_SELCHAR` (projection) | 7 (login, create, delete, capsule, transfer, client) | 4 (`STRUCT_SCORE`, `STRUCT_ITEM`, ...) | 0.36 | Medium |
| `CFileDB::ProcessMessage` (dispatcher) | 1 (`ProcessClientMessage`) | 29 message handlers | 0.96 | High |
| `DBWriteAccount` / `DBReadAccount` (primitives) | 16 call sites | 3 (`BASE_GetFirstKey`, CRT I/O, `Log`) | 0.16 | High |
| `DBExportAccount` | 2 (`_MSG_DBSaveMob`, `_MSG_SavingQuit`) | 2 | 0.50 | Low |
| `CreateCharacter` / `DeleteCharacter` | 4 (`_MSG_DBCreateCharacter`, `_MSG_DBDeleteCharacter`, `_MSG_DBOutCapsule`, host NP path) | 2 | 0.33 | Medium |
| `AddAccount` / `UpdateAccount` | 2 (both `CReadFiles`) | 4 | 0.67 | Medium |
| `GetIndex` (both overloads) | 18 call sites | 1 | 0.05 | High |
| `AddAccountList` / `RemoveAccountList` | 6 | 2 (`CUser::Count`, `Log`) | 0.25 | High |
| `DBGetSelChar` | 5 | 3 | 0.38 | Low |
| `GetAccountsByMac` | 1 | 1 | 0.50 | Low |
| `DBCheckImpleName`, `InitAccountList`, `SendDBMessage` | 0 | 0-1 | n/a | None (dead) |
| `CUser` (DBSrv session slot) | 20+ (`Count`, `Mode`, `cSock`, `Level`, `DisableID`, `Year/YearDay`) | 2 (`CPSock`, `Basedef`) | 0.09 | High |
| `RankingSystem` | 2 in this component | 7 | 0.78 | Medium |
| `CReadFiles` | 1 in this component (`WriteGuildInfo`) | 5 | 0.83 | Low |

Observations:

- `CFileDB` is the highest-afferent class in DBSrv after `CUser`, and it is simultaneously the highest-efferent business class: it is a hub, not a leaf. Its instability of ~0.32 combined with ~20 efferent symbols makes it the most change-sensitive type in the subsystem.
- `GetIndex(char*)` and the primitives `DBWriteAccount`/`DBReadAccount` are the true load-bearing seams: 34 call sites depend on their exact (and in the index case, ambiguous) semantics.
- The three message handlers with `Ca == 0` (`DBCheckImpleName`, `InitAccountList`, `SendDBMessage`) are dead code and are declared in the class contract anyway, which inflates the perceived public surface.

---

## 7. Endpoints

This component exposes no REST/GraphQL/gRPC surface. Its endpoints are binary message types on the DBSrv TCP listener (`DB_PORT 7514`, `Code/Basedef.h:45`) for game servers and the admin listener (`ADMIN_PORT 8895`, `:46`) for the NP tool. Types are guarded by direction flags (`FLAG_GAME2DB`, `FLAG_DB2GAME`, `FLAG_NP2DB`) and by `MSG_STANDARD.Type/ID/Size/KeyWord`.

Transport framing: `MSG_STANDARD` header, obfuscated payload with a checksum (`Code/CPSock.cpp:440-467`), asynchronous Win32 socket notifications.

| Message Type | Direction | ID | Request payload | Response(s) | Handler |
|---|---|---|---|---|---|
| `_MSG_DBAccountLogin` (1\|GAME2DB) | TMSrv -> DB | player id | `AccountPassword[16]`, `AccountName[16]`, `Zero[52]`, `ClientVersion`, `DBNeedSave`, `AdapterName[4]` | `_MSG_DBCNFAccountLogin`, `_MSG_DBAccountLoginFail_Account`, `_MSG_DBAccountLoginFail_Pass`, `_MSG_DBAccountLoginFail_Block`, `_MSG_DBAlreadyPlaying`, `_MSG_DBStillPlaying` + `_MSG_DBSavingQuit`, `_MSG_DBCheckPrimaryAccount` | `CFileDB.cpp:611` |
| `_MSG_DBCharacterLogin` (19\|?) | TMSrv -> DB | player id | `Slot` | `_MSG_DBCNFCharacterLogin` | `CFileDB.cpp:1028` |
| `_MSG_DBCreateCharacter` (2\|GAME2DB) | TMSrv -> DB | player id | `Slot`, `MobName[16]`, `MobClass` | `_MSG_DBCNFNewCharacter`, `_MSG_DBNewCharacterFail` | `CFileDB.cpp:856` |
| `_MSG_DBCreateArchCharacter` (31\|GAME2DB) | TMSrv -> DB | player id | `MobName[16]`, `MobClass`, `MortalFace`, `MortalSlot` | `_MSG_DBCNFArchCharacterSucess`, `_MSG_DBCNFArchCharacterFail`, `_MSG_DBNewCharacterFail`, `_MSG_DBCNFNewCharacter` | `CFileDB.cpp:1423` |
| `_MSG_DBDeleteCharacter` (9\|GAME2DB) | TMSrv -> DB | player id | `Slot`, `MobName[16]`, `Password[16]` | `_MSG_DBCNFDeleteCharacter`, `_MSG_DBDeleteCharacterFail` | `CFileDB.cpp:1276` |
| `_MSG_DBSaveMob` (7\|GAME2DB) | TMSrv -> DB | player id | `Slot`, `MOB`, `Cargo[128]`, `Coin`, `ShortSkill[16]`, `AccountName[16]`, `Export`, `extra`, `affect[32]`, `Donate` | none | `CFileDB.cpp:1124` |
| `_MSG_SavingQuit` (6\|GAME2DB) | TMSrv -> DB | player id | `Slot`, `MOB`, `Cargo[128]`, `Coin`, `ShortSkill[16]`, `AccountName[16]`, `extra`, `affect[32]`, `Donate` | `_MSG_DBCNFAccountLogOut` | `CFileDB.cpp:1191` |
| `_MSG_DBNoNeedSave` (5\|GAME2DB) | TMSrv -> DB | player id | header only | none | `CFileDB.cpp:1096` |
| `_MSG_DBNewAccount` (1\|GAME2DB) | TMSrv -> DB | player id | `STRUCT_ACCOUNTINFO`, `hue` | `_MSG_DBCNFAccountLogin`, `_MSG_DBNewAccountFail` | `CFileDB.cpp:510` |
| `_MSG_AccountSecure` (222\|all) | Client/Game -> DB | player id | `NumericToken[6]`, `Unknown[10]`, `ChangeNumeric` | `_MSG_AccountSecure`, `_MSG_AccountSecureFail` | `CFileDB.cpp:1357` |
| `_MSG_DBCapsuleInfo` (60\|both) | Game <-> DB | player id | `Parm` = capsule index | `_MSG_CNFDBCapsuleInfo` | `CFileDB.cpp:1624` |
| `_MSG_DBPutInCapsule` (61\|both) | Game <-> DB | player id | `Parm` = slot | `_MSG_DBCNFCapsuleSucess`, `MSG_CNFDeleteCharacter` | `CFileDB.cpp:1684` |
| `_MSG_DBOutCapsule` (62\|both) | Game <-> DB | player id | `Slot`, `SourType`, `SourPos`, `DestType`, `DestPos`, `GridX/Y`, `WarpID`, `MobName[16]` | `_MSG_DBCNFCapsuleCharacterFail`, `_MSG_DBCNFCapsuleCharacterFail2`, `MSG_CNFNewCharacter`, `_MSG_DBNewCharacterFail` | `CFileDB.cpp:1768` |
| `_MSG_DBServerChange` (20\|GAME2DB) | TMSrv -> DB | player id | `NewServerID`, `Slot` | `_MSG_DBCNFServerChange`, `_MSG_DBServerSend1` | `CFileDB.cpp:1929` |
| `_MSG_ReqTransper` (170\|all) | Client/Game/NP -> DB | player id | `Slot`, `OldName[16]`, `NewName[16]` | `_MSG_ReqTransper` (Result 0/4), `MSG_NPCreateCharacter` to NPTool | `CFileDB.cpp:243` |
| `_MSG_UpdateExpRanking` | TMSrv -> DB | player id | `RankInfo{Name, Value, ClassMaster, Level, Class}` | `_MSG_UpdateIndividualRank`, range updates | `CFileDB.cpp:1990` |
| `_MSG_DBItemDayLog` (21\|GAME2DB) | TMSrv -> DB | player id | `Parm` = item id | none | `CFileDB.cpp:2044` |
| `_MSG_DBPrimaryAccount` (23\|GAME2DB) | TMSrv -> DB | player id | `Mac[4]`, `IP` | `_MSG_DBCheckPrimaryAccount` to all servers, `_MSG_DBClientMessage` | `CFileDB.cpp:2068` |
| `_MSG_GuildZoneReport` (13\|GAME2DB) | TMSrv -> DB | server id | `Guild[5]` | `_MSG_GuildReport` to all servers | `CFileDB.cpp:357` |
| `_MSG_War` (14\|all) | Client/Game -> DB | - | `Parm1` = guild, `Parm2` = day | `_MSG_War` signal to all servers | `CFileDB.cpp:388` |
| `_MSG_GuildAlly` (18\|all) | Client/Game -> DB | - | `Guild`, `Ally` | `_MSG_GuildAlly` signal to all servers | `CFileDB.cpp:420` |
| `_MSG_GuildInfo` (19\|all) | Client/Game -> DB | - | `Guild`, `STRUCT_GUILDINFO` | `_MSG_GuildInfo` to all servers | `CFileDB.cpp:454` |
| `_MSG_DBUpdateSapphire` (10\|GAME2DB) | TMSrv -> DB | - | `Parm` (1 = double) | `_MSG_DBSetIndex` to all servers | `CFileDB.cpp:483` |
| `_MSG_MessageDBRecord` (12\|GAME2DB) | TMSrv -> DB | - | `Record[256]` | none (file archive) | `CFileDB.cpp:563` |
| `_MSG_MessageDBImple` (11\|both) | TMSrv <-> DB | - | `Level`, `String[96]` | relay to all servers | `CFileDB.cpp:592` |
| `_MSG_NPAppeal` | TMSrv -> DB | - | header | relay to all admins | `CFileDB.cpp:579` |
| `_MSG_MagicTrumpet` (29\|all) | TMSrv -> DB | - | `String[96]` | relay to all servers + admins | `CFileDB.cpp:1591` |
| `_MSG_DBNotice` (30\|all) | TMSrv -> DB | - | `String[96]` | relay to all servers | `CFileDB.cpp:1612` |
| `_MSG_DBActivatePinCode` (22\|GAME2DB) | TMSrv -> DB | player id | `PinCode`, `AccountName`, `MobName`, `Mac[4]`, `IP` | none (no-op body) | `CFileDB.cpp:2060` |

Non-message entry points (called in-process, not over the wire): `AddAccount`, `UpdateAccount`, `DBReadAccount`, `DBWriteAccount`, `DBExportAccount`, `CreateCharacter`, `DeleteCharacter`, `GetIndex`, `GetAccountByChar`, `GetAccountsByMac`, `DBCheckImpleName`, `InitAccountList`, `SendDBSignal*`, `SendDBMessage`, `SendGuildInfo`, `SendDBSavingQuit`, `DBGetSelChar`, `GetEncPassword`, `SetEncPassword`, and the public `pAccountList` array.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| TMSrv game servers (`pUser[0..9]`) | Internal service | Login, save, quit, character CRUD, capsule, guild, ranking | TCP/`CPSock` on `DB_PORT 7514` | Binary `MSG_*` structs, obfuscated, checksummed | Return codes logged and dropped; failure signals only on validation, not on persistence |
| NPTool admin clients (`pAdmin[0..9]`, `AdminSocket`) | Internal service | Account provisioning, character lookup, ban/unban, notices | TCP/`CPSock` on `ADMIN_PORT 8895` | Binary `MSG_NP*` structs | Sends `_MSG_NPNotFound` / `_MSG_NPState`; password-length and level checks |
| NPTool relay (`AdminClient`) | External service | Character transfer handshake | TCP outbound, `redirect.txt` host/port/credentials | `MSG_NPCreateCharacter` / `_MSG_NPCreateCharacter_Reply` | `AdminClient.Sock == 0` disables the feature; results 0-4 mapped to client results |
| Account files (`./account/**`) | Local filesystem | Primary account and character storage | POSIX-emulated `_open/_read/_write/_lseek` | `STRUCT_ACCOUNTFILE` fixed binary (~7.7 KB) | Return code only; most callers ignore it; no atomicity |
| Character index files (`./char/**`) | Local filesystem | Global character-name uniqueness and reverse lookup | `_open` + `fopen` | 16-byte account name | `errno`-specific logs; `FALSE` on failure |
| Capsule files (`./capsule/<n>`) | Local filesystem | Sealed character storage | `_open/_read/_write` | `STRUCT_CAPSULE` (MOB + MOBEXTRA) | `errno` logs; index derived from item effects |
| `Config.txt` | Local filesystem | Sapphire scale and `LastCapsule` counter | `fopen("rt"/"wt")` | Text, two lines | Missing file aborts boot with a message box; truncating rewrite |
| `../../Common/ImportItem/*` | Local filesystem | Capsule-item delivery jobs and generic item grants | `fopen("a+")` + `fprintf` | Space-separated text line | `fopen` result not checked before `fprintf` |
| `../../Common/record<ServerIndex>/*.txt` | Local filesystem | Server-submitted text archive | `fopen("wt")` | Text | `NULL` check returns `FALSE`; caller logs |
| `S:/export/account<ServerIndex>/*` | Local filesystem | Record mirror | `_open/_write` | `STRUCT_ACCOUNTFILE` | Silent `FALSE`; no caller reacts |
| `./BaseMob/{TK,FM,BM,HT}` | Local filesystem | Class templates for character creation | `_open/_read` | `STRUCT_MOB` | Boot aborts with a message box if a file is missing |
| `RankingSystem` | Internal service | Boot-time ranking load and live updates | In-process | `STRUCT_RANKING` | Owner of the ranking buffers; not error-checked here |
| `CReadFiles` | Internal service | Guild info persistence, item delivery, bulk import | In-process | Mixed | Mixed |
| `Log` / `StartLog` | Internal service | Rotating text log to `fLogFile` + window title | In-process | Formatted text | `fLogFile` NULL-checked |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / Note |
|---|---|---|---|
| Global singleton service | `CFileDB cFileDB;` with `extern` in the host header | `Code/DBSrv/Server.cpp:89`, `Server.h:113` | One instance per process; all state and behaviour hangs off it |
| Message dispatcher / switch router | `switch(std->Type)` over 29 types, single function | `Code/DBSrv/CFileDB.cpp:240-2103` | Central inbound routing; 1867-line function |
| Command message (GoF) as data-only structs | `MSG_*` structs reinterpret-cast from a raw buffer | `Code/Basedef.h:983-1437` | Type is a flag-tagged short; payload arrived by reinterpret cast |
| Cache-aside with write-through | `pAccountList` populated on login, written through on save | `Code/DBSrv/CFileDB.cpp:745-756`, `:1173-1187` | No read-through on demand; every read re-reads disk |
| Direct-mapped cache / address space | `conn * MAX_USER + id` | `Code/DBSrv/CFileDB.cpp:2328-2333` | Index is an address, not a hash; no collision handling needed by construction |
| Session registry (stateful slot ownership) | `Login`, `Slot`, `SecurePass`, `Mac[4]` per entry | `Code/DBSrv/CFileDB.h:25-33` | Conflates cache metadata with protocol state |
| Sentinel values instead of option types | index 0 = not found; `Slot = -1` = none; `SecurePass = -1` = unverified; `NumericToken[0] = -1` = unset; `MobName[0] == 0` = free slot; level 1000 = admin marker; level 219 = delete cap | multiple | Pervasive; the root cause of several ambiguity defects |
| Fixed-layout serialization (struct dump) | `_read/_write` of `sizeof(struct)` | `Code/DBSrv/CFileDB.cpp:2444`, `:2562` | No versioning, no endianness handling, no packing pragma on the record |
| Sharding by first key | `BASE_GetFirstKey` directory fan-out | `Code/Basedef.cpp:4114-4160` | Bounds directory size; also encodes a DOS/Hangul compatibility quirk |
| Two-phase reference + content storage | `./char/<name>` token + `Char[4]` record | `Code/DBSrv/CFileDB.cpp:2236` vs `:964-999` | Two writes that can diverge |
| Template method / prototype | `memcpy(mob, &g_pBaseSet[cls], sizeof(STRUCT_MOB))` | `Code/DBSrv/CFileDB.cpp:973-983`, `:1530-1540` | Base mob files act as new-character prototypes |
| Fan-out broadcast | loop over `pUser[0..MAX_SERVER)` with socket liveness test | `CFileDB.cpp:377-384`, `:409-416`, `:443-450`, `:468-478`, `:497-504`, `:601-608`, `:1593-1609`, `:1614-1621`, `:794-800`, `:2084-2090` | Ten copies of the same loop with three different liveness predicates |
| Reply-builder helpers | `SendDBSignal`, `...Parm`, `...Parm2`, `...Parm3`, `SendDBMessage`, `SendGuildInfo` | `Code/DBSrv/CFileDB.cpp:2108-2200` | Thin wrappers over `CPSock::SendOneMessage`, all returning `TRUE` unconditionally |
| Outbox / append-only job queue (filesystem) | `../../Common/ImportItem/*` lines consumed by `CReadFiles` | `Code/DBSrv/CFileDB.cpp:1761-1765`, `CReadFiles.cpp:180-340` | Asynchronous work hand-off via files, no acknowledgement |
| External sidecar for irreversible operations | `AdminClient` transfer handshake | `Code/DBSrv/CFileDB.cpp:340-352`, `Server.cpp:626-741` | Keeps the "delete original character" decision outside DBSrv |
| Monitor-free single-threaded actor | Win32 message pump, no locks anywhere | `Code/DBSrv/Server.cpp:589-623` | All handlers run on the UI thread; I/O blocks the loop |

Architectural decisions worth recording (observed, not recommended):

- **State and storage are one object.** `CFileDB` is simultaneously the file-format owner, the cache, the authentication authority, the protocol dispatcher and the guild/Sapphire state holder. Coupling metrics reflect this: high afferent and high efferent on the same class.
- **The database is a directory tree.** There is no index file, no journal, no schema version. Backup, restore, migration and corruption repair are all filesystem operations performed outside the code.
- **Durability is delegated to close().** No flush, no rename, no journal; the only recovery mechanism in the code is the 8-byte `ShortSkill` shim in `DBReadAccount`.
- **Failures are logged, not propagated.** Of 16 save sites, 12 discard `DBWriteAccount`'s result. The protocol has failure signals for validation errors and none for persistence errors.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|---|---|---|---|
| High | `DBWriteAccount` (`CFileDB.cpp:2390-2469`) | In-place whole-record overwrite; no temp+rename, no flush, no backup, no checksum; short writes undetected (`ret == -1` only) | Torn/lost account records on crash or power loss; silent corruption accepted on next read |
| High | Index semantics (`CFileDB.cpp:2348`, `2335-2349`) | `GetIndex(char*)` returns 0 for both "not found" and the (server 0, player 0) record; used directly as an array index at 9 sites | Operations applied to an unrelated account; silent eviction of slot 0 |
| High | Dispatcher bound (`Server.cpp:1343`) | `std->ID > MAX_USER` allows `ID == MAX_USER`, aliasing the next connection's slot 0 (`conn*1000 + 1000`) | Cross-account write in the cache; `_MSG_ReqTransper` uses the correct `>=` |
| High | Cache eviction (`CFileDB.cpp:2221-2234`, `Server.cpp:890-900`) | No eviction or reconciliation when a TMSrv disconnects; entries stay `Login = TRUE` forever | After a game-server crash, affected accounts are permanently "already playing" until DBSrv restarts |
| High | Capsule restore (`CFileDB.cpp:1876-1908`) | `DBWriteAccount` is called before the restored mob/extra are copied into the cache; the character is RAM-only afterwards | Restored character lost on the next crash; capsule file already deleted |
| High | `DBReadAccount` callers (`Server.cpp:1366-1369`, `1426-1429`) | Uninitialised `STRUCT_ACCOUNTFILE`; short legacy file leaves indeterminate tail that `DBWriteAccount` then persists | Stack garbage written into account records |
| High | Account overwrite via `_MSG_DBNewAccount` (`CFileDB.cpp:510-561`) | No existence probe, unlike `AddAccount`; writes over an existing account with `Info` replaced and the rest zeroed | Destructive account re-creation by a game server |
| High | Password storage and comparison (`CFileDB.cpp:677-682`, `113`) | Plaintext, unsalted; `strncmp`/`strcmp` stop at NUL; `strncpy` leaves 16-byte passwords unterminated | Prefix matching of short passwords; no audit trail; trivially readable by anyone with file access |
| High | Authentication ordering (`CFileDB.cpp:619-682`) | Distinct `Fail_Account` / `Fail_Pass` / `Fail_Block` signals before and after the password test | Account enumeration and block-state oracle |
| High | `_MSG_DBServerChange` (`CFileDB.cpp:1929-1988`) | `TempKey` is only written to RAM; no `DBWriteAccount`; `SetEncPassword` empty; `GetEncPassword` always `FALSE` | Server-change token path is effectively non-functional; mismatch branch silently wipes the token and sends no reply |
| Medium | Date block (`CFileDB.cpp:655`) | `Year >= when.tm_year \|\| (Year >= when.tm_year && YearDay >= when.tm_yday)` reduces to `Year >= current_year` | Day-of-year component ignored; bans cannot expire within the same year |
| Medium | `SecurePass` gate coverage (`CFileDB.cpp:882`, `1043`, `1293` vs `1423`) | Gates create/login/delete but not save, capsule, or arch creation; resets to -1 each login | Secondary token is bypassable through the arch and capsule paths |
| Medium | Character deletion (`CFileDB.cpp:1331-1343`) | Index file deleted before the record is rewritten; both results unchecked; level 219 and class-master rules bypassed by capsule sealing | Name reservation and record state can diverge; high-level characters can be sealed instead of deleted |
| Medium | `DBGetSelChar` (`CFileDB.cpp:2623-2624`) | `sel->SPX[i]` assigned twice; `SPY` never populated | Character-select screen receives the Y coordinate in both axes |
| Medium | Capsule counter (`CFileDB.cpp:1698`, `Server.cpp:412-425`) | 16-bit counter persisted by truncating `Config.txt`; crash loses the counter and regresses to 0 | Next seal overwrites `./capsule/1` and destroys a previously sealed character |
| Medium | Capsule put-in (`CFileDB.cpp:1761-1765`) | `fopen("a+")` result not checked before `fprintf` | Null-pointer write if `../../Common/ImportItem` is missing |
| Medium | Capsule take-out item trust (`CFileDB.cpp:1806-1831`) | Capsule id taken from the item's effect bytes without validating the item is a capsule | Arbitrary capsule file selection if item state is manipulated; relies on file existence |
| Medium | `_MSG_ReqTransper` (`CFileDB.cpp:309-336`) | Rename applied to the cache before the old-name comparison; failure leaves the cache renamed | Rejected transfers are persisted by the next save; ranking/name mismatch |
| Medium | Transfer reply (`Server.cpp:692-719`) | Only `STRUCT_MOB` cleared, not `mobExtra[slot]`; `./char` index not removed | Residue in the record; old names permanently reserved |
| Medium | `DBCheckImpleName` (`CFileDB.cpp:2579-2609`) | No callers; bounds test uses `ilchat[j][i+4]` with a string offset | Configured banned-word list is not enforced at all |
| Medium | Error handling duplication (`CFileDB.cpp:2417-2428`, `2281-2292`, `1707-1718`) | `if/else` attaches only to the final `if`; earlier `errno` cases log twice (`EACCES` + `UNKNOWN`) | Noisy, misleading logs during I/O failures |
| Medium | `ProcessRecord` format (`CFileDB.cpp:219`) | `%d2.2d` consumes `tm_sec` as `%d` then emits `2.2d`; `%2d` takes `conn` | Filenames do not match the intended timestamp; collisions overwrite in the same second |
| Medium | `UpdateAccount` (`CFileDB.cpp:173-205`) | Cache patched only when `Idx != 0`; trailing block writes to a local after persistence; uninitialised `file` on short reads | Password change may not reach the live cache; dead code obscures intent |
| Medium | Arch creation (`CFileDB.cpp:1423-1589`) | No `./char` index reservation; no `SecurePass` check; `Equip[1..]`/`Carry[0..59]` wipe differs from restore (`MAX_EQUIP-1`/`MAX_CARRY-4` vs `MAX_EQUIP-2`/`MAX_CARRY-4`) | Duplicate character names possible at the record level |
| Medium | `AddAccount` (`CFileDB.cpp:91-137`) | Probe-then-create is racy; `file.Info.AccountName` is stored in its original case while the path is uppercased | Low-frequency duplicate creation; inconsistent stored casing |
| Medium | Ranking feed (`CFileDB.cpp:851`, `1091`) | `new STRUCT_RANKING(...)` per login, ownership not visible in this component | Potential per-login leak (contract ambiguity, see R35) |
| Low | `AddAccount` `memset(file.ShortSkill, -1, 8)` (`CFileDB.cpp:124`) | 8 bytes cleared of a 64-byte array | Slot 0 quick-skills differ from every other slot |
| Low | Duplicated reserved-name matcher (`CFileDB.cpp` x8) | Same 6-line test copied eight times; misses `CON`/`PRN`/`AUX`/`NUL`/`COM10` | DOS device names outside the covered set remain ambiguous |
| Low | `GetItemPointer` result (`CFileDB.cpp:1806`) | Return value used without a NULL check | Crash on an invalid source/destination descriptor |
| Low | Fan-out liveness predicates (`CFileDB.cpp` x10 loops) | Three different combinations of `Mode != USER_EMPTY` and `Sock != 0` | Messages silently skipped for partially torn-down connections |
| Low | Dead code (`CFileDB.cpp:2579`, `:2666`, `:2170`, `:2685`, `:2060`) | Three uncalled functions, one empty function, one no-op message body | Misleading public API; wasted analysis surface |
| Low | `DBExportAccount` (`CFileDB.cpp:2477`, `:2488`) | Hard-coded drive `S:`; `strncpy(..., 16)` instead of the constant; silent failure | Mirror absent on hosts without `S:`; undetected drift |
| Low | Logging (`CFileDB.cpp` throughout) | Single mutex-free `fprintf` plus `SetWindowText` per event; mixed Portuguese/English messages; account names copied into `sprintf` buffers of fixed size | Log is a UI-coupled bottleneck; inconsistent operability |
| Low | Memory footprint (`CFileDB.h:40`) | ~10000 x ~7.7 KB static array (~77 MB) always resident | Large fixed RSS; no way to shrink at runtime |

---

## 11. Test Coverage Analysis

**Result: no test coverage exists for this component, and no test infrastructure exists in the project.**

Evidence (exhaustive search across the workspace, excluding the ignored folders `.agents`, `.codegraph`, `.opencode`, `openspec`):

| Check | Command / method | Result |
|---|---|---|
| Solution projects | `.sln` `Project(` entries | Only `DBSrv` and `TMSrv` (`W2PP Code Project.sln:6`, `:8`), plus a `Solution Items` folder. No test project |
| File names containing test/spec/mock/fixture | recursive name match `(?i)test\|spec\|mock\|fixture` | No matches (the only hit is the ignored `openspec` directory) |
| Test framework usage | grep for `gtest`, `catch2`, `doctest`, `TEST_CASE`, `TEST(`, `assert(`, `CTest` in `Code/**/*.cpp|*.h` | No matches |
| File inventory | extension census of the whole workspace | 80 `.cpp`, 24 `.h`, 2 `.vcxproj`, 2 `.rc`, 38 `.md`, plus build artefacts (`.obj`, `.tlog`, `.pdb`, `.ipch`). No test harness, no fixture data, no golden files |
| Inline test hooks | `#ifdef _PACKET_DEBUG` in `DBSrv.vcxproj:61` | Packet-level debug logging only; no assertions or verification |
| Documentation | `README.md`, `docs/reports/**` | No test strategy, no test plan, no coverage report |

Component-level coverage table (all zero, stated per unit of the class):

| Component / Unit | Unit Tests | Integration Tests | Coverage | Test Quality |
|---|---|---|---|---|
| `AddAccount` / `UpdateAccount` | 0 | 0 | None | Not exercised; the bonus-cargo and reserved-name branches are unverified |
| `ProcessMessage` (29 cases) | 0 | 0 | None | Only reachable by a live TMSrv; no protocol-level harness exists |
| `_MSG_DBAccountLogin` (authentication) | 0 | 0 | None | No test for password mismatch, block dates, TempKey, duplicate session, negative coin |
| `_MSG_DBCreateCharacter` / `_MSG_DBCreateArchCharacter` | 0 | 0 | None | No test for slot/class ranges, reserved names, `íí`, failed writes, duplicate names |
| `_MSG_DBCharacterLogin` | 0 | 0 | None | No test for the empty-`MobName` path or the `SecurePass` gate |
| `_MSG_DBSaveMob` | 0 | 0 | None | No test for the slot-mismatch/`Login == 0` guards |
| `_MSG_SavingQuit` | 0 | 0 | None | No test for the three rejection paths, the export ordering, or eviction |
| `_MSG_DBDeleteCharacter` | 0 | 0 | None | No test for password, class-master, level-219 or the index-file ordering |
| `_MSG_AccountSecure` (4-branch state machine) | 0 | 0 | None | No test for token binding, matching, change-without-verify |
| Capsule put-in / take-out / info | 0 | 0 | None | No test for counter wrap, missing files, the restore ordering defect, item consumption |
| `_MSG_DBServerChange` + `Get/SetEncPassword` | 0 | 0 | None | No test; both helpers are degenerate and untested |
| `_MSG_ReqTransper` + admin reply | 0 | 0 | None | No test for the three validations or the host reply path |
| `DBWriteAccount` / `DBReadAccount` / `DBExportAccount` | 0 | 0 | None | No test for open/seek/write failures, short reads, or the 8-byte `ShortSkill` shim |
| `CreateCharacter` / `DeleteCharacter` (index files) | 0 | 0 | None | No test for EEXIST, errno branches, or delete failure |
| `AddAccountList` / `RemoveAccountList` / `GetIndex` / `GetAccountsByMac` | 0 | 0 | None | No test for the double-add guard, the index-0 ambiguity, or MAC counting |
| `DBGetSelChar` | 0 | 0 | None | The SPX/SPY defect (R38) would be caught immediately by any assertion |
| `SendDBSignal*` / `SendGuildInfo` / `SendDBSavingQuit` | 0 | 0 | None | No test for payload layout or socket-absent behaviour |
| Guild / Sapphire / record / item-log handlers | 0 | 0 | None | No test for range validation, fan-out sets, or clamping |
| Ranking feed | 0 | 0 | None | No test for insert/evict/raise notification ranges |
| `DBCheckImpleName` / `InitAccountList` / `SendDBMessage` | 0 | 0 | None | Dead code; nothing to test until wired |

Consequences for confidence in this report: every business rule above was derived by reading the implementation and its callers, not by observing verified behaviour. Rules whose runtime effect depends on state outside the component carry explicit confidence notes (R9, R20, R26, R35). Any regression testing of this component today requires a manual live server run with a game client, which is the main reason several long-standing defects (index-0 aliasing, the capsule restore ordering, the SPX/SPY assignment) remain undetected.

