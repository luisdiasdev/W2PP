# Component Deep Analysis Report — CWarTower

**Component name:** CWarTower
**Scope:** Guild war tower logic (channel/base war event)
**Project:** W2PP (legacy C/C++ decompilation server, Visual Studio 2015)
**Location:** `legacy/Code/TMSrv/CWarTower.cpp` / `legacy/Code/TMSrv/CWarTower.h`
**Analyzed on:** 2026-08-19 17:13:23
**Folders ignored:** `.git`, `.opencode`

---

## 1. Executive Summary

`CWarTower` is a small, self-contained C++ class (declared in
`legacy/Code/TMSrv/CWarTower.h`, implemented in `legacy/Code/TMSrv/CWarTower.cpp`)
that encapsulates the **guild war tower** ("Guerra da Torre" / channel war) event
for the TMSrv game server. It is classified as an **Event System** component in the
architecture report (`docs/reports/architectural-analyzer/architectural-report-2026-08-19 17:13:23.md`,
line 146), with Low afferent coupling and Medium efferent coupling.

The component is entirely **static**: the class exposes four static methods
(`GuildProcess`, `MobKilled`, `GGenerateMob`, `TowerAttack`) that operate on shared
global server state (`pMob[]`, `GuildInfo[]`, and three module-level globals
`GTorreState`, `GTorreGuild`, `GTorreHour`). It is not instantiated; the declared
default constructor has no definition and is never called.

The component orchestrates a **one-hour, timed, guild-controlled tower event** that
runs only on the Newbie Event Server during weekdays at a configurable hour. During
the event a special tower mob (`GTORRE`, ID 1078) spawns in a fixed map region. Guilds
may attack and "capture" the tower; the guild that holds ownership when the hour ends
is awarded guild fame. The component integrates with the DBSrv process over the shared
`Basedef.h` packet contract to persist the fame update.

**Key findings:**
- The whole event is driven by a simple **3-state time machine** gated by wall-clock
  minutes of a configurable hour, evaluated once per minute.
- Ownership is transferred on kill; the reward is granted at minute 59 of the event hour.
- There is a **comment/code discrepancy**: the Portuguese comment states the defending
  guild receives "50 de fama" (50 fame), but the code adds **100** (`+= 100`).
- Multiple latent issues: an unused local variable (`int Server`), an undefined
  constructor, hardcoded map coordinates, and a `short` fame field that is incremented
  by 100.
- **No test files exist anywhere in the repository** — the component has zero test
  coverage (see section 11).

---

## 2. Data Flow Analysis

The component has no network-facing entry of its own; it is invoked from four
integration points in the wider server. Data flows are described per entry point.

### 2.1 Timer-driven lifecycle (`GuildProcess`)

```
1. ProcessMinTimer() fires once per minute (ProcessSecMinTimer.cpp:2313)
2. Global GuildProcess() (Server.cpp:6203) resolves localtime
3. CWarTower::GuildProcess(timeinfo) called (Server.cpp:6651)
4. Gate check: NewbieEventServer==1 && weekday Mon-Fri && hour == GTorreHour
5. State machine on GTorreState (0 -> 1 -> 2 -> 0)
   - Phase start (min <= 5):  SendNotice(ChannelWar begin) ; GTorreState=1
   - Spawn     (min >= 6):    ClearArea() -> GenerateMob(GTORRE) -> SendNotice ; GTorreState=2
   - End       (min == 59):   ClearArea() -> scan towers -> +100 Fame -> MSG_GuildInfo -> DBSrv
                               -> DeleteMob(tower) ; GTorreState=0, GTorreGuild=0
6. Fame persistence: DBServerSocket.SendOneMessage(MSG_GuildInfo) -> DBSrv CFileDB
   (CFileDB.cpp:454) writes GuildInfo + WriteGuildInfo()
```

### 2.2 Kill / capture (`MobKilled`)

```
1. Global MobKilled(target, conn, PosX, PosY) resolves combat (MobKilled.cpp:41)
2. CWarTower::MobKilled(target, conn, PosX, PosY) called (MobKilled.cpp:2027)
3. If target is GTORRE and GTorreState != 0:
   - If killer has a guild: BASE_GetGuildName -> SendNotice(killer guild killed tower)
     -> GTorreGuild = killer's guild   (ownership transfer)
   - ClearArea() -> GenerateMob(GTORRE)  (tower respawns under new owner)
```

### 2.3 Spawn ownership stamping (`GGenerateMob`)

```
1. GenerateMob(index, posx, posy) spawns any mob (Server.cpp:2995)
2. CWarTower::GGenerateMob(index, PosX, PosY, tmob) called (Server.cpp:3147)
3. If index == GTORRE && GTorreGuild != 0:
   - pMob[tmob].MOB.Guild = GTorreGuild ; pMob[tmob].MOB.GuildLevel = 0
```

### 2.4 Attack gating (`TowerAttack`)

```
1. Exec_MSG_Attack() computes an attack against target idx (with connection conn)
   (_MSG_Attack.cpp)
2. CWarTower::TowerAttack(conn, idx) called (_MSG_Attack.cpp:398)
3. If target is not GTORRE -> TRUE (allow; not this event's concern)
   If attacker Guild == 0 or attacker Guild == tower Guild -> FALSE (block)
   Otherwise -> TRUE (allow)
4. Caller zeroes the damage entry when FALSE: m->Dam[i].TargetID=0; Damage=0; continue
```

---

## 3. Business Rules & Logic

## Overview of the business rules:

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Event runs only on Newbie Event Server (`NewbieEventServer == 1`) | CWarTower.cpp:44 |
| Validation | Event runs only Mon-Fri (excludes Sunday=0 and Saturday=6) | CWarTower.cpp:44 |
| Validation | Event runs only at the configured `GTorreHour` (default 22) | CWarTower.cpp:44 |
| State Machine | 3-phase lifecycle: idle(0) -> pre-battle(1) -> active(2) -> idle(0) | CWarTower.cpp:46,54,65 |
| Scheduling | Announcement at minutes 0-5, spawn at minute >= 6, end at minute 59 | CWarTower.cpp:46,54,65 |
| Business Logic | Owning guild receives +100 fame at end of event (comment says 50) | CWarTower.cpp:90 |
| Business Logic | Tower ownership transfers to killer's guild on kill | CWarTower.cpp:129 |
| Business Logic | Spawned tower mob is stamped with owning guild + GuildLevel 0 | CWarTower.cpp:142-143 |
| Validation | Attacker with no guild or the owning guild cannot damage the tower | CWarTower.cpp:152-153 |
| Persistence | Fame change persisted by sending `MSG_GuildInfo` to DBSrv | CWarTower.cpp:94 |

## Detailed breakdown of the business rules:
---

### Business Rule: Event Activation Gate

**Overview:**
The entire guild war tower event is gated by a compound predicate evaluated at the top
of `CWarTower::GuildProcess`. Unless every condition holds, the method is a no-op and
the state machine never advances.

**Detailed description:**
The gate requires three simultaneous conditions. First, the server must be operating in
Newbie Event mode (`NewbieEventServer == 1`). `NewbieEventServer` is a global integer
defined in `Server.cpp:310` (initialized to 0) and set to 1 during the per-minute
`ProcessMinTimer` routine in `ProcessSecMinTimer.cpp:2040-2041` for the designated
newbie server of a server group. This means the tower event does not run on a regular
game server; it is reserved for the special Newbie server instance. Second, the current
weekday must not be Sunday (`tm_wday != 0`) nor Saturday (`tm_wday != 6`), restricting
the event to weekdays (Monday through Friday). Third, the current hour must equal the
configured event hour `GTorreHour`, whose default is 22 (`Server.cpp:347`).

`GTorreHour` is externally configurable through two paths: it is read from a runtime
config file in `Server.cpp:1107` (the `value[2]` slot of a generated `gameconfig.txt`
line) and it can be changed live by an in-game administrator command `gtorrehour`
handled in `imple.cpp:1757-1764`. Because the gate is evaluated at the top of
`GuildProcess` each minute, the event only becomes observable at the exact configured
hour, and only on a weekday on the newbie server. There is no persisted state across
server restarts; `GTorreState` and `GTorreGuild` are in-memory globals that reset to 0.

**Rule workflow:**
1. `ProcessMinTimer` runs and the newbie server flag may be (re)set.
2. `GuildProcess` reads `timeinfo` and checks the three-part gate.
3. If any condition fails, return without side effects.
4. If all hold, proceed into the phase state machine for the current minute.

---

### Business Rule: Three-Phase Event State Machine

**Overview:**
Once the activation gate passes, the event is driven by a minute-by-minute state machine
over the global `GTorreState`, which takes values 0 (idle), 1 (pre-battle/announcement),
and 2 (active battle). Transitions are triggered purely by `timeinfo->tm_min` thresholds
within the event hour.

**Detailed description:**
In **state 0** (idle) and when the current minute is less than or equal to 5, the method
formats a channel-war-begin announcement using message string `_DN_CHANNELWAR_BEGIN`
(index 501 in `Language.h:520`) with a 5-minute countdown, broadcasts it via `SendNotice`,
sets `GTorreState = 1`, and clears any previous tower owner by setting `GTorreGuild = 0`.
This is the announcement phase: players are warned the war begins in five minutes.

In **state 1** (pre-battle) and when the minute reaches 6 or greater, the method clears
the event area by invoking `ClearArea(2445, 1850, 2546, 1920)`, spawns the tower mob via
`GenerateMob(GTORRE, 0, 0)`, broadcasts the "base war start" message using
`_DN_BASEWORSTART` (index 425 in `Language.h:444`), and transitions `GTorreState = 2`.
The battle is now active and the tower is present in the world.

In **state 2** (active) and when the minute equals 59, the method executes the end-of-war
sequence: it clears the area again, iterates all non-player mob slots
(`i` from `MAX_USER` to `MAX_MOB`, i.e. 1000 to 25000) looking for any live tower mob
(`pMob[i].GenerateIndex == GTORRE`), awards fame to the tower's owning guild, deletes the
tower, and resets `GTorreState = 0` and `GTorreGuild = 0`. The event then returns to idle
until the next occurrence.

**Rule workflow:**
1. State 0, minute <= 5: announce countdown, arm state 1, clear owner.
2. State 1, minute >= 6: clear area, spawn tower, announce start, arm state 2.
3. State 2, minute == 59: clear area, award fame per surviving tower, delete tower,
   disarm to state 0 and clear owner.

---

### Business Rule: Tower Ownership Capture on Kill

**Overview:**
When the tower is destroyed during the active battle, the guild of the player (or mob)
that delivered the killing blow becomes the new owner of the tower. Ownership is
recorded in the global `GTorreGuild` and is applied to the respawned tower.

**Detailed description:**
The kill hook `CWarTower::MobKilled(target, conn, PosX, PosY)` is invoked from the global
combat-resolution function `MobKilled` in `MobKilled.cpp:41` (called at line 2027) after
a mob death has been processed. It first reads the killed target's `GenerateIndex`. If
that index equals the tower ID `GTORRE` and the event is currently active
(`GTorreState` non-zero), it proceeds. If the killer (`conn`) belongs to a guild
(`pMob[conn].MOB.Guild` non-zero), it resolves the guild's name via `BASE_GetGuildName`,
broadcasts an announcement using `_SS_BASEWORKILLTOWER` (index 536 in `Language.h:549`)
that names the killer and their guild, and finally sets `GTorreGuild = pMob[conn].MOB.Guild`.
If the killer has no guild, no ownership is recorded (the tower is not captured).

Regardless of whether ownership changed, the method clears the event area and respawns
the tower with `GenerateMob(GTORRE, 0, 0)`. The respawn goes through the spawn hook
`GGenerateMob`, which stamps the new tower mob with the current `GTorreGuild` (see the
ownership-stamping rule). This creates the core capture mechanic: destroying an
opponent's tower transfers it to your guild, and the tower continues to fight on behalf
of the new owner until it is captured again or the hour ends.

**Rule workflow:**
1. A `GTORRE` mob dies while `GTorreState != 0`.
2. If the killer has a guild: resolve guild name, broadcast kill announcement.
3. Set `GTorreGuild` to the killer's guild (ownership transfer).
4. Clear the area and respawn the tower under the new owner.

---

### Business Rule: Spawn-Time Ownership Stamping

**Overview:**
Every time a tower mob is spawned, its guild affiliation and guild level are set from the
current global ownership state, ensuring the freshly-spawned tower belongs to the
currently owning guild and is not a guild leader.

**Detailed description:**
`CWarTower::GGenerateMob(index, PosX, PosY, tmob)` is a callback invoked from within the
generic `GenerateMob` routine in `Server.cpp` (call site at line 3147) for every mob
spawned in the world. When the spawned mob's `index` equals `GTORRE` and there is an
active owner (`GTorreGuild` non-zero), the method writes the owner onto the new mob slot:
`pMob[tmob].MOB.Guild = GTorreGuild` and `pMob[tmob].MOB.GuildLevel = 0`.

`MOB.Guild` is the `unsigned short` guild identifier (Basedef.h:443) and `MOB.GuildLevel`
is the `unsigned char` guild level (Basedef.h:474) used elsewhere to distinguish guild
leaders (9) from members (0). Setting the level to 0 means the tower is treated as a
regular guild member entity, not as a guild master. This stamped affiliation is what the
attack-gating rule (`TowerAttack`) later reads to decide who may damage the tower, and it
is also what the end-of-war fame-award loop scans for.

**Rule workflow:**
1. `GenerateMob` creates a new mob slot `tmob`.
2. `GGenerateMob` is called with the spawn `index`.
3. If `index == GTORRE` and `GTorreGuild != 0`: set tower guild to `GTorreGuild`,
   set guild level to 0.
4. The spawned tower is now affiliated with the owning guild.

---

### Business Rule: End-of-War Fame Award

**Overview:**
At the end of the event hour (minute 59), the guild(s) that currently own the tower(s)
are credited with guild fame, and the updated fame is persisted to the database server.

**Detailed description:**
When `GTorreState == 2` and the current minute equals 59, `GuildProcess` iterates over
every non-player mob slot from `MAX_USER` (1000) to `MAX_MOB` (25000) and selects any slot
whose `GenerateIndex == GTORRE`. For each such tower with a non-zero `MOB.Guild`, it
resolves the guild name, constructs an `MSG_GuildInfo` packet (`sm`), and increments the
in-memory guild fame: `GuildInfo[usGuild].Fame += 100`. It then sends the updated
`MSG_GuildInfo` to the database server via `DBServerSocket.SendOneMessage`, and logs the
fame award to the system log with the format `etc,war_tower1 guild:%d guild_fame:%d`.

Notably, the surrounding comment in Portuguese states that the defending guild "recebe 50
de fama" (receives 50 fame), but the actual code adds **100**. This is an inconsistency
between documentation and implementation that should be flagged (see section 10). On the
database side, the `_MSG_GuildInfo` handler in `CFileDB.cpp:454-481` validates that the
guild index is in `(0, 65536)`, stores the updated `STRUCT_GUILDINFO` in memory, re-sends
it to all connected game servers, and calls `CReadFiles::WriteGuildInfo()` to persist it.
The `STRUCT_GUILDINFO.Fame` field is a `short` (Basedef.h:798), so incrementing by 100 has
overflow implications at high fame values.

**Rule workflow:**
1. State 2 and minute == 59 triggers the end-of-war sequence.
2. Iterate all non-player mob slots for live towers (`GenerateIndex == GTORRE`).
3. For each tower with a guild: resolve name, add +100 to `GuildInfo[guild].Fame`.
4. Build and send `MSG_GuildInfo` to DBSrv for persistence.
5. Delete the tower and reset event state to idle.

---

### Business Rule: Tower Attack Gating

**Overview:**
During combat resolution, only attackers from a guild different from the tower's owning
guild are permitted to damage the tower. Unguilded attackers and members of the owning
guild are blocked.

**Detailed description:**
`CWarTower::TowerAttack(conn, idx)` returns a `BOOL` that the caller (`Exec_MSG_Attack` in
`_MSG_Attack.cpp:398`) uses to decide whether a particular damage entry is kept. The rule
has three cases. First, if the attack target (`idx`) is not the tower (`pMob[idx]
.GenerateIndex != GTORRE`), the method returns `TRUE`, meaning the attack is allowed and
this event has no opinion on it. Second, if the attacker has no guild
(`pMob[conn].MOB.Guild == 0`) or the attacker's guild equals the tower's guild
(`pMob[conn].MOB.Guild == pMob[idx].MOB.Guild`), it returns `FALSE`, blocking the attack.
Third, otherwise (attacker belongs to a different guild than the tower owner) it returns
`TRUE`, allowing the attack.

The caller interprets `FALSE` by zeroing the damage entry (`m->Dam[i].TargetID = 0;
m->Dam[i].Damage = 0; continue;` in `_MSG_Attack.cpp:400-402`), so a blocked attack deals
no damage and is dropped from the damage list. This rule enforces that the tower can only
be contested by rival guilds — a guild cannot damage its own tower, and players not
belonging to any guild cannot damage it either. The tower's guild affiliation is the one
stamped by the ownership-stamping rule at spawn time.

**Rule workflow:**
1. `Exec_MSG_Attack` computes a potential hit on target `idx` by attacker `conn`.
2. `TowerAttack(conn, idx)` is consulted.
3. Non-tower targets: allow (TRUE).
4. Unguilded attacker or attacker from the owning guild: block (FALSE).
5. Attacker from a different guild: allow (TRUE).
6. Caller zeroes the damage entry when blocked.

---

## 4. Component Structure

The component is minimal: one class declaration and one implementation file, plus the
global state it shares with the server.

```
legacy/Code/TMSrv/
├── CWarTower.h              # Class declaration (37 lines)
│   ├── class CWarTower
│   │   ├── CWarTower();                       # Declared, never defined/used
│   │   ├── static void GuildProcess(tm*)      # Timer-driven lifecycle
│   │   ├── static void MobKilled(...)         # Kill/capture hook
│   │   ├── static void GGenerateMob(...)      # Spawn ownership stamp
│   │   └── static BOOL TowerAttack(...)       # Attack gating
├── CWarTower.cpp            # Implementation (156 lines)
│   ├── GuildProcess()      # CWarTower.cpp:42   — event state machine + fame award
│   ├── MobKilled()         # CWarTower.cpp:110  — tower capture on kill
│   ├── GGenerateMob()      # CWarTower.cpp:138  — ownership stamping on spawn
│   └── TowerAttack()       # CWarTower.cpp:147  — attack permission check
```

**Shared global state** (defined in `Server.cpp`, externed in `Server.h:407-409`):

| Global | Default | Location | Purpose |
|--------|---------|----------|---------|
| `GTorreState` | 0 | Server.cpp:346 | Event phase (0 idle, 1 pre-battle, 2 active) |
| `GTorreHour` | 22 | Server.cpp:347 | Configurable event hour |
| `GTorreGuild` | 0 | Server.cpp:348 | Current tower owning guild |

**Key constant:** `GTORRE = 1078` (`Basedef.h:386`) — the war tower mob's generation ID.

**Build integration:** the pair is registered in the Visual Studio project
`TMSrv.vcxproj` (ClCompile at line 98, ClInclude at line 176) and grouped under the
`CWarTower` filter in `TMSrv.vcxproj.filters` (lines 64, 291-292, 344-345).

---

## 5. Dependency Analysis

The component sits inside the TMSrv executable and depends on shared server globals,
helper functions, and the DBSrv packet contract. There are no third-party runtime
libraries beyond the Windows SDK (the `.cpp` includes `<Windows.h>`).

### Internal Dependencies

```
CWarTower::GuildProcess
   ├─► SendNotice(msg)                        SendFunc.cpp:47   — broadcast to all users
   ├─► ClearArea(x1,y1,x2,y2)                 Server.cpp:5754   — recall players out of zone
   ├─► GenerateMob(GTORRE,...)                Server.cpp:2995   — spawn tower mob
   ├─► BASE_GetGuildName(...)                 Basedef.cpp:1314  — resolve guild name
   ├─► GuildInfo[].Fame += 100                Server globals      — in-memory guild fame
   ├─► DBServerSocket.SendOneMessage(...)     CPSock.cpp:686     — send MSG_GuildInfo to DBSrv
   ├─► Log(temp, "-system", 0)                CPSock.cpp/Log     — system log
   └─► DeleteMob(i, 1)                        Server.cpp:7020   — remove tower mob

CWarTower::MobKilled
   ├─► BASE_GetGuildName(...)                 Basedef.cpp:1314
   ├─► SendNotice(msg)                        SendFunc.cpp:47
   ├─► ClearArea(...)                         Server.cpp:5754
   └─► GenerateMob(GTORRE,...)                Server.cpp:2995

CWarTower::GGenerateMob
   └─► pMob[tmob].MOB.Guild / .GuildLevel     (direct global writes)

CWarTower::TowerAttack
   └─► pMob[conn].MOB.Guild / pMob[idx]...    (direct global reads)

DBSrv side (persistence contract):
   └─► CFileDB.cpp:454 (case _MSG_GuildInfo)   — stores + persists GuildInfo
```

### External Dependencies

| Dependency | Type | Purpose | Notes |
|------------|------|---------|-------|
| Windows SDK / Win32 | Platform | Sockets, file I/O, `time`/`localtime` | `<Windows.h>`, `<time.h>` |
| DBSrv process | Internal service | Persists guild fame / guild info | Over TCP via shared `Basedef.h` packet layout |
| External runtime data files | Data | Tower mob (1078) stats, message strings, `gameconfig.txt` | Loaded at startup; **not present in the repository** |

The message strings (`_DN_CHANNELWAR_BEGIN`=501, `_DN_BASEWORSTART`=425,
`_SS_BASEWORKILLTOWER`=536) are indices into `g_pMessageStringTable`, which is loaded at
runtime from an external language file by `BASE_InitializeMessage` (`Basedef.cpp:4637`).
The actual localized text is therefore external to the codebase.

---

## 6. Afferent and Efferent Coupling

The component is a static-method C++ class; the coupling units are the individual public
methods plus the shared global state they read/write.

| Component / Method | Afferent Coupling | Efferent Coupling | Critical |
|--------------------|-------------------|-------------------|----------|
| CWarTower::GuildProcess | 1 (global GuildProcess, Server.cpp:6651) | 8 (SendNotice, ClearArea, GenerateMob, BASE_GetGuildName, GuildInfo[], DBServerSocket, Log, DeleteMob) | Medium |
| CWarTower::MobKilled | 1 (global MobKilled, MobKilled.cpp:2027) | 4 (BASE_GetGuildName, SendNotice, ClearArea, GenerateMob) | Medium |
| CWarTower::GGenerateMob | 1 (GenerateMob, Server.cpp:3147) | 2 (pMob writes) | Low |
| CWarTower::TowerAttack | 1 (_MSG_Attack.cpp:398) | 2 (pMob reads) | Low |
| GTorreState / GTorreGuild / GTorreHour (globals) | 14 read/write sites across 8 files | n/a | High |

Afferent coupling is uniformly low (each method has a single caller), making the class
easy to locate and reason about. Efferent coupling is moderate for the two larger methods
because they reach into the shared helper layer and the DBSrv socket. The global
`GTorreState`/`GTorreGuild`/`GTorreHour` triple is the highest-risk shared surface: it is
referenced across `MobKilled.cpp` (lines 65, 2156), `_MSG_Attack.cpp` (405), `_MSG_PKMode`
(51), `_MSG_MessageWhisper` (549), `_MSG_QuitTrade` (45), `SendFunc.cpp` (1758),
`Server.cpp` (3129), and `CWarTower.cpp`, creating broad, implicit coupling.

---

## 7. Endpoints

The `CWarTower` component does **not** expose any network endpoints (no REST, GraphQL, or
gRPC). It is an in-process, static utility class invoked from the TMSrv event loop and
combat handlers. Its only outbound communication is the `MSG_GuildInfo` packet sent to the
DBSrv process over the game-server socket, which is not a client-facing endpoint.
This section is therefore intentionally omitted per the analysis guidelines.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| DBSrv (`DBServerSocket`) | Internal service | Persist guild fame on tower capture end | TCP, `CPSock::SendOneMessage` | `MSG_GuildInfo` packet (`Basedef.h:1326`) | Send-and-forget; no retry/ack in component |
| `g_pMessageStringTable` | External data | Localized event announcement text | File loaded at startup (`BASE_InitializeMessage`) | `char[128]` rows indexed by message ID | Indices assumed valid; no in-component validation |
| Tower mob definition (ID 1078) | External data | Tower stats/behavior | File loaded by `CReadFiles` | `STRUCT_MOB` via generator list | None in component |
| `gameconfig.txt` | External config | `GTorreHour` value | File read in `Server.cpp:1107` | Whitespace-delimited integers | Bounds checked via `value[] == -1` at Server.cpp:1096 |

The fame-award flow relies on the DBSrv `_MSG_GuildInfo` handler (`CFileDB.cpp:454`),
which guards the guild index (`myguild <= 0 || myguild >= 65536` → break) and persists via
`CReadFiles::WriteGuildInfo()`.

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Singleton-like static utility | All members static; no instance state | CWarTower.h:32-36 | Expose event logic without object lifecycle; operates on globals |
| Finite State Machine | 3-phase `GTorreState` transitions by wall-clock minute | CWarTower.cpp:46-106 | Drive the timed battle lifecycle |
| Event/Callback hooks | `GGenerateMob` (spawn), `MobKilled` (kill) called from generic server routines | Server.cpp:3147; MobKilled.cpp:2027 | Inject event logic at spawn/death points |
| Shared-header packet contract | `MSG_GuildInfo`/`STRUCT_GUILDINFO` defined in `Basedef.h` shared with DBSrv | Basedef.h:798,1326 | Couple TMSrv and DBSrv without an interface/IDL |
| Global mutable state module | `GTorreState`/`GTorreGuild`/`GTorreHour` | Server.cpp:346-348 | Cross-module event coordination |

Architecturally, the component follows the codebase's overall single-threaded event-loop
design (per the architecture report, pattern 2): all `GuildProcess`/`MobKilled`/
`GGenerateMob`/`TowerAttack` calls are serialized on the Win32 message pump, so the global
state needs no locking.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| Medium | GuildProcess (CWarTower.cpp:90) | Comment says "50 de fama" but code adds 100 | Misleading documentation; actual reward may differ from intended design |
| Medium | GuildProcess / MobKilled (CWarTower.cpp:76,119) | Local `int Server` assigned but never used | Dead code / compiler warning; readability cost |
| Medium | STRUCT_GUILDINFO.Fame is `short` (Basedef.h:798) | `Fame += 100` on a 16-bit signed field | Potential signed overflow / wrap at high fame values |
| Medium | CWarTower.h:28 | `CWarTower()` constructor declared but never defined or used | Latent dead declaration; class relies solely on statics |
| Medium | CWarTower.cpp:56,132,67 | Hardcoded map coordinates `(2445,1850,2546,1920)` and tower bounds duplicated in `_MSG_PKMode`/`_MSG_MessageWhisper`/`_MSG_QuitTrade` (2430-2560, 1825-1925) | Inconsistent/duplicated magic numbers; drift risk between event area and PK-zone |
| Medium | Global state (GTorreState/Guild/Hour) | Broad cross-file read/write coupling (8 files) | Change ripple; implicit contract between event and unrelated handlers |
| Low | CWarTower.cpp:65-106 | End-of-war loop scans up to 24,000 mob slots each minute-59 | Minor per-minute CPU cost; negligible |
| High | Entire component | **No automated tests anywhere in the repository** | Regressions in event timing/ownership unguarded |
| Low | CWarTower.cpp:44 | Only runs on Newbie Event Server — behavior differs per server config | Event may silently not occur on non-newbie servers |

---

## 11. Test Coverage Analysis

A repository-wide search for test files was performed (including filenames matching
`*test*`, `*Test*`, `TEST*`, `*unittest*`, and `*spec*`) across all source, project, and
report directories, excluding the ignored folders (`.git`, `.opencode`) and the codegraph
index. **No test files exist** in the W2PP repository, and there is no test project in
either Visual Studio solution (`legacy/W2PP Code Project.sln`). Consequently there are no
unit, integration, or coverage artifacts for `CWarTower` or any other component.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| CWarTower | 0 | 0 | 0% (no test harness present) | N/A — no tests |

**Test file locations:** none found. CodeGraph analysis also reported "no covering tests
found" for `GuildProcess`, `MobKilled`, `TowerAttack`, and `GGenerateMob`. The absence of
a test harness is a significant risk given the component's time-sensitive state machine
and cross-file coupling to globals.

---

## 12. Summary

`CWarTower` is a compact, static C++ event component that manages a weekly, hour-long
guild tower battle on the TMSrv Newbie Event Server. Its logic is a straightforward
3-state time machine (idle → pre-battle → active → idle) gated by `NewbieEventServer`,
weekday, and the configurable `GTorreHour`. Ownership is transferred to the guild that
destroys the tower, stamped onto respawned towers, gated at attack time, and finally
converted into a +100 guild fame award (despite a comment claiming 50) that is persisted
through the DBSrv process.

The component is low-coupling (one caller per method) but carries medium efferent coupling
through the shared helper/socket layer and, more importantly, a broad implicit coupling
through the `GTorreState`/`GTorreGuild`/`GTorreHour` globals referenced by eight files.
Key risks are the total absence of automated tests, a comment/implementation mismatch on
the fame amount, an overflowing-capable `short` fame field, an undefined constructor, and
duplicated hardcoded map coordinates. The component integrates with DBSrv via the shared
`Basedef.h` packet contract and depends on external runtime data files (tower mob
definition, localized strings, and `gameconfig.txt`) that are not present in the
repository.

---

*Analysis performed without modifying any project files. All file paths are relative to
the repository root; line numbers reference the current on-disk source.*
