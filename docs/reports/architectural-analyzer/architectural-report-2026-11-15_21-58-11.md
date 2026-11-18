# Architectural Analysis Report

**Project:** W2PP
**Scope analyzed:** entire project root (`E:\wyd\test-setup\w2pp`)
**Folders excluded:** `.agents`, `.codegraph`, `.opencode`, `openspec`
**Report date:** 2026-11-15 21:58:11
**Analyzer role:** architecture/system analysis only (no code was modified)

---

## 1. Executive Summary

W2PP is a C/C++ Win32 game-server emulator for the MMORPG "W2" (Hanbitsoft), stated in `README.md` to be a decompilation of a "Polly's Server Release". The repository contains **104 source files (~49,330 lines, ~1.6 MB)** organised into two Visual C++ application projects and one empty/leftover project stub:

- **TMSrv** (`Code/TMSrv`) - the game/world server: accepts client connections on TCP 8281, simulates the world (players, mobs, NPCs, combat, items, guilds, PvP events), and talks to DBSrv on TCP 7514 and optionally to a billing server.
- **DBSrv** (`Code/DBSrv`) - the account/character database server: listens on TCP 7514 for game servers and TCP 8895 for the NP administrative tool, and persists accounts/characters/rankings to **raw binary files on disk** (no SQL engine).
- **ClientPatch_v7662** (`Code/ClientPatch_v7662`) - only a `ClientPatch_v7662.vcxproj.user` exists; **no project file, source, or build target is present** and it is not part of `W2PP Code Project.sln`.

Key architectural findings:

1. **A single-threaded, Win32-window-driven, cooperative multitasking architecture.** Both servers are `WinMain` GUI applications whose window procedure (`MainWndProc`) is also the socket I/O demultiplexer (`WSAAsyncSelect`) and the timer callback (`SetTimer`). There are **zero threading primitives** in the codebase (no `CreateThread`, `_beginthread`, critical sections, or `std::thread`). Every client packet, every timer tick, and every disk write executes on one thread.
2. **Two runtime tiers only, with a shared static-library-free code base.** `Basedef.cpp`/`Basedef.h` and `CPSock.cpp`/`CPSock.h` are compiled independently into both executables; a 2,505-line `Basedef.h` acts as the single domain/protocol contract for the whole system (100 `MSG_*` structures, 191 message-type constants, all persistence structs).
3. **File-system-as-database.** DBSrv stores each account and character as a fixed-size binary image of `STRUCT_ACCOUNTFILE` and `STRUCT_MOB` via `_open`/`_read`/`_write`. There is no transaction, journal, lock, or backup mechanism, and no database server, ORM, or schema.
4. **Strong coupling to a global mutable world state.** `Server.cpp` (TMSrv) is a 9,449-line, 110-function God object that owns the Win32 host, configuration, combat, guild, teleport, item economy, persistence, and logging. Player session slots and mob entity IDs share one array (`pMob[0..MAX_USER-1]` are players, `pMob[MAX_USER..MAX_MOB-1]` are mobs).
5. **A wide, weakly validated protocol surface.** 191 message types, 100 packed structures, custom framing with a hard-coded 512-byte obfuscation table and an 8-bit additive checksum. The per-message size-validation function `BASE_CheckPacket` is **entirely commented out** and always returns `FALSE`, so no structural validation of packets happens in normal builds.
6. **Plaintext credential handling.** Account passwords are stored verbatim inside the binary account record and compared with `strcmp`/`strncmp`; administrative access is granted when a character's level is >= 1000 (level minus 1000 becomes the admin level), and the NP tool credentials are read from plaintext files.

---

## 2. System Overview

### 2.1 Repository structure (analysis scope)

```
project-root/
├── W2PP Code Project.sln          # VS solution (2 projects: DBSrv, TMSrv)
├── README.md                      # project description, GPLv3
├── license.txt
├── docs/
│   └── reports/                   # analysis output (this report)
└── Code/
    ├── Basedef.h                  # 2,505-line shared domain + protocol contract
    ├── Basedef.cpp                # 6,814-line shared rules engine / content loaders
    ├── CPSock.h                   # socket + HEADER + _AUTH_GAME definitions
    ├── CPSock.cpp                 # Winsock async-select transport, framing, obfuscation
    ├── ItemEffect.h               # 153-line item-effect constant dictionary
    ├── ClientPatch_v7662/         # only *.vcxproj.user present (no source)
    ├── DBSrv/                     # database server project (12 source files)
    │   ├── Server.cpp             # host, dispatch, admin tool protocol (2,342 lines)
    │   ├── CFileDB.cpp            # account/character binary store (2,688 lines)
    │   ├── CRanking.cpp           # grind-ranking subsystem
    │   ├── CReadFiles.cpp         # bulk file import/export, web status publishing
    │   ├── CUser.cpp              # server-slot entity
    │   ├── DBSrv.vcxproj / .rc / .aps / resource.h / stdafx.h
    │   └── Debug/, Release/, DBSrv/Release/   # MSBuild intermediates (.iobj/.ipdb)
    ├── TMSrv/                     # game server project (87 source files)
    │   ├── Server.cpp             # God object / Win32 host / gameplay services
    │   ├── ProcessClientMessage.cpp/.h   # inbound client dispatcher + 61 handler decls
    │   ├── ProcessDBMessage.cpp/.h       # inbound DB dispatcher
    │   ├── ProcessSecMinTimer.cpp        # 500 ms / 12 s timer logic (2,314 lines)
    │   ├── SendFunc.cpp                  # outbound message construction / fan-out
    │   ├── GetFunc.cpp                   # derivation + world queries
    │   ├── MobKilled.cpp                 # death resolution (2,469 lines, 1 function)
    │   ├── imple.cpp                     # admin/GM command interpreter
    │   ├── CMob.cpp, CNPCGene.cpp, CCastleZakum.cpp, CWarTower.cpp
    │   ├── CReadFiles.cpp, CUser.cpp, CItem.cpp, Language.h
    │   ├── _MSG_*.cpp             # 60 client command handlers (one per message family)
    │   ├── TMSrv.vcxproj / .rc / resource.h
    │   └── Debug/, Release/, TMSrv/Release/   # MSBuild intermediates
    └── release/TMSrv/run/         # prebuilt TMSrv.exe (509 KB) + TMSrv.pdb (2.8 MB)
```

### 2.2 Identified architectural patterns

The system exhibits **several concurrent patterns**, documented separately:

| Pattern | Where it is evidenced | Notes |
|---|---|---|
| **Layered (partially enforced)** | `MainWndProc` -> `ProcessClientMessage`/`ProcessDBMessage` -> `_MSG_*` handlers -> `GetFunc`/`SendFunc` -> `Basedef` rules -> `CPSock` -> socket | Layers are conventional only; handlers call `pUser[]`/`pMob[]` globals and skip layers freely. |
| **Message Router / Command dispatch** | `ProcessClientMessage.cpp` `switch (std->Type)` over 191 message constants; `ProcessDBMessage.cpp`; `CFileDB::ProcessMessage`; `ProcessAdminMessage` | Type-tagged flat message space with bit-flag namespaces (`FLAG_CLIENT2GAME`, `FLAG_GAME2DB`, `FLAG_DB2NP`, ...). |
| **God Object / global-state monolith** | `Code/TMSrv/Server.cpp` (110 functions, 9,449 lines); global arrays `pUser[MAX_USER]`, `pMob[MAX_MOB]`, `pItem[MAX_ITEM]`, `pMobGrid/pItemGrid/pHeightGrid[4096][4096]` | Nearly every handler reaches directly into these globals. |
| **Cooperative multitasking event loop** | `WinMain` `GetMessage`/`DispatchMessage`, `WSAAsyncSelect`, `SetTimer(TIMER_SEC, 500)`, `SetTimer(TIMER_MIN, 12000)` | One thread services all sockets, timers, gameplay, and disk I/O. |
| **Active Record (file-based)** | `CFileDB` `DBReadAccount`/`DBWriteAccount`/`DBExportAccount`, `STRUCT_ACCOUNTFILE` | The in-memory account list *is* the on-disk record layout. |
| **Table-driven / data-driven content** | `Basedef.cpp` loaders for `itemlist.csv`, `extraitem.csv`, `InitItem.csv`, `SkillData.csv`, `Guilds.txt`, `serverlist.txt`, `AttributeMap.dat`, `UI/strdef.bin`, and even `ItemEffect.h` parsed at runtime | Content plane is CSV/text/binary in `../../Common`; binary caches are generated by the server itself. |
| **Repository / bulk-sync integration** | `DBSrv/CReadFiles.cpp` scans `../../Common/ImportUser/*.*`, `ImportItem/*.*`, `ImportDonate/*.*`, `../../Common/servNN/update/*.*` | Operator tooling integrates by dropping files into watched directories, processed in the server thread. |
| **Publish/fan-out (in-process)** | `SendFunc.cpp` `SendNotice`, `SyncMulticast`, `GridMulticast`, `MapaMulticast`, `SendMessageArea` loop over `MAX_USER`/`MAX_SERVER` | No event bus; broadcast is an O(N) loop per call. |
| **Anti-pattern: duplicated business rules** | Item-move/use logic forked between `_MSG_UseItem.cpp`, `_MSG_PutoutSeal.cpp`, `_MSG_DropItem.cpp`, `_MSG_UpdateItem.cpp`; identical "dead character resend item" block copy-pasted in two handlers | Same invariant enforced in several places with different bounds checks. |
| **Anti-pattern: header as wiring hub** | `ProcessClientMessage.h` includes Basedef, CPSock, ItemEffect, Language, CItem, Server, GetFunc, SendFunc, CMob, CUser, CNPCGene, CReadFiles, CCastleZakum, CWarTower and declares all 61 handlers | Every one of the 60 handler TUs depends on all 14 subsystems. |

### 2.3 Runtime topology

```
                       TCP 8281 (custom binary protocol)
  [W2 client build 7662] <--------------------------------> [ TMSrv.exe ] (1 per shard, Win32 x86)
                                                                  |  ^
                                            TCP 7514 (game<->DB)  |  |
                                                                  v  |
                                                          [ DBSrv.exe ] (1 per server group)
                                                                  |  ^
                                       TCP 8895 (NP admin tool)   |  |
                                                                  v  |
                                                    [ NPTool / admin client ] (external, not in repo)

  TMSrv --> [ Billing/BI server ]  (BillServerAddress:BillServerPort from biserver.txt, optional, BILLING>0)
  DBSrv --> [ Web server root "C:/wamp/www" ] (status HTML) and ../../Common/dataNN.csv (status CSV)
  Both  --> [ Filesystem: ./account, ./char, ./capsule, ../../Common, ./Log, localip.txt, serverlist.txt ]
```

---

## 3. Critical Components Analysis

### 3.1 How coupling was measured

**Afferent coupling (Ca)** is the number of incoming dependencies: how many other modules rely on a component's exported API or global state. A component with high Ca is widely reused, so any change to it propagates to many dependents; it is also a change-amplification risk and, in this codebase, a blast-radius indicator for shared globals such as `pUser[]` and `pMob[]`.

**Efferent coupling (Ce)** is the number of outgoing dependencies: how many distinct internal components a module itself pulls in (via `#include` of internal headers and by calling their symbols). High Ce means the component is fragile with respect to other components and hard to test in isolation.

For this C/C++ codebase the values were derived **statically and approximately**: Ca counts the number of distinct `.cpp`/`.h` translation units in `Code/` that reference the component's exported names (e.g. `pUser[`, `BASE_*`, `Exec_MSG_*`, `CPSock|cSock`, `CFileDB`), and Ce counts distinct *internal* components reachable through direct `#include`s plus direct call targets - system headers and the CRT are excluded. Two effects inflate the numbers and should be understood when reading the table: (a) headers are transitive, because `Server.h` includes `Basedef.h`/`CPSock.h` and `ProcessClientMessage.h` includes almost everything, so a single `#include "Server.h"` adds several Ce units; (b) the 60 `_MSG_*.cpp` handlers all include the same aggregate header, which is why their Ce is uniformly 14 while their individual Ca is 1.

### 3.2 Component inventory

| Component | Type | Location | Afferent Coupling | Efferent Coupling | Architectural Role |
| ----------- | ------ | ---------- | ------------------- | ------------------- | ------------------- |
| TMSrv Server Core | God Object / Host | `Code/TMSrv/Server.cpp` | ~68 | 12 | Win32 host (`WinMain`, `MainWndProc`), owner of all world globals, gameplay services (teleport, guild, PvP, loot, ranking, mount, logging), configuration loader (`gameconfig.txt`, `LevelItem.txt`, `localip.txt`, `biserver.txt`) |
| Client Message Dispatcher | Inbound Adapter / Router | `Code/TMSrv/ProcessClientMessage.cpp` | ~69 | 14 | Single entry point for all client packets; validates ID range and `ServerDown` state, drops `_MSG_Ping`, routes 60 message families to `Exec_MSG_*` handlers |
| Client Handler Interface Contract | Shared Contract (header) | `Code/TMSrv/ProcessClientMessage.h` | ~60 | 14 | Declares the 61 `Exec_MSG_*` entry points and aggregates every TMSrv subsystem header; the de-facto "server API" for handlers |
| DB Message Dispatcher | Inbound Adapter / Router | `Code/TMSrv/ProcessDBMessage.cpp` | ~9 | 14 | Consumes DBSrv responses: account/character confirmations, item donation, capsule, guild report, ranking updates, server-index assignment; contains the 450-line `_MSG_DBCNFCharacterLogin` branch |
| UseItem Handler | Command Handler (largest) | `Code/TMSrv/_MSG_UseItem.cpp` | 1 | 14 | 160 KB single function covering potions, scrolls, stat items, mounts, souls, hunting scrolls, seals, bonus application |
| Quest Handler | Command Handler | `Code/TMSrv/_MSG_Quest.cpp` | 1 | 14 | 73 KB NPC quest interaction engine (quest IDs 1..35, reward/coin/level tables) |
| Attack Handler | Command Handler (combat authority) | `Code/TMSrv/_MSG_Attack.cpp` | 1 | 14 | 44 KB damage resolution, PvP/PK rules, area attacks, skill dispatch, mob aggro |
| Whisper and GM Chat Handler | Command Handler (privileged entry) | `Code/TMSrv/_MSG_MessageWhisper.cpp` | 2 | 14 | 35 KB whisper/party/guild chat plus the `/gm` command that forwards to the admin interpreter |
| CombineItemOdin Handler | Command Handler (crafting) | `Code/TMSrv/_MSG_CombineItemOdin.cpp` | 1 | 14 | 18 KB combination recipe resolution and success rolls |
| TradingItem Handler | Command Handler (trade) | `Code/TMSrv/_MSG_TradingItem.cpp` | 1 | 14 | 13 KB item-for-item trade handshake |
| Trade Handler | Command Handler (trade) | `Code/TMSrv/_MSG_Trade.cpp` | 1 | 14 | 12 KB trade window lifecycle and escrow |
| CombineItemEhre Handler | Command Handler (crafting) | `Code/TMSrv/_MSG_CombineItemEhre.cpp` | 1 | 14 | 12 KB recipe table and refine logic |
| Action Handler | Command Handler (movement) | `Code/TMSrv/_MSG_Action.cpp` | 1 | 14 | 9 KB move/position desync checks and grid updates |
| Buy Handler | Command Handler (economy) | `Code/TMSrv/_MSG_Buy.cpp` | 1 | 14 | 9 KB shop purchase validation and coin transfer |
| Sell Handler | Command Handler (economy) | `Code/TMSrv/_MSG_Sell.cpp` | 1 | 14 | 6 KB shop sale and price computation |
| GetItem Handler | Command Handler (inventory) | `Code/TMSrv/_MSG_GetItem.cpp` | 1 | 14 | 7 KB ground-item pickup, destination slot validation |
| DropItem Handler | Command Handler (inventory) | `Code/TMSrv/_MSG_DropItem.cpp` | 1 | 14 | 4 KB drop placement into the item grid |
| ApplyBonus Handler | Command Handler (progression) | `Code/TMSrv/_MSG_ApplyBonus.cpp` | 1 | 14 | 8 KB score/special/skill point allocation |
| MessageChat Handler | Command Handler (social) | `Code/TMSrv/_MSG_MessageChat.cpp` | 1 | 14 | 5 KB local chat and chat-mode flags |
| AccountLogin Handler | Command Handler (auth) | `Code/TMSrv/_MSG_AccountLogin.cpp` | 1 | 14 | Client version check, fail counter, MAC capture, forward of credentials to DBSrv |
| CharacterLogin Handler | Command Handler (session) | `Code/TMSrv/_MSG_CharacterLogin.cpp` | 1 | 14 | Character-slot selection and DB confirmation handshake |
| UpdateItem Handler | Command Handler (world gates) | `Code/TMSrv/_MSG_UpdateItem.cpp` | 1 | 14 | 3 KB keyed gate/door state changes, delegates to `CCastleZakum` |
| Remaining Client Command Handlers | Command Handler family (46 files) | `Code/TMSrv/_MSG_*.cpp` | 1 each | 14 each | `AcceptParty`, `AccountSecure`, `CapsuleInfo`, `Challange`, `ChallangeConfirm`, `ChangeCity`, `CharacterLogout`, `CombineItem`, `CombineItemAgatha`, `CombineItemAilyn`, `CombineItemAlquimia`, `CombineItemExtracao`, `CombineItemLindy`, `CombineItemShany`, `CombineItemTiny`, `CreateCharacter`, `DeleteCharacter`, `DeleteItem`, `Deposit`, `Deprivate`, `GuildAlly`, `InviteGuild`, `Motion`, `NoViewMob`, `PKMode`, `PutoutSeal`, `QuitTrade`, `RemoveParty`, `ReqBuy`, `ReqRanking`, `REQShopList`, `ReqTeleport`, `ReqTradeList`, `Restart`, `SendAutoTrade`, `SendReqParty`, `SetShortSkill`, `SplitItem`, `War`, `Withdraw` (40 named; the family total includes the same-shape files without individual rows) |
| SendFunc Outbound Projection Layer | Outbound Adapter / Serializer | `Code/TMSrv/SendFunc.cpp` | ~59 | 5 | 50 functions that build `MSG_*` payloads and perform fan-out (`SendNotice`, `SyncMulticast`, `GridMulticast`, `MapaMulticast`, `SendCreateMob`, `SendScore`, `SendShopList`, ...) |
| GetFunc Derivation Layer | Query / Derivation | `Code/TMSrv/GetFunc.cpp` | ~29 | 7 | Read-only derivations: view checks, item/mob DTO construction (`GetCreateMob`, `GetCreateItem`, `GetAttack`), empty-slot searches, PK/guilt counters, parry/exp math |
| Timer Scheduler | Scheduler / Event Loop Body | `Code/TMSrv/ProcessSecMinTimer.cpp` | ~7 | 9 | 500 ms and 12 s tick bodies: decay, ranking, castle/war-tower events, idle timeouts, mob regeneration, affect ticking, guild process, world events |
| MobKilled Resolution Module | Domain Service | `Code/TMSrv/MobKilled.cpp` | ~10 | 11 | One 2,400-line function: exp/coin distribution, party/guild split, drop rolls, treasure keys, castle and tower hooks |
| Admin Command Interpreter | Privileged Command Processor | `Code/TMSrv/imple.cpp` | ~5 | 9 | `ProcessImple` parses `set/save/read/reboot/gift/cp/frag/log/event/...` GM commands (level-gated), `SaveAll` |
| CMob Entity and AI | Domain Entity | `Code/TMSrv/CMob.cpp` | ~67 | 4 | Per-mob state machine (idle/peace/combat/return/flee/roam), enemy list, path segments, per-tick processors |
| CNPCGene Spawn System | Domain Service (content) | `Code/TMSrv/CNPCGene.cpp` | ~10 | 5 | NPC generator table parsing (`NPCGenerator.txt`), region map, group spawn/drop/level lists; uses `std::list` for mob-name lists |
| CCastleZakum Event Module | Domain Service (event) | `Code/TMSrv/CCastleZakum.cpp` | ~9 | 11 | Castle quest line, gate keys, castle timers, event mob kills |
| CWarTower Event Module | Domain Service (event) | `Code/TMSrv/CWarTower.cpp` | ~7 | 11 | Guild-war tower state, tower attacks, scheduled guild process |
| TMSrv CReadFiles Config Loader | Configuration Adapter | `Code/TMSrv/CReadFiles.cpp` | ~17 | 6 | Loads `../../Common/Settings/*.txt`, `Admin.txt`, `Mac.txt`, `Chall_*.txt`, `Guild_*.txt`, `ChampionCity_*.txt`; writes challanger/guild state back |
| TMSrv CUser Session Entity | Domain Entity | `Code/TMSrv/CUser.cpp` | ~68 | 3 | Per-connection session: account, mode state machine, socket, cargo, trade/autotrade buffers, timers, admin flag |
| CItem Ground Item Entity | Domain Entity | `Code/TMSrv/CItem.cpp` | ~20 | 1 | Ground item record: item, position, decay delay, rotation, money, quest flag |
| Language Message Catalog | Shared Data (header) | `Code/TMSrv/Language.h` | ~52 | 0 | 600+ `_NN_*`/`_SN_*` indices into `g_pMessageStringTable` localized strings |
| Basedef Domain Contract | Shared Contract (header) | `Code/Basedef.h` | ~95 | 1 | 2,505 lines: all persistence structs (`STRUCT_MOB`, `STRUCT_ACCOUNTFILE`, `STRUCT_ITEM`, ...), 100 `MSG_*` payloads, 191 message constants, flag namespaces, tuning constants, port numbers |
| Basedef Rules Engine | Shared Library (static link) | `Code/Basedef.cpp` | ~49 | 3 | 6,814 lines of game rules: item/mob ability math, score computation, hit/damage rates, grid routing (`BASE_GetRoute`), item list/skill/message/init-item loaders, checksum helpers |
| CPSock Transport | Infrastructure / Transport | `Code/CPSock.cpp` | ~40 | 2 | Winsock 1.1 async-select wrapper, 128 KB fixed send/recv buffers, message framing, header checksum, static obfuscation table, billing-message variant |
| ItemEffect Dictionary | Shared Data (header) | `Code/ItemEffect.h` | ~27 | 0 | Effect ID contract (`EF_DAMAGE`, `EF_SANC`, `EF_VOLATILE`, ...) shared by server rules and client |
| DBSrv Server Core | Host / Router | `Code/DBSrv/Server.cpp` | ~6 | 8 | Win32 host, three listening sockets (7514, 8895, redirect client), admin dispatch (`ProcessAdminMessage`, 550 lines), graceful/`ServerDown` control, day logs |
| CFileDB Account and Character Store | Persistence Repository | `Code/DBSrv/CFileDB.cpp` | ~6 | 5 | `pAccountList[MAX_DBACCOUNT]` in-memory cache plus raw binary account/char/capsule files; login, create/delete character, save/quit, capsule, server-change, ranking feeds |
| RankingSystem | Domain Service | `Code/DBSrv/CRanking.cpp` | ~6 | 3 | `GrindRanking` (500 slots of `new STRUCT_RANKING`) plus `RankingSystem` load/insert/rise/notify; persists `../../Common/Ranking.txt` |
| DBSrv CReadFiles Bulk I/O | Integration Adapter | `Code/DBSrv/CReadFiles.cpp` | ~12 | 4 | Bulk import/export of users/items/donations from watched directories, guild info read/write, connection HTML/CSV publishing, ranking file write |
| DBSrv CUser Server Slot | Domain Entity | `Code/DBSrv/CUser.cpp` | ~6 | 2 | One slot per connected game server or admin tool (IP, mode, encode challenge, admin level) |
| ClientPatch_v7662 (source absent) | Unbuilt / Placeholder | `Code/ClientPatch_v7662/ClientPatch_v7662.vcxproj.user` | 0 | 0 | README describes a client-unlock DLL; no project, source, or build target exists in the repository |
| Win32 Resource Bundle | Build Resource | `Code/TMSrv/TMSrv.rc`, `Code/DBSrv/DBSrv.rc`, `icon1.ico`, `resource.h` | 2 | 0 | Icons, version/menu resources for the two executables (`.aps` is a checked-in generated resource cache) |
| Prebuilt Release Artifact Set | Build Output | `Code/release/TMSrv/run/TMSrv.exe`, `TMSrv.pdb` | 0 | 0 | Checked-in binary and full debug symbols; note the project `OutDir` resolves to `$(SolutionDir)..\release\...` (i.e. outside the repo), so this folder is not the current build destination |

*Note on the handler family row:* individual rows are given for the handlers that carry distinct business domains or exceed ~5 KB. The remaining 40+ files are structurally identical (`#include "ProcessClientMessage.h"` plus one `Exec_MSG_<Name>` function) and are enumerated in the "Remaining Client Command Handlers" row.

---

## 4. Dependency Mapping

### 4.1 High-level dependency graph (TMSrv)

```
                       Win32 message pump (MainWndProc @ Server.cpp)
        ┌──────────────────────────────┬──────────────────────────────┬──────────────────────────┐
        │ WM_TIMER                     │ WSA_ACCEPT                   │ WSA_READ / WSA_READDB /  │
        │                              │                              │ WSA_READBILL             │
        ▼                              ▼                              ▼                          │
 ProcessSecTimer /               GetEmptyUser + AcceptUser      CPSock::Receive/ReadMessage    │
 ProcessMinTimer                        │                              │                          │
        │                               │                              ▼                          │
        │                               │                     ProcessClientMessage (router)      │
        │                               │                              │  switch(Type)            │
        │                               │                              ▼                          │
        │                               │                        Exec_MSG_* (60 handlers)          │
        │                               │                              │                          │
        │                               │              ┌───────────────┼───────────────┐          │
        │                               │              ▼               ▼               ▼          │
        │                               │          GetFunc         Basedef rules     SendFunc      │
        │                               │              │               │               │          │
        │                               │              └───────────────┼───────────────┘          │
        │                               │                              ▼                          │
        │                               │                    Global world state (Server.cpp)     │
        │                               │                    pUser[] pMob[] pItem[] grids         │
        │                               │                              │                          │
        │                               │                              ▼                          │
        │                               │                       CPSock (AddMessage/SendMessageA)   │
        │                               │                              │                          │
        │                               │                       client sockets (8281)             │
        ▼                               ▼                                                          │
 CCastleZakum/CWarTower        pUser[].cSock ────────────────────────────────────────────────────┘
 ProcessDecayItem/ProcessRanking/GuildProcess
        │
        ▼
 CCastleZakum::Process*  →  MobKilled / Server.cpp helpers  →  CReadFiles (persist)

 DBSrvSocket (CPSock) ←→ ProcessDBMessage (router) ←→ account/char confirmations, guild, ranking, capsule
 BillServerSocket (CPSock) → SendBilling/SendBilling2 (outbound only; ProcessBILLMessage is empty)
```

### 4.2 High-level dependency graph (DBSrv)

```
        Win32 message pump (MainWndProc @ DBSrv/Server.cpp)
   ┌──────────────┬────────────────────────┬──────────────────────────┐
   │ WM_TIMER     │ WSA_ACCEPT (7514)      │ WSA_ACCEPTADMIN (8895)   │
   ▼              ▼                        ▼                          │
ProcessSecTimer  pUser[svr].AcceptUser   pAdmin[i].AcceptUser         │
(1 s tick)        (IP allow-list from      (IP allow-list from Admin.txt)
   │              serverlist.txt)                    │                 │
   │                  │                               ▼                 │
   │                  ▼                    MSG_NPIDPASS challenge       │
   │           ProcessClientMessage → CFileDB::ProcessMessage          │
   │                  │  (28 message cases)                            │
   │                  ▼                                                │
   │           CFileDB (accounts/chars/capsule) ←→ RankingSystem        │
   │                  │                                                │
   │                  ▼                                                │
   │           raw binary files: ./account/<X>/<NAME>, ./char/<X>/<NAME>,
   │           ./capsule/<id>, ../../Common/record<idx>/
   │
   └─► CReadFiles: ImportUser / UpdateUser / ImportItem / ImportDonate / UpdateConnection(HTML/CSV)
```

### 4.3 Control-flow and data-flow observations

- **Slot identity aliasing.** In TMSrv a connection index `conn` is simultaneously the `pUser[conn]` session index, the entity index (`pMob[conn]`) for players, and the packet `ID` used on the DB link. Any off-by-one in `conn` crosses the player/mob boundary (`MAX_USER` = 1000; mob IDs start exactly at 1000).
- **Request/response over two hops.** Client requests that need persistence are rewritten into a `_MSG_DB*` message, sent to DBSrv, and answered asynchronously in `ProcessDBMessage`; the session is parked in an intermediate mode (`USER_SELCHAR`, `USER_CHARWAIT`, `USER_WAITDB`, `USER_SAVING4QUIT`).
- **Single shared scratch buffer.** A global `char temp[4096]` (declared in `Code/TMSrv/Server.h`) is used as the formatting buffer by most functions, including log helpers and `CrackLog`, which `strcat`s onto it - there is one instance per process and it is not re-entrant.
- **Time as ambient state.** `CurrentTime` (`timeGetTime`), `SecCounter`, `MinCounter`, `LastSendTime`, and `localtime()` comparisons on exact second/minute values drive all scheduling; there is no scheduler queue or event abstraction.
- **Content flows one way and back.** `Basedef` reads CSV text, writes binary caches (`ItemList.bin`, `SkillData.bin`, `InitItem.bin`, `extraitem.bin`, `UI/strdef.bin`, `serverlist.bin`) and can also emit C headers into the source tree (`..\ItemCSum.h`, `..\SkillCSum.h`) - the runtime and the build share the same data files.

---

## 5. Integration Points

| Integration | Type | Location | Purpose | Risk Level |
|-------------|------|----------|---------|------------|
| W2 game client | TCP binary protocol, port 8281 | `Code/CPSock.cpp`, `Code/TMSrv/ProcessClientMessage.cpp` | All client traffic; custom `HEADER` + obfuscated payload | High |
| DBSrv game link | TCP binary protocol, port 7514 | `Code/TMSrv/ProcessDBMessage.cpp`, `Code/DBSrv/Server.cpp` | Login/character/item/capsule/guild/ranking persistence; IP allow-list from `serverlist.txt` | High |
| NP administrative tool | TCP binary protocol, port 8895 | `Code/DBSrv/Server.cpp` (`ProcessAdminMessage`, `ProcessAdminClientMessage`) | Account administration, notices, transfers, donation, day logs; IP allow-list from `Admin.txt` plus account-level challenge | High |
| Billing / BI server | TCP, 196-byte opaque `_AUTH_GAME` frames | `Code/TMSrv/Server.cpp` (`SendBilling`, `SendBilling2`), `biserver.txt` | Optional billing authorization (`BILLING > 0`); inbound handler `ProcessBILLMessage` is an empty stub | High |
| Redirect / transfer server | TCP (outbound client socket) | `Code/DBSrv/Server.cpp` (`redirect.txt`), `AdminClient` | Server-to-server character transfer/relay; enabled only if `redirect.txt` exists | Medium |
| Account and character files | Raw binary fixed-size records (`_open`/`_read`/`_write`) | `Code/DBSrv/CFileDB.cpp` (`./account/<First>/<NAME>`, `./char/<First>/<NAME>`, `./capsule/<id>`) | Primary data store | High |
| Ranking file | Text file | `Code/DBSrv/CReadFiles.cpp` (`../../Common/Ranking.txt`) | Ranking persistence | Medium |
| Content data plane | CSV / text / binary files | `Code/Basedef.cpp` (`../../Common/itemlist.csv`, `extraitem.csv`, `InitItem.csv`, `SkillData.csv`, `Guilds.txt`, `serverlist.txt`, `AttributeMap.dat`, `UI/strdef.bin`) | Items, skills, initial items, guilds, server topology, map attributes | High |
| Runtime tuning files | Text config | `gameconfig.txt`, `LevelItem.txt`, `localip.txt`, `biserver.txt`, `MobName.txt`, `Language.txt`, `heightmap.dat` (TMSrv); `Config.txt`, `Trans.txt`, `redirect.txt`, `Admin.txt` (DBSrv) | Rates, events, identity, topology | Medium |
| World state files (TMSrv) | Text | `Code/TMSrv/CReadFiles.cpp` (`../../Common/Settings/*.txt`, `Chall_%02d_%02d.txt`, `Guild_%02d_%02d.txt`, `ChampionCity_%02d_%02d.txt`, `Admin.txt`, `Mac.txt`) | Sanc/quest/comp rates, mob mercs, challangers, guild state, MAC blocking | Medium |
| Operator file-drop channels | Watched directories | `Code/DBSrv/CReadFiles.cpp` (`ImportUser`, `ImportItem`, `ImportDonate`, `servNN/update`, `Error`, `ImportDonateError`) | Bulk account/item/donation imports and exports; processed every tick | High |
| Web status publishing | Files served by a web server | `Code/DBSrv/CReadFiles.cpp` (`C:/wamp/www/serv%02d.htm`, `../../Common/data%02d.csv`) | Server population/status pages | Medium |
| Log sinks | Local text files | `Code/TMSrv/Server.cpp` (`.\Log\TM_*.txt`, chat log, item log), `Code/DBSrv/Server.cpp` (`.\Log\DB_*.txt`) | Audit and diagnostics | Medium |
| Item effect contract | Compile-time header, also parsed at runtime | `Code/ItemEffect.h` (parsed by `BASE_InitializeEffectName`) | Effect IDs shared with the client build | Medium |
| Windows system services | Win32 APIs | `gdi32`/`user32` (status window, `TextOutA`, fonts), `winmm` (`timeGetTime`), `ws2_32` (Winsock 1.1) | Host UI, clock, networking | Low |

---

## 6. Architectural Risks and Single Points of Failure

| Risk Level | Component | Issue | Impact | Details |
| ----------- | ----------- | -------- | -------- | --------- |
| Critical | TMSrv process (single instance per shard) | No horizontal scaling, no failover, no supervision | Whole shard offline | One `TMSrv.exe` hosts up to `MAX_USER` (1000) players; there is no clustering, watchdog, or restart logic beyond a manual `Reboot()` that re-reads all content files |
| Critical | DBSrv process | Hard dependency: game server exits when the DB link drops | Total outage of all attached game servers | `MainWndProc` `WSA_READDB` attempts 2 reconnects (200 ms apart) and then calls `PostQuitMessage`, terminating the game server |
| Critical | `CFileDB` persistence | No transactions, journaling, locking, or atomic replace | Data loss/corruption on crash or concurrent write | Records are fixed-size binary images written in place over the previous file (`_open(O_RDWR)` + `_write` of `sizeof(STRUCT_ACCOUNTFILE)`); a partial write leaves an unreadable record |
| Critical | Single-threaded event loop (`MainWndProc`) | All socket I/O, gameplay, timers, and blocking file I/O on one thread | One slow operation stalls every connected client | Synchronous `fopen`/`_write` for saves, logs, ranking, bulk imports, and the 16.7M-iteration grid clear in `Reboot()` all run inside the message pump |
| High | `pMob[MAX_MOB]` = 25,000 static entities | Per-tick scans over the whole array | Tick overrun and client-visible lag | `ProcessSecTimer`/`ProcessMinTimer` iterate `MAX_USER` (1000) and `MAX_MOB` (25,000) arrays on a 500 ms timer; several loops are nested (`MAX_PARTY` 12 inside `MAX_USER`) |
| High | 32-bit process address space with fixed reservations | ~80 MB of static grid memory (`pMobGrid[4096][4096]` word, `pItemGrid` word, `pHeightGrid` byte) plus ~50 MB of `CMob` entities and `pUser`/`pItem` arrays | Ceiling on players, mobs, and map size; no dynamic capacity | All capacities are compile-time constants in `Code/Basedef.h`; there is no allocator or pool abstraction |
| High | `Code/TMSrv/Server.cpp` God object | 9,449 lines and 110 functions spanning host, protocol, combat, economy, teleport, guild, logging | Change amplification; any edit risks unrelated features | Combat helpers (`ProcessAffect`), loot (`SetItemBonus`), teleport (`DoTeleport`), guild (`GuildProcess`), persistence (`SaveUser`), and Win32 UI coexist in one translation unit |
| High | `Code/TMSrv/ProcessClientMessage.h` aggregate header | Couples all 60 handlers to 14 subsystems | Whole-server recompilation on any interface change; hidden dependencies | Every `_MSG_*.cpp` includes only this header, so its transitive closure is the entire server |
| High | Protocol boundary validation | `BASE_CheckPacket` body is commented out and returns `FALSE`; `CPSock::ReadMessage` only enforces `HEADER <= Size <= 8192` | Malformed/short packets reach handlers that cast and read struct fields | Handlers assume `sizeof(MSG_*)`; several also write terminator bytes into the packet (e.g. `_MSG_PutoutSeal`, `ProcessDBMessage`) before/after the received bytes |
| High | Content data plane (`../../Common`) | Shared mutable files read by both servers with relative paths anchored on the process working directory | Startup failure or divergent state if the directory layout is wrong | `BASE_InitModuleDir` sets CWD to the module directory; all loaders then use `../../Common/...`; some paths are absolute and machine-specific (`C:/wamp/www/serv%02d.htm`, `S:/export/account%d/%s`) |
| High | Admin/GM authorization model | Privilege is derived from character level (`level - 1000`) in both TMSrv and DBSrv | Any level-inflation defect or GM item becomes remote code-equivalent command access | `_MSG_MessageWhisper` computes `level = CurrentScore.Level - 1000` and calls `ProcessImple`; DBSrv grants NP admin when `maxlevel >= 1000` |
| Medium | Global `temp[4096]` scratch buffer | Non-reentrant, unbounded `strcat` in `CrackLog` | Log corruption or overflow when nested calls format long strings | ~715 `sprintf`, 102 `strcpy`, 57 `strcat` calls across the code base; several format strings are built at runtime and passed as the format argument (`fprintf(fp, temp)` in `CReadFiles::UpdateConnectionData`, `fprintf(fLogFile, LogTemp)` in `Log`) |
| Medium | Timer drift and calendar comparisons | Events fire on exact second/minute values (`tm_sec == 0`, `when.tm_min == 15/35/55`), 500 ms tick, 12 s "minute" timer | Missed or duplicated events under load | `SetTimer(..., TIMER_MIN, 12000)` is 12 seconds, not a minute; logic that assumes 60 ticks per minute is absent - the code uses calendar fields instead |
| Medium | `MobKilled.cpp` single 2,400-line function | Drop, exp, party, guild, castle, tower, quest and log effects in one control flow | Hard to reason about; duplicate reward paths | One entry point, dozens of independent `if` blocks over item indices (e.g. `3443`, `3993`, `3994`, `3432..3437`) |
| Medium | Ranking subsystem | `GrindRanking` allocates 500 `STRUCT_RANKING` with `new` and has no destructor; `getElement` treats index 0 as out of range (`index >= LAST` where `LAST == 0`) | Memory retained for process lifetime; slot 0 unreachable | `Code/DBSrv/CRanking.cpp`; indexing constants `FIRST = MAX_RANK_INDEX - 1`, `LAST = 0` |
| Medium | Duplicated item-move logic | `_MSG_UseItem` and `_MSG_PutoutSeal` share a copy-pasted block that indexes `Carry/SourPos`, `Cargo/SourPos`, `Equip/SourPos` with unvalidated packet values before `GetItemPointer` validation | Out-of-bounds read inside the receive buffer, inconsistent behaviour between handlers | Same 8-line block at `_MSG_UseItem.cpp:69-76` and `_MSG_PutoutSeal.cpp:41-48` |
| Medium | Build configuration drift | TMSrv Release uses `MultiThreadedDebugDLL` (debug CRT) while Debug sets `RandomizedBaseAddress=false`; DBSrv Release omits `_CRT_SECURE_NO_WARNINGS`; toolset is `v143` while README states Visual Studio 2015; solution maps all `x64` configurations to `Win32` | Unpredictable runtime characteristics; ASLR disabled in debug builds | `Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj`, `W2PP Code Project.sln` |
| Medium | Build output location | `OutDir` = `$(SolutionDir)..\release\$(ProjectName)\run\` resolves outside the repository, yet `Code/release/TMSrv/run` contains a checked-in binary and PDB | Deployment artifacts and build products diverge | Verified from `TMSrv.vcxproj`/`DBSrv.vcxproj` and `Code/release/TMSrv/run` contents |
| Medium | Missing project artifacts | `ClientPatch_v7662` has no `.vcxproj` or sources; README lists 12 components (BISrv, DropTool, EDITAPMOB, EDITAPNPC, ExpTool, NPTool, SearchPass, ZerarSkill, AttributeMap_Editor) that are absent | Documented surface exceeds buildable surface | `README.md` versus `W2PP Code Project.sln` (2 projects) and `Code/ClientPatch_v7662/` (one `.vcxproj.user`) |
| Low | Checked-in generated state | `.iobj`/`.ipdb` (4 MB + 494 KB) under `Code/TMSrv/Release` and `Code/TMSrv/TMSrv/Release`, `DBSrv.aps` (83 KB), prebuilt `.pdb` | Repository bloat and stale-artifact confusion | Duplicate `Release`/`TMSrv\Release` trees also indicate two historical output layouts |
| Low | `stdafx.h` in DBSrv includes `<afxwin.h>` | MFC dependency declared but unused (no precompiled header is configured) | Misleading dependency footprint | `Code/DBSrv/stdafx.h` line 21 |

---

## 7. Technology Stack Assessment

| Layer | Technology | Assessment |
|---|---|---|
| Language | C++ in a C style (no classes for protocol; classes only for entities/services: `CUser`, `CMob`, `CItem`, `CPSock`, `CFileDB`, `CNPCGenerator`, `CCastleZakum`, `CWarTower`, `CReadFiles`, `GrindRanking`) | Consistent style, but C-style `memset`/`memcpy` over class objects (`CUser`, `CMob`) is used where constructors exist |
| Toolchain | MSVC, `PlatformToolset v143`, project `ToolsVersion 14.0`, solution header `Visual Studio Version 17`; `WindowsTargetPlatformVersion 10.0` | README still documents Visual Studio 2015; solution-level `x64` platforms are mapped to `Win32` |
| Platforms | `Win32` (x86) only | `Release|x64` and `Debug|x64` have no build entries, so no 64-bit path exists |
| UI subsystem | Raw Win32 (`RegisterClass`, `CreateWindow`, `WM_PAINT`, `TextOutA`, `CreateFont("Fixedsys")`), popup menus | The status "console" is a painted GDI window; server state is displayed by `DrawConfig` |
| Networking | Winsock 1.1 (`WSAStartup(MAKEWORD(1,1))`), `WSAAsyncSelect`, window-message demultiplexing, blocking `connect`, `SOCK_STREAM` | Single-threaded async pattern; no IOCP, no overlapped I/O, no thread pool |
| Framing/serialization | Custom `HEADER` (`Size`, `KeyWord`, `CheckSum`, `Type`, `ID`, `ClientTick`), per-byte transform against a static 512-byte table, 8-bit additive checksum, one `INITCODE` handshake dword | Binary-packed structs with `#pragma pack`; no schema, no versioning beyond `APP_VERSION` in `_MSG_AccountLogin` |
| Persistence | Raw binary files via `_open`/`_read`/`_write`/`_filelength`; text logs via `fopen`/`fprintf` | Fixed-size record images; no DBMS, no ORM, no migrations |
| Content pipeline | CSV (`itemlist.csv`, `InitItem.csv`, `SkillData.csv`, `extraitem.csv`), text (`Guilds.txt`, `serverlist.txt`, `MobName.txt`, `Language.txt`), binary (`ItemList.bin`, `SkillData.bin`, `InitItem.bin`, `AttributeMap.dat`, `heightmap.dat`, `UI/strdef.bin`) | Server both consumes and generates binary caches; `ItemEffect.h` doubles as build header and runtime data file |
| Libraries | Win32 + CRT only; `Winmm.lib`, `ws2_32.lib`, `kernel32/user32/gdi32/winspool/comdlg32/advapi32/shell32/ole32/oleaut32/uuid/odbc32/odbccp32` linked | `odbc32`/`odbccp32` and several shell/COM libraries are linked but no ODBC, registry, or COM call exists in the sources; only `<list>`/`<vector>` from the STL is used (in `CNPCGene.cpp`) |
| Runtime libraries | Debug: `MultiThreadedDebug` (static); TMSrv Release: `MultiThreadedDebugDLL`; DBSrv Release: default | Mismatched CRT flavour between configurations and between the two projects |
| Observability | Text log files (`.\Log\TM_*.txt`, `.\Log\DB_*.txt`, chat log, item log, day log), `SetWindowTextA` status line | No metrics, tracing, health check, or structured logging; log calls use variadic `fprintf` with runtime-built format strings |
| Testing / CI | None present | No test project, no test framework, no CI configuration (only `openspec/config.yaml`, which is out of scope) |
| Packaging / deployment | Manual: build `Debug|Win32` or `Release|Win32`, copy executable next to the expected directory tree | No installer, container, service wrapper, or config templating |

---

## 8. Security Architecture and Risks

### 8.1 Trust boundaries

1. **Client -> TMSrv (port 8281)** - untrusted, internet-facing. Authenticated only at the application layer (`_MSG_AccountLogin` + optional `_MSG_AccountSecure` PIN/token), after which identity is the socket-to-slot mapping.
2. **TMSrv -> DBSrv (port 7514)** - semi-trusted; DBSrv admits a peer only if its IP appears in `serverlist.txt` for the configured `ServerIndex`, otherwise the socket is closed.
3. **NP admin tool -> DBSrv (port 8895)** - privileged; two-factor-ish admission (IP allow-list from `Admin.txt` + account password + `Encode1/Encode2` challenge), then the account's highest character level determines admin level.
4. **Billing server -> TMSrv** - inbound frames are read but `ProcessBILLMessage` is empty; billing influence is effectively outbound-only.
5. **File system -> both servers** - any process able to write `../../Common`, `Admin.txt`, `serverlist.txt`, `Guilds.txt`, or the item/skill CSVs can alter game and privilege behaviour without touching either executable.
6. **Both servers -> web server** - DBSrv writes population HTML/CSV into a web publication path (`C:/wamp/www/servNN.htm`), coupling the game host to a web host.

### 8.2 Transport and protocol integrity

- Traffic protection is **obfuscation, not cryptography**: every byte is shifted by a value from a compile-time 512-byte table (`pKeyWord`, annotated "7.xx keys") selected by a per-message random keyword byte. The same table is embedded in the client, so it is a shared static secret recoverable from either binary.
- Integrity is an **8-bit additive checksum** (`Sum2 - Sum1` over `unsigned char`). Weak against both accidental and deliberate modification.
- On checksum mismatch `ReadMessage` still returns the message pointer and sets `ErrorCode = 1`; the dispatchers in `Server.cpp` break out of the loop on `Error == 1 || Error == 2`, but the DB path reassigns `ret`/`Error` in ways that make the guarantee subtle (`if (ret == FALSE) { ret = WSAGetLastError(); ... }`).
- **No per-message length validation**: `BASE_CheckPacket` - which maps every `_MSG_*` type to `sizeof(MSG_*)` - is fully commented out and unconditionally returns `FALSE`. Only the global envelope (`HEADER <= Size <= MAX_MESSAGE_SIZE = 8192`) is enforced.
- Handshake is a single magic dword (`INITCODE 0x1F11F311`) prefixed to the stream; there is no nonce, session key, or replay protection. `ClientTick` is echoed back (`pSMsg->ClientTick = CurrentTime`) and used for a `SKIPCHECKTICK` bypass in `ProcessClientMessage`.
- Inter-server links reuse the same framing and obfuscation; `_AUTH_GAME` (196 bytes) is an opaque struct whose layout is marked "NEEDS TO BE FIXED" in the header and filled/parsed without validation.

### 8.3 Authentication and credential handling

- Account passwords are stored **in clear text** inside `STRUCT_ACCOUNTINFO.AccountPass` (16 bytes) in the binary account record and compared with `strcmp` (`CFileDB.cpp:677`) and `strncmp` (`CFileDB.cpp:1302`, `Server.cpp:1708`).
- No hashing, salting, stretching, or KDF exists anywhere in the codebase.
- Administrative credential material lives in plaintext files: `Admin.txt` (allowed admin IPs, re-read on every admin accept) and `redirect.txt` (`adminclientid`, `adminclientpass`).
- Partial mitigations that do exist: a failed-login counter (`CheckFailAccount`, threshold 3), an account disable/expiry window (`DisableAccount`/`EnableAccount` with `Year`/`YearDay`), a MAC-address block list (`Mac.txt`, `pMac[MAX_MAC]`, `GetAccountsByMac`), and an optional numeric PIN (`_MSG_AccountSecure`, `SecurePass`, `NumericToken[6]`).
- MAC address arrives in `_MSG_AccountLogin.AdapterName[4]` from the client and is trusted as an identity/anti-multi-client signal (`pMac`, `STRUCT_BLOCKMAC`, `MAX_ACCOUNTMAC`), which is trivially spoofable and is also used as a ban mechanism.

### 8.4 Authorization

- **Admin level = character level - 1000.** `_MSG_MessageWhisper` treats `/gm` as privileged when `CurrentScore.Level >= 1008`, and DBSrv promotes an NP account when any character's `BaseScore.Level >= 1000`. Authorization therefore depends on game-state integrity rather than a separate principal/role store.
- `ProcessImple` guards with `if (conn <= 0 && conn >= MAX_USER) return;` - a conjunction that can never be true, so the intended rejection of out-of-range `conn` is not enforced (the `conn == 0` system path is legitimate, but the guard itself is inert).
- Privileged UI paths also exist outside the protocol: `WM_CREATE` installs a menu with `IDC_MOBRELOAD`, `IDC_REBOOT`, `IDC_SAVEALL`, `IDC_READGUILD`, `IDC_READGAMECONFIG`, `IDC_SHUTDOWNNP`, giving local desktop access operational control of the server.
- DBSrv's admin accept loop reads `pAdmin[i].IP` for `i <= MAX_ADMIN` while `pAdmin` has exactly `MAX_ADMIN` (10) elements - an out-of-bounds read past the last slot on every admin connection.

### 8.5 Input handling and memory safety exposure

- **Unbounded copy from packet to fixed field:** `_MSG_AccountLogin` does `sscanf(m->AccountName, "%s", pUser[conn].AccountName)` into a 16-byte field from a 16-byte packet field that may not be NUL-terminated, so the `%s` conversion can read past the field and write past the destination.
- **Widthless string formatting at large scale:** 715 `sprintf`, 102 `strcpy`, 57 `strcat` call sites; the heaviest offenders are `_MSG_UseItem.cpp` (145 `sprintf`), `Server.cpp` (104 `sprintf`, 14 `strcpy`, 3 `strcat`), `_MSG_Quest.cpp` (71 `sprintf`), `imple.cpp` (14 `strcpy`, 10 `strcat`), and `CNPCGene.cpp` (45 `strcpy`), the last copying generator text into fixed 80-byte action buffers (`SegmentAction[5][80]`, `FightAction[4][80]`, ...).
- **Format-string exposure:** `Log()` builds `LogTemp` with `sprintf(LogTemp, "%s %s %s \n", LogTemp, str2, str1)` (self-referential) and then calls `fprintf(fLogFile, LogTemp)`; `CReadFiles::UpdateConnectionData` also calls `fprintf(fp, temp)` with a runtime buffer. Attacker-influenced text (names, chat, admin commands) flows into these paths.
- **Index trust on privileged/failure paths:** the dead-character branch of `_MSG_UseItem`/`_MSG_PutoutSeal` indexes `Carry/SourPos`, `Cargo/SourPos`, `Equip/SourPos` directly from the packet before `GetItemPointer` is consulted.
- **Bounded paths exist and are used unevenly:** `GetItemPointer` validates type/position against `MAX_EQUIP`/`MAX_CARRY`/`MAX_CARGO` and validates `sIndex < MAX_ITEMLIST`; `_MSG_GetItem`, `_MSG_DropItem`, and `_MSG_UpdateItem` perform explicit range checks. The inconsistency, not the absence of all checks, is the exposure.
- **File-content trust:** `_read` of `heightmap.dat` into a 16 MB buffer ignores the returned byte count; `ParseString`/`ReadNPCGenerator` copy generator lines into fixed `NPCGENLIST` fields.

### 8.6 Filesystem-as-security-boundary

- Account and character names are concatenated into paths: `./account/%s/%s`, `./char/%s/%s`, `./capsule/%d`, `S:/export/account%d/%s`. Sanitisation consists of uppercasing and rejecting the DOS device prefixes `COM0-9`/`LPT0-9`; there is **no charset validation** (no rejection of `.`, `/`, `\`, `:`, `%`, spaces) on the account name received from the client before it reaches `DBReadAccount`.
- Character names *are* validated on the TMSrv side by `BASE_CheckValidString` (length 4..15, alphanumeric plus `-`, plus a reserved-word blacklist), but the account name is only `strupr`'d and `sscanf`'d.
- Export path `S:/export/account%d/%s` presumes a mapped network drive; the web path `C:/wamp/www/...` presumes a WAMP installation - both are hard-coded absolute paths belonging to the original operator environment.

### 8.7 Operational security posture

- Debug configuration disables ASLR (`<RandomizedBaseAddress>false</RandomizedBaseAddress>` in `TMSrv.vcxproj`); no `/DYNAMICBASE`, `/NXCOMPAT`, `/GUARD:CF`, or `/SAFESEH` overrides are present.
- A full PDB for the release executable is committed under `Code/release/TMSrv/run/TMSrv.pdb` (2.8 MB), and `.iobj`/`.ipdb` intermediates are committed as well.
- Servers run as interactive desktop applications with a visible window and an operations menu; there is no service account separation, no least-privilege deployment, and no sandboxing of the content directories.
- Anti-cheat consists of `AddCrackError` counters (packet anomalies, `_MSG_UpdateScore` honeypot at `ProcessClientMessage.cpp:106`), `CrackLog` text logging, and `NumError` thresholds that force a logout; there is no integrity attestation of the client.

---

## 9. Infrastructure Analysis

Infrastructure artifacts present: `.sln`, two `.vcxproj`, two `.vcxproj.user`, `.rc`/`.aps` resources, and a checked-in binary folder. **No Dockerfile, docker-compose, Kubernetes manifests, CI/CD pipeline, Makefile, or deployment scripts exist in the repository** (the only YAML is `openspec/config.yaml`, which is out of scope). The deployment model is therefore inferred from project settings, README, and the hard-coded runtime paths.

| Concern | Finding | Evidence |
|---|---|---|
| Build system | MSBuild via Visual Studio solution; two `Application` targets, `Win32` only; `PlatformToolset v143`; `WholeProgramOptimization` in Release | `W2PP Code Project.sln`, `Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj` |
| Build output | `OutDir` = `$(SolutionDir)..\release\$(ProjectName)\run\` (resolves to `E:\wyd\test-setup\release\...`, outside the repo) | same vcxproj files |
| Committed artifacts | `Code/release/TMSrv/run/TMSrv.exe` (509,440 bytes) and `TMSrv.pdb` (2,851,712 bytes); duplicate MSBuild intermediate trees (`Code/TMSrv/Release`, `Code/TMSrv/TMSrv/Release`, `Code/DBSrv/Release`, `Code/DBSrv/DBSrv/Release`) | directory listing |
| Debug launch config | `LocalDebuggerWorkingDirectory = $(OutDir)`; debugger `WindowsLocalDebugger` | `Code/TMSrv/TMSrv.vcxproj.user` |
| Runtime layout expected | Executable directory must contain/see: `gameconfig.txt`, `localip.txt`, `biserver.txt`, `LevelItem.txt`, `MobName.txt`, `Language.txt`, `heightmap.dat`, `serverlist.txt`, `./Log`, `../../Common/...`, `../../DBSRV/Run/{Admin.txt,Mac.txt}`, `./account`, `./char`, `./capsule` | `Code/TMSrv/Server.cpp`, `Code/TMSrv/CReadFiles.cpp`, `Code/DBSrv/CFileDB.cpp` |
| Process model | Two long-lived interactive Win32 GUI processes; no Windows service wrapper; shutdown via menu handler / `PostQuitMessage` / `ServerDown` countdown | `MainWndProc` in both `Server.cpp` files |
| Ports | 8281 (game), 7514 (DB), 8895 (admin), plus optional outbound billing and redirect connections | `Code/Basedef.h` `GAME_PORT`, `DB_PORT`, `ADMIN_PORT` |
| Scaling model | Vertical and shard-based: `MAX_SERVER` (10) game servers per DBSrv per server group, `MAX_SERVERGROUP` (10); shard identity from `localip.txt` + `serverlist.txt` | `Code/Basedef.h`, `Code/TMSrv/Server.cpp` (`WinMain` group/index resolution) |
| Capacity ceilings | `MAX_USER` 1000, `MAX_MOB` 25000, `MAX_ITEM` 5000, `MAX_NPCGENERATOR` 8192, `MAX_DBACCOUNT` 10000 (1000 x 10), `MAX_RANK_INDEX` 500, grids 4096 x 4096 | `Code/Basedef.h` |
| Environment/config management | File-based per-instance configuration; no templating, no environment variables (except an indirect dependence on `TMP`/CWD not used), no secrets management | `ReadConfig`/`DrawConfig` (both servers), `localip.txt`, `redirect.txt`, `Admin.txt` |
| Observability | Rolling text logs per process start (`TM_dd_mm_yyyy_hh_mm_ss.txt`, `DB_...txt`), chat log, item log, day log; GDI status window | `StartLog`, `StartChatLog`, `StartItemLog`, `DayLog_*` |
| Backup / DR | None in code; single copy of `./account`, `./char`, `../../Common/Ranking.txt` | `Code/DBSrv/CFileDB.cpp`, `Code/DBSrv/CReadFiles.cpp` |
| Dependency footprint | Zero third-party binaries/libraries; the executables depend only on the VC runtime implied by the configuration (`MultiThreadedDebug` or `MultiThreadedDebugDLL`) and system DLLs | vcxproj `AdditionalDependencies`, source `#include` inventory |

---

## Appendix A - Analysis Method, Coverage and Limitations

- **Coverage:** all 104 `.cpp`/`.h` files under `Code/` were inventoried; every header and every translation unit with architectural significance was read or symbol-scanned (line counts, function lists, `#include` sets, global declarations). Nine files (`ClientPatch_v7662` stub, `stdafx.h`, `resource.h` x2, `.rc` x2, `.aps`, `.ico`) were inspected only for role. Build/debug/binary artifacts (`*.iobj`, `*.ipdb`, `*.exe.recipe`, `*.pdb`, `*.exe`, `TMSrv/DBSrv.tlog`) were treated as non-source.
- **Coupling numbers** are approximate static reference counts, not per-symbol call-graph in-degrees (no compiler-grade dependency graph was produced). They are suitable for relative prioritisation, not for precision metrics.
- **Uncertainties:** the exact runtime directory layout could not be verified because the `Common/`, `DBSRV/Run/`, `Log/`, `account/`, `char/`, `capsule/` data trees and `heightmap.dat` are not committed; configuration values (`BILLING`, rates, event switch settings) are absent as `gameconfig.txt` is generated at first run. Behaviour of `ClientPatch_v7662` could not be analysed at all (no sources). The checked-in `TMSrv.exe` was not decompiled or compared against the sources.
- **Out of scope by request:** `.agents`, `.codegraph`, `.opencode`, `openspec`.
- No project files were modified during this analysis.
