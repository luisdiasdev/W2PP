# W2PP - Project Overview

**Generated on**: 2026-08-19 17:17:00

## Summary

W2PP is a legacy Windows-native MMORPG private-server backend for the game "W2", derived from a decompilation of "Polly's Server Release" and written entirely in C/C++. It is a single Visual Studio solution containing two Win32 GUI applications — **TMSrv** (the game/connection server, ~44,300 lines) and **DBSrv** (the accounts/characters server, ~6,500 lines) — that communicate over a custom binary socket protocol defined in the shared `Basedef.h` header. Both servers use a single-threaded, Win32 message-pump architecture driven by asynchronous Winsock sockets (`WSAAsyncSelect`), with all game simulation, network I/O, and persistence serialized on one thread.

The codebase (~113 files, ~50,800 lines in TMSrv) is a tightly coupled monolith that is heavily dependent on global state, raw packet-struct casting, and file-based persistence (per-account binary files rather than a relational database). Persistence, account handling, and all core game logic flow through a small number of very large translation units (`Server.cpp` ~9,400 lines, `CFileDB.cpp` ~2,700 lines, `MobKilled.cpp` ~2,500 lines). The architecture is characterized by minimal abstraction, no automated tests anywhere in the project, and legacy-grade security controls.

## Architecture Overview

The system is layered but tightly coupled: `Server.cpp` (socket I/O layer) → `ProcessClientMessage`/`ProcessDBMessage` (dispatchers) → `_MSG_*` per-message handlers (business logic) → `GetFunc`/`SendFunc`/`Basedef` (shared helpers). A central `switch`-based packet dispatcher routes client messages to 58 dedicated `Exec_MSG_*` handler files. The two server binaries are coupled by a shared-header contract (`Basedef.h`), which defines both the wire protocol and the domain model, requiring lockstep rebuilds. Clients connect to TMSrv on port 8281; TMSrv communicates with DBSrv on port 7514 and optionally a billing server; DBSrv exposes an admin port (8895) for the NPTool and a redirect/transfer client. Persistence is file-based with no transactions, and periodic world updates run from `ProcessSecMinTimer`.

## Dependencies Health

The project has **no third-party open-source runtime libraries** and no package manager or lockfile; all dependencies are Microsoft platform components (Winsock 2, Winmm, Windows SDK, CRT/STL) plus the commercial Themida protection tool. Critical issues: a proprietary, non-standard custom packet "encryption" key table in `CPSock.cpp` (`pKeyWord[512]`) with no vetted crypto library; all network traffic is plaintext TCP with no TLS; the original Visual Studio 2015 toolchain is End of Life (extended support ended 2025-10-15); MFC headers are referenced but no MFC libraries are linked; and dead ODBC libraries are linked but unused. Builds are non-reproducible due to the absence of a package manifest.

## Components Analyzed

- **CPSock** — Shared async-socket + buffering transport wrapping Winsock; implements a custom `HEADER`-framed protocol with a `pKeyWord`-driven obfuscation/checksum scheme. Highest-security-relevant component; zero tests.
- **Basedef** — Shared foundation: wire protocol (98 `_MSG_*` types + 100 packed structs), domain model (24 `STRUCT_*` structs), and 99 `BASE_*` game-rule functions. Highest fan-in (639 call sites); packet-size validation commented out.
- **Server (TMSrv)** — Win32 message-pump game server; the architectural hub. Single-threaded; handles 1000 users (MAX_USER), boots/accepts connections, drives dispatch and shutdown. Largest monolith file.
- **ProcessClientMessage** — Central inbound packet dispatcher routing client packets to handlers; includes anti-spoof (`SKIPCHECKTICK`) and inline crack-detection guards. No `default:` case (silent drops).
- **ProcessDBMessage** — DBSrv response dispatcher applying database-confirmed outcomes to live game state; a ~1,330-line monolithic switch with a stale prototype mismatch in its header.
- **_MSG_* Handlers (58)** — Per-message business logic handlers covering movement, combat, trading, item combining, guilds, quests, chat, and more. Monolith handlers (`_MSG_UseItem` 5,726 lines); piecemeal coin-cap enforcement.
- **SendFunc** — Outbound packet builders; `SendClientMessage` has ~671 call sites. Includes `GridMulticast` view-grid reconciliation. Missing bounds guards and a suspected duplicated-condition bug.
- **GetFunc** — Shared game-logic helpers (combat scoring, item-combination recipes, positioning, packet packing, player-meta persistence). Suspected latent bug in empty-grid scanners; duplicated ~280-line blocks.
- **CUser (TMSrv)** — Per-player session state (`pUser[1000]`); highest fan-in component (63 files, 1227 references). Session state machine plus idle/anti-speedhack/rate limits.
- **CMob** — In-world entity + AI modeling both players and mobs in a 25,000-entity array; bitmask-returning standing/battle state machines. Dead skill-cast path; `GetCurrentScore` highest coupling.
- **CItem** — In-world dropped-item data class over `pItem[5000]` and the `pItemGrid` spatial index. Anemic data holder; `ProcessDecayItem` double-increment bug and unreachable branches.
- **CNPCGene** — Mob/NPC spawn generators and summons; passive data model/loader parsed from `NPCGener.txt`. High afferent coupling; unbounded config parsing.
- **MobKilled** — Monolithic kill-resolution funnel (2,469 lines) handling all deaths, EXP distribution, loot/drops, and PK penalties. 30 business rules; high efferent coupling; "god function" pattern.
- **ProcessSecMinTimer** — Heartbeat/world-update engine: `ProcessSecTimer` (500ms) and `ProcessMinTimer` (12s) aggregating ~40 scheduled subsystems. Copy-paste index errors in rune-track prize distribution.
- **imple** — In-process GM/admin command dispatcher with 90+ commands across four privilege tiers; also `SaveAll()`. Unbounded `sscanf`/`sprintf` and unguarded global mutation.
- **CCastleZakum** — Static class implementing the Castle Zakum party raid event; 12 business rules, two-stage clear state machine, timed resets.
- **CWarTower** — Static class driving the weekly guild tower battle event (three-phase state machine, ownership capture, fame award).
- **CReadFiles (TMSrv)** — Data/config-file loading into process-global tables (9 config files). Dead `ReadMobMerc()` code and an inverted-condition bug at a call site.
- **Server (DBSrv)** — File-backed accounts/characters server; accepts TMSrv and admin connections on ports 7514/8895; 19 business rules; no transactions, plaintext passwords.
- **CFileDB** — Persistence hub of DBSrv; routes 29 protocol cases; sole persistence authority. 1,900-line monolith, no transactions/checksums, plaintext passwords.
- **CReadFiles (DBSrv)** — File-based data import/export (item grants, account registration, ranking/guild snapshots). Hard-coded WAMP path; fuzzed monitoring data; plaintext password logging.
- **CRanking** — EXP leaderboard subsystem (500 slots) hosted in DBSrv; re-ranks and pushes snapshots to TMSrv. 64→32-bit EXP truncation risk; unimplemented methods.
- **CUser (DBSrv)** — Per-game-server session-state container; 11 business rules covering connection lifecycle, admin auth (challenge-response), and duplicate-session guards.

## Critical Findings

### Security Risks
- Custom packet "encryption" using a hardcoded 512-byte key table (`CPSock.cpp`) — weak, non-standard, trivially reversible; no vetted crypto library used anywhere (CPSock, Basedef, dependency report).
- All client↔TMSrv, TMSrv↔DBSrv, and TMSrv↔Billing traffic is plaintext TCP with no TLS (dependency report).
- Account passwords stored and compared in plaintext on disk and over the network (`STRUCT_ACCOUNTINFO.AccountPass`); no hashing/salting (architectural + CFileDB reports).
- Packet validation (`BASE_CheckPacket`) is only compiled under `_PACKET_DEBUG` (Debug builds); Release builds trust incoming packet structs with minimal validation (architectural report).
- Raw packet-struct casting from `char*` without rigorous validation — malformed/oversized packets can cause overreads/writes (architectural + ProcessClientMessage reports).
- Unauthenticated drop-folders for account/item imports and plaintext password logging in DBSrv `CReadFiles` (component report).

### Technical Debt
- Monolithic core files: `TMSrv/Server.cpp` (~9.4k lines), `DBSrv/CFileDB.cpp` (~2.7k lines), `MobKilled.cpp` (~2.5k lines), `ProcessSecMinTimer.cpp` (~2.3k lines), and monolith `_MSG_` handlers (up to 5,726 lines).
- Single-threaded cooperative model with a fixed user ceiling; cannot exploit multi-core hosts.
- File-based persistence with no transactions, locking, backup, or recovery mechanism.
- Shared-header protocol contract fusing wire protocol + domain model with no versioning/abstraction; requires lockstep rebuilds.
- Heavy global mutable state (extern arrays `pUser`, `pMob`, `pItem`, grids) shared implicitly across all handlers.
- Hard-coded magic numbers, coordinates, and item indices throughout rather than data-driven config.
- Zero automated test coverage anywhere in the project; all correctness relies on manual playtesting.
- Dead dependencies (ODBC libraries linked but unused), incomplete MFC linkage, stale SVN metadata, and legacy VS2015 references.

### Single Points of Failure
- `CPSock` — the sole networking abstraction used by both servers; its custom crypto and buffering underpin all communication.
- `Basedef.h` — shared-header contract that every module and both binaries depend on.
- DBSrv as a single instance serving all TMSrv instances (MAX_SERVER 10) — single point of failure for accounts/chars.
- `ProcessClientMessage` dispatcher — any networking/parse defect propagates to all gameplay features.
- `CFileDB` — sole persistence authority for accounts/characters; no transactional integrity.
- Single-threaded event loop — a long-running handler blocks all players and I/O.

## Reports Index

See [MANIFEST.md](./MANIFEST.md) for complete list of all generated reports.
