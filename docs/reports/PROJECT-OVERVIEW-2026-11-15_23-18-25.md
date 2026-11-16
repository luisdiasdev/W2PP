# W2PP - Project Overview

**Generated on**: 2026-11-15 23:18:25

## Summary

W2PP is a C/C++ Win32 game-server emulator for the MMORPG "W2" (Hanbitsoft), described in its README as a decompilation of a "Polly's Server Release". The repository holds 104 source files and ~49,330 lines across two Visual C++ application projects — **TMSrv** (the game/world server, 87 files / 36,340 lines) and **DBSrv** (the account/character store, 12 files / 5,059 lines) — plus five shared sources at `Code/` root (`Basedef.cpp/.h`, `CPSock.cpp/.h`, `ItemEffect.h`, 7,931 lines). A third target, `ClientPatch_v7662`, exists only as a git-ignored `.vcxproj.user` stub with an empty `<PropertyGroup />` and is not part of the solution.

Architecturally the system is single-threaded and cooperative: both servers are `WinMain` GUI applications whose window procedure doubles as the socket demultiplexer (`WSAAsyncSelect`) and timer callback (`SetTimer`), and the codebase contains no threading primitives at all. Two runtime tiers communicate over custom-framed TCP (clients 8281, DB 7514, NP admin 8895), and DBSrv persists state by writing fixed-size binary images of `STRUCT_ACCOUNTFILE` (7,800 B) and `STRUCT_MOB` (816 B) straight to disk — there is no SQL engine, journal, lock, or transaction. A 2,505-line `Basedef.h` is the single domain/protocol contract for the entire system, declaring 100 `MSG_*` payloads and 191 message constants.

All 48 components identified by the architectural analysis were deep-analysed. The dominant cross-cutting results are: **no automated tests exist anywhere in the repository** (verified independently by every one of the 48 component reports via solution contents, filename/framework searches, absence of CI, and absence of assertions); the per-message packet size validation `BASE_CheckPacket` is entirely commented out and unconditionally returns `FALSE`; access control in both tiers derives from a single character-level comparison (`Level - 1000`) with no identity, role, or IP check; and dozens of features are provably unreachable due to tautological guards, inverted conditions, or write-only fields.

## Architecture Overview

**Deployment topology (inferred from project settings and hard-coded runtime paths).** Clients connect to TMSrv on TCP 8281. TMSrv connects to DBSrv on TCP 7514 and optionally to a billing server (`BillServerAddress:BillServerPort` from `biserver.txt`, active when `BILLING > 0`). DBSrv listens additionally on TCP 8895 for the external NP administrative tool (NPTool, not in the repository), publishes status HTML to a web root (`C:/wamp/www`) and CSV to `../../Common/dataNN.csv`, and reads/writes `./account`, `./char`, `./capsule`, `../../Common`, `./Log`, `localip.txt` and `serverlist.txt`. No container, orchestration, or CI manifests exist.

**Two runtime tiers, no shared library.** `Basedef` and `CPSock` are compiled independently into both executables. TMSrv's `Server.cpp` is a 9,449-line, 110-function God object owning the Win32 host, configuration, combat, guild, teleport, item economy, persistence and logging. Player sessions and mobs share one array (`pUser[1000]`, `pMob[25000]`, where `pMob[0..MAX_USER-1]` are players). Static pools account for a 180 MB `.data` virtual size in the shipped binary.

**Inbound paths.** A single `ProcessClientMessage` router validates ID range and `ServerDown` state, drops `_MSG_Ping`, and dispatches **58** `Exec_MSG_*` handlers (the architecture report's "61" was corrected by the component analysis, which counted 58 declarations, 58 definitions and 58 call sites). `ProcessDBMessage` consumes DBSrv responses, including a 447-line `_MSG_DBCNFCharacterLogin` branch. There is no `default:` label in the dispatcher, so unlisted message types are silently dropped.

**Outbound path.** `SendFunc` provides ~50 constructors and fan-out primitives (`SendNotice`, `SyncMulticast`, `GridMulticast`, `MapaMulticast`, `SendCreateMob`, `SendScore`, `SendShopList`). `GetFunc` provides ~36 read-only derivations. Two periodic ticks drive the world: a 500 ms `ProcessSecTimer` (25 passes) and a 12 s `ProcessMinTimer` (14 steps).

**Coupling profile.** `Basedef.h` has the highest afferent coupling (~95 translation units); `TMSrv CUser` is the most stable dependency (touched by 61 of 63 TMSrv TUs, instability ≈0.05). Command handlers are uniformly leaf-like (Ca 1, Ce 14). `Admin Command Interpreter` is the least stable (0.96).

**Transport.** `CPSock` is a Winsock async-select wrapper with 128 KB fixed send/recv buffers, a 12-byte header, a size window of 12–8192, a hard-coded 512-byte obfuscation table and an **8-bit additive checksum**. Runtime negotiation requests Winsock **1.1** (`WSAStartup(MAKEWORD(1,1))`), not 2.2.

## Dependencies Health

The project has **no package manifests, no lockfiles, and no vendored third-party source**. Every dependency is supplied by Microsoft or the OS: MSVC 14.44.35207 (toolset `v143`), Windows SDK 10.0.26100.0, Winsock, core Windows import libraries, and an orphaned MFC include. No library-version CVE identifiers apply.

Critical issues:

- **The shipped "Release" binary links the debug CRT.** `Code/release/TMSrv/run/TMSrv.exe` imports `MSVCP140D.dll`, `VCRUNTIME140D.dll` and `ucrtbased.dll`. `Code/TMSrv/TMSrv.vcxproj:80` sets `MultiThreadedDebugDLL` in `Release|Win32`; the PDB compile line confirms `-DNDEBUG ... -MDd`. The artifact is non-redistributable and cannot start on a clean host.
- **The committed Release configurations cannot reproduce the shipped binary.** Neither `Release|Win32` declares `<AdditionalDependencies>`, yet the code requires `ws2_32.lib` and `winmm.lib`, which are not in the MSBuild Win32 core default set. The shipped `.exe` does import both, proving it came from a project state not in source control.
- **Toolchain drift.** Declared/observed `v143` / 14.44.35207 and SDK 10.0.26100.0 versus current `v145` / 14.50 (VS 2026) and SDK 10.0.28000.2957.

High issues: Winsock 1.1 requested while the code mixes `winsock.h` and `winsock2.h` across projects; `odbc32.lib`/`odbccp32.lib` linked but never called; MFC declared in `DBSrv/stdafx.h` but never included by any translation unit; no CI, no lockfile, floating `WindowsTargetPlatformVersion`. Artifact inspection also found the binary was built from the `test-setup` lineage (76 TUs including `DumpStruct.cpp`, absent at HEAD) and embeds an ungated packet-fixture dumper writing `fixtures.ndjson`.

## Components Analyzed

All 48 components from the architectural report's Critical Components Analysis were analysed; each has a dedicated report in `component-deep-analyzer/`.

**Servers and hosts**

- **TMSrv Server Core** — 9,449-line Win32 host and world-state owner; 89 rules; debt concentrated in config parsing and DB reconnect (which always quits).
- **DBSrv Server Core** — host, admission control and NP command interpreter; 24 rules; the architecture report's "ServerDown control" does not exist in DBSrv; `pAdmin[10]` OOB read and an unauthenticated `_MSG_NPDonate`.

**Dispatchers and contracts**

- **Client Message Dispatcher** — 13 rules; four-guard chain, 59 case groups, no `default:`, zero payload validation delegated to handlers.
- **Client Handler Interface Contract** — 58 `Exec_MSG_*` entry points; documented `sizeof(m)` pointer-size use as a packet size at `_MSG_Action.cpp:272,278`.
- **DB Message Dispatcher** — 51 rules over 30+ DBSrv producer sites; the 447-line character-login confirmation branch has 18 enumerated sub-rules.

**Command handlers (TMSrv)**

- **UseItem Handler** — 160 KB, 5,726 lines, 68 dispatch branches, 86 rules; the largest single unit in the project.
- **Quest Handler** — 35 NPC modes, 50 rules, 33 graded findings including an unrewarded quest and an OOB `Carry[i+1]`.
- **Attack Handler** — 82 rules; OOB read on `SkillIndex == 103`, HP desync at `:1647`, per-skill cooldowns computed then never read.
- **Whisper and GM Chat Handler** — 38 rules; GM privilege is purely `Level - 1000`; `/create` deducts coins before a `fopen` that can fail with no rollback.
- **CombineItemOdin Handler** — 11 recipes, 29 rules; process-global `strcat` overflow, `rand()%1 == 0` deterministic break.
- **CombineItemEhre Handler** — 8 recipes, 34 rules; the `rand()%115 -15` fold makes every configured success rate lower than documented; recipe 5 can reset a max-level character to level 0.
- **TradingItem Handler** — 31 rules; an unsatisfiable warehouse `WarpID` guard admits a negative index into `pMob`.
- **Trade Handler** — 30 rules; settlement authority is a whole-struct `memcmp` freeze; failed settlement leaves both sessions latched.
- **Action Handler** — 26 rules; the `sizeof(pointer)` frame bug confirmed; the 900 ms movement limiter is permanently dead.
- **Buy Handler** — 26 rules; `goto DonateBuy` skips initialisation of prices read later; OOB `g_pGuildZone[Village]` read before its range guard.
- **Sell Handler** — 28 rules; 24-entry risk register.
- **GetItem Handler** — 22 rules; no ownership model exists (`CItem` has no owner field); `itemID == 1727` compares a world slot, not `ITEM.sIndex`.
- **DropItem Handler** — 20 rules; transposed decay grid indices at `Server.cpp:8381`.
- **ApplyBonus Handler** — 30 rules; unsigned-short wraparound makes an inflated skill bonus permanent.
- **MessageChat Handler** — 25 rules; remote format-string reachability via the chat audit path; ineffective censor; no rate limiting.
- **AccountLogin Handler** — 20 rules; `AddFailAccount` has zero callers, so the 3-failure gate is unreachable; version gating compiled out in Debug.
- **CharacterLogin Handler** — 13 rules, 4 provably unreachable; the billing/parental ladder is inert project-wide.
- **UpdateItem Handler** — 22 rules; `Server.cpp:7472` always returns early, making its eviction loop and gate-spawn branch unreachable; `g_pGroundMask` is never populated.
- **Remaining Client Command Handlers** — the 40-file family, 77 rules; free-slot off-by-two writing `Carry[60]`, `ReqBuy` precedence bug, double 50M charge in `CombineItemAilyn`.

**Layers and services**

- **SendFunc Outbound Projection Layer** — 35 rules across 50 functions; 528-vs-196 byte over-read in `SendAutoTrade`; gameplay mutation inside `GridMulticast`.
- **GetFunc Derivation Layer** — 25 rules; `GetMatchCombineTiny` unreachable via a tautology; `GetAttackArea` dead behind `if (0 == 1)`.
- **Timer Scheduler** — 54 rules; `SecCounter` is a 500 ms counter and `MinCounter` a 12 s counter, so every "seconds/minutes" constant is off by 2× or 5×; no drift compensation; inverted MAC-block reload.
- **MobKilled Resolution Module** — one 2,469-line function, 45 rules; `rand()` is never seeded anywhere in `Code/`.
- **Admin Command Interpreter** — 90 rules over 65 commands and 60 `set` subcommands; authority derives solely from level; `pUser[].Admin` is write-only.
- **CMob Entity and AI** — 25 rules; 5 of 8 state constants are never assigned; three OOB reads in `SetSegment`; leash anchors on the current waypoint.
- **CNPCGene Spawn System** — 32 rules, 27 risks; ~25.5 MB `pList`; no spawn content ships in the repo.
- **CCastleZakum Event Module** — 30 rules, 33 risks; writes to `CastleQuest[-1]` and `CastleQuest[64]`; duplicate boss payouts.
- **CWarTower Event Module** — 28 rules; duplicate fame award on multi-capture wars; latched state on a missed minute-59 tick.
- **TMSrv CReadFiles Config Loader** — 34 rules; inverted reload guard re-parses the MAC list ~2×/second; dead `ReadMobMerc` with two latent OOB writes.
- **TMSrv CUser Session Entity** — 62 rules; `pUser[n].Admin = 1` index bug; `CloseUser` slot leak and shallow reset (4 of 55 fields).
- **CItem Ground Item Entity** — 22 rules; anemic 76-byte record with no owner/isolation model.
- **Language Message Catalog** — 18 rules; 530 defines, of which 215 index values have no reachable code path.
- **Basedef Domain Contract** — 21 rules; 191 message constants evaluated numerically (0 exact collisions, 30 base IDs reused across 18 flag masks); struct sizes measured by compiling the header.
- **Basedef Rules Engine** — 30 rules over 101 functions; 639 `BASE_*` call sites; `BASE_CheckPacket` body commented out.
- **CPSock Transport** — 25 rules; 8-bit additive checksum; full-buffer condition misclassified as connection close.
- **ItemEffect Dictionary** — 26 rules; loader off-by-one at `Basedef.cpp:5222` and a `j`/`v` confusion that stores effect ID 128 in a 0–127 namespace.

**DBSrv persistence**

- **CFileDB Account and Character Store** — 39 rules; non-atomic in-place overwrite, 12 of 16 save sites ignore the write result; `GetIndex(char*)` returns 0 for both "not found" and record (0,0).
- **RankingSystem** — 22 rules; class tier absolutely dominates experience, with two inconsistent formulations; a `ClassMaster` gate mismatch permits stored-experience regression.
- **DBSrv CReadFiles Bulk I/O** — 20 rules over watched-directory import/export and HTML/CSV publishing.
- **DBSrv CUser Server Slot** — 21 rules; unauthenticated `_MSG_NPCreateCharacter`/`_MSG_NPDonate`; unseeded `rand()` challenge with `||` verification.

**Build resources and artifacts**

- **ClientPatch_v7662 (source absent)** — no code exists; git history shows a 53-line `main.cpp` with two hardcoded client byte patches, deleted in `dfe35d0`.
- **Win32 Resource Bundle** — no `VERSIONINFO` and no menu resources; both servers call `LoadIcon(hInstance, "MAINICON")` but only ordinal 101 is exported, so the icon never loads.
- **Prebuilt Release Artifact Set** — EXE/PDB pair (GUID `1ae429cb-d036-4e88-ae81-7a57e3b0ae66`), debug CRT confirmed, unsigned, no CFG, no `VERSIONINFO`, 172.5 MiB `SizeOfImage` from a 180 MB `.data` section in a 509 KB file.

## Critical Findings

### Security Risks

- **No structural packet validation.** `BASE_CheckPacket` (`Basedef.cpp:6475-6596`) is entirely commented out and returns `FALSE`; per-message size assertions across handlers are dead code, and 57 of 58 handlers perform no size-versus-struct validation.
- **Plaintext credentials.** Account passwords are stored verbatim in binary account records and compared with `strcmp`/`strncmp` (`CFileDB.cpp:1302`, `Server.cpp:596`, `CReadFiles.cpp:1021`); there is no cryptographic dependency anywhere.
- **Level-derived authority.** Admin access is granted when a character's level is ≥ 1000 (`Level - 1000` becomes the admin level); `pUser[].Admin` is written but never read; the `conn` guard in `imple.cpp:91` is a tautology repeated five times. The DB admin path trusts a client-supplied `Level`.
- **Unauthenticated NP surfaces.** `_MSG_NPDonate` is reachable without authentication; `_MSG_NPCreateCharacter` likewise. The NP challenge uses an unseeded `rand()` with `||` verification.
- **Format-string injection.** Player-supplied text reaches `fprintf(stream, LogTemp)` at `Server.cpp:9244`/`6723`, and `CombineItemTiny` misuses a format string.
- **Memory-safety defects with attacker reach.** `SendAutoTrade` 528-vs-196 byte over-read; `SendClientMessage` fixed 96-byte over-read; attacker-controlled `m->Size` re-broadcast to up to 1,088 clients via `GridMulticast` (up to 8,188 bytes of stale send-buffer disclosure); OOB reads at `SkillIndex == 103`, `pAdmin[10]`, `g_pGuildZone[5]`, `pItem[ival2]`, `CastleQuest[-1]`/`[64]`, `SegmentListX[5]`/`[-1]`, and `Carry[-1]`.
- **Buffer overflows.** Unbounded `strcpy` into `MobName[16]` from `read`/`name`; unbounded `%s` stack writes in the castle parser; process-global `strcat` overflow in `CombineItemOdin`.
- **Economy exploits documented with evidence.** Stack inflation via missing self-swap check (`TradingItem`), permanent skill-bonus wraparound (`ApplyBonus`), free-slot off-by-two into `Carry[60]` (`ReqBuy`), double 50M charge (`CombineItemAilyn`), duplicate fame/boss payouts, and a `combine == 0` destructive fallback.
- **No rate limiting** anywhere in the chat, login, trade, or packet paths; the movement limiter is dead.
- **Debug CRC linkage.** The shipped binary's debug CRT and embedded fixture dumper (`fixtures.ndjson`, `.git\HEAD` revision stamping, login packet logging) constitute a data-exposure surface.

### Technical Debt

- **Zero automated tests, fixtures, or CI project-wide.** Every one of the 48 component reports independently verified: only two `Application` projects in `W2PP Code Project.sln`, no test-named files, no framework references (gtest/Catch2/CppUnit/doctest/Boost.Test), zero `assert`/`static_assert` in `Code/`, no CMake/Makefile/CI YAML. The only test-like hook is the `LOCALSERVER` hardcoded bypass (`192.168.0.50`) that forces 100 % crafting success, plus the prebuilt binary's fixture dumper.
- **Large dead-code surface.** Provably unreachable paths were documented in nearly every component: `UpdateItem`'s eviction loop, `GetMatchCombineTiny`, `GetAttackArea`, five of eight `CMob` states, `AddFailAccount`, `_MSG_Action2`'s bypass, dead `MOB_*` transitions, dead `CItem::Money`, dead `CreateTreasureBox`, dead `DBCheckImpleName`/`InitAccountList`/`SendDBMessage`, and the whole `PKDrop` compile-time branch.
- **Inverted or tautological logic.** Inverted `SecCounter % 120` reload, inverted `WSAAsyncSelect(...) > 0` polarity, unsatisfiable `RouteType <= 0 && RouteType > 4` and warehouse `WarpID` guards, `conn <= 0 && conn >= MAX_USER`, `!= 4 || != 5` recipe tautology, `i % 8 == 0` after loop exit, and `>` instead of `>=` bounds in two dispatchers.
- **Initialisation and durability defects.** `goto` over initialisation in `Buy`, uninitialised `GuildTax` and `atoi` buffers, `Pista[3].Party[3]` OOB aliasing, non-atomic binary persistence with undetected short writes and no eviction sweep, capsule restore ordering that writes the account before the cache, and `TempKey` never persisted.
- **Unit/convention drift.** 500 ms/12 s counters documented as seconds/minutes; a 50-vs-100 fame comment/code mismatch; a 1000× HP-divisor mismatch against its own comment; two incompatible ranking order predicates; `ClassMaster` reused as party size; `long long → int` truncations in exp and ranking.
- **Configuration and build hygiene.** Missing `AdditionalDependencies` in Release, inverted runtime-library selection, an unconfigured UTF-8 key literal in an MBCS build, a case-mismatched `DBSrv.rc` dialog ID (`IDD_MAINDLG` vs `IDD_MainDlg`), a git-ignored `.aps` cache with a stale absolute path, and no icon actually loading.
- **Absent content.** No `Common/` data tree, no `NPCGenerator.txt`, `LevelItem.txt`, `ItemList.bin`, `itemlist.csv`, or `UI/strdef.bin` in the repository, so archetypes, message strings and spawn tables cannot be verified from source alone.

### Single Points of Failure

- **`Basedef.h` as the universal contract** — ~95 afferent translation units; a single header change propagates to every module in both servers.
- **`Basedef.cpp` rules engine** — 639 `BASE_*` call sites across 47 TUs; it hosts the disabled packet validator, all score/combat math and every content loader.
- **TMSrv `Server.cpp` God object** — one 9,449-line translation unit owns the host, all world globals (`pUser[]`, `pMob[]`, `pItem[]`), combat, guilds, teleport, economy, config and logging.
- **`TMSrv CUser` session entity** — 61 of 63 TUs read `pUser` directly (1,630 field accesses); instability ≈0.05.
- **`CMob` entity** — ~67 afferent units; player slots and mobs share the same `pMob[]` array.
- **Single-threaded cooperative loop** — every packet, timer tick and disk write executes on one thread with no drift compensation; one blocking call (e.g. the `MessageBoxA` in the boot path) stalls the entire server.
- **`CPSock` framing** — one transport implementation carries client, DB and billing traffic; a full send buffer is misclassified as a closed connection.
- **DBSrv binary file store** — no transaction, journal, lock or backup; a single corrupted or short-written account/character image has no recovery path, and 12 of 16 save sites ignore the write result.
- **Unseeded RNG** — `rand()` is never seeded anywhere in `Code/`, making every RNG-derived rule (loot, crafting, challenge values) deterministic across restarts.
- **Hard-coded environment dependencies** — `C:/wamp/www` publish root, `192.168.0.50` `LOCALSERVER` bypass, `../../Common` data path, and plaintext `localip.txt`/`serverlist.txt`/`Admin.txt` allow-lists.

## Reports Index

See [MANIFEST.md](./MANIFEST.md) for the complete list of all generated reports.
