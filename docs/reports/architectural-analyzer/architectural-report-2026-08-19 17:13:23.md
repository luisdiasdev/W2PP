# Architectural Analysis Report

**Project:** W2PP (legacy C/C++ game server)
**Scope analyzed:** `/home/luisdias/dev/github/luisdiasdev/w2pp/legacy`
**Date:** 2026-08-19 17:13
**Folders ignored:** `.git`, `.opencode`

---

## 1. Executive Summary

The W2PP project is a legacy Windows-based Massively Multiplayer Online (MMO) game
server release for the game "W2", derived from a decompilation of a private-server
release. It is written entirely in C/C++ and targets Microsoft Visual Studio 2015
(project files are platform toolset v143, migrated from the original 2015 toolset).
The codebase is a single Visual Studio solution (`W2PP Code Project.sln`) containing
two Win32 GUI applications:

- **TMSrv** (`Code/TMSrv/`) — the game/connection server that clients connect to,
  managing in-game simulation, player state, combat, items, trading, guilds, parties,
  mobs, quests, and world events.
- **DBSrv** (`Code/DBSrv/`) — the accounts/characters database server that persists
  account and character data to the filesystem and serves one or more TMSrv instances.

The two servers communicate over a custom binary socket protocol defined in the shared
`Code/Basedef.h` header. Both applications use a **single-threaded, Win32 message-pump
architecture driven by asynchronous sockets** (`WSAAsyncSelect` + `WM_*` window messages)
rather than thread pools, which is the defining architectural characteristic of this
legacy codebase.

There are approximately 104 `.cpp`/`.h` source files (~113 total files in the tree
including resources), with TMSrv comprising the vast majority of the logic
(about 44,300 lines of `.cpp` in TMSrv vs. about 6,500 lines in DBSrv). The shared
`Basedef.h` (2,505 lines) and `Basedef.cpp` (6,814 lines) define the core data structures
and packet protocol.

**Key architectural findings:**

- Layered but tightly coupled monolith inside each executable, connected by a shared
  header-driven protocol.
- Network I/O and game simulation run on a single thread; all concurrency is cooperative
  via timers and socket events.
- Persistence is **file-based** (binary per-account files on disk), not a database server.
- Heavy use of raw pointers, casts between `char*` and packet structs, and fixed-size
  arrays (packed structs assumed to match the wire protocol).
- Security relies on client-supplied version checks, MAC/account failure counting, and a
  lightweight admin handshake; account passwords are stored in plaintext on disk.

---

## 2. System Overview

### Project Structure

```
legacy/
├── W2PP Code Project.sln            # Visual Studio solution
└── Code/
    ├── Basedef.h / Basedef.cpp      # SHARED: packet protocol + core data structures
    ├── CPSock.h / CPSock.cpp        # SHARED: asynchronous socket abstraction
    ├── ItemEffect.h                 # SHARED: item effect constants
    ├── DBSrv/                       # Accounts/chars database server
    │   ├── DBSrv.vcxproj
    │   ├── Server.cpp/.h            # Main loop, socket handling, admin protocol
    │   ├── CFileDB.cpp/.h           # Account/character file persistence layer
    │   ├── CReadFiles.cpp/.h        # Import/export of external data files
    │   ├── CRanking.cpp/.h          # Ranking system (player rank table)
    │   └── CUser.cpp/.h             # Per-connection session state
    └── TMSrv/                       # Game/connection server
        ├── TMSrv.vcxproj
        ├── Server.cpp/.h            # Main loop, sockets, core game logic (9.4k lines)
        ├── ProcessClientMessage.cpp/.h  # Client packet dispatcher
        ├── ProcessDBMessage.cpp/.h      # DBSrv response dispatcher
        ├── ProcessSecMinTimer.cpp       # Periodic timers / world updates
        ├── SendFunc.cpp/.h              # Outgoing packet builders (server->client)
        ├── GetFunc.cpp/.h               # Shared game-logic helpers
        ├── imple.cpp                    # Admin/command handler
        ├── MobKilled.cpp                # Combat/kill resolution
        ├── CMob.cpp/.h, CItem.cpp/.h    # Mob and item world entities
        ├── CNPCGene.cpp/.h              # NPC/mob generators & summons
        ├── CUser.cpp/.h                 # Per-player session state
        ├── CReadFiles.cpp/.h            # Data/config file loading
        ├── CCastleZakum.cpp/.h          # Castle (Zakum) event logic
        ├── CWarTower.cpp/.h             # Guild war tower logic
        ├── Language.h                   # Localized message strings
        └── _MSG_*.cpp (58 files)        # Per-message-type handlers
```

### Architectural Patterns Identified

1. **Message-Driven / Packet-Dispatcher pattern (primary).** Each incoming network
   message type (`_MSG_*`) has a dedicated handler (`Exec_MSG_*`) routed through a central
   `switch` in `ProcessClientMessage`. This is a classic command-dispatch table.

2. **Single-Threaded Event Loop (Win32 message pump).** Both servers use
   `GetMessage`/`DispatchMessage` in `WinMain`, with `WSAAsyncSelect` delivering socket
   events as window messages. All processing is serialized on the UI thread.

3. **Layered internal structure within each executable:** `Server.cpp` (I/O layer) →
   `ProcessClientMessage`/`ProcessDBMessage` (dispatcher) → `_MSG_*` handlers (business
   logic) → `GetFunc`/`SendFunc`/`Basedef` (shared helpers).

4. **Shared-header contract between services.** `Basedef.h` is compiled into both TMSrv
   and DBSrv, defining the exact packet layout and the `STRUCT_*` domain models. The two
   binaries are coupled by this source-level contract rather than an interface/IDL.

5. **File-based persistence (not a relational database).** DBSrv stores one binary
   account file per account under `./account/<first>/<name>`, plus guild/transfer/import
   text files.

---

## 3. Critical Components Analysis

### Definitions of Coupling Metrics

For this report, **afferent coupling (Ca)** is the number of distinct source components
(modules/files/functions) that depend on a given component — i.e., the incoming fan-in
(who uses it). **Efferent coupling (Ce)** is the number of distinct components that a
given component depends on — i.e., the outgoing fan-out (what it calls/uses). These were
estimated by analyzing call-site counts from the CodeGraph knowledge graph and `grep`
call-site tallies across the two projects. Higher afferent coupling indicates a component
is a widely-used hub (a common dependency, hence a stability point); higher efferent
coupling indicates a component reaches out to many others (higher fragility/volatility).
The metrics below are relative estimates based on call-site frequency, not an absolute
dependency-graph enumeration.

| Component | Type | Location | Afferent Coupling | Efferent Coupling | Architectural Role |
|-----------|------|----------|-------------------|-------------------|-------------------|
| CPSock | Infrastructure / Network | Code/CPSock.cpp/.h | Very High (used by all socket code) | Low (Winsock only) | Async socket + buffering abstraction for all services |
| Basedef | Core Definitions / Protocol | Code/Basedef.cpp/.h | Very High (included by every module) | Low | Central packet protocol + domain structs + game constants |
| Server (TMSrv) | Application Core | Code/TMSrv/Server.cpp/.h | High | Very High | Main loop, socket I/O, world/core game logic (largest file) |
| ProcessClientMessage | Dispatcher | Code/TMSrv/ProcessClientMessage.cpp | Medium (from Server) | High (all handlers) | Central switch dispatching client packets to handlers |
| ProcessDBMessage | Dispatcher | Code/TMSrv/ProcessDBMessage.cpp | Medium (from Server) | High | Routes DBSrv responses back to game handlers |
| _MSG_* Handlers (58) | Business Logic (per-message) | Code/TMSrv/_MSG_*.cpp | Medium (from dispatcher) | Medium-High | Individual game-action implementations |
| SendFunc | Messaging / Outbound | Code/TMSrv/SendFunc.cpp/.h | Very High (672+ call sites) | Medium | Builds and sends server-to-client packets |
| GetFunc | Shared Helpers | Code/TMSrv/GetFunc.cpp/.h | High | Medium | Shared game-logic utilities (combat, view, grids) |
| CUser (TMSrv) | Domain Entity / Session | Code/TMSrv/CUser.cpp/.h | High | Medium | Per-player connection + session state (incl. cargo/trade) |
| CMob | Domain Entity | Code/TMSrv/CMob.cpp/.h | High | Medium | Mob/player in-world entity and its AI state machine |
| CItem | Domain Entity | Code/TMSrv/CItem.cpp/.h | Medium | Low | In-world dropped item entity |
| CNPCGene | World Generation | Code/TMSrv/CNPCGene.cpp/.h | Medium (161+ sites) | Medium | Mob/NPC spawn generators, summons, map regions |
| MobKilled | Combat Resolution | Code/TMSrv/MobKilled.cpp | Medium | High | Kills, loot/drops, exp distribution |
| ProcessSecMinTimer | Scheduler | Code/TMSrv/ProcessSecMinTimer.cpp | Low (from Server) | High | Per-second/minute world updates, billing, affections |
| imple | Command/Admin Handler | Code/TMSrv/imple.cpp | Low | Medium | In-game command/administrator commands |
| CCastleZakum | Event System | Code/TMSrv/CCastleZakum.cpp/.h | Low | Medium | Castle (Zakum) siege event logic |
| CWarTower | Event System | Code/TMSrv/CWarTower.cpp/.h | Low | Medium | Guild war tower logic |
| CReadFiles (TMSrv) | Data Loading | Code/TMSrv/CReadFiles.cpp/.h | Medium | Medium | Loads config, item/mob data, guild/region files |
| Server (DBSrv) | Application Core | Code/DBSrv/Server.cpp/.h | Medium | High | DBSrv main loop, accepts game servers + admin tool |
| CFileDB | Persistence Layer | Code/DBSrv/CFileDB.cpp/.h | High | Medium | Account/character file storage + read/write + account mgmt |
| CReadFiles (DBSrv) | Data Import/Export | Code/DBSrv/CReadFiles.cpp/.h | Medium | Medium | Imports new accounts/items/donations from files |
| CRanking | Ranking Subsystem | Code/DBSrv/CRanking.cpp/.h | Low | Medium | Player ranking table (top-N by exp) |
| CUser (DBSrv) | Domain Entity / Session | Code/DBSrv/CUser.cpp/.h | Medium | Low | Per-game-server connection session state |

---

## 4. Dependency Mapping

### High-Level Component Dependencies

```
CLIENT (game client)
   │  TCP GAME_PORT (8281)
   ▼
TMSrv: Server.cpp (socket accept/read)
   │
   ├─► ProcessClientMessage.cpp ──► _MSG_* handlers ──► GetFunc / SendFunc / Basedef
   │                                                       │
   │                          ┌────────────────────────────┘
   │                          ▼
   │              CMob / CItem / CUser / CNPCGene / MobKilled
   │                          │
   │              ProcessSecMinTimer (world simulation)
   │
   │  TCP DB_PORT (7514)
   ▼
DBSrv: Server.cpp (accepts TMSrv connections)
   │
   ├─► ProcessClientMessage / ProcessAdminMessage
   │        │
   │        ▼
   │    CFileDB (account/character persistence)
   │        │
   │        ▼
   │    Filesystem (./account/... per-account binary files)
   │
   └─► AdminSocket / AdminClient (NPTool, redirect/transfer server)

TMSrv ──► BillServer (billing/auth, optional, TCP)
```

### Message Flow

1. Client connects to TMSrv on `GAME_PORT` (8281); TMSrv accepts and stores the session in
   `CUser pUser[MAX_USER]`.
2. Client sends packets; `Server.cpp` reads them and calls `ProcessClientMessage(conn, msg)`.
3. `ProcessClientMessage` validates the packet header and dispatches to the matching
   `Exec_MSG_*` handler.
4. Handlers use `GetFunc`/`Basedef` helpers to mutate world state and `SendFunc` to build
   responses back to the client; cross-user effects use `GridMulticast`/`SyncMulticast`.
5. Login/character/account operations are forwarded to DBSrv over `DBServerSocket`;
   `ProcessDBMessage` consumes the asynchronous replies.
6. DBSrv receives the request, performs file persistence via `CFileDB`, and replies to the
   originating TMSrv over that server's socket.
7. Periodic world events (mob AI, item decay, billing checks, castle/war state) run from
   `ProcessSecMinTimer` / `ProcessMinTimer`.

---

## 5. Integration Points

| Integration | Type | Location | Purpose | Risk Level |
|-------------|------|----------|---------|------------|
| Game Client | External client (TCP) | TMSrv `Server.cpp` on GAME_PORT 8281 | Primary inbound traffic; all gameplay packets | High |
| DBSrv (accounts) | Inter-process (TCP) | TMSrv `DBServerSocket` ↔ DBSrv `pUser[]` on DB_PORT 7514 | Account/character persistence & login flow | High |
| BillServer (billing) | External service (TCP) | TMSrv `BillServerSocket`, `SendBilling` | Optional billing/authentication; enabled by BILLING flag | Medium |
| NPTool / Admin tool | External admin (TCP) | DBSrv `AdminSocket` on ADMIN_PORT 8895 | Administrative account management | High |
| Redirect/Transfer server | External service (TCP) | DBSrv `AdminClient` (from `redirect.txt`) | Character transfer between servers (`TransperCharacter`) | Medium |
| Filesystem (accounts) | Local storage | DBSrv `CFileDB` / `CReadFiles` | Persistent account/character/guild data; import/export folders | High |
| BaseMob data files | Local files | DBSrv & TMSrv startup (`./BaseMob/*`) | Class base-mob definitions (TK/FM/BM/HT) | Medium |
| Item/Mob/Guild config files | Local files | TMSrv `CReadFiles` | Item list, mob generators, regions, guild zones | Medium |

---

## 6. Architectural Risks & Single Points of Failure

| Risk Level | Component | Issue | Impact | Details |
|------------|-----------|-------|--------|---------|
| Critical | Single-threaded event loop | No parallelism; all sockets + simulation on one thread | Scalability & responsiveness | Both TMSrv and DBSrv serialize all work; a long-running handler blocks all other players and I/O. 1000 users per server (MAX_USER) handled sequentially. |
| Critical | TMSrv `Server.cpp` monolith | Single 9,400-line file holding core loop + game logic | Maintainability, error blast radius | Fuses I/O, world state, and game rules in one translation unit; high coupling with GetFunc/SendFunc. |
| Critical | CFileDB file persistence | No transactional DB; raw binary file writes | Data integrity | Account files written directly; a crash mid-write can corrupt accounts. No locking/transaction; backups not evident. |
| High | Shared-header protocol contract | `Basedef.h` compiled into both services | Coupling / fragility | Any struct change in the shared header silently changes the wire contract for both binaries; must be rebuilt in lockstep. |
| High | Plaintext passwords | `STRUCT_ACCOUNTINFO.AccountPass` stored raw | Security | Account credentials stored and compared in plaintext on disk and over the network. |
| High | DBSrv single instance | All TMSrv instances depend on one DBSrv | Availability | Single point of failure for account/chars; MAX_SERVER (10) game servers all funnel through one DB server. |
| High | Raw packet casting | `char*` cast to packet structs without rigorous validation | Security / stability | Relies on `BASE_CheckPacket` (size checks, only active in `_PACKET_DEBUG`) to validate; malformed or oversized packets can cause overreads/writes. |
| Medium | Admin socket trust | `pAdminIP` allow-list + simple handshake | Security | Admin tool connections validated by IP allow-list and an Encode1/Encode2 challenge; protocol otherwise unencrypted. |
| Medium | Global mutable state | Massive number of `extern` globals in `Server.h` | Concurrency & maintainability | Global arrays (pUser, pMob, pItem, grids) are shared implicitly across all handlers. |
| Medium | Billing dependency | Optional external BillServer | Availability | If billing enabled and server unreachable, login/play flow can be disrupted (graceful fallback to BILLING=0 exists). |
| Low | DBSrv startup file checks | Hard requirement on `./BaseMob/*` and server-list match | Operational | Boot aborts with message box if files missing or local IP not in server list. |

---

## 7. Technology Stack Assessment

| Layer | Technology | Notes |
|-------|-----------|-------|
| Language | C/C++ (legacy, C-style with classes) | Predominantly C-style procedural code; minimal use of STL; manual memory management |
| Build | Visual Studio 2015 (v140, migrated to v143 toolset) | Solution/project files; MultiByte character set; Win32 x86 |
| UI / Runtime | Win32 GUI application | Each server is a windowed app using a message pump; not a console service |
| Networking | Winsock 2 (`ws2_32.lib`) + `WSAAsyncSelect` | Asynchronous socket events delivered as window messages; custom buffered socket class `CPSock` |
| Persistence | Custom binary file format | Per-account files; no SQL/relational DB |
| Linked libraries | `ws2_32`, `winmm`, `odbc32`/`odbccp32`, standard Win32 libs | Standard Windows system libs only; no third-party frameworks |
| Threading model | Single-threaded, cooperative | Timers (`SetTimer`) drive periodic updates; no threads/mutexes in gameplay path |
| Localization | `Language.h` message-string table | `g_pMessageStringTable` for client messages |
| Version control | SVN (AnkhSVN) historically, now Git | `.sln` retains Subversion Scc settings |

---

## 8. Security Architecture and Risks

The architecture has minimal, legacy-grade security controls. Notable concerns:

- **Plaintext credential storage.** Account passwords are stored as raw bytes in
  `STRUCT_ACCOUNTINFO.AccountPass` within binary account files and compared with
  `strcmp` against the login packet's `AccountPassword`. There is no hashing/salting.

- **Unencrypted network traffic.** Client↔TMSrv and TMSrv↔DBSrv packets carry
  credentials and game data in cleartext. Only the DBSrv↔NPTool admin handshake performs a
  lightweight encode challenge (`Encode1`/`Encode2` in `MSG_NPIDPASS`).

- **Packet validation is conditional.** `BASE_CheckPacket` (a large set of `Size !=
  sizeof(...)` checks) is only compiled under `_PACKET_DEBUG`, which is defined for Debug
  builds but not Release. In Release, incoming packet structs are trusted with minimal
  runtime validation. The dispatcher does check `ID` bounds and a `ClientTick` sentinel
  (`SKIPCHECKTICK`), but relies heavily on the client.

- **Failure/abuse counters.** TMSrv tracks failed logins per account (`CheckFailAccount`,
  `AddFailAccount`) and per-user error counts (`AddCrackError`), and TMSrv stores client
  MAC addresses (`AdapterName`) used by DBSrv for primary-account detection — a lightweight
  anti-abuse mechanism.

- **Admin access control.** DBSrv restricts admin connections to an IP allow-list
  (`pAdminIP`) and the game-server list; unknown senders are rejected at accept time.

- **Reserved account/character names.** DBSrv rejects accounts/chars matching reserved
  patterns (e.g., `COM`/`LPT` prefixes) and command-like names (`KING`, `KINGDOM`, etc.)
  to prevent collisions or command injection.

- **Version/anti-cheat.** TMSrv verifies the client `APP_VERSION` on login and uses
  `ClientTick` server-timestamp checks to reject stale packets. A `_PACKET_DEBUG` logging
  path exists but is not enabled in release builds.

Given the legacy nature and the study-only intent stated in the README, these are expected
characteristics; nevertheless, they represent substantial security exposure if deployed in
an untrusted environment (plaintext credentials, minimal release-build packet validation,
unencrypted transport, file-based persistence with no backup/transaction layer).

---

## 9. Infrastructure Analysis

Infrastructure information in this repository is limited. There is **no** Docker,
Kubernetes, CI/CD pipeline, or cloud deployment configuration in the analyzed scope. The
deployment model is entirely manual and Windows-native:

- Each server (TMSrv, DBSrv) is built as a standalone Win32 executable and runs as a
  windowed process on a Windows host.
- The solution outputs to `$(SolutionDir)..\release\$(ProjectName)\run\`, implying a
  conventional folder-based release layout where each executable sits in its own `run`
  directory alongside its data folders (`./account/`, `./BaseMob/`, import/export folders).
- Runtime configuration is via plaintext files read at boot: `redirect.txt`, server-list
  configuration, `ReadConfig()` settings, and the `BaseMob/*` binary class files.
- Data directories are implicitly relative to the working directory (e.g., `./account/...`,
  `../../Common/ImportDonate/...`), indicating a filesystem-centric operational model.
- Operational monitoring is file/log based (`Log`, `ChatLog`, `ItemLog`, day-log exports).

Because no containerization, orchestration, or CI infrastructure files are present, this
section is limited to the observations above; the architecture is a classic on-premises
Windows server deployment with manual operations.

---

## 10. Architectural Debt Summary

- **Monolithic core files:** `TMSrv/Server.cpp` (~9.4k lines) and `DBSrv/CFileDB.cpp`
  (~2.7k lines) concentrate disproportionate logic, complicating change and review.
- **Tightly coupled shared header:** The packet protocol and domain models are fused into
  one header shared across both binaries, creating a compile-time contract with no
  versioning or interface abstraction.
- **Single-threaded, cooperative model:** No threading or async/await; scales only to a
  fixed user ceiling and cannot exploit multi-core hosts.
- **File-based persistence with no transactions:** No ACID guarantees, backup strategy,
  or recovery mechanism for account data.
- **Duplicated domain model:** `STRUCT_MOB`/`STRUCT_ITEM`/`STRUCT_ACCOUNTFILE` and related
  structs are defined once in `Basedef.h` and replicated in per-account files and wire
  packets, requiring strict layout discipline (packed structs) with high risk of drift.
- **Minimal abstraction layers:** No interface for persistence, messaging, or event
  handling; components reach through global state and helper functions rather than through
  defined contracts.
- **Legacy build/runtime:** Win32 GUI message-pump servers, plaintext config/data files,
  and no automated testing (CodeGraph reports no covering tests for core handlers).

---

## Limitations

- Coupling metrics are **relative estimates** derived from call-site frequency and the
  CodeGraph dependency graph, not an exhaustive static-coupling enumeration.
- Infrastructure section is limited because no container/CI/deployment files exist in the
  analyzed scope.
- The analysis is read-only and did not modify any project files; no refactoring or
  implementation recommendations are provided per scope.
