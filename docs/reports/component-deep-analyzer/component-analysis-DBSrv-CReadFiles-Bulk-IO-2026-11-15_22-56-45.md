# Component Deep Analysis Report - DBSrv CReadFiles Bulk I/O

| Attribute | Value |
|-----------|-------|
| Component | DBSrv CReadFiles Bulk I/O |
| Primary location | `Code/DBSrv/CReadFiles.cpp` (1,066 lines, 19,587 bytes), `Code/DBSrv/CReadFiles.h` (56 lines, 1,872 bytes) |
| Owning executable | DBSrv (`Code/DBSrv/DBSrv.vcxproj`), Win32 x86 GUI subsystem, `MultiByte` character set, MSVC v143 toolset |
| Architectural role | Integration Adapter (file-system ingress/egress): bulk import of accounts/items/donations, guild blob persistence, connection HTML/CSV publishing, ranking file publishing |
| Scheduler | Single Win32 UI thread (`WM_TIMER` -> `ProcessSecTimer`, `Code/DBSrv/Server.cpp:748-754`) plus boot-time calls and one static-initialization call |
| Protocol peers | No network protocol of its own; consumes/produces files and injects `_MSG_DBSendItem` / `_MSG_DBSendDonate` into the DBSrv-to-TMSrv TCP link |
| Analysis date | 2026-11-15 22:56:45 |
| Analyzed scope | Entire project root `E:\wyd\test-setup\w2pp`, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`, and excluding `Code/TMSrv/CReadFiles.cpp` / `.h` (covered by a separate report) |
| Method | Full read of the component and header; every call site in `Code/DBSrv/Server.cpp`, `Code/DBSrv/CRanking.cpp`, `Code/DBSrv/CFileDB.cpp`; every callee (`CFileDB`, `CUser`, `CPSock`, `Basedef`, `CRanking`); shared data structures in `Code/Basedef.h`; producer-side message handlers on the TMSrv side; project/build configuration; repository-wide test and CI scan |

---

## 1. Executive Summary

### 1.1 Purpose

`CReadFiles` is a stateless, all-static service class (`Code/DBSrv/CReadFiles.h:26-54`) that gives the DBSrv process a second, file-based control plane next to its TCP protocol. It performs six distinct jobs:

1. **Bulk account provisioning** - reads 9-line text records from `../../Common/ImportUser/*.*`, creates account files, deletes the source record (`CReadFiles.cpp:380-690`).
2. **Bulk item delivery** - reads single-line item grant records from `../../Common/ImportItem/*.*`, injects the item both at runtime into the owning TMSrv and into the on-disk account record (`CReadFiles.cpp:130-378`).
3. **Bulk donation credit** - reads `ACCOUNT AMOUNT` records from `../../Common/ImportDonate/*.*`, adds to `STRUCT_ACCOUNTFILE::Donate` and notifies the owning TMSrv (`CReadFiles.cpp:726-908`).
4. **Account password updates** - reads 2-line records from `../../Common/servNN/update/*.*` and rewrites the account password (`CReadFiles.cpp:910-1041`).
5. **Guild state persistence** - reads/writes the 512 KB `GuildInfo` array as an opaque binary blob at `../../Common/GuildInfo` (`CReadFiles.cpp:692-724`).
6. **Publishing** - writes per-server population HTML into the WAMP web root (`C:/wamp/www/servNN.htm`), appends a population CSV (`../../Common/dataNN.csv`), and regenerates the human-readable ranking file (`../../Common/Ranking.txt`) (`CReadFiles.cpp:60-128`, `1043-1066`).

The class holds **no state at all** and exposes **only static methods**; all of its state lives in process globals (`cFileDB`, `pUser[]`, `UserConnection[]`, `GuildInfo[]`, `ServerIndex`, `hWndMain`, `rankingSystem`). It is therefore best understood as a named namespace of eight independent file adapters rather than as an object.

### 1.2 Role in the system

Inside DBSrv the component sits at the boundary between the operating system's file system and two internal subsystems: the `CFileDB` persistence repository (which owns the raw binary account store) and the `CPSock`-based TMSrv link. Every operation it performs is either (a) ingest of an operator-dropped file, (b) egress of a derived artifact for external consumers (web server, operators, ranking page), or (c) direct binary persistence of a global array.

Its afferent callers are three translation units: `Code/DBSrv/Server.cpp` (9 call sites), `Code/DBSrv/CRanking.cpp` (1 call site, from the global constructor chain), and `Code/DBSrv/CFileDB.cpp` (1 call site, per guild message). It has **no network listener and no client-facing surface**; the only "protocol" it speaks is the on-disk file formats it defines by convention.

### 1.3 Key findings

1. **The whole component is thread-confined to the Win32 message loop.** All recurring work is driven by `SetTimer(hWndMain, TIMER_SEC, 1000, NULL)` (`Server.cpp:613`) and `ProcessSecTimer` (`Server.cpp:2013-2084`). No locks exist, and none are needed for `pAccountList`/`pUser` because socket events and timers are serialized by the same thread. The file system, however, is *not* serialized: external editors, the web server, and TMSrv-side writes to the same account files are outside this guarantee.
2. **The import path is an unauthenticated, locally-privileged write interface.** Anything able to create a file in `../../Common/ImportUser`, `ImportItem` or `ImportDonate` can create accounts (with arbitrary SSN/e-mail/address fields), mint items, and credit donations. There is no signature, owner check, extension filter, or rate limit other than "10 files per 30 seconds" for items/donations.
3. **Account names from dropped files are not sanitized before being used as path components.** `sscanf(temp, "%s %d ...", ids, ...)` writes an unbounded token into a 16-byte stack buffer (`CReadFiles.cpp:180-201`, `767-788`), and the resulting name is passed to `CFileDB::AddAccount`/`DBReadAccount`/`DBWriteAccount`, which build paths as `"./account/<FirstKey>/<NAME>"` (`CFileDB.cpp:97`, `2411`, `2542`). Both a stack overflow and a relative-path traversal are reachable from file content.
4. **Import completion semantics are at-least-once and not idempotent.** A file is deleted only after the write succeeds, and `RunTimeSent` deliberately suppresses quarantine when the item was already pushed to a live TMSrv (`CReadFiles.cpp:269-282`, `340-350`, `845-858`, `866-880`). Combined with the absence of any request ID, a leftover file is re-processed every 30 seconds, which can deliver the same item or donation repeatedly.
5. **There is no atomicity anywhere.** Every output is a truncate-and-write on the final path (`fopen(..., "wt")`, `_open(..., O_CREAT)` + `_write`), with no temporary file, no rename, no flush, and no lock. A crash mid-write leaves a short file that the system will silently accept on the next boot, because `DBReadAccount` treats any successful `_read` as success (`CFileDB.cpp:2562-2576`) and `ReadGuildInfo` ignores the `_read` return value entirely (`CReadFiles.cpp:705`).
6. **Publishing intentionally misreports population.** `UpdateConnection` writes `(4 * Count / 3) + rand() % 4` (`CReadFiles.cpp:82`), i.e. a value roughly 33 percent above the true concurrent account count plus 0-3 units of jitter. The value is not the true count and is not stable across consecutive writes.
7. **Cadence is not what the code comments claim.** Item and donation imports are labelled "Once every two minutes" (`Server.cpp:2022`) but run every 30 seconds; the CSV sample is labelled as an hourly counter (`HourCounter++`, `Server.cpp:2037`) but is actually written every 15 minutes; `HourCounter` is incremented and never read.
8. **Failure handling is log-only and non-recovering.** Of the 11 `MoveFile` calls in this file, 8 discard the return value and only 3 inspect it (`CReadFiles.cpp:312`, `362`, `892`). If the quarantine directory does not exist - and the component never creates it - a permanently invalid file is retried forever and logged every cycle.
9. **Static-initialization ordering is wrong for the ranking path.** `RankingSystem rankingSystem;` (`CRanking.cpp:29`) runs its constructor, which calls `loadRanking()` and then `CReadFiles::WriteRanking()` (`CRanking.cpp:96`, `110`), *before* `BASE_InitModuleDir()` normalizes the working directory in `WinMain` (`Server.cpp:443`). The boot-time ranking load and its first file write therefore resolve `./account/*` and `../../Common/Ranking.txt` against the launcher's working directory.
10. **Zero automated tests exist.** The solution has exactly two projects (DBSrv, TMSrv); there is no test project, no test framework reference, no assertion in `Code/`, and no CI configuration. See section 11 for the complete evidence table.

---

## 2. Data Flow Analysis

### 2.1 Execution context

```
DBSrv.exe WinMain                                        (Server.cpp:427)
  ├─ InitInstance / InitApplication                      (Server.cpp:435-440)
  ├─ BASE_InitModuleDir()  -> CWD := directory of DBSrv.exe   (Basedef.cpp:2608-2629)
  │     NOTE: global `RankingSystem rankingSystem` (CRanking.cpp:29) has already
  │           been constructed BEFORE this line; its loadRanking() -> WriteRanking()
  │           therefore used the launcher's CWD.
  ├─ ServerIndex resolved from local IP vs g_pServerList (Server.cpp:546-581)
  ├─ CReadFiles::ImportItem()                            (Server.cpp:583)   [boot, one pass]
  ├─ ListenSocket.StartListen(7514) / AdminSocket.StartListen(8895)   (Server.cpp:586-587)
  ├─ CReadFiles::ReadGuildInfo()                         (Server.cpp:615)   [boot, one pass]
  └─ message loop GetMessage/DispatchMessage             (Server.cpp:617)
        ├─ WM_TIMER TIMER_SEC  -> ProcessSecTimer()      (Server.cpp:748-751)
        │     ├─ ImportUser()          every 1 s, unbounded batch   (Server.cpp:2015)
        │     ├─ UpdateUser()          every 1 s, unbounded batch   (Server.cpp:2016)
        │     ├─ ImportItem()          every 30 s, max 10 files     (Server.cpp:2020-2024)
        │     ├─ ImportDonate()        every 30 s, max 10 files     (Server.cpp:2020-2024)
        │     ├─ UpdateConnection()    every 30 s                   (Server.cpp:2030)
        │     ├─ UpdateConnectionData()every 15 min (MinCounter%30) (Server.cpp:2034-2038)
        │     ├─ WriteRanking()        every 600 s                  (Server.cpp:2041-2046)
        │     └─ WriteGuildInfo()      Sunday 00:00:00 exactly      (Server.cpp:2061-2083)
        └─ WSA_READDB / WSA_READADMIN / WSA_ACCEPT ...  (socket handlers, same thread)
              └─ CFileDB::ProcessMessage
                    └─ case _MSG_GuildInfo -> CReadFiles::WriteGuildInfo()  (CFileDB.cpp:480)
              └─ ProcessAdminMessage
                    └─ case _MSG_NPDonate -> writes ImportDonate file       (Server.cpp:1982-2006)
```

The component never runs on a second thread. Its only concurrency exposure is with actors outside the process (web server reading the HTML/CSV, operators editing `GuildInfo`, TMSrv writing account files indirectly through `_MSG_DBSaveMob`).

### 2.2 Flow A - item import (`CReadFiles::ImportItem`, `CReadFiles.cpp:130-378`)

```
1.  Enumerate            _findfirst("../../Common/ImportItem/*.*")        :141
2.  Batch gate           count > 10 -> stop                                :146-153
3.  Skip dot entries     c_file.name[0] == '.' -> _findnext                 :155-163
4.  Open                 fopen("../../Common/ImportItem/<name>", "rt")      :165-168
                         failure -> _findnext, continue                     :170-178
5.  Read one line        fgets(temp, 256, fp)                               :183
                         EOF -> close + MoveFile to ../../Common/Error/     :185-199
6.  Parse                sscanf("%s %d %d %d %d %d %d %d", ids, Index,
                         Eff1,Val1,Eff2,Val2,Eff3,Val3)                     :201
7.  Canonicalize         _strupr(ids)                                       :207
8.  Validate             Index in [0, MAX_ITEMLIST); Eff1..3 in [0,255]     :209-221
                         failure -> MoveFile to Error + misleading log      :211-213
9.  Build item           STRUCT_ITEM{ sIndex, stEffect[0..2]{cEffect,cValue}} :223-231
10. Resolve account      cFileDB.GetIndex(ids)   (linear scan, pAccountList) :235
11. Runtime delivery     if IdxName>0 && <MAX_DBACCOUNT && Slot in [0,3]:
                           svr = IdxName / MAX_USER ; id = IdxName % MAX_USER
                           MSG_DBSendItem -> pUser[svr].cSock.SendOneMessage  :237-257
                           RunTimeSent = 1                                   :256
12. Disk read            STRUCT_ACCOUNTFILE{AccountName=ids} ; DBReadAccount  :259-267
                         FALSE -> if(!RunTimeSent) MoveFile to Error        :269-282
13. Placement            first-fit grid scan i in [0,126) via BASE_CanCargo  :284-296
                         else reverse empty-slot scan i in [127..0]         :298-308
                         none -> MoveFile to Error + log                     :310-329
14. Mutate               8 bytes copied into file.Cargo[Pos]                 :331-332
15. Persist              cFileDB.DBWriteAccount(&file)  (full record)        :334
                         FALSE -> if(!RunTimeSent) MoveFile, log, next       :336-350
16. Complete             Log("-import item cargo ...") ; DeleteFile          :352-358
                         delete failed -> MoveFile to Error + warning        :360-368
17. Advance              _findnext ; loop until 10 files or end of dir       :370-372
```

### 2.3 Flow B - donation import (`CReadFiles::ImportDonate`, `CReadFiles.cpp:726-908`)

Identical shape to Flow A with a two-token record, and three differences: there is **no dot-entry guard** (the `.`/`..` entries are skipped only because `fopen` on a directory fails); the runtime message (`MSG_DBSendDonate`) is built **without `memset`**, so its `Result` field carries uninitialized stack bytes onto the wire (`CReadFiles.cpp:819-829`); and the mutation is an arithmetic accumulation `file.Donate += Donate` (`CReadFiles.cpp:862`) rather than a placement decision.

```
1. enumerate ../../Common/ImportDonate/*.*                    :735-740
2. count > 10 -> stop                                          :748-749
3. fopen(".../<name>","rt") ; failure -> next                  :752-765
4. fgets(temp,256) ; EOF -> MoveFile(ImportDonateError)        :770-786
5. sscanf("%s %d", ids, &Donate)                               :788
6. _strupr(ids) ; Donate < 0 -> MoveFile(ImportDonateError)    :794-808
7. GetIndex -> runtime MSG_DBSendDonate (no memset)            :812-833
8. DBReadAccount ; FALSE && !RunTimeSent -> MoveFile           :843-858
9. file.Donate += Donate ; DBWriteAccount                      :862-864
10. log "-import donate" ; DeleteFile ; fallback MoveFile      :882-898
```

### 2.4 Flow C - account import (`CReadFiles::ImportUser`, `CReadFiles.cpp:380-690`)

```
 1. _findfirst("../../Common/ImportUser/*.*")                     :387-391
 2. dot entry -> _findnext, continue                              :408-416
 3. fopen("../../Common/ImportUser/<name>","rt")                  :418-419
 4. line 1  id       sscanf("%s")   ; len in [1,15]  ; _strupr    :443-460
 5. line 2  pass     sscanf("%s")   ; len in [1,11]               :474-489
 6. line 3  realname sscanf("%s")   ; len in [1,23]               :502-517
 7. line 4  ssn1     sscanf("%d")   ; must be non-zero            :531-541
 8. line 5  ssn2     sscanf("%d")   ; must be non-zero            :555-565
 9. line 6  email    whole line, strip trailing byte; len<48      :567-598
10. line 7  tel      whole line, strip trailing byte; len<16      :600-631
11. line 8  addr     whole line, strip trailing byte; len<=78     :633-664
12. line 9  bonus    optional; default "0"; sscanf("%d")          :666-674
13. cFileDB.AddAccount(id,pass,name,ssn1,ssn2,email,tel,addr,bonus)
        -> rejects COM0-9 / LPT0-9, rejects existing account file,
           writes ./account/<FirstKey>/<NAME> (7.6 KB record)     :677 / CFileDB.cpp:78-143
14. AddAccount != 0 -> DeleteFile(source) ; else source is kept    :679-680
15. _findnext ; loop (no batch limit)                             :682-686
```

### 2.5 Flow D - password update (`CReadFiles::UpdateUser`, `CReadFiles.cpp:910-1041`)

```
1. _findfirst("../../Common/servNN/update/*.*")  (NN = ServerIndex)   :918-923
2. skip dot entries                                                    :930-938
3. fopen ; line 1 id (1..15, _strupr) ; line 2 pass (1..11)             :942-1009
4. cFileDB.UpdateAccount(id, pass)
     -> rejects COM/LPT names, requires EXISTING account file,
        patches pAccountList[Idx] in memory, rewrites the record      :1015 / CFileDB.cpp:145-208
5. DeleteFile(source) in BOTH outcomes (success and failure)            :1017-1030
6. _findnext ; loop (no batch limit)                                    :1033-1037
```

### 2.6 Flow E - guild blob

```
boot      ReadGuildInfo : _open(O_RDONLY|O_BINARY) -> _read(handle, GuildInfo, sizeof(GuildInfo)) : CReadFiles.cpp:696-706
                           missing file -> MessageBoxA("no GuildInfo file") then continue booting
runtime   WriteGuildInfo: _open(O_RDWR|O_CREAT|O_BINARY) -> _write(GuildInfo, 524288 bytes) : CReadFiles.cpp:713-723
             triggered by (a) every _MSG_GuildInfo from any TMSrv   : CFileDB.cpp:466-480
                          (b) Sunday 00:00:00 in ProcessSecTimer     : Server.cpp:2061-2083
```

### 2.7 Flow F - connection HTML (`CReadFiles.cpp:60-86`)

```
1. path   sprintf("C:/wamp/www/serv%2.2d.htm", ServerIndex)      :64
2. open   fopen(path,"wt")  ; NULL -> return silently            :66-69
3. for i in [0, MAX_SERVER):
     empty slot (Mode == USER_EMPTY) -> write "-1\n"             :73-77
     else: peak-track UserConnection[i] = max(peak, Count)       :79-80
           write ((4*Count/3) + rand()%4) "\n"                   :82
4. close                                                          :85
```

### 2.8 Flow G - population CSV (`CReadFiles.cpp:88-128`)

```
1. path   sprintf("../../Common/data%2.2d.csv", ServerIndex)      :92
2. open   fopen(path,"a+") ; NULL -> return silently              :94-97
3. stamp  sprintf("%4.4d_%2.2d_%2.2d_%2.2d ", Y,M,D,H)            :104
4. append timestamp with fprintf(fp, temp) (non-literal format)    :106
5. for i in [0, MAX_SERVER): append ",%4d  " of UserConnection[i]
   and reset UserConnection[i] = 0                                 :113-122
6. append ",%4d  " of total, then "\n"                             :124-125
```

### 2.9 Flow H - ranking file (`CReadFiles.cpp:1043-1066`)

```
1. open   fopen("../../Common/Ranking.txt","wt") ; NULL -> silent   :1047-1050
2. labels classmaster[6] = { "SEM CLASSMASTER","M","A","C","C","SC" } :1052
          classe[4]      = { "TK","FM","BM","HT" }                   :1053
3. for i = 499 (FIRST) down to 1 (i > LAST, so index 0 never emitted) :1055
     skip if Name[0] == 0 or Level >= 1000                            :1057
     emit  "<pos> <Name> <classmaster label> <class label>"          :1059-1061
4. close                                                              :1065
```

### 2.10 Data ownership matrix

| Artifact | Writer | Reader(s) | Format | Tolerance |
|----------|--------|-----------|--------|-----------|
| `../../Common/ImportUser/*` | operator / NPTool | `ImportUser` | 9-line text, whitespace-tokenized | no header, no checksum, no extension filter |
| `../../Common/ImportItem/*` | operator / web shop | `ImportItem` | 1 line, 8 tokens | same |
| `../../Common/ImportDonate/*` | `ProcessAdminMessage` `_MSG_NPDonate` (`Server.cpp:1982-2006`) and operators | `ImportDonate` | 1 line, 2 tokens | same |
| `../../Common/Error/*`, `../../Common/ImportDonateError/*` | `ImportItem`, `ImportDonate` | operators | quarantined originals | directory must pre-exist |
| `../../Common/servNN/update/*` | operators | `UpdateUser` | 2-line text | same |
| `./account/<FirstKey>/<NAME>` | `CFileDB::DBWriteAccount` (called from this component) | `CFileDB`, `RankingSystem`, `DayLog_ReadAccountInDir` | raw `STRUCT_ACCOUNTFILE` (~7.6 KB) | file size accepted in 7500..7600 or exact sizeof (`CRanking.cpp:175`) |
| `../../Common/GuildInfo` | `WriteGuildInfo` | `ReadGuildInfo` | raw array of 65,536 x `STRUCT_GUILDINFO` = 524,288 bytes | none |
| `C:/wamp/www/servNN.htm` | `UpdateConnection` | external web server (not in repository) | 10 lines of integers, plain text despite `.htm` | none |
| `../../Common/dataNN.csv` | `UpdateConnectionData` | external reporting | custom styled CSV, 15-minute rows | none |
| `../../Common/Ranking.txt` | `WriteRanking` | external ranking page / operators | `<pos> <name> <cm-class> <class>` | none |

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | Processing | Watched directories are polled by directory enumeration; item/donation batches are capped at 10 candidates per 30-second tick, user import and password update are uncapped per 1-second tick | `CReadFiles.cpp:141-153`, `737-749`, `387-395`, `920-926`; `Server.cpp:2015-2024` |
| BR-02 | Validation | Item import accepts exactly one line of 8 whitespace tokens and requires `0 <= Index < 6500`, `0 <= Eff1..3 <= 255`; effect values are not range-checked; `sscanf` result ignored | `CReadFiles.cpp:183-221` |
| BR-03 | Business Logic | Item placement is a first-fit grid search over 126 cargo cells (9x14) using `BASE_CanCargo`, then a reverse scan of empty slots 127..0 | `CReadFiles.cpp:284-308` |
| BR-04 | Business Logic | Runtime delivery only when the account is registered in `pAccountList` with a selected character slot 0..3; server = index / 1000, player id = index % 1000 | `CReadFiles.cpp:237-257`, `812-833` |
| BR-05 | Persistence | All account mutations are full-record read-modify-write of the entire `STRUCT_ACCOUNTFILE`; no partial field update, no transaction | `CReadFiles.cpp:259-267`, `331-334`, `835-864` |
| BR-06 | Lifecycle | Source file is deleted only after a successful write; `RunTimeSent` suppresses quarantine/move when runtime delivery already happened | `CReadFiles.cpp:269-282`, `336-350`, `358-368`, `843-880`, `888-898` |
| BR-07 | Error Handling | Invalid/unprocessable import files are moved to `../../Common/Error/` (items) or `../../Common/ImportDonateError/` (donations); move result is checked in only 3 of 11 call sites | `CReadFiles.cpp:166`, `189`, `211`, `272`, `312`, `341`, `362`, `753`, `776`, `798`, `848`, `871`, `892` |
| BR-08 | Validation | Donation amount must be `>= 0` and is added to `Donate` with no upper bound; online delivery uses a non-zeroed packet | `CReadFiles.cpp:788-810`, `819-833`, `862` |
| BR-09 | Format | Account import file is exactly 8 mandatory lines + 1 optional line in the order id, pass, realname, ssn1, ssn2, email, telephone, address, bonus | `CReadFiles.cpp:431-674` |
| BR-10 | Validation | Field length policy per field: id 1..15, pass 1..11, name 1..23, email 1..47, telephone 1..15, address 1..78 (boundary off-by-one), ssn1/ssn2 non-zero | `CReadFiles.cpp:447`, `478`, `506`, `533`, `557`, `588`, `621`, `654` |
| BR-11 | Validation | Account names are uppercased, must be shorter than 16 bytes, and the reserved device names COM0-COM9 / LPT0-LPT9 are refused at every repository entry point | `CReadFiles.cpp:457-460`; `CFileDB.cpp:86-89`, `153-156`, `2400-2403`, `2531-2534` |
| BR-12 | Business Logic | `bonus != 0` on account creation pre-fills cargo slots 0..99 with item 401 (slots 0-49) and item 406 (slots 50-99) | `CReadFiles.cpp:666-674`; `CFileDB.cpp:126-135` |
| BR-13 | Business Logic | Imports never overwrite an existing account: `AddAccount` returns FALSE if the account file already exists, and the source file is then retained | `CFileDB.cpp:99-106`; `CReadFiles.cpp:677-680` |
| BR-14 | Business Logic | Password update files live in a per-server-group directory and are deleted regardless of success or failure | `CReadFiles.cpp:918`, `1015-1030` |
| BR-15 | Persistence | Guild state is an opaque 524,288-byte binary blob, read once at boot and rewritten in full on every guild message and at Sunday midnight | `CReadFiles.cpp:692-724`; `CFileDB.cpp:480`; `Server.cpp:2061-2083` |
| BR-16 | Publishing | Connection HTML contains exactly 10 lines; empty server slots are `-1`; live slots publish `(4*Count/3) + rand()%4`, a deliberately inflated value | `CReadFiles.cpp:71-83` |
| BR-17 | Publishing | Population CSV appends one row per 15 minutes with hour-granularity timestamp and per-server peak values, then resets the peaks | `CReadFiles.cpp:104-125`; `Server.cpp:2028-2039` |
| BR-18 | Publishing | Ranking file emits at most 499 entries, ordered best-to-worst, skipping empty names and `Level >= 1000`, and never emits slot 0 | `CReadFiles.cpp:1055-1064` |
| BR-19 | Error Handling | Every failure is either silently ignored or logged; the only retry mechanism is re-processing the still-present source file on the next tick | `CReadFiles.cpp:69`, `97`, `107-109`, `1049-1050` |
| BR-20 | Security | The file-drop directories are an unauthenticated privileged interface: local write access equals account creation, item minting and donation credit; account names are not sanitized before path construction | `CReadFiles.cpp:180-201`, `767-788`; `CFileDB.cpp:97`, `2411`, `2542` |

---

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 Watched-directory polling and per-tick batch limit

**Overview**:
The component does not use file-system notifications. It re-enumerates each watched directory on every scheduled tick and stops item/donation work after 10 candidates; user import and password update have no cap.

**Detailed description**:
Each entry point begins with a wildcard enumeration (`_findfirst("../../Common/ImportItem/*.*")`, `CReadFiles.cpp:139-144`; `"../../Common/ImportUser/*.*"`, `:387-391`; `"../../Common/ImportDonate/*.*"`, `:735-740`; `"../../Common/serv%2.2d/update/*.*"`, `:918-923`) and terminates with `_findclose` unless the handle is `-1`. Because the pattern is `*.*`, the extension of a dropped file is irrelevant; any regular file in those directories is treated as a command record. Because enumeration is done fresh each time, files added by an operator are picked up within one tick with no restart, and files already processed are not remembered anywhere - progress is expressed only as file deletion or quarantine.

The throttles differ sharply. `ImportItem` (`:146-153`) and `ImportDonate` (`:742-749`) increment a local `count` at the top of the loop and break when `count > 10`, so each call inspects at most 10 directory entries, which includes the `.` and `..` entries returned by the CRT. Since these two run every 30 seconds (`Server.cpp:2020-2024`), each queue drains at most 8 payload files per cycle in practice - roughly 16 records per minute - because two of the ten slots are consumed by `.` and `..` in the donation path, which lacks the explicit dot guard present at `CReadFiles.cpp:155-163`. When a batch hits the cap, the remaining files simply wait for the next tick; there is no backlog metric, no warning, and no catch-up acceleration.

`ImportUser` (`:395-686`) and `UpdateUser` (`:925-1037`) have **no** cap and run every second (`Server.cpp:2015-2016`). A drop of several thousand account records therefore performs several thousand `AddAccount` calls, each of which does an `_open` existence check plus a 7.6 KB create-and-write, inside the Win32 message loop that also services sockets and the redraw timer. During that time no `_MSG_DB*` request is serviced, so the game servers' DB latency rises with the size of the drop.

**Rule workflow**:
```
every 1000 ms:
  ImportUser()      -> enumerate ImportUser/*.*   -> for each entry (unbounded): process
  UpdateUser()      -> enumerate servNN/update/*.* -> for each entry (unbounded): process
  every 30th tick:
    ImportItem()    -> enumerate ImportItem/*.*   -> at most 10 entries, then stop
    ImportDonate()  -> enumerate ImportDonate/*.* -> at most 10 entries, then stop
  gaming traffic on the same thread waits behind all of the above
```

---

### Business Rule: BR-02 Item import line format and field validation

**Overview**:
One item grant per file, expressed as a single line of eight whitespace-separated tokens: account name, item index, and three effect/value pairs.

**Detailed description**:
The record is read with a single `fgets(temp, 256, fp)` (`CReadFiles.cpp:183`) and parsed by `sscanf(temp, "%s %d %d %d %d %d %d %d  ", ids, &Index, &Eff1, &Val1, &Eff2, &Val2, &Eff3, &Val3)` (`:201`). No header, delimiter, comment, quoting or continuation syntax is supported; extra tokens are ignored, and a missing token leaves its target at its initialized value of 0 (`:181`). The return value of `sscanf` is discarded, so a line containing only an account name is accepted and produces a grant of item index 0 with three effects of 0 - an empty item written into a cargo slot, with the file then deleted.

Validation (`:209`) covers exactly four conditions: `Index` must be in `[0, MAX_ITEMLIST)` with `MAX_ITEMLIST = 6500` (`Basedef.h:139`), and each of `Eff1`, `Eff2`, `Eff3` must be in `[0, 255]`. Nothing else is checked: `Val1..Val3` have no range check and are silently truncated to 8 bits when assigned to `STRUCT_ITEM::stEffect[].cValue` (`:227-231`), so a value such as 300 is stored as 44; `Index` is not validated against the item catalogue, so a non-existent item index is accepted; the account name is not validated for character content, only uppercased (`:207`); and there is no duplicate or quota check, so the same item may be granted any number of times.

Two further parse hazards live in this rule. The account token is written into `char ids[ACCOUNTNAME_LENGTH]`, i.e. 16 bytes (`:180`), by an unbounded `%s` conversion reading from a 256-byte line, so a token longer than 15 bytes overwrites adjacent stack variables before the range check can run. And the length that is later used to bound the name - `strncpy(sm.Account, ids, ACCOUNTNAME_LENGTH)` (`:249`) and `memcpy(file.Info.AccountName, ids, ACCOUNTNAME_LENGTH)` (`:265`) - copies a fixed 16 bytes rather than the token length, which is only safe because the copy target is also 16 bytes and because `_strupr` and the trailing-zero writes in `CFileDB::DBReadAccount` (`CFileDB.cpp:2520-2523`) mask the missing terminator.

**Rule workflow**:
```
read 1 line (<=255 bytes)
scan "name idx e1 v1 e2 v2 e3 v3"
if Index < 0 or Index >= 6500 -> quarantine(file), log "no contents"
if any Eff < 0 or Eff > 255  -> quarantine(file), log "no contents"
else build STRUCT_ITEM and continue to placement (BR-03) and delivery (BR-04)
values Val1..Val3 are truncated to unsigned char with no diagnostic
```

---

### Business Rule: BR-03 Cargo placement policy

**Overview**:
The imported item must be placed into a free area of the 128-slot cargo array using the same grid-fit test the game server uses, with a permissive fallback.

**Detailed description**:
The primary search iterates `i` from 0 to 125, calling `BASE_CanCargo(&tempsour, file.Cargo, i % 9, i / 9)` (`CReadFiles.cpp:288`). `BASE_CanCargo` (`Basedef.cpp:4480-4537`) reconstructs occupancy by reading each occupied cargo item's `EF_GRID` ability mask and projecting it onto a 9x14 cell grid, then checks that the candidate item's own mask fits inside the grid at the candidate origin. The 9x14 geometry comes from `CARGOGRIDX = 9` / `CARGOGRIDY = 14` (`Basedef.h:90-91`), giving exactly 126 in-grid origins; `MAX_CARGO` is 128 (`Basedef.h:77`), so slots 126 and 127 are outside the modelled grid. The search stops at the first position that passes, which means placement is deterministic but not best-fit: an item is placed in the lowest-numbered feasible origin, producing a top-left-biased inventory layout.

If no grid position passes, the code runs a second, looser search from 127 down to 0 that accepts any slot whose `sIndex` is zero (`:300-308`). This fallback ignores grid occupancy entirely, so a multi-cell item can be placed so that it overlaps the footprints of existing items, or into the two slots that the grid model does not cover. Whether that matters is decided later by TMSrv: when the item is delivered live, TMSrv performs its own `BASE_CanCargo` loop (`Code/TMSrv/ProcessDBMessage.cpp:259-323`) and, if it also fails, replies with `Result = 3` - a reply that DBSrv never reads.

If both searches fail, the file is quarantined and the log line says `"-IMPORT no space, file moved to err"` (`:321`), preceded by a second, incorrect log line when the move itself failed (`:318`). The `i` used in the success log (`sprintf(log, "-import item cargo %s(%d)", ids, i)`, `:354`) is the stale loop variable from the last search, not `Pos`, so the logged slot number can differ from the slot actually written.

**Rule workflow**:
```
Pos = -1
for i in [0,126):                       # in-grid origins only
    if BASE_CanCargo(item, cargo, i%9, i/9) == TRUE: Pos = i; break
if Pos == -1:                           # permissive fallback
    for i in [127..0]: if cargo[i].sIndex == 0: Pos = i; break
if Pos == -1:
    MoveFile(source, Error dir); log "no space"; next file
copy 8 bytes of STRUCT_ITEM into cargo[Pos]
```

---

### Business Rule: BR-04 Online delivery gate and message construction

**Overview**:
An import is additionally delivered to the live game server only when the account is currently registered in the DB server's in-memory account list and has an active character slot.

**Detailed description**:
The gate is `IdxName > 0 && IdxName < MAX_DBACCOUNT && cFileDB.pAccountList[IdxName].Slot >= 0 && cFileDB.pAccountList[IdxName].Slot <= 3` (`CReadFiles.cpp:237`, `814`). `GetIndex` (`CFileDB.cpp:2335-2349`) performs a linear scan over 10,000 slots, skipping entries with `Login == 0` and comparing `AccountName`, returning 0 for "not found" - which is unambiguous because index 0 corresponds to server 0 / player 0 and the account-id space starts at 1 (`Basedef.h:56-60`). `MAX_DBACCOUNT` is `MAX_USER * MAX_SERVER = 1000 * 10 = 10000`. The `Slot` test is the real gate: `Slot` is set to -1 when the account is added to the list (`CFileDB.cpp:2216`) and to a concrete 0..3 value only once a character is chosen, so the message is sent only to players who are at character select or in game - never to a player still typing credentials.

Delivery is a fire-and-forget `pUser[svr].cSock.SendOneMessage(...)` (`:253`, `:829`). The return value is ignored, the target socket is not checked for connectivity (`pUser[svr].Mode` / `cSock.Sock` are not tested here, unlike other call sites such as `CFileDB.cpp:2384`), and `RunTimeSent` is set to 1 unconditionally (`:256`, `:832`). Consequently a send to an empty slot, or a send that `CPSock` rejects because its buffer is full (`CPSock.cpp:680-683`), still marks the file as "runtime handled" for the purposes of BR-06.

The TMSrv side has its own gates: it requires `pUser[conn].Mode >= USER_SELCHAR`, requires `strcmp(pUser[conn].AccountName, m->Account) == 0`, then performs its own placement and replies with `Result = 0` or `3` (`Code/TMSrv/ProcessDBMessage.cpp:233-291`, `325-327`). DBSrv never examines that reply - the response arrives later on the read path, not inside `ImportItem` - so a runtime rejection cannot influence the file's fate.

**Rule workflow**:
```
IdxName = cFileDB.GetIndex(ids)                     # 0..9999, 0 = absent
if IdxName in (0, MAX_DBACCOUNT) or Slot outside [0,3]:
    runtime delivery skipped (file still processed on disk)
else:
    svr = IdxName / 1000 ; id = IdxName % 1000
    build MSG_DBSendItem{Type,Size,ID=id,Account,Item}   (memset first)
    or MSG_DBSendDonate{Type,Size,ID=id,Account,Donate}  (NOT memset)
    pUser[svr].cSock.SendOneMessage(...)   # result ignored
    RunTimeSent = 1
```

---

### Business Rule: BR-05 Full-record read-modify-write persistence

**Overview**:
Every account mutation performed by this component reads the complete `STRUCT_ACCOUNTFILE` from disk, changes one field, and writes the complete record back over the original file.

**Detailed description**:
The pattern is always the same three steps: zero a local `STRUCT_ACCOUNTFILE`, copy the account name into `file.Info.AccountName`, call `cFileDB.DBReadAccount(&file)` (`CReadFiles.cpp:261-267`, `837-843`), then mutate and call `cFileDB.DBWriteAccount(&file)` (`:334`, `:864`). `DBReadAccount` (`CFileDB.cpp:2518-2577`) opens `./account/<FirstKey>/<NAME>` read-only, reads `sizeof(STRUCT_ACCOUNTFILE)` bytes with a single `_read`, and returns TRUE whenever `_read` did not return -1 - it does not compare the read count with the requested size. `DBWriteAccount` (`CFileDB.cpp:2390-2469`) opens the same path with `O_RDWR|O_CREAT`, seeks to 0, and writes the whole record with a single `_write`. There is no temporary file, no rename, no flush, no length validation, and no lock.

Three consequences follow. First, a short or truncated account file is silently accepted and then "repaired" by the next import, which rewrites the entire record with whatever the read produced (the buffer was zeroed beforehand, so the tail becomes zeros). Second, the record is a single ~7.6 KB write on a file that is also written by `_MSG_DBSaveMob` handling (`Code/TMSrv/Server.cpp:7056-7083` sends the save; DBSrv writes it) - two logical writers mutating different fields of the same record through whole-record writes. Third, the mutation is performed on a snapshot: between `DBReadAccount` and `DBWriteAccount` no other work happens in the same function, but the state read may already be stale relative to a `_MSG_DBSaveMob` that was queued by TMSrv in response to the very delivery performed 60 lines earlier. This is a lost-update window for concurrent character saves, and a duplicate-grant window for items (if TMSrv's own save landed before the read, the item is already present on disk and will be added a second time at the next free slot).

The read path performs three defensive rewrites that matter for this rule: it forces `AccountName[14..15]` and `AccountPass[14..15]` to zero (`CFileDB.cpp:2520-2523`), it rejects the account names COM0-COM9 and LPT0-LPT9 (`:2531-2534`), and it derives the directory key from the first character only (`BASE_GetFirstKey`, `Basedef.cpp:4114-4160`).

**Rule workflow**:
```
zero STRUCT_ACCOUNTFILE
set Info.AccountName = uppercase(imported name)
DBReadAccount:
    reject COM0-9 / LPT0-9
    First = BASE_GetFirstKey(name)      # A-Z/a-z char, or Korean initial, else "etc"
    _open("./account/<First>/<NAME>", O_RDONLY|O_BINARY)
    _read(entire struct) -> TRUE if != -1     # size not verified
mutate: place item into Cargo[Pos]   or   Donate += amount
DBWriteAccount:
    reject COM0-9 / LPT0-9
    _open(same path, O_RDWR|O_CREAT|O_BINARY, S_IREAD|S_IWRITE)
    _lseek(0) ; _write(entire struct) ; _close
```

---

### Business Rule: BR-06 Source-file lifecycle and the RunTimeSent suppression rule

**Overview**:
A consumed import file is deleted only on success; a file whose runtime delivery succeeded but whose disk write did not is intentionally left in place - and therefore reprocessed.

**Detailed description**:
The happy path ends with `DeleteFile(filename)` (`CReadFiles.cpp:358`, `888`), and if that fails the file is moved to the quarantine directory and a warning is logged (`:360-368`, `:890-898`). Before that, every failure branch consults `RunTimeSent`. In `ImportItem`, if `DBReadAccount` fails: `if (!RunTimeSent) MoveFile(filename, movefile);` (`:271-272`), then the file is left untouched and logging continues with `"-IMPORT no account"` (`:274`). The same guard appears when `DBWriteAccount` fails (`:340-341`) and in the donation equivalents (`:847-848`, `:870-871`).

The intent is defensible - do not destroy evidence of a grant that a player already received - but the effect is that the file becomes a permanent retry record with no attempt counter, no backoff, and no de-duplication. `ImportItem` runs every 30 seconds, so a file referring to an account that does not exist on this DB server is re-read, re-parsed, and (if the account is in `pAccountList`) re-delivered, indefinitely. Because nothing records that a given file was already delivered, each retry that reaches BR-04 sends a further `_MSG_DBSendItem`; TMSrv places each one in a new cargo slot. The donation path has the same shape with `Donate` credit.

The asymmetry between "runtime sent" and "disk written" is therefore the component's central consistency weakness: there is no idempotency key (the file name is not used as one), no journal, and no reconciliation between the runtime delivery and the persisted record. The only convergence mechanism is TMSrv's `SaveUser(conn, 0)` at `Code/TMSrv/ProcessDBMessage.cpp:288`, which persists the player's cargo when the delivery arrives during character select - a mechanism that is absent for in-game and offline players.

**Rule workflow**:
```
RunTimeSent = 0
if online gate passes:
    send message ; RunTimeSent = 1           # regardless of send result
read account
if read failed:
    if !RunTimeSent: quarantine(file)
    log "no account" ; next file             # file REMAINS in watched dir
    -> next tick: same file, same path, possible second delivery
write account
if write failed:
    if !RunTimeSent: quarantine(file)
    log "... ACCOUNT SAVE FAIL" ; next file  # file REMAINS
DeleteFile(source)
if delete failed: MoveFile(source, quarantine) ; warn
```

---

### Business Rule: BR-07 Quarantine policy and its silent failure mode

**Overview**:
Rejected import files are supposed to be relocated into an error directory so that operators can inspect them; the relocation is best-effort and never validated in eight of eleven call sites.

**Detailed description**:
Two quarantine targets are defined as compile-time constants: `IMPORT_ITEM_ERROR_PATH = "../../Common/Error/%s"` (`CReadFiles.cpp:44`) and `IMPORT_DONATE_ERROR_PATH = "../../Common/ImportDonateError/%s"` (`:53`). The moves are issued with `MoveFile`, which fails if the destination directory does not exist, if the destination file already exists, or if the source is locked. Nothing in DBSrv creates these directories: a repository-wide search for `CreateDirectory` and `_mkdir` under `Code/DBSrv` returns no matches, so the directory layout is an external deployment prerequisite that is not documented in code.

The returns are handled inconsistently. They are ignored at `:189` (empty item file), `:211` (item validation failure), `:272` (account not found after runtime delivery), `:341` (item write failure), `:776` (empty donation file), `:798` (negative donation), `:848` (donation account not found) and `:871` (donation write failure), and inspected only at `:312` (`no space`), `:362` and `:892` (post-success delete failure). When the move is silently dropped, the invalid file stays in the watched directory and is re-read on the next tick - forever, at 30 seconds per iteration, with one log line per iteration. For a permanently malformed file this is an unbounded log-growth and I/O-churn condition that will only stop when an operator removes the file.

A second inconsistency is the message text: both the invalid-index path (`:213`) and the amount path (`:800`) log `"-IMPORT no contents"`, the same text used for the genuinely empty-file case (`:191`, `:778`), so the log cannot distinguish "empty file" from "field out of range". The item path additionally logs the parsed line rather than the file name at `:205` (`Log(temp, "-IMPORT starting", 0)`) while all sibling lines log `filename`.

**Rule workflow**:
```
reject(reason):
    switch reason:
      empty file            -> MoveFile(src, Error) ; log "no contents"
      field out of range    -> MoveFile(src, Error) ; log "no contents"      # unverified
      account not found     -> MoveFile(src, Error) if !RunTimeSent          # unverified
      no cargo space        -> MoveFile(src, Error) ; log "no space"         # verified
      negative donation     -> MoveFile(src, ImportDonateError)              # unverified
    if MoveFile failed and the caller ignored the result:
        src remains -> retried every 30 s indefinitely, one log line per retry
```

---

### Business Rule: BR-08 Donation import rules

**Overview**:
One donation credit per file, expressed as `ACCOUNT AMOUNT`; the amount must be non-negative and is accumulated onto the existing balance.

**Detailed description**:
The record is read with `fgets` and parsed by `sscanf(temp, "%s %d ", ids, &Donate)` (`CReadFiles.cpp:770-788`), followed by `_strupr(ids)` and a single validation `if (Donate < 0)` (`:796`). There is no upper bound: a value such as 2,000,000,000 is accepted, and because the target field is a 32-bit `int` `Donate` (`Basedef.h:853`), the accumulation `file.Donate += Donate` (`:862`) can overflow into a negative balance with no diagnostic. Parsing failure leaves `Donate = 0` (`:768`), which passes validation and results in a no-op write plus deletion of the file - the record is consumed but nothing is credited, and there is no "zero amount" rejection.

The producer of these files is in-process: `_MSG_NPDonate` handling writes `"%s %d"` (without a trailing newline) to `../../Common/ImportDonate/<ddmmyyyyHHMMSS + rand()%255>` (`Server.cpp:1982-2006`). Because the file name is derived from a one-second timestamp plus a 0-254 random suffix, two donations in the same second have a 1-in-255 collision chance; a collision silently overwrites the first file, losing that credit. This producer rule is relevant because it defines the trust and collision properties of the input queue that `ImportDonate` drains.

The delivery packet is the one place in the component where a structure is sent partly uninitialized: `MSG_DBSendDonate sm;` is declared without `memset` (`:819`) and only `Type`, `Size`, `ID`, `Account` and `Donate` are assigned (`:821-827`), unlike the item path which does `memset(&sm, 0, sizeof(MSG_DBSendItem))` (`:243`). The `Result` field (`Basedef.h:1295`) therefore carries whatever was on the stack at that point to the game server. TMSrv overwrites `Result` before echoing the message back (`Code/TMSrv/ProcessDBMessage.cpp:1244-1246`), so the practical impact is limited to information exposure and to any future consumer that reads `Result` on this direction.

**Rule workflow**:
```
read 1 line ; scan "NAME AMOUNT"
if EOF -> quarantine ; log "no contents"
if AMOUNT < 0 -> quarantine ; log "no contents"
if online gate passes -> MSG_DBSendDonate (Result uninitialized) -> send
DBReadAccount ; failure -> if !RunTimeSent quarantine ; file remains
Donate_balance += AMOUNT            # unchecked, can overflow
DBWriteAccount ; failure -> if !RunTimeSent quarantine ; file remains
DeleteFile ; fallback MoveFile to ImportDonateError
```

---

### Business Rule: BR-09 Account import record format

**Overview**:
An account provisioning request is a nine-line text file: eight mandatory fields followed by an optional starter-bonus marker.

**Detailed description**:
The reader is a flat sequence of nine `fgets(temp, 127, fp)` calls, each followed by `sscanf` (`CReadFiles.cpp:431-674`). Lines 1-5 are tokenized with `%s`/`%d` (id, password, real name, SSN1, SSN2); lines 6-8 (e-mail, telephone, address) are taken as the whole line with only the trailing byte removed (`len = strlen(temp); temp[len-1] = 0;`, `:579-581`, `:612-614`, `:645-647`), which relies on `fgets` retaining the newline; line 9 is read only if present and defaults to `"0"` when absent (`:668-673`). A short file therefore fails on the first missing mandatory line and - per BR-19 - is simply skipped, closed, and retried on every subsequent second with no log line at all.

Field order is positional and there is no header, no key=value syntax, no comments, no quoting, and no way to leave a middle field empty: an empty line yields `len == 0` and rejects the whole record (`:447`, `:478`, `:506`, `:588`, `:621`, `:654`). Whitespace inside the name, e-mail, telephone and address lines is preserved verbatim (they are copied from the raw line), but embedded whitespace cannot survive in the tokenized fields. File encoding is the process ANSI code page (`MultiByte` character set, `DBSrv.vcxproj:27`), so non-ASCII real names address bytes are copied into fixed-size ANSI arrays.

Because there is no transaction around the nine reads, a partially written file (an operator dumping a file while it is being copied) can be consumed in a truncated state; the length checks will reject it, but the file is left in place and the corrupt record will be rejected again on every tick until it is complete - at which point it succeeds. This gives the import a crude but real tolerance for in-progress copies, at the cost of up to one second of latency per retry and total silence about the condition.

**Rule workflow**:
```
open file (text mode)
line 1 -> id        (required, token)
line 2 -> password  (required, token)
line 3 -> real name (required, token)
line 4 -> ssn1      (required, integer, non-zero)
line 5 -> ssn2      (required, integer, non-zero)
line 6 -> e-mail    (required, raw line, newline stripped)
line 7 -> telephone (required, raw line, newline stripped)
line 8 -> address   (required, raw line, newline stripped)
line 9 -> bonus     (optional, integer, default 0)
AddAccount(...) -> on success DeleteFile(source) ; on failure keep source
```

---

### Business Rule: BR-10 Field-level validation and truncation policy

**Overview**:
Each imported account field has an explicit length window; the checks reject out-of-range values but use inconsistent boundary comparisons and truncate defensively after the fact.

**Detailed description**:
The accepted windows are: id `len != 0 && len < ACCOUNTNAME_LENGTH (16)` (`:447`); password `len != 0 && len < ACCOUNTPASS_LENGTH (16)` (`:478`); real name `len != 0 && len < REALNAME_LENGTH (24)` (`:506`); e-mail `len != 0 && len < EMAIL_LENGTH (48)` (`:588`); telephone `len != 0 && len < TELEPHONE_LENGTH (16)` (`:621`); address `len != 0 && len > ADDRESS_LENGTH (78)` - note the inverted comparison, which allows a length of exactly 78 (`:654`). SSN1 and SSN2 must be non-zero (`:533`, `:557`); zero is treated as "not supplied". No content validation exists for any field: e-mail is not checked for an `@`, telephone for digits, address for length semantics, or the identifier for allowed characters.

After each check, the code performs a belt-and-braces truncation: `id[14] = 0; id[15] = 0;` (`:459-460`), `pass[10] = 0; pass[11] = 0;` (`:488-489`), `name[22] = 0; name[23] = 0;` (`:516-517`). For the three raw-line fields the pre-truncation is done on the destination before the length test, not on the source: `email[47] = 0; email[46] = 0;` (`:583-584`), `tel[15] = 0; tel[14] = 0;` (`:616-617`), `addr[77] = 0; addr[76] = 0;` (`:649-650`). Since `strncpy` then overwrites those bytes with source content, the pre-truncation only helps when the source is shorter.

The address boundary is a genuine defect: with `len == 78`, `strncpy(addr, temp, 78)` (`:664`) fills the 78-byte array completely with source bytes, defeating the earlier `addr[77] = 0` and leaving a non-terminated string. That array is then passed to `AddAccount`, which does `strncpy(file.Info.Address, addr, ADDRESS_LENGTH)` (`CFileDB.cpp:117`), producing a persisted `Info.Address` with no terminator; any later `strlen`/`%s` on that field reads past the 78-byte member into adjacent structure bytes.

**Rule workflow**:
```
id:        reject if empty or >=16 ; force bytes 14,15 = 0 ; uppercase
password:  reject if empty or >=16 ; force bytes 14,15 = 0
realname:  reject if empty or >=24 ; force bytes 22,23 = 0
ssn1/ssn2: reject if 0
email:     strip newline ; reject if empty or >=48 ; copy 48 bytes
telephone: strip newline ; reject if empty or >=16 ; copy 16 bytes
address:   strip newline ; reject if empty or > 78 ; copy 78 bytes  <-- len 78 not terminated
```

---

### Business Rule: BR-11 Account-name canonicalization, reserved names, and path derivation

**Overview**:
Account names are uppercased, length-limited, screened against DOS device names, and used - unsanitized - as the final component of an on-disk path.

**Detailed description**:
`_strupr` is applied in all four ingest paths (`:207`, `:457`, `:794`, `:977`) and again inside the repository (`CFileDB.cpp:84`, `151`, `2398`, `2529`), making the account namespace case-insensitive by construction rather than by comparison logic (`GetIndex` uses a plain `strcmp` on already-canonical strings, `CFileDB.cpp:2344`). The only forbidden names are `COM0`-`COM9` and `LPT0`-`LPT9`, rejected by an explicit five-character pattern test in `AddAccount` (`:86-89`), `UpdateAccount` (`:153-156`), `DBWriteAccount` (`:2400-2403`), `DBReadAccount` (`:2531-2534`) and `DBExportAccount` (`:2481-2484`). That list covers Windows device-name aliasing for serial and parallel ports, but not `CON`, `PRN`, `AUX`, `NUL`, `CLOCK$`, or the `COM\u00b9`/`LPT\u00b9` superscript variants.

The directory key is derived from the first byte only: `BASE_GetFirstKey` (`Basedef.cpp:4114-4160`) returns the single uppercase or lowercase letter for ASCII names, a two-byte Korean initial for EUC-KR lead bytes, or the literal `"etc"` for anything else (digits, punctuation, and all out-of-range double-byte values). The resulting path is `sprintf(temp, "./account/%s/%s", First, check)` with no length check against `temp[128]` (`CFileDB.cpp:164`, `2411`, `2542`) - and `check` is the account name, which by BR-02 can be up to 255 bytes.

Two OS-level consequences follow directly. First, because a 16-byte name that overflows the 16-byte `ids` buffer is possible in the item and donation paths before any length check runs, arbitrary bytes can reach `check` and then the path. Second, because neither `/` nor `\` nor `..` is filtered, a crafted name such as `..\..\x` resolves the path outside the account tree entirely; `AddAccount` will refuse it only if that resolved file already exists (`:99-106`), and `DBWriteAccount` will happily create it (`:2413`). The practical reach is bounded by the requirement that the resolved path be a single trailing component of `./account/<First>/`, but that is still enough to create or clobber files elsewhere in the installation tree with a 7.6 KB binary record.

**Rule workflow**:
```
name = uppercase(raw token)
reject if empty or >= 16 bytes
reject if name matches COM[0-9] or LPT[0-9] (exact 5-byte pattern)
First = (A-Z/a-z) first char
        | Korean initial derived from lead/trail byte pair
        | "etc"
path  = "./account/" + First + "/" + name       # name not sanitized
        -> creation, read, and overwrite all use this resolved path
```

---

### Business Rule: BR-12 Starter bonus cargo grant

**Overview**:
A non-zero ninth line in an account import file triggers a fixed starter cargo load of 100 items on the new account.

**Detailed description**:
`ImportUser` reads the optional bonus line into an `int bonus` initialized to 0 (`CReadFiles.cpp:666-674`) and passes it to `AddAccount` as the last argument (`:677`). Inside `AddAccount`, after the account record has been built and before it is written, `if (bonus != 0)` fills cargo slots 0 through 99: indices 0-49 receive `sIndex = 401`, indices 50-99 receive `sIndex = 406` (`CFileDB.cpp:126-135`). Only the item index is set; all three effect slots remain zero because the record was zeroed at `:110`.

The rule is binary in effect - any value other than 0 produces the identical 100-item load, so the "bonus" field can never express an amount, a selection, or a rate. There is no validation that the two hardcoded item indexes exist in the item catalogue, no check that they are stackable or that 100 copies fit the intended economy, and no configuration mechanism: the numbers are literals in `CFileDB.cpp`, not constants in `Basedef.h` and not files. Because the grant uses the first 100 slots directly rather than the grid-fitting logic used by `ImportItem`, it also bypasses any multi-cell footprint consideration for those items.

Operationally this makes the ninth line a "premium account" flag. Any operator file that accidentally leaves a non-numeric ninth line results in `sscanf` failing and `bonus` staying 0, so the default is "no bonus" - which is the safe direction - but a file that ends with a stray digit from the address line will silently grant 100 items.

**Rule workflow**:
```
bonus_text = optional line 9 ; if absent -> "0"
bonus = atoi-style scan (failure -> 0)
AddAccount(..., bonus):
    if bonus != 0:
        for i in [0,100):
            Cargo[i].sIndex = (i < 50) ? 401 : 406
    DBWriteAccount(record)
```

---

### Business Rule: BR-13 Duplicate-account suppression on import

**Overview**:
Account import is create-only. An attempt to import an account that already exists is rejected by the repository, and the source file is retained and retried forever.

**Detailed description**:
`AddAccount` performs an existence probe by opening `./account/<First>/<NAME>` read-only and treating a successful open as "already exists" (`CFileDB.cpp:99-106`); on that path it returns FALSE without logging. `ImportUser` then simply does not delete the file: `if (ret != 0) ret = DeleteFile(filename);` (`CReadFiles.cpp:679-680`) with no `else` branch. There is no log line, no quarantine, and no counter, so a duplicate import is completely invisible except as a growing file count in `ImportUser` and one `_open` per file per second forever.

The rule is the correct primitive for a provisioning queue (import must not reset an existing player's password or character data), but its failure signalling is inverted: the single most common operator mistake - re-uploading an existing account - is the one case that produces neither a log nor a quarantine. Since `ImportUser` runs every second with no batch cap (BR-01), N duplicate files cost N `_open` calls plus N `fopen`/`fgets`/`sscanf` sequences per second, indefinitely.

The check is also time-of-check to time-of-use: the probe and the subsequent `DBWriteAccount` are separate `_open` calls, so two interns dropping the same account simultaneously (or an account created by `NPTool` between probe and write) can both pass the probe, with the second write silently overwriting the first. In practice both writers produce the same empty record, so the impact is limited to losing whichever bonus/SSN values the loser carried.

**Rule workflow**:
```
probe: _open("./account/<First>/<NAME>", O_RDONLY) == -1 ?
   no  -> return FALSE (exists)  ; ImportUser keeps the file ; no log ; retried every 1 s
   yes -> build zeroed record, apply bonus rule, DBWriteAccount
          success -> DeleteFile(source)
          failure -> keep source (also silent)
```

---

### Business Rule: BR-14 Password update files and unconditional deletion

**Overview**:
Per-server-group password reset requests are two-line files; the request file is deleted whether or not the reset succeeded.

**Detailed description**:
The watched directory is `../../Common/serv%2.2d/update/*.*` formatted with `ServerIndex` (`CReadFiles.cpp:55-56`, `918`), so each DB server only consumes requests addressed to its own group - a deliberate partitioning between server groups that share the `../../Common` tree. The record is `id` on line 1 and `pass` on line 2, with the same length windows as BR-10 (`:965`, `:996`). `UpdateUser` runs every second with no batch cap, so a bulk password reset of thousands of accounts executes inside a single timer tick.

Completion is unconditional: both the success branch and the failure branch call `DeleteFile(filename)` (`:1017-1023`, `1024-1030`), and both log with a `[WARNING]` suffix (`"-UPDATE USER SUCCESS - FILE DELETED [WARNING]"`, `"-UPDATE USER FAIL - FILE DELETED [WARNING]"`). `UpdateAccount` returns FALSE in three distinct situations - reserved name, account file missing, and write failure - all of which destroy the request. A typo in the account name therefore loses the reset silently apart from one log line, and a transient write failure (`EACCES`, disk full, antivirus lock) loses it too, with no retry.

There is also an in-memory side effect: when the account is currently logged in, `UpdateAccount` patches `pAccountList[Idx].File.Info.AccountPass` before touching disk (`CFileDB.cpp:173-176`), so the new password is visible to the session layer immediately. That patch occurs before the disk read, so if the subsequent read or write fails the in-memory cache holds a password that was never persisted - the next successful whole-record save by another path would either persist it accidentally or revert it.

**Rule workflow**:
```
for each file in ../../Common/servNN/update/*.*:
    parse id (1..15) and pass (1..11) ; malformed -> close, keep file, retry next second
    UpdateAccount(id, pass):
        reject COM/LPT names          -> FALSE
        require existing account file -> FALSE
        patch pAccountList[Idx] password (if logged in)
        DBReadAccount + DBWriteAccount
    DeleteFile(source)   # regardless of result
    log "-UPDATE USER SUCCESS/FAIL - FILE DELETED [WARNING]"
```

---

### Business Rule: BR-15 Guild information binary blob persistence

**Overview**:
The 65,536-element `GuildInfo` array is persisted as a single opaque 524,288-byte binary blob, read once at boot and rewritten in full on every guild update and at Sunday midnight.

**Detailed description**:
`STRUCT_GUILDINFO` is 8 bytes (`short Fame`, `char Clan`, `char Citizen`, `char Sub1..Sub3`, `Basedef.h:796-806`), and the global array `STRUCT_GUILDINFO GuildInfo[65536]` (`CFileDB.cpp:53`) therefore occupies 524,288 bytes. `ReadGuildInfo` opens `../../Common/GuildInfo` read-only/binary and issues one `_read(handle, GuildInfo, sizeof(GuildInfo))` (`CReadFiles.cpp:696-706`), discarding the return value. `WriteGuildInfo` opens the same path with `O_RDWR|O_CREAT|O_BINARY` and issues one `_write(handle, GuildInfo, sizeof(GuildInfo))` (`:713-723`), also discarding the return value. There is no header, magic, version, length field, checksum, or record count; the file is the memory image.

The write triggers are aggressively frequent. Every `_MSG_GuildInfo` received from any game server stores one element and immediately calls `WriteGuildInfo()` (`CFileDB.cpp:466-480`), so a single guild rename or fame change writes 512 KB synchronously on the UI thread. In a busy siege or a mass guild-creation burst this is a per-message 512 KB write, and because there is no coalescing, no dirty flag, and no deferred flush, several hundred guild messages in a second become several hundred full-blob writes. The second trigger is the Sunday-midnight branch: `when.tm_hour == 0 && when.tm_wday == 0 && when.tm_min == 0 && when.tm_sec == 0` (`Server.cpp:2061`), which requires a `WM_TIMER` tick to land exactly on second 0 of 00:00 - a condition that `SetTimer`'s 1000 ms resolution and normal scheduling drift do not guarantee, so the weekly write may simply never fire.

Both I/O paths replace a missing file with a modal `MessageBoxA(hWndMain, "no GuildInfo file", "BOOTING ERROR", MB_OK)` (`:700`, `:717`). On boot the read failure is non-fatal - the process continues with an all-zero guild table and an operator must dismiss a dialog - which means a missing or locked `GuildInfo` silently blanks every guild's fame and citizen data for the session, and the next guild message will persist that blank state over whatever was on disk. A truncated read is worse: `_read` returns a short count, the untouched tail of the array stays zero, and the next write persists the zeroed tail as authoritative.

**Rule workflow**:
```
boot:    _open("../../Common/GuildInfo", O_RDONLY|O_BINARY)
         fail -> MessageBoxA("no GuildInfo file","BOOTING ERROR") ; continue with zeros
         _read(524288 bytes) -> return value ignored
every _MSG_GuildInfo (any game server):
         GuildInfo[guild] = payload ; broadcast to TMSrv peers
         WriteGuildInfo() -> _open(O_RDWR|O_CREAT) ; _write(524288) ; _close
Sunday 00:00:00 (exact second):
         clear all Fame, broadcast, WriteGuildInfo()
```

---

### Business Rule: BR-16 Connection HTML publishing and population inflation

**Overview**:
Every 30 seconds the component rewrites `C:/wamp/www/servNN.htm` with one line per server slot, publishing a deliberately inflated population figure.

**Detailed description**:
The path is a hardcoded absolute path formatted with `ServerIndex` (`CReadFiles.cpp:39`, `64`), and the file is opened with `"wt"`, so each write truncates and replaces the previous content (`:66`). There is no HTML at all: the file contains `MAX_SERVER` (10) lines of plain integers and nothing else, meaning the `.htm` extension is a label for an include or a script-side fetch, and the consuming web page is not part of this repository (no `.php`, `.html` or `.htm` file exists anywhere in the tree). The contract with the consumer is therefore implicit and unversioned.

The per-line rule is: if `pUser[i].Mode == USER_EMPTY` write `-1`, otherwise write `((4 * pUser[i].Count / 3) + rand() % 4)` (`:73-82`). `Count` is the number of accounts currently registered on game server `i`, incremented in `CFileDB::AddAccountList` (`CFileDB.cpp:2213`) and decremented in `RemoveAccountList` (`:2228`) - it counts authenticated sessions from character select onwards, not players in game. The published value is therefore integer-divided to about 133 percent of the session count plus 0-3 random units; it is neither the true population nor a stable transformation of it, and two consecutive writes of the same underlying count can differ by up to 3. Whatever the web page labels this number, the number is not real.

Two silent behaviours accompany the rule. If `fopen` fails - WAMP not installed, the directory removed, permissions changed - the function returns without a log line (`:68-69`), so the published page simply stops updating with no operator signal. And when a slot is non-empty, the function also updates the peak tracker `UserConnection[i] = max(UserConnection[i], pUser[i].Count)` (`:79-80`), coupling the HTML publisher to the CSV sampler (BR-17); on an empty slot the peak is not updated, so a server that empties out keeps its last peak until the CSV consumes and resets it.

**Rule workflow**:
```
path = "C:/wamp/www/serv" + zero-padded ServerIndex + ".htm"
open "wt" ; failure -> silent return
for i in [0,10):
    if pUser[i].Mode == USER_EMPTY: write "-1\n"
    else: track peak(UserConnection[i]) ; write ((4*Count/3) + rand()%4) "\n"
close
```

---

### Business Rule: BR-17 Population CSV sampling and peak reset

**Overview**:
A 10-column-plus-total CSV row is appended every 15 minutes, carrying the maximum session count observed since the previous row, and then the peak counters are zeroed.

**Detailed description**:
`UpdateConnectionData` opens `../../Common/data%2.2d.csv` with `"a+"` (`CReadFiles.cpp:40`, `94`), writes a timestamp prefix `"%4.4d_%2.2d_%2.2d_%2.2d "` (year, month, day, hour) via `fprintf(fp, temp)` - a non-literal format string (`:104-106`), then for each of the 10 slots appends `",%4d  "` of `UserConnection[i]` and resets it to zero (`:113-122`), finally appends the total and a newline (`:124-125`). The file is never rotated, truncated, or size-checked, so it grows forever; the timestamp carries no delimiter that a CSV parser would recognize (underscores instead of separators, and no quoting), so it is a "CSV" only by comma count.

The cadence is 15 minutes, not an hour. `ProcessSecTimer` runs every second and enters the publishing block when `SecCounter % 30 == 0`, at which point `MinCounter++` happens; `MinCounter % 30 == 0` then fires the CSV write. Thirty 30-second intervals is 15 minutes, while `HourCounter++` is incremented and never read anywhere in the codebase. Four rows therefore share the same hour-precision timestamp, distinguishable only by their order in the file.

The peak semantics make the CSV a *high-water mark* series, not a time series of instantaneous counts: `UserConnection` is only raised by BR-16, and only for non-empty slots, so a row reports the busiest moment since the previous row and the reset at `:121` makes each row independent. If `fopen` fails, the counters are not reset (the function returns at `:96-97` before the loop), so the next successful row absorbs the missed interval - a self-healing property, but also one that silently compresses the series without any gap marker.

**Rule workflow**:
```
open ../../Common/dataNN.csv "a+"
timestamp = "YYYY_MM_DD_HH "        (hour precision)
append ",%4d  " for each slot from UserConnection[i] ; reset UserConnection[i] = 0
append ",%4d  " for total ; append "\n"
(no fflush, no error check on fprintf, no rotation)
```

---

### Business Rule: BR-18 Ranking file publishing

**Overview**:
The in-memory top-500 ranking is serialized as `position name classmaster class` lines, best first, filtered and renumbered, with the last ranked slot never emitted.

**Detailed description**:
`WriteRanking` opens `../../Common/Ranking.txt` with `"wt"` (`CReadFiles.cpp:1047`) and iterates `for (int i = RankPos::FIRST, pos = 1; i > RankPos::LAST; i--)` (`:1055`). With `FIRST = MAX_RANK_INDEX - 1 = 499` and `LAST = 0` (`CRanking.h:25-30`; `MAX_RANK_INDEX = 500`, `Basedef.h:61`), the loop walks indices 499 down to 1 - index 0 is never visited, so at most 499 of the 500 ranked players are written. Each visited element is skipped when `Name[0] == 0` or `Level >= 1000` (`:1057`), and otherwise emitted with a monotonically increasing `pos`, so the output is compacted and always starts at 1 regardless of which array slots are populated.

The label arrays are `classmaster[6] = {"SEM CLASSMASTER","M","A","C","C","SC"}` and `classe[4] = {"TK","FM","BM","HT"}` (`:1052-1053`). Because `RankingSystem::tryInsertInRanking` normalizes the class-master value before storing it - `MORTAL (2)` becomes 1, `ARCH (1)` becomes 2, `CELESTIAL (3)` stays 3 (`CRanking.cpp:224-228`) - the emitted label mapping is 0 = "SEM CLASSMASTER" (unset), 1 = "M" (mortal), 2 = "A" (arch), 3 = "C" (celestial). Entries 4 and 5 ("C", "SC") are unreachable through normal insertion. The class label indexes `classe` directly with `Class` (`:1061`) with no bounds check; `Class` is an `unsigned char` copied from account files (`Basedef.h:444`), so a corrupt or edited record with `Class >= 4` makes `fprintf` dereference an out-of-bounds pointer from the stack array.

Refreshes happen every 600 seconds from the timer (`Server.cpp:2041-2046`) and once from `RankingSystem::loadRanking` during static initialization (`CRanking.cpp:110`) - the latter before the working directory is normalized (BR-19, finding 9). The file is a derived projection only: it is never read back by any code in the repository, so a failed or misplaced write is invisible to DBSrv itself and affects only the external consumer.

**Rule workflow**:
```
open ../../Common/Ranking.txt "wt" ; failure -> silent return
pos = 1
for i = 499 down to 1:
    if Name[0] == 0: continue
    if Level >= 1000: continue
    fprintf("%d %s %s %s\n", pos, Name, classmaster[ClassMaster], classe[Class])
    pos++
close
(slot 0 never considered; Class not bounds-checked)
```

---

### Business Rule: BR-19 Failure handling and retry semantics

**Overview**:
The component has no error propagation, no retry policy, no backoff, and no health signal; failures are either logged once or silently dropped, and the only retry is the next scheduler tick over the same input.

**Detailed description**:
Every entry point returns `void`. Failures inside are expressed as (a) an early `return`, (b) a `Log(...)` call, or (c) nothing at all. Silent paths include `fopen` failure in `UpdateConnection` (`:68-69`) and `UpdateConnectionData` (`:96-97`), `fopen` failure of an import file (which is skipped and retried, `:170-178`, `:421-427`), and the missing `else` after `AddAccount` failure (`:679-680`). Logged paths include every parse rejection, every account-read/write failure, and every delete/move anomaly, with the message text varying in quality (see BR-07). Nothing increments a counter, sets a flag, or surfaces in `DrawConfig`.

Because the sources are files that remain in place, the effective retry policy is "the next tick, forever". Item and donation retries are bounded to one attempt per 30 seconds only by the batch cap, not by any per-file backoff; user-import retries happen once per second. The only permanent give-up mechanism is a successful relocation into a quarantine directory (BR-07), which eight of eleven call sites do not verify.

`Log` itself is a UI-coupled sink: it formats with `sprintf(LogTemp, "%s %s %s \n", LogTemp, str2, str1)` (`Server.cpp:2133`, source-overlapping `sprintf`), appends to the daily `.\Log\DB_*.txt` if `fLogFile` is open, and pushes the line to the window title (`SetWindowText`). A component that logs once per file per tick therefore also drives a window-message update per file per tick.

**Rule workflow**:
```
failure situation -> {silent return | Log(...) | both}
no status code, no metric, no metric reset, no backoff, no dead-letter after N attempts
next tick: enumerate again, re-process whatever is still present
```

---

### Business Rule: BR-20 Trust boundary: file drop is an unauthenticated privileged interface

**Overview**:
Local write access to three directories grants the ability to create accounts, mint arbitrary items, and credit arbitrary donations in the game database, with no authentication, no authorization, and no audit trail beyond a line in a text log.

**Detailed description**:
The component reads files from fixed relative paths anchored on the DBSrv working directory (`BASE_InitModuleDir` sets CWD to the executable's directory, `Basedef.cpp:2608-2629`, so `../../Common/...` resolves to the installation's `Common` folder). It performs no ownership check, no ACL inspection, no signature or HMAC verification, no extension or content-type filtering, and no limit on how much value a single file may convey. Whoever can create a file there - any local user with write permission on `Common`, any process running as the DBSrv account, a misconfigured share, a backup/restore tool, an FTP drop from a web shop - is by construction a trusted provisioning client.

The data path amplifies this. Account names, passwords, real names, SSN1/SSN2, e-mail, telephone and address all arrive from the file and are written verbatim into the account record (`CFileDB.cpp:112-122`), so the file drop is also a PII ingestion channel with no validation of the SSN format, no deduplication against existing players, and no consent/retention handling. The item channel accepts any index below 6500 and any 8-bit effect/value triple, including combinations that no gameplay path can produce. The donation channel accepts any non-negative 32-bit amount. The only guard rails are the reserved device names (BR-11), the duplicate-account probe (BR-13), and the field length windows (BR-10).

The path handling inside this boundary is the weakest point: account names are used as path components without sanitization (BR-11), so file content controls the resolved file system target of both the read and the write. Combined with the 16-byte `ids` buffer in the item and donation paths, a file drop is enough to overflow the stack and to redirect a 7.6 KB record write to an attacker-chosen relative path. Those two facts mean the boundary is not only unauthenticated but also memory-unsafe.

**Rule workflow**:
```
threat model:
   actor with write access to ../../Common/ImportUser   -> create accounts (any SSN/PII), grant 100-item bonus
   actor with write access to ../../Common/ImportItem   -> mint any item with any effect, long name overflows stack
   actor with write access to ../../Common/ImportDonate -> credit any amount (int range), overwrite collision
   actor with write access to ../../Common/servNN/update-> force any account password change (then delete request)
   actor with write access to ../../Common/GuildInfo    -> rewrite all guild fame/citizen state
controls present: none (no auth, no ACL check, no signature, no rate limit, no size limit, no audit)
```

---

## 4. Component Structure

```
Code/DBSrv/
├── CReadFiles.h                  # 56 lines - class declaration only:
│                                 #   14 private static const char* path constants
│                                 #   9 public static void methods
│                                 #   no data members, no constructor, no vtable
├── CReadFiles.cpp                # 1,066 lines - 8 free-standing adapters:
│   ├── path constants            # :39-58   (14 literals, all referenced from code)
│   ├── UpdateConnection()        # :60-128  HTML publisher (C:/wamp/www/servNN.htm)
│   ├── UpdateConnectionData()    # :88-128  CSV sampler      (../../Common/dataNN.csv)
│   ├── ImportItem()              # :130-378 item grant ingest (ImportItem -> Error)
│   ├── ImportUser()              # :380-690 account create  (ImportUser -> ./account/...)
│   ├── ReadGuildInfo()           # :692-707 guild blob load  (../../Common/GuildInfo)
│   ├── WriteGuildInfo()          # :709-724 guild blob store (../../Common/GuildInfo)
│   ├── ImportDonate()            # :726-908 donation ingest  (ImportDonate -> ImportDonateError)
│   ├── UpdateUser()              # :910-1041 password reset  (servNN/update)
│   └── WriteRanking()            # :1043-1066 ranking export (../../Common/Ranking.txt)
├── CFileDB.cpp/.h                # callee: account store, pAccountList, GuildInfo, DBRead/Write
├── CUser.cpp/.h                  # callee: pUser[] slot state (Mode, Count, cSock)
├── CRanking.cpp/.h               # callee: rankingSystem, RankPos; caller of WriteRanking
├── Server.cpp/.h                 # caller: ProcessSecTimer, WinMain boot, Log, globals
├── DBSrv.vcxproj                 # build: MultiByte, /W3, static CRT, no PCH
└── stdafx.h, resource.h, *.rc    # unrelated to this component
```

Structural observations:

* **No encapsulation boundary.** The class exposes only static public methods and declares its path constants `private`, but it has no invariants to protect; the `private` is cosmetic. There is no way to construct a `CReadFiles` with a different base directory, clock, or repository, which is precisely why it is untestable (section 11).
* **One translation unit, eight independent concerns.** HTML publishing, CSV sampling, three ingest queues, one blob store, and one export share a file but no code: the only intra-file reuse is the repeated log-and-`_findnext` block, which appears seven times inline rather than as a helper.
* **Configuration is compile-time only.** All 12 format strings are `static const char* const` members initialized at `:39-58`. The only runtime variability is `ServerIndex` substituted into `serv%2.2d/update`, `data%2.2d.csv` and `serv%2.2d.htm`. `Config.txt` (`Server.cpp:395-406`) contains only `Sapphire` and `LastCapsule`, so no deployment can change a path without recompiling.
* **Two distinct path anchoring styles coexist.** Four artefacts use `../../Common/...` (relative to the executable, resolved through the process CWD), two use `C:/wamp/www/...` (absolute, machine-specific, Windows-drive-specific), and the account store uses `./account/...` (relative to the executable). The web target is the only one that escapes the installation tree.
* **Header is minimal and stable.** It includes only `<stdio.h>` and `..\Basedef.h`; every real dependency (`CFileDB`, `CUser`, `Server`, `CRanking`) is pulled in by the implementation file (`CReadFiles.cpp:32-37`), which keeps the header cheap to include but hides the component's true fan-out from any consumer that only reads the header.

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (efferent, outbound)

```
CReadFiles
  ├─► CFileDB (cFileDB, 1 instance, global in Server.cpp:89)
  │     ├─ GetIndex(char*)                CReadFiles.cpp:235, 812          (linear scan, 10,000 slots)
  │     ├─ DBReadAccount(STRUCT_ACCOUNTFILE*)  :267, 843                 (whole-record read)
  │     ├─ DBWriteAccount(STRUCT_ACCOUNTFILE*) :334, 864                 (whole-record write)
  │     ├─ AddAccount(...)                :677                           (create-only, 9 args)
  │     ├─ UpdateAccount(char*,char*)     :1015                          (password only)
  │     └─ pAccountList[MAX_DBACCOUNT]    :237, 814                       (direct member access)
  │
  ├─► CUser (pUser[MAX_SERVER], global in Server.cpp:82)
  │     ├─ .Mode  (USER_EMPTY test)       :73
  │     ├─ .Count (population + peaks)    :79-80, 82, 115
  │     └─ .cSock.SendOneMessage(...)     :253, 829     (through CPSock)
  │
  ├─► CRanking (rankingSystem, global in CRanking.cpp:29)
  │     ├─ grindRanking.getElement(i)     :1057-1061
  │     ├─ STRUCT_RANKING::Name/ClassMaster/Class/Level
  │     └─ RankPos::FIRST / RankPos::LAST :1055
  │
  ├─► Server.cpp globals/functions
  │     ├─ ServerIndex                    :64, 92, 918, 940
  │     ├─ UserConnection[MAX_SERVER]     :79-80, 115-121
  │     ├─ GuildInfo[65536]               :705, 722   (declared in CFileDB.h:85, defined CFileDB.cpp:53)
  │     ├─ hWndMain                       :700, 717   (MessageBoxA owner)
  │     ├─ Log(char*, char*, uint)        :191, 205, 213, 255, 274, 318, 321, 338, 356, 365, 367,
  │     │                                              778, 792, 800, 831, 850, 868, 884, 895, 897,
  │     │                                              1022, 1029          (37 call sites)
  │     └─ rand()                         :82          (CSV/HTML jitter; CRT global RNG state)
  │
  └─► Basedef (shared types and helpers)
        ├─ BASE_CanCargo(STRUCT_ITEM*, STRUCT_ITEM*, int, int)   :288   (defined Basedef.cpp:4480)
        ├─ BASE_GetFirstKey(char*, char*)                        (indirect, through CFileDB)
        └─ STRUCT_ITEM, STRUCT_ACCOUNTFILE, MSG_DBSendItem, MSG_DBSendDonate,
           MAX_SERVER, MAX_USER, MAX_DBACCOUNT, MAX_ITEMLIST, ACCOUNTNAME_LENGTH,
           ACCOUNTPASS_LENGTH, REALNAME_LENGTH, EMAIL_LENGTH, TELEPHONE_LENGTH,
           ADDRESS_LENGTH, MAX_CARGO, CARGOGRIDX, CARGOGRIDY, USER_EMPTY, MAX_RANK_INDEX
```

### 5.2 External dependencies (platform)

| Dependency | Version / form | Purpose | Failure behaviour |
|------------|----------------|---------|-------------------|
| MSVCRT file API (`fopen`,`fgets`,`sscanf`,`fprintf`,`fclose`) | static CRT (`MultiThreaded`/`MultiThreadedDebug`), MSVC v143 | text ingest and text egress | NULL return checked in most, ignored in `fprintf`; `fclose` never checked |
| MSVCRT low-level IO (`_open`,`_read`,`_write`,`_lseek`,`_close`,`_filelength`) | same | binary account and guild persistence (delegated to `CFileDB`) | checked for `-1`, short reads not checked |
| MSVCRT directory scan (`_findfirst`,`_findnext`,`_findclose`) | same | watched-directory enumeration | `-1` means "nothing to do"; no distinction between empty and error |
| Win32 `DeleteFile` / `MoveFile` / `GetLastError` | kernel32 | source-file lifecycle and quarantine | return checked in 3 of 11 `MoveFile` sites |
| Win32 `MessageBoxA` | user32 | boot error for missing `GuildInfo` | blocks the message loop; no non-interactive mode |
| Win32 `GetLastError` | kernel32 | one error code captured at `:316`, then unused in the message | diagnostic only |
| C runtime `rand()` | CRT global state, unseeded in this path | population jitter | shared global RNG, not thread-safe by contract |
| `_strupr` | CRT | account canonicalization | locale/ANSI dependent |
| Filesystem layout (`../../Common`, `C:/wamp/www`, `./account`) | deployment convention | all ingress/egress | missing directory = silent no-op or silent retry |
| External web server (WAMP) | not in repository | consumer of `servNN.htm` | absent server = file written to a dead directory |

### 5.3 Runtime dependency chains

```
WM_TIMER(TIMER_SEC)
  -> ProcessSecTimer                        (Server.cpp:2013)
       -> ImportUser      -> CFileDB::AddAccount -> DBWriteAccount -> ./account/<First>/<NAME>
       -> UpdateUser      -> CFileDB::UpdateAccount -> DBReadAccount + DBWriteAccount -> same
       -> ImportItem      -> CFileDB::GetIndex -> pAccountList
                          -> pUser[svr].cSock.SendOneMessage -> _MSG_DBSendItem -> TMSrv
                          -> CFileDB::DBReadAccount + DBWriteAccount -> same
                          -> BASE_CanCargo -> g_pItemGrid / g_pItemList (Basedef)
       -> ImportDonate    -> CFileDB::GetIndex ; pUser[svr].cSock ; DBRead/WriteAccount
       -> UpdateConnection      -> pUser[].Mode/Count -> C:/wamp/www/servNN.htm
       -> UpdateConnectionData  -> UserConnection[]   -> ../../Common/dataNN.csv
       -> WriteRanking          -> rankingSystem.grindRanking -> ../../Common/Ranking.txt
       -> WriteGuildInfo        -> GuildInfo[65536]   -> ../../Common/GuildInfo

WSA_READDB on guild message
  -> CFileDB::ProcessMessage case _MSG_GuildInfo        (CFileDB.cpp:455-481)
       -> GuildInfo[guild] = payload ; SendGuildInfo() to peers ; CReadFiles::WriteGuildInfo()

static initialization (before WinMain)
  -> RankingSystem::RankingSystem() -> loadRanking() -> readAccountsInDir("./account/*")
       -> CReadFiles::WriteRanking() -> ../../Common/Ranking.txt   (CWD not yet normalized)
```

---

## 6. Afferent and Efferent Coupling

Counted at C++ scope: `CReadFiles` is the only class in the file set; "afferent" is the number of distinct translation units (and call sites) that reference it, "efferent" is the number of distinct code entities it depends on. The architecture report estimates ~12 afferent / 4 efferent; the measurements below are per method and per entity.

### 6.1 Per-method coupling

| Method | Afferent callers | Afferent call sites | Efferent entities | Criticality |
|--------|------------------|---------------------|-------------------|-------------|
| `ImportItem` | `Server.cpp` (boot + timer) | 2 | 11 (`cFileDB` x4, `pUser`, `BASE_CanCargo`, `Log` x11, `MoveFile`/`DeleteFile`, `_findfirst`/`_findnext`, `sscanf`, `fopen`/`fgets`) | High |
| `ImportUser` | `Server.cpp` (timer) | 1 | 5 (`cFileDB::AddAccount`, `DeleteFile`, `_find*`, `sscanf`, `_strupr`) - no `Log` call at all | High |
| `ImportDonate` | `Server.cpp` (timer) | 1 | 11 (same shape as `ImportItem`, plus `pUser` send, `Log` x9) | High |
| `UpdateUser` | `Server.cpp` (timer) | 1 | 5 (`cFileDB::UpdateAccount`, `Log` x2, `DeleteFile` x2, `_find*`, `_strupr`) | Medium |
| `WriteRanking` | `Server.cpp` (timer), `CRanking.cpp` (static init) | 2 | 4 (`rankingSystem`, `RankPos`, `fopen`/`fprintf`, `Log` indirect) | Medium |
| `WriteGuildInfo` | `Server.cpp` (timer), `CFileDB.cpp` (per guild msg) | 2 | 5 (`GuildInfo`, `hWndMain`, `MessageBoxA`, `_open`/`_write`, `_close`) | High (write amplification) |
| `ReadGuildInfo` | `Server.cpp` (boot) | 1 | 4 (`GuildInfo`, `hWndMain`, `MessageBoxA`, `_open`/`_read`) | High (boot) |
| `UpdateConnection` | `Server.cpp` (timer) | 1 | 5 (`ServerIndex`, `pUser[].Mode/Count`, `UserConnection`, `rand`, `fopen`) | Low |
| `UpdateConnectionData` | `Server.cpp` (timer) | 1 | 4 (`ServerIndex`, `UserConnection`, `localtime`, `fopen`) | Low |

### 6.2 Component-level coupling metrics

| Component (C++ scope) | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Instability (Ce/(Ca+Ce)) | Critical | Notes |
|-----------------------|------------------------|------------------------|--------------------------|----------|-------|
| `CReadFiles` (class) | 3 dependents / 11 call sites | 8 project entities + 8 platform APIs | 0.73 (entity basis) | High | Zero cohesion between methods; static-only, no polymorphic surface |
| `CFileDB` (as seen from here) | 3 call sites from `CReadFiles` | - | - | High | Repository is the only persistence path used by imports |
| `CUser` (as seen from here) | 3 field reads + 2 sends from `CReadFiles` | - | - | Medium | `Mode`/`Count` read without synchronization (single thread only) |
| `CRanking`/`RankingSystem` | 1 read-only consumer (`WriteRanking`) + 1 caller of `WriteRanking` | - | - | Low-Medium | Bidirectional: `CRanking` calls `CReadFiles`, `CReadFiles` reads `rankingSystem` |
| `Server.cpp` globals (`ServerIndex`, `UserConnection`, `hWndMain`, `Log`) | 7 direct uses | - | - | High | Ambient coupling: 3 of 7 globals are read without validation |
| `GuildInfo[65536]` | 1 reader + 1 writer | - | - | High | 512 KB shared mutable global, no ownership boundary |
| `Log` free function | 22 call sites in this file | - | - | Medium | Remaining in the class after everything else, i.e. logging dominates the coupling surface |

### 6.3 Cohesion assessment

| Measure | Value | Interpretation |
|---------|-------|----------------|
| LCOM-style method cohesion | Near zero - the 9 methods share only `Log` and path constants; no method calls another | The class is a namespace, not a cohesive object |
| Public surface | 9 methods, 0 fields, 0 constructors | No substitutability, no lifecycle, no state machine |
| Data coupling | Every method takes no parameters and returns `void`; all inputs and outputs are ambient (files + globals) | Maximum implicit coupling, minimum explicit contract |
| Temporal coupling | `ImportItem` must run after `ServerIndex` resolution; `UpdateConnection(Data)` must run after login activity; `WriteRanking` must run after `rankingSystem` construction | Enforced only by call order in `WinMain`/`ProcessSecTimer`, never by code |
| Bidirectional dependency | `CRanking.cpp:110` -> `CReadFiles::WriteRanking`; `CReadFiles.cpp:1057` -> `rankingSystem` | Cycle at file level (`CReadFiles` <-> `CRanking`) |

---

## 7. Endpoints

**No network endpoints exist in this component.** `CReadFiles` registers no socket, exports no handler, and is not reachable from any peer over a protocol. Its entire external surface is the file system, and for completeness those surfaces are enumerated here in the endpoint table format, since they are the interfaces that external actors actually drive.

| Surface | Direction | Trigger | Payload | Response |
|---------|-----------|---------|---------|----------|
| `../../Common/ImportUser/*.*` | inbound (queue) | polled every 1 s | 9-line text record | account file created; source deleted, or silently retained |
| `../../Common/ImportItem/*.*` | inbound (queue) | polled every 30 s, max 10/tick | 1-line, 8-token record | item placed in cargo (disk + live TMSrv); source deleted or quarantined |
| `../../Common/ImportDonate/*.*` | inbound (queue) | polled every 30 s, max 10/tick | `ACCOUNT AMOUNT` | `Donate` increased (disk + live TMSrv); source deleted or quarantined |
| `../../Common/servNN/update/*.*` | inbound (queue) | polled every 1 s | 2-line record (id, pass) | password rewritten; source always deleted |
| `../../Common/Error/*` | outbound (dead letter) | item import rejection | original file | - |
| `../../Common/ImportDonateError/*` | outbound (dead letter) | donation rejection | original file | - |
| `C:/wamp/www/servNN.htm` | outbound (publish) | every 30 s | 10 integer lines | consumed by external web server (not in repo) |
| `../../Common/dataNN.csv` | outbound (append) | every 15 min | 1 header-less CSV row | consumed by external reporting (not in repo) |
| `../../Common/Ranking.txt` | outbound (publish) | every 600 s + startup | up to 499 lines | consumed by external ranking page (not in repo) |
| `../../Common/GuildInfo` | in/out (blob) | boot read; write per guild msg + Sunday 00:00:00 | 524,288-byte raw array | consumed by this component only |
| `./account/<FirstKey>/<NAME>` | in/out (indirect, via `CFileDB`) | per imported account/item/donation/update | `STRUCT_ACCOUNTFILE` (~7.6 KB) | the game's primary data store |
| `_MSG_DBSendItem` (TCP 7514 to TMSrv) | outbound (protocol) | item import while player online | `MSG_DBSendItem{ID, Account, Item}` | `Result` reply ignored by this component |
| `_MSG_DBSendDonate` (TCP 7514 to TMSrv) | outbound (protocol) | donation import while player online | `MSG_DBSendDonate{ID, Account, Donate}` (Result uninitialized) | `Result` reply ignored by this component |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol / mechanism | Data format | Error handling |
|-------------|------|---------|----------------------|-------------|----------------|
| `CFileDB` account store | Internal service (repository) | create accounts, read-modify-write records, password reset | in-process static calls -> `_open`/`_read`/`_write` | raw `STRUCT_ACCOUNTFILE` (~7.6 KB, no header) | return-code checked; short reads and short writes not detected; no rollback |
| `pUser[]` / `CPSock` TMSrv link | Internal service (transport) | live item and donation delivery | `SendOneMessage` -> TCP 7514, CPSock framing/obfuscation | `MSG_DBSendItem`, `MSG_DBSendDonate` (binary, `_MSG` header) | return value ignored; no ack correlation; peer rejections dropped |
| `rankingSystem` (`CRanking`) | Internal service (domain) | source of the published ranking | direct in-memory access to `grindRanking` | `STRUCT_RANKING{Name[32], Value, ClassMaster, Level, Class}` | none; `getElement` may return NULL outside [LAST,FIRST] |
| `BASE_CanCargo` (`Basedef`) | Internal utility | cargo grid fit test | in-process function call | 9x14 occupancy grid derived from `EF_GRID` masks | boolean; no diagnostic |
| `GuildInfo[65536]` | Shared global | guild fame/citizen state | direct array access + binary blob file | 8-byte records x 65,536 | none; short read leaves zeros |
| Watched import directories | Operator tooling (file drop) | account/item/donation/password provisioning | directory enumeration (`_findfirst`/`_findnext`) | ad-hoc text (see BR-02/BR-08/BR-09) | quarantined or retried; 4 of 6 moves unverified |
| WAMP web root `C:/wamp/www` | External service (web server) | publish per-server population page | file write into the web server's document root | plain text with `.htm` extension, 10 lines | silent if `fopen` fails |
| `../../Common/dataNN.csv` | External reporting | population history | append text | custom CSV, hour-granularity timestamp | silent if `fopen` fails; write errors ignored |
| `../../Common/Ranking.txt` | External ranking page | top-player publication | truncate-and-write text | `<pos> <name> <cm> <class>` | silent if `fopen` fails |
| `../../Common/GuildInfo` | Deployment artefact | guild persistence across restarts | raw binary blob | 524,288 bytes, no header | `MessageBoxA` on open failure; read/write counts ignored |
| Operating system file system | Platform | all of the above | Win32/WinCRT | - | `errno`/`GetLastError` used in a minority of paths |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose | Consequence |
|---------|----------------|----------|---------|-------------|
| Static service class / namespace object | `class CReadFiles` with 9 `static void` methods, 0 fields | `CReadFiles.h:26-54` | Group file adapters without instantiation | No dependency injection, no state isolation, no test seams |
| Integration adapter (file-drop queue) | Directory polling + delete-on-success | `ImportItem`, `ImportUser`, `ImportDonate`, `UpdateUser` | Decouple operator tooling from the game's TCP protocol | Pull-based, no notification, no per-item ack, at-least-once |
| Repository (delegated) | All persistence via `cFileDB.DBReadAccount`/`DBWriteAccount`/`AddAccount`/`UpdateAccount` | `CReadFiles.cpp:267`, `334`, `677`, `864`, `1015` | Keep binary format knowledge out of the adapters | Adapters still compose paths through account names, couples to layout |
| Read-modify-write (whole aggregate) | Load entire `STRUCT_ACCOUNTFILE`, mutate one field, store entire aggregate | `:259-334`, `:835-864` | Simplicity given a flat binary record | Lost updates, duplication, no partial-failure safety |
| Dead-letter channel | `MoveFile` to `../../Common/Error` / `ImportDonateError` | `:44`, `:53`, `:189-213`, `:776-800` | Preserve rejected requests for operators | Target directory never created; move result often ignored |
| At-least-once delivery with a "delivered" short-circuit flag | `RunTimeSent` global within the function, consulted in every failure branch | `:233`, `256`, `271`, `340`, `810`, `832`, `847`, `870` | Avoid quarantining a request already pushed to a live player | File stays forever; repeated live delivery |
| Polling scheduler | `WM_TIMER` at 1 s; modulo counters for 30 s / 15 min / 10 min | `Server.cpp:613`, `2013-2046` | Single-threaded, lock-free periodic work | No jitter, no drift compensation, missed edge triggers (Sunday midnight) |
| Snapshot publishing | Truncate-and-rewrite whole artefact each cycle | `:66`, `94`, `1047` | Derived artefacts always fully regenerated | Readers can observe empties/partials mid-write; no atomic rename |
| Creational collision guard | Existence probe before create | `CFileDB.cpp:99-106` | Create-only semantics for provisioning | TOCTOU window; silent failure with retained file |
| Inflated public metric | `(4*Count/3) + rand()%4` | `:82` | Deliberate obfuscation of real population | Published value is knowingly false and non-deterministic |
| Global-state access | Direct reads of `pUser[]`, `UserConnection[]`, `GuildInfo[]`, `ServerIndex`, `hWndMain` | throughout | Avoid plumbing | Hidden coupling; correctness depends entirely on call order |
| Compile-time configuration | 12 `static const char* const` path literals | `:39-58` | Zero-config deployment | Paths cannot be changed without rebuild; one is an absolute Windows path |

Architectural decomposition as observed: the component is best classified as an **Integration Adapter layer** with two sub-roles that are mixed in one file - a *message translator* (text file -> `STRUCT_ACCOUNTFILE` mutation -> binary record / protocol message) and a *projection publisher* (globals -> HTML/CSV/ranking). These two roles have incompatible reliability requirements (the first must not lose or duplicate; the second may be lossy and eventually consistent), yet they share the same error strategy: log-and-continue.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | `ImportItem` / `ImportDonate` input parsing (`:180-201`, `:767-788`) | `sscanf("%s ...")` writes an unbounded token into `char ids[16]`; a longer token overflows the stack frame, then `_strupr` walks the overflowed buffer | Local memory corruption reachable by any actor able to create an import file; no length check precedes the copy |
| High | Trust boundary (BR-20) | Import directories require no authentication, ownership check, signature, extension filter, or rate limit; file content becomes account PII, item stats, or money | Anyone with write access to `../../Common/Import*` can create accounts, mint items, credit donations |
| High | Path construction (`CFileDB.cpp:97`, `164`, `2411`, `2542`) fed from `:201`/`:788` | Account name is used verbatim as a path component; `..`, `/`, `\` are not filtered | Relative-path traversal allows creating/overwriting a ~7.6 KB binary record outside `./account` |
| High | `RunTimeSent` suppression (`:269-282`, `:340-350`, `:845-880`) | A file is retained when the live delivery succeeded but persistence failed, and is re-processed every 30 s with no attempt counter or key | Repeated item minting / donation credit; unbounded log growth; player inventory divergence |
| High | `UpdateUser` (`:1017-1030`) | Source file deleted on both success and failure; three distinct failure causes (reserved name, missing account, write error) are indistinguishable in the log | Permanent loss of password-reset requests with no retry |
| High | `WriteGuildInfo` (`:709-724`, called from `CFileDB.cpp:480`) | Full 512 KB synchronous rewrite on every guild message, on the UI thread | Write amplification under guild churn; per-message latency spike affecting DB protocol service |
| High | `ReadGuildInfo` (`:700-706`) | `_read` return ignored; missing file only raises a modal `MessageBoxA` and boot continues with an all-zero guild table | Silent loss of all guild fame/citizen state after a restart, then persisted over the good file |
| Medium | `ImportUser` handle management (`:688-689`) | `_findclose` is called only `if (fp != NULL)`; a failure to open the last candidate leaks the find handle | Handle exhaustion over time; imports silently stop when `_findfirst` starts returning `-1` |
| Medium | `ImportUser` / `UpdateUser` (`:395-686`, `:925-1037`) | No batch cap, runs every second; each record performs an existence probe plus a 7.6 KB create/write on the UI thread | A large drop stalls the message loop and all DB request servicing for the duration |
| Medium | Quarantine policy (`:189`, `211`, `776`, `798`) | `MoveFile` return ignored; the error directories are never created by the process | A malformed file is retried at 30 s intervals forever, one log line per retry |
| Medium | `ImportUser` failure signalling (`:677-680`) | `AddAccount` failure (already exists, or write error) produces no log and no quarantine; file is retained | Duplicate provisioning attempts are invisible; permanent 1 Hz I/O churn per duplicate file |
| Medium | Address boundary (`:654`, `:664`) | `len > ADDRESS_LENGTH` instead of `>=`, so a 78-byte address is copied with `strncpy(..., 78)` and left unterminated in a 78-byte array | Non-terminated `Info.Address` persisted; later `%s`/`strlen` reads into adjacent record bytes |
| Medium | Path buffers (`:384`, `:397`, `:900`, `:913`) | `sprintf` of `"../../Common/…/%s"` into `char filename[256]` / `[128]`; `_finddata_t::name` can be up to 259 bytes | Stack overflow with a long file name in the watched directory |
| Medium | Atomicity (all publishers and writers) | No temp-file + rename, no lock, no flush; every artefact is truncated and rewritten in place | Web server, operators, and `RankingSystem` can read empty or partially written files |
| Medium | Static-initialization order (`CRanking.cpp:29`, `96`, `110` vs `Server.cpp:443`) | `WriteRanking` and `loadRanking` execute before `BASE_InitModuleDir()` | Boot-time ranking load and first write resolve against the launcher's CWD; may load 0 accounts and write to the wrong tree |
| Medium | Guild weekly trigger (`Server.cpp:2061`) | Requires `WM_TIMER` to fire exactly at second 0 of Sunday 00:00:00 | The weekly guild flush may never execute; no catch-up flag exists |
| Medium | Population inflation (`:82`) | Published HTML value is `~133%` of the real session count plus 0-3 jitter | Any external consumer treating the number as population is systematically misled; not reproducible between writes |
| Medium | CSV shape (`:104-125`) | Hour-granularity underscore timestamp, unquoted padded fields, 15-minute cadence, infinite growth, `fprintf(fp, temp)` with a non-literal format | Not machine-parseable as standard CSV; 4 rows per indistinguishable hour; unbounded file |
| Medium | `C:/wamp/www` hardcoded (`:39`) | Absolute Windows path to a third-party web root; failure to open is silent | No deployment portability; publishing stops with no signal if WAMP is moved/absent |
| Low | Item validation (`:209`) | Effect *values* unvalidated (silently truncated to 8 bits); `Index == 0` accepted; `sscanf` result unchecked | Malformed records consume a cargo slot with an empty item and are then deleted |
| Low | `MSG_DBSendDonate` uninitialized (`:819`) | No `memset`, unlike the item path; `Result` carries stack bytes to TMSrv | 4 bytes of stack information sent on a trusted link; latent defect if a consumer reads `Result` |
| Low | Ranking off-by-one (`:1055`) | `i > RankPos::LAST` stops at 1; slot 0 is never emitted | At most 499 of 500 ranked players ever appear in the file |
| Low | Ranking label indexing (`:1061`) | `classe[Class]` with `Class` from a record, no bounds check | Out-of-bounds read of a stack pointer table if a corrupted record has `Class >= 4` |
| Low | Log correctness (`:205`, `:213`, `:318`, `:354`) | Wrong text on validation failure ("no contents"), a log line after a failed move, and a stale loop index used as the placed slot | Misleading operational diagnostics |
| Low | `HourCounter` (`Server.cpp:2037`) | Incremented, never read; the CSV is labelled hourly but is 15-minute | Documentation/comment drift; no functional effect |
| Low | Duplicated `fp == NULL` check (`:108-109`) | Dead second null check after `fprintf` | Noise; illustrates unverified write path (`fprintf` return never checked) |
| Low | `_strupr` + ANSI (`MultiByte`) | Case folding and Korean directory keys depend on the process ANSI code page | Locale-dependent account identity; non-ANSI names collapse to `"etc"` |
| Low | `Log` implementation (`Server.cpp:2133`) | `sprintf(buf, "%s %s %s\n", buf, ...)` overlaps source and destination | Undefined behaviour per the C standard; 37 call sites in this component depend on it |

---

## 11. Test Coverage Analysis

### 11.1 Search performed

A repository-wide verification was executed over the entire project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`, `.git`, and the MSBuild output folders (`Debug`, `Release`, `DBSrv/Release`).

| Check | Command / pattern | Result |
|-------|-------------------|--------|
| Test projects in the solution | `Select-String '^Project' "W2PP Code Project.sln"` | 2 code projects only: `DBSrv`, `TMSrv` (plus a "Solution Items" folder). No test project. |
| Test-named files anywhere | recursive `Get-ChildItem -Include '*test*','*Test*','*spec*'` | **0 files** |
| Test framework symbols in sources | grep `gtest|CppUnit|doctest|CATCH_CONFIG|BOOST_TEST|TEST_F|TEST_CASE|unittest(\)` over `Code/**/*.{cpp,h,vcxproj,md}` | **0 matches in `Code/`** (hits exist only inside `docs/reports/*` prose) |
| Assertions as substitute | grep `assert(` over `Code/**` | **0 matches** |
| Build systems / CI | `Test-Path CMakeLists.txt`; recursive `*.yml`, `*.yaml`; `.github`, `.gitlab`, `.circleci` directories | **none** (MSBuild `.sln`/`.vcxproj` only) |
| Test/verification fixtures for imports | grep `ImportItem|ImportUser|ImportDonate|Ranking.txt|GuildInfo` outside `Code/` and `docs/` | **0 files** - no sample records, no golden files, no fixture directory |
| Harness entry points | grep for `main(`, `WinMain(`, `--test`, `argv` parsing in DBSrv | only the production `WinMain` (`Server.cpp:427`); no headless or test mode |

### 11.2 Per-function coverage assessment

| Function | Unit tests | Integration tests | Manual verification aids in code | Coverage | Test quality |
|----------|------------|-------------------|----------------------------------|----------|--------------|
| `ImportItem` | 0 | 0 | `Log` lines per branch (`:191`, `205`, `213`, `255`, `274`, `318`, `321`, `338`, `356`, `365`, `367`) | 0% automated | Branches are distinguishable only by log text; the "no contents" text is shared by three different causes, so even manual triage is ambiguous |
| `ImportUser` | 0 | 0 | essentially none: success is silent, duplicate/failure is silent (`:677-680`) | 0% automated | Weakest observability in the component; only side effects (created account files) are visible |
| `ImportDonate` | 0 | 0 | `Log` at `:778`, `792`, `800`, `831`, `850`, `868`, `884`, `895`, `897` | 0% automated | Same message-collision defect as items; no assertion on the resulting `Donate` value |
| `UpdateUser` | 0 | 0 | `Log` at `:1022`, `1029`, both labelled `[WARNING]` | 0% automated | Success and failure differ only by the word SUCCESS/FAIL; the account file is not re-read to confirm |
| `ReadGuildInfo` | 0 | 0 | `MessageBoxA` on open failure only (`:700`) | 0% automated | A truncated file produces no signal at all |
| `WriteGuildInfo` | 0 | 0 | `MessageBoxA` on open failure only (`:717`) | 0% automated | Write success/short-write is never observed |
| `UpdateConnection` | 0 | 0 | none (silent `fopen` failure) | 0% automated | Output is intentionally non-deterministic (`rand()`), so it is not even snapshot-testable as written |
| `UpdateConnectionData` | 0 | 0 | none | 0% automated | Append-only file grows forever; no fixture reset path |
| `WriteRanking` | 0 | 0 | indirect `Log` from `RankingSystem::loadRanking` (`CRanking.cpp:107-108`) | 0% automated | Ordering, filtering and the slot-0 omission are untested |

### 11.3 Structural testability

| Obstacle | Detail | Effect |
|----------|--------|--------|
| No injectable inputs | Every method takes zero arguments; inputs are the process CWD plus fixed literal paths (`:39-58`) | A test cannot point the component at a temporary directory |
| Ambient dependencies | `cFileDB`, `pUser[]`, `rankingSystem`, `GuildInfo[]`, `ServerIndex`, `hWndMain`, `Log` are unresolved globals linked from the executable | Any test must link the whole DBSrv image, including winsock and the Win32 window |
| UI coupling | `MessageBoxA(hWndMain, ...)` on the guild paths (`:700`, `:717`) blocks a non-interactive runner | Guild tests hang without a message pump |
| Time coupling | Behaviour depends on `localtime`, day-of-week, `SecCounter`/`MinCounter`, and `rand()` (`:82`, `104`, `1055`, `Server.cpp:2028-2061`) | Cadence and jitter are not controllable |
| Non-determinism | `rand() % 4` in published output (`:82`) | Byte-exact output comparison impossible without changing production code |
| No seams | `static` methods, no interfaces, no callbacks, no feature flags | Substitution requires preprocessor or link-time tricks |

### 11.4 Assessment

Automated coverage of this component is **0%** by every measurable definition: no test project, no test source, no assertion, no fixture, no CI job, no headless entry point. The component is also the least testable module examined in this codebase: it is a pure side-effect module whose entire contract is "read these fixed paths, mutate these globals, write those fixed paths", with no parameter, no return value, and no interface through which a harness could observe or control behaviour.

The only verification mechanism that exists today is operational: read `.\Log\DB_<date>.txt` (written by `Log`, `Server.cpp:2115-2139`) after dropping files into the watched directories, and inspect the produced `servNN.htm` / `dataNN.csv` / `Ranking.txt` / `GuildInfo`. That mechanism covers the happy paths of `ImportItem` and `ImportDonate` reasonably well (each branch logs a distinguishable line), covers `UpdateUser` weakly (success/failure differ by one word), and covers `ImportUser` essentially not at all, because both the duplicate-account and the write-failure paths are silent. None of the properties that matter most for correctness - idempotency of re-processed files, byte-exact cargo placement, `Donate` accumulation, guild blob round-trip fidelity, ranking ordering - is verified by any code in the repository.

---

## Appendix A - Measured metrics

| Metric | Value | Source |
|--------|-------|--------|
| Implementation lines | 1,066 | `Code/DBSrv/CReadFiles.cpp` (1,066 CR / 1,066 LF, 19,587 bytes) |
| Header lines | 56 | `Code/DBSrv/CReadFiles.h` |
| Public methods | 9 (all `static`, all `void`, zero parameters) | `CReadFiles.h:44-53` |
| Private members | 14 `static const char* const`, all 14 referenced from code | `CReadFiles.h:29-42` |
| Instance fields | 0 | `CReadFiles.h` |
| Call sites (afferent) | 11 across 3 TUs | `Server.cpp:583,615,2015,2016,2022,2023,2030,2036,2044,2082`; `CRanking.cpp:110`; `CFileDB.cpp:480` |
| `Log` call sites | 22 | grep `\bLog\(` over `CReadFiles.cpp` |
| `fopen` call sites | 7 (4 inbound, 3 outbound) | `:66, 94, 168, 419, 755, 942, 1047` |
| `_open`/`_read`/`_write` call sites | 4 | `:696, 705, 713, 722` |
| `MoveFile` call sites | 11 (3 verified at `:312`, `:362`, `:892`; 8 unverified) | `:189, 211, 272, 312, 341, 362, 776, 798, 848, 871, 892` |
| Hardcoded absolute paths | 1 in scope (`C:/wamp/www/serv%2.2d.htm`) | `:39` (outside scope: `S:/export/...` in `CFileDB.cpp:2488`) |
| Guarded vs unguarded global reads | `pUser[i].Mode` unguarded at `:73`; `pUser[svr].cSock` send without `Sock`/`Mode` guard at `:253`, `:829` | compare `CFileDB.cpp:2384` which does guard |
| Static-initialization hazards | 1 (`rankingSystem` constructed before CWD normalization) | `CRanking.cpp:29,96,110`; `Server.cpp:443` |
| Automated tests | 0 | section 11.1 |
| Git history | 5 commits on the `.cpp`, last functional change `ab14656` 2018-05-22 | `git log -- Code/DBSrv/CReadFiles.cpp` |

## Appendix B - Method, ambiguities, and confidence

**Method.** The full implementation and header were read end to end. Every call site was located and read in context (`Server.cpp` boot block 427-624 and scheduler 2013-2089; `CRanking.cpp` full; `CFileDB.cpp` 60-260, 2200-2239, 2380-2590). Every callee that the component relies on was read (`DBReadAccount`, `DBWriteAccount`, `AddAccount`, `UpdateAccount`, `GetIndex`, `BASE_CanCargo`, `BASE_GetFirstKey`, `BASE_InitModuleDir`, `Log`, `CPSock::SendOneMessage`). The TMSrv-side consumers of the two emitted messages were read to establish the delivery contract (`ProcessDBMessage.cpp:229-334`, `1217-1251`). Build configuration and the solution file were inspected. A repository-wide test/CI scan was executed.

**Business-rule confidence.**

| Rule group | Confidence | Basis |
|------------|------------|-------|
| BR-01, BR-02, BR-08, BR-09, BR-10, BR-13, BR-14 | High | Directly readable from linear code with no indirection |
| BR-03 | High for the algorithm; Medium for the *intent* of the fallback | Grid semantics come from `BASE_CanCargo` in `Basedef.cpp`; why slots 126/127 are allowed is not documented |
| BR-04 | High | Gate condition is explicit; packet layouts read from `Basedef.h:1283-1298` |
| BR-05, BR-06, BR-07 | High | Whole control flow is visible; duplication and retention are consequences of explicit flags |
| BR-11 | High for the mechanized rules; Medium for the Korean `BASE_GetFirstKey` table semantics | The 18-entry Korean initial table is data, not logic, and was not enumerated byte by byte |
| BR-12 | High | Literals 401/406 and the 50/50 split are explicit at `CFileDB.cpp:126-135` |
| BR-15 | High for read/write mechanics and triggers; Medium for the operational impact of a missed Sunday trigger | The "exact second" condition is explicit; whether real deployments ever hit it depends on timer drift, which cannot be determined statically |
| BR-16, BR-17 | High for mechanics; Low for how external consumers interpret the numbers | The web page and reporting consumer are not in the repository, so the meaning of the published value is inferred from the formula alone |
| BR-18 | High for ordering/filtering; Medium for the classmaster label mapping | The mapping is derived from the normalization performed in `CRanking.cpp:224-228`; the labels are single letters whose semantics are not documented |
| BR-19, BR-20 | High for mechanics; Medium for severity | Severity depends on deployment ACLs on `../../Common`, which are not in the repository |

**Stated ambiguities (not fabricated).**

1. The consumer of `C:/wamp/www/servNN.htm`, `dataNN.csv` and `Ranking.txt` is not in the repository; all statements about *consumer* behaviour are therefore limited to the bytes produced.
2. Whether the ImportItem fallback scan (slots 126/127) is intentional tolerance or an off-by-one from `MAX_CARGO` (128) versus the 9x14 grid (126) cannot be decided from the code; both readings are documented.
3. `classmaster[4] = "C"` and `classmaster[5] = "SC"` are unreachable after normalization; whether another writer inserts `ClassMaster > 3` could not be proven (only `tryInsertInRanking` and `increaseRankingElementValue` write ranking elements, and both normalize).
4. The 15-minute CSV cadence is derived from `SecCounter % 30` + `MinCounter % 30` under a 1,000 ms `WM_TIMER`; if the timer drifts or is coalesced by Windows, the real cadence is slower than or equal to 15 minutes.
5. The `_MSG_NPDonate` producer's `rand()%255` filename suffix is unseeded in the observed code path; the collision probability stated (1/255 per same-second pair) assumes the default CRT seed behaviour.
6. No evidence exists of any deployment tooling that creates `../../Common/Error` or `../../Common/ImportDonateError`; the statement that they "may not exist" is an inference from the absence of `CreateDirectory`/`_mkdir` in `Code/DBSrv`, not from deployment artifacts.
