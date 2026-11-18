# Component Deep Analysis Report

## Component: CWarTower Event Module

| Attribute | Value |
|-----------|-------|
| Component name | CWarTower Event Module |
| Programming component type | C++ class with static-only methods (OOP paradigm, used as a namespace) |
| Primary files | `Code/TMSrv/CWarTower.h` (37 lines), `Code/TMSrv/CWarTower.cpp` (156 lines) |
| Owning binary / project | TMSrv (game / world server), `Code/TMSrv/TMSrv.vcxproj:99`, `:177` |
| Project scope analyzed | Entire repository root `E:\wyd\test-setup\w2pp` |
| Excluded from analysis | `.agents`, `.codegraph`, `.opencode`, `openspec` |
| Architectural role | Domain service (scheduled event): guild-war tower state, tower attack arbitration, tower ownership, weekly guild fame award |
| Architectural report reference | `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:156` (Ca ~7, Ce 11) |
| Analysis date | 2026-11-15 22:45:04 |
| Files modified | None (analysis only) |

---

## 1. Executive Summary

`CWarTower` is a 156-line static-method C++ class that implements the server side of the "Base War" / guild tower war event of TMSrv (internally `GTorre` = "Guerra da Torre"). It owns exactly four operations: a scheduled state machine driven from the minute timer (`GuildProcess`), a kill hook (`MobKilled`), a mob-generation hook (`GGenerateMob`), and an attack-permission predicate (`TowerAttack`). It holds no instance data; the entire event state lives in three process-global integers declared in `Code/TMSrv/Server.cpp:346-348` (`GTorreState`, `GTorreHour`, `GTorreGuild`) plus one global flag (`NewbieEventServer`, `Code/TMSrv/Server.cpp:310`).

The component is a *state coordinator*, not a self-contained service. Every meaningful effect is delegated: the tower NPC itself is spawned through the generic `GenerateMob` path (`Code/TMSrv/Server.cpp:2995`), area eviction goes through `ClearArea` → `DoRecall` (`Code/TMSrv/Server.cpp:5754`, `:7558`), announcement text comes from an external `Language.txt` string table loaded by `BASE_InitializeMessage` (`Code/Basedef.cpp:4637`, `Code/TMSrv/Server.cpp:7160`), guild fame is mutating a process-global `STRUCT_GUILDINFO GuildInfo[65536]` array and pushed to DBSrv as a `_MSG_GuildInfo` packet (`Code/TMSrv/CWarTower.cpp:83-94`). Consequently the module's real code footprint is four functions but its behavioural footprint spans fifteen call sites in five translation units and one inter-process protocol.

Key findings:

1. **The event is fully data-driven but the data is absent from the repository.** Tower HP, level, defence, sprite and the generator entry are defined in the NPC generator table (`mNPCGen.pList[1078]`), the messages in `Language.txt`; neither file is tracked in this repository (`Code/release/TMSrv/run` contains only `TMSrv.exe`, `TMSrv.pdb`). Only the generator index constant `GTORRE 1078` (`Code/Basedef.h:386`) is in source. Tower hit points therefore cannot be established from the codebase and are recorded as unknown-by-design.
2. **The war schedule is a four-state implicit machine (0/1/2 + reset) with hard-coded wall-clock comparisons and no persistence.** It runs Monday–Friday (`tm_wday != 0 && != 6`), at hour `GTorreHour` (default 22, `Code/TMSrv/Server.cpp:347`), announcing at minutes 0–5, spawning at minute ≥ 6, and ending at exactly minute 59 (`Code/TMSrv/CWarTower.cpp:44-107`). Missing the single `tm_min == 59` tick leaves state 2 latched, which carries over to the next day.
3. **Ownership is a single global, not a per-tower structure.** `GTorreGuild` is set by the killing blow (`Code/TMSrv/CWarTower.cpp:129`), stamped onto the newly generated tower mob by `GGenerateMob` (`Code/TMSrv/CWarTower.cpp:138-145`), and cleared when a new war starts or ends (`:51`, `:105`). Ownership is never persisted; a server restart during the event loses the owner silently.
4. **Attack arbitration is a three-case predicate with no state check.** `TowerAttack` (`Code/TMSrv/CWarTower.cpp:147-156`) blocks guildless attackers and the owning guild, permits everyone else, and never consults `GTorreState`; enforcement happens not by refusing the action but by zeroing the damage entries in `Code/TMSrv/_MSG_Attack.cpp:398-403`.
5. **The reward path has a code/comment mismatch and a duplicate-award hazard.** The in-code comment says the defending guild receives 50 fame (`Code/TMSrv/CWarTower.cpp:64`) while the implementation adds 100 (`:90`). The end-of-war sweep iterates *all* mob slots whose `GenerateIndex == GTORRE` without checking `Mode`, while `DeleteMob` deliberately does not reset `GenerateIndex` (`Code/TMSrv/Server.cpp:7020-7054`); each tower spawn that occupies a distinct slot during the war is therefore liable to pay out again.
6. **The kill hook is reachable only through a narrow, undocumented player-clan filter.** `CWarTower::MobKilled` is invoked at `Code/TMSrv/MobKilled.cpp:2027`, inside the `else` of `Code/TMSrv/MobKilled.cpp:636`, so only killers whose `Clan` is 0, 4, 7 or 8 reach the tower-claim logic; killers with clan 1–3/5–6 skip it entirely (`:637`).
7. **The module silently grants PvP immunities.** While `GTorreState != 0` there is no death-experience loss (`Code/TMSrv/MobKilled.cpp:2156`), the "Nyerds" fairy shield is not depleted (`Code/TMSrv/MobKilled.cpp:65`), and clients inside a wider arena rectangle receive the aggressive-PK flag (`Code/TMSrv/SendFunc.cpp:1758`, `Code/TMSrv/_MSG_PKMode.cpp:51`, `Code/TMSrv/_MSG_QuitTrade.cpp:45`, `Code/TMSrv/_MSG_MessageWhisper.cpp:549`).
8. **Zero automated tests exist anywhere in the repository** and no test project is declared in `W2PP Code Project.sln`. All verification of this component is manual and in-game.
9. **The component is highly coupled to mutable global state** (`pMob[]`, `GuildInfo[]`, `g_pMessageStringTable[]`, `mNPCGen`), which makes it impossible to exercise in isolation and is the root cause of most of the risks listed in section 9.

Confidence: the state machine, ownership and attack rules are read directly from source and are high-confidence. Reward duplication and the clan-gated kill hook are derived from control-flow and helper-function semantics and are high-confidence but were not executed. Tower statistics, message text, and generator configuration are medium-confidence statements about external data that do not exist in the repository.

---

## 2. Data Flow Analysis

### Flow A — Scheduled war lifecycle (timer driven)

```
1. Win32 timer WM_TIMER / TIMER_MIN fires every 12000 ms
   (Code/TMSrv/Server.cpp:3632 SetTimer(hWndMain, TIMER_MIN, 12000, NULL);
    dispatch at Code/TMSrv/Server.cpp:3727-3728)
2. ProcessMinTimer() executes the per-tick bodies
   (Code/TMSrv/ProcessSecMinTimer.cpp:2019)
3. ... unrelated per-minute subsystems (castle, BR, RvR, newbie-server election) ...
4. GuildProcess() [global, not the class one]
   (Code/TMSrv/ProcessSecMinTimer.cpp:2313 -> Code/TMSrv/Server.cpp:6203)
5. CWarTower::GuildProcess(timeinfo)
   (Code/TMSrv/Server.cpp:6651 -> Code/TMSrv/CWarTower.cpp:42)
6. Guard chain: NewbieEventServer == 1, tm_wday in [1..5], tm_hour == GTorreHour
   (Code/TMSrv/CWarTower.cpp:44)
7. State 0 -> 1 at tm_min <= 5: format _DN_CHANNELWAR_BEGIN(5), SendNotice, GTorreGuild = 0
   (:46-52)
8. State 1 -> 2 at tm_min >= 6: ClearArea(2445,1850,2546,1920), GenerateMob(GTORRE,0,0),
   SendNotice(_DN_BASEWORSTART)  (:54-62)
9. State 2 at tm_min == 59: ClearArea, scan mob slots for GenerateIndex == GTORRE,
   award GuildInfo[guild].Fame += 100, emit _MSG_GuildInfo to DBSrv, DeleteMob(i,1),
   reset state 0 / owner 0  (:65-106)
10. GenerateMob(index=1078) copies the NPC template into a free mob slot and calls
    CWarTower::GGenerateMob -> tower mob is stamped with GTorreGuild + GuildLevel 0
    (Code/TMSrv/Server.cpp:3069, :3147 -> Code/TMSrv/CWarTower.cpp:138)
11. DBSrv persists fame: case _MSG_GuildInfo stores GuildInfo[guild] and writes the guild
    file (Code/DBSrv/CFileDB.cpp:454-481)
```

### Flow B — Tower attack arbitration (combat driven)

```
1. Client sends MSG_Attack; ProcessClientMessage dispatches to Exec_MSG_Attack
   (Code/TMSrv/ProcessClientMessage.cpp)
2. Per-target loop validates size, dead target, merchant flags, view-grid distance,
   anti-cheat damage markers (Code/TMSrv/_MSG_Attack.cpp:331-361)
3. Guild / clan values are normalised, GuildDisable respected (:373-395)
4. CWarTower::TowerAttack(conn, idx) is evaluated (:398)
   - idx is not a tower          -> TRUE  (normal combat continues)
   - attacker guild == 0         -> FALSE
   - attacker guild == tower guild -> FALSE
   - otherwise                   -> TRUE
5. FALSE: m->Dam[i].TargetID = 0; m->Dam[i].Damage = 0; continue  (:400-402)
   TRUE : normal physical/skill damage resolution continues (:421+)
6. Damage application path, death detection
7. On death, MobKilled(target, conn, PosX, PosY) is entered
   (Code/TMSrv/MobKilled.cpp:41)
```

### Flow C — Kill, ownership transfer and respawn

```
1. MobKilled validates indices, handles level>=1000 bosses, Nyerds shield, castle door
   (Code/TMSrv/MobKilled.cpp:43-109)
2. Branch PvE (target >= MAX_USER or level > MAX_LEVEL+5) (:207-211)
3. Experience, party split, ranking, die-line, drop rolls, event drops (:213-1945)
4. DeleteMob(target, 1)   (:1990)   [NOTE: GenerateIndex is NOT cleared by DeleteMob]
5. Pista/Kefra respawn hooks (:1992-2025)
6. CWarTower::MobKilled(target, conn, PosX, PosY)   (:2027 -> Code/TMSrv/CWarTower.cpp:110)
   6a. GenerateID = pMob[target].GenerateIndex (survives DeleteMob)
   6b. if GenerateID == GTORRE && GTorreState != 0:
       - if killer MOB.Guild != 0: BASE_GetGuildName, format _SS_BASEWORKILLTOWER,
         SendNotice, GTorreGuild = killer guild
       - ClearArea(2445,1850,2546,1920) -> every player recalled to city/save grid
       - GenerateMob(GTORRE, 0, 0) -> spawn replacement tower
7. Inside GenerateMob: CWarTower::GGenerateMob stamps the new tower with GTorreGuild and
   GuildLevel 0 (Code/TMSrv/Server.cpp:3147), then pMob[tmob].GenerateIndex = 1078 (:3149),
   HP set to MaxHp (:3163)
8. Client observers get _MSG_CreateMob for the new tower (Code/TMSrv/Server.cpp:3219+)
9. DBSrv never learns about tower ownership (no packet emitted in this flow)
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Availability gate | Tower war only runs when `NewbieEventServer == 1` (the day's elected "newbie" channel or `LOCALSERVER`) | `Code/TMSrv/CWarTower.cpp:44` |
| BR-02 | Schedule (weekday) | War only on Monday–Friday; Sunday (0) and Saturday (6) are excluded | `Code/TMSrv/CWarTower.cpp:44` |
| BR-03 | Schedule (hour) | War hour equals the configurable `GTorreHour`, default 22 | `Code/TMSrv/CWarTower.cpp:44`, `Code/TMSrv/Server.cpp:347` |
| BR-04 | State machine | Countdown phase: state 0 → 1 announced only while `tm_min <= 5`, with a hard-coded "5 minute" parameter | `Code/TMSrv/CWarTower.cpp:46-52` |
| BR-05 | State machine | Spawn phase: state 1 → 2 at `tm_min >= 6`, mandatory arena clear, tower generated, start notice | `Code/TMSrv/CWarTower.cpp:54-62` |
| BR-06 | State machine | End phase: state 2 → 0 at exactly `tm_min == 59`, mandatory arena clear, per-slot fame award, all tower slots deleted | `Code/TMSrv/CWarTower.cpp:65-106` |
| BR-07 | State reset | Starting a war clears the previous owner (`GTorreGuild = 0`) | `Code/TMSrv/CWarTower.cpp:51` |
| BR-08 | Spawn constraint | No NPC may be generated inside the arena rectangle unless its template uses `Equip[0].sIndex == 219` | `Code/TMSrv/Server.cpp:3060-3064` |
| BR-09 | Spawn constraint | Tower generation is subject to the generator's `CurrentNumMob < MaxNumMob` population cap | `Code/TMSrv/Server.cpp:3044-3048` |
| BR-10 | Ownership stamping | Every tower generated while an owner exists inherits `GTorreGuild` and `GuildLevel = 0` | `Code/TMSrv/CWarTower.cpp:138-145` |
| BR-11 | Attack permission | A guildless player may never damage the tower | `Code/TMSrv/CWarTower.cpp:152-153` |
| BR-12 | Attack permission | The owning guild may never damage its own tower | `Code/TMSrv/CWarTower.cpp:152-153` |
| BR-13 | Attack permission | Every other guild may damage the tower; `GTorreState` is irrelevant to the predicate | `Code/TMSrv/CWarTower.cpp:147-155` |
| BR-14 | Attack enforcement | A rejected tower attack zeroes the target id and damage of that entry and skips it, leaving other targets in a multi-target attack untouched | `Code/TMSrv/_MSG_Attack.cpp:398-403` |
| BR-15 | Kill claim | Ownership transfers on the tower's death only if `GTorreState != 0` and the killer belongs to a guild | `Code/TMSrv/CWarTower.cpp:114-130` |
| BR-16 | Kill claim | The claim is broadcast server-wide with killer name and guild name | `Code/TMSrv/CWarTower.cpp:126-127` |
| BR-17 | Arena eviction on kill | Killing the tower recalls every player inside (2445,1850)-(2546,1920) and respawns the tower | `Code/TMSrv/CWarTower.cpp:132-134` |
| BR-18 | Respawn | A killed tower is always regenerated immediately, in war state, regardless of remaining war time | `Code/TMSrv/CWarTower.cpp:134` |
| BR-19 | Reward | At war end, each mob slot whose `GenerateIndex == GTORRE` and whose `MOB.Guild != 0` grants that guild +100 fame | `Code/TMSrv/CWarTower.cpp:69-98` |
| BR-20 | Reward propagation | Fame is published to DBSrv via `_MSG_GuildInfo`; DBSrv persists it and rebroadcasts to all servers | `Code/TMSrv/CWarTower.cpp:83-94`, `Code/DBSrv/CFileDB.cpp:454-481` |
| BR-21 | Reward lifetime | All guild fame is reset weekly at Sunday 00:00:00 by DBSrv | `Code/DBSrv/Server.cpp:2061-2083` |
| BR-22 | PvP exemption | During the tower war no death experience is lost (treated as `AtWar`) | `Code/TMSrv/MobKilled.cpp:2156` |
| BR-23 | PvP exemption | During the tower war the Nyerds death-protection item (equip 13 `sIndex 769`) is not consumed | `Code/TMSrv/MobKilled.cpp:65` |
| BR-24 | Client aggression flag | Players inside (2430,1825)-(2560,1925) while `GTorreState != 0` are flagged aggressive to clients | `Code/TMSrv/SendFunc.cpp:1758`, `Code/TMSrv/_MSG_PKMode.cpp:51`, `Code/TMSrv/_MSG_QuitTrade.cpp:45`, `Code/TMSrv/_MSG_MessageWhisper.cpp:549` |
| BR-25 | Isolation | During the tower war, BR "guardian" mobs (`Equip[0].sIndex` 219/220) do not inherit the BR owner's guild | `Code/TMSrv/Server.cpp:3124-3145` |
| BR-26 | Configuration | `GTorreHour` is loaded from `gameconfig.txt` line 13 and can be changed at runtime by the GM command `gtorrehour` | `Code/TMSrv/Server.cpp:1085-1114`, `Code/TMSrv/imple.cpp:1757-1764` |
| BR-27 | Guild identity | Guild ids are `ServerIndex * 4096 + counter`, stored as `unsigned short` (0 = no guild), so guild id 0 always means "guildless" | `Code/Basedef.h:147`, `Code/Basedef.h:443`, `Code/TMSrv/_MSG_MessageWhisper.cpp:210` |
| BR-28 | Message contract | Four message-table indices drive all player-visible text; one (`_DN_BASEWORRESULT`) is declared but unused | `Code/TMSrv/Language.h:424-425`, `:501`, `:536` |

### Detailed breakdown of the business rules

---

### Business Rule: BR-01 Event availability gate

**Overview**:
The tower war is a non-permanent feature that only exists on one server of a server group per day. The entire `GuildProcess` body is wrapped in `NewbieEventServer == 1` (`Code/TMSrv/CWarTower.cpp:44`), so no state transition, spawn, notice or reward can occur unless that flag is set.

**Detailed description**:
`NewbieEventServer` is a global `int` (`Code/TMSrv/Server.cpp:310`) that the minute timer re-evaluates every second tick: `NewbieServerID = (tm_mday - 1) % NumServerInGroup`, and the flag is set to 1 when `LOCALSERVER != 0` or `ServerIndex == NewbieServerID`, otherwise forced back to 0 (`Code/TMSrv/ProcessSecMinTimer.cpp:2036-2057`). `NumServerInGroup` is derived from the populated entries of `g_pServerList[ServerGroup]`, clamped to the range 1..10 (`Code/TMSrv/Server.cpp:3572-3582`). Because the election is keyed on the day of the month, the tower war hops between physical channels from one day to the next; the same guild may therefore face a different population on consecutive war days.

The gate has three consequences. First, the war is silently absent on every other server in the group, which is why the module never needs a per-server "enabled" configuration: the election is the enablement. Second, because the flag is also used as a global "newbie server" marker in many unrelated rules (experience penalties, PK mode, auto-trade, revives), toggling it for the election also flips those unrelated behaviours on the elected channel. Third, the flag is recomputed every two minute-ticks, so if the election changes mid-day (for example because a server is added to the group list and the process is restarted) the tower state machine can be interrupted between its "spawn" and "reward" phases with the state left where it was.

For verification purposes: `LOCALSERVER` is set when the configured DB address equals the literal `192.168.0.50` (`Code/TMSrv/Server.cpp:3587-3588`), and `TESTSERVER` when it equals `192.168.0.153` (`:3584-3585`). A local test deployment therefore always satisfies the gate.

**Rule workflow**:
```
Minute tick -> NewbieServerID = (mday-1) % NumServerInGroup
            -> if (LOCALSERVER || ServerIndex == NewbieServerID) NewbieEventServer = 1
               else NewbieEventServer = 0 (and open trades are cancelled)
CWarTower::GuildProcess -> if (NewbieEventServer != 1) return without touching state
```

---

### Business Rule: BR-02 Schedule (weekday restriction)

**Overview**:
The event is bound to weekdays: `timeinfo->tm_wday != 0 && timeinfo->tm_wday != 6` (`Code/TMSrv/CWarTower.cpp:44`). With C `tm_wday` semantics (0 = Sunday, 6 = Saturday) the war can only run Monday through Friday.

**Detailed description**:
This is a pure wall-clock rule with no data-driven calendar and no holiday awareness. It means a five-day rotation of war days in any given week, and it means the event never coincides with Sunday's weekly fame reset (BR-21) or with Saturday, which the guild-invite handler prohibits for guild creation/expansion (`Code/TMSrv/_MSG_InviteGuild.cpp:58-62`). A consequence is that the tower event is deliberately kept out of the Sunday window in which DBSrv zeroes every guild's fame, so the reward granted on Friday night is visible for two days before being wiped.

Because the comparison is applied to `localtime()` output inside `GuildProcess()` (`Code/TMSrv/Server.cpp:6205-6209`), the rule follows the operating-system timezone of the machine that hosts TMSrv. A timezone change or a manual system-clock adjustment during the hour of the event changes which in-game day the war belongs to, without any guard in the component.

If `NewbieEventServer` is 1 on a Saturday or Sunday, the entire block is skipped, and any residual `GTorreState` from a previous day is *not* reset here; that residue is instead handled (or mishandled) by BR-06 on the next eligible day.

**Rule workflow**:
```
tm_wday evaluation -> 0 (Sunday)   : skip block entirely
                    -> 6 (Saturday) : skip block entirely
                    -> 1..5         : continue to BR-03
```

---

### Business Rule: BR-03 Schedule (configurable hour)

**Overview**:
The event is anchored to `timeinfo->tm_hour == GTorreHour`. `GTorreHour` is a global initialised to 22 (`Code/TMSrv/Server.cpp:347`) and overwritten from configuration and from a GM command.

**Detailed description**:
The value is parsed from a fixed-position line of `gameconfig.txt`: line 13 is scanned with a rigid format string that expects the literal tokens `partydif kefrastatus GTorreHour RVRHour DropItem BRHour maxNightmare PotionDelay` in that order, and refuses to start if any token mismatches or any value is `-1` (`Code/TMSrv/Server.cpp:1085-1103`). The value is then copied into `GTorreHour` (`:1107`). At runtime the GM command `gtorrehour <n>` changes the value in memory and redraws the configuration panel (`Code/TMSrv/imple.cpp:1757-1764`), and `DrawConfig` prints it back (`Code/TMSrv/Server.cpp:1313-1314`). Nothing persists the runtime value back to disk, so a restart reverts to the file value.

The hour is used as a hard equality test, which means the whole event lifetime is confined to a single 60-minute window: spawn at minute 6, end at minute 59. There is no concept of "next occurrence" and no queueing. If the process is down during the window, the event for that day does not happen retroactively. If the GM changes `GTorreHour` while `GTorreState` is 1 or 2, the running event is abandoned mid-flight: the state is not reset, and the tower is neither deleted nor rewarded until a later day reaches minute 59 inside an eligible hour.

The value is an `int` with no range validation in the component. `gtorrehour 30` therefore produces an hour that never matches and silently disables the event; a negative value behaves the same way.

**Rule workflow**:
```
gameconfig.txt line 13 -> GTorreHour (default 22, no range check)
GM "gtorrehour N"      -> GTorreHour = N (memory only)
tick                   -> if (tm_hour != GTorreHour) return; else evaluate minutes (BR-04..BR-06)
```

---

### Business Rule: BR-04 Countdown phase (state 0 → 1)

**Overview**:
While the state is 0 and the minute is 0 through 5, the module emits the countdown notice with a literal parameter of 5 and moves to state 1, clearing the previous owner.

**Detailed description**:
The code formats the message `_DN_CHANNELWAR_BEGIN` with the integer `5` (`Code/TMSrv/CWarTower.cpp:48`) and passes it to `SendNotice`, which logs it as `not <text>` and delivers it to every player in `USER_PLAY` mode (`Code/TMSrv/SendFunc.cpp:47-62`). The 5 is not derived from the remaining time; even if the tick that first satisfies the condition happens at minute 4, the broadcast still claims five minutes remain. Because `ProcessMinTimer` is driven by a 12-second Win32 timer (`Code/TMSrv/Server.cpp:3631-3632`), the branch is re-evaluated roughly five times per minute, but the state change from 0 to 1 makes the announcement fire exactly once per war.

The same branch performs the only "new war" bookkeeping in existence: `GTorreGuild = 0` (`Code/TMSrv/CWarTower.cpp:51`). There is no deletion of leftover tower mobs, no reset of `GuildInfo`, no persistence write and no cancellation of a still-running previous war. If the previous war ended without resetting (for example because the 59th minute was never sampled), the new war starts with a stale tower slot that still carries the old owner's guild, and `GGenerateMob` will immediately re-stamp the new tower with the old owner until someone kills it.

The `tm_min <= 5` upper bound is also the only thing that keeps this branch from firing later in the hour: if the state is 0 when the hour begins at minute 6 or later (server started late, or the previous war ended early), no countdown is announced and no spawn occurs, because the second branch requires state 1.

**Rule workflow**:
```
if state == 0 and tm_min <= 5:
    message = format(_DN_CHANNELWAR_BEGIN, 5)
    SendNotice(message)                 -> log + all playing clients
    state = 1
    GTorreGuild = 0
```

---

### Business Rule: BR-05 Spawn phase (state 1 → 2)

**Overview**:
Between minute 6 and the end of the hour, a state-1 war clears the arena, generates the tower mob `GTORRE` (1078) and announces the start.

**Detailed description**:
The branch executes `ClearArea(2445, 1850, 2546, 1920)` before spawning (`Code/TMSrv/CWarTower.cpp:56`). `ClearArea` iterates player slots only (`i < MAX_USER`), skips non-playing slots, skips players whose grid coordinates fall outside the rectangle, resurrects anyone with `Hp <= 0` to 2 HP with a score update, and calls `DoRecall(i)` (`Code/TMSrv/Server.cpp:5754-5775`). `DoRecall` chooses a random point inside the player's city (`g_pGuildZone[CityID]`), overrides it with the guild-zone spawn point if the player's guild owns a zone, overrides it again with the newbie city point for `MORTAL` players below `FREEEXP`, and multicasts an action packet with `Effect = 1` (`Code/TMSrv/Server.cpp:7558-7597`). The eviction is therefore unconditional and identity-agnostic: it does not exempt the defending guild.

The tower is created with `GenerateMob(GTORRE, 0, 0)`, i.e. through the generic generator rather than a bespoke path (`Code/TMSrv/CWarTower.cpp:58`). That call enforces BR-08 and BR-09 and, on success, runs `CWarTower::GGenerateMob` (BR-10). The generate call's return value is `void`, so the module cannot detect a failed spawn: if the generator is at its population cap (`Code/TMSrv/Server.cpp:3044-3048`), or the arena rectangle guard rejects the template (`:3060-3064`), or no free mob slot is available (`:3050-3057`), the module still announces "war started" and sets state 2, producing a war with no tower.

The transition to state 2 is unconditional after the generate call, and the start notice is `_DN_BASEWORSTART` (`Code/TMSrv/CWarTower.cpp:60`). Because the condition is `tm_min >= 6` rather than `tm_min == 6`, the spawn will occur at the first sampled tick at or after minute 6, including late ticks caused by a blocked message loop.

**Rule workflow**:
```
if state == 1 and tm_min >= 6:
    ClearArea(2445,1850,2546,1920)      -> recall all players, revive dead
    GenerateMob(1078, 0, 0)             -> template copy, cap check, area guard, GGenerateMob stamp
    SendNotice(_DN_BASEWORSTART)
    state = 2
```

---

### Business Rule: BR-06 End phase (state 2 → 0) and per-slot reward sweep

**Overview**:
At exactly minute 59 the arena is cleared, every mob slot carrying `GenerateIndex == GTORRE` is examined, guild-owned ones grant +100 fame to their recorded guild, all of them are deleted, and the state and owner are reset.

**Detailed description**:
The end branch is gated by `GTorreState == 2 && timeinfo->tm_min == 59` (`Code/TMSrv/CWarTower.cpp:65`). This is an equality test on a value that advances in real time, sampled by a 12-second timer; the branch therefore has roughly five opportunities to run inside the correct minute, and exactly zero tolerance for a stall longer than the remaining part of that minute. If it is missed, state stays 2 and `GTorreGuild` keeps its value. On the next eligible day the hour guard passes and the state-2 branch waits until minute 59 again, at which point the *stale* tower slots (which may have been lying dormant for 24 hours) are rewarded a second time before being deleted. The same latched state also suppresses the next day's countdown and spawn, because those branches require state 0 and state 1 respectively.

The reward sweep iterates `i` from `MAX_USER` to `MAX_MOB` and tests only `pMob[i].GenerateIndex == GTORRE` (`:69-71`). It does not test `Mode`, `Hp` or grid occupancy. `DeleteMob(i, 1)` sets `Hp = 0`, `Mode = 0` and clears the grid cell, but deliberately leaves `GenerateIndex` untouched (`Code/TMSrv/Server.cpp:7047-7049`). Consequently every tower spawn that survived in a distinct slot keeps matching the filter after death. `GetEmptyNPCMob` returns the first slot with `Mode == MOB_EMPTY` starting from a rotating cursor (`Code/TMSrv/Server.cpp:6765-6791`), so a respawn normally lands on a different slot than the one just freed. Each additional tower kill during the war therefore adds one more matching slot, and each matching slot with a non-zero `MOB.Guild` pays +100 fame to the guild recorded in that slot. The total awarded at minute 59 can exceed 100 and can even be split between the previous and the current owner.

The sweep also calls `ClearArea` first (`:67`), so players are removed before the towers disappear, avoiding clients watching a vanish inside the arena. `DeleteMob(i, 1)` also decrements `mNPCGen.pList[1078].CurrentNumMob` for each slot it deletes, which means the count can be decremented below the number of actually living towers and is clamped at zero (`Code/TMSrv/Server.cpp:7034-7044`).

**Rule workflow**:
```
if state == 2 and tm_min == 59:
    ClearArea(2445,1850,2546,1920)
    for i in [MAX_USER, MAX_MOB):
        if pMob[i].GenerateIndex == 1078:
            if pMob[i].MOB.Guild != 0:
                BASE_GetGuildName(ServerGroup, guild, buf)
                GuildInfo[guild].Fame += 100
                send _MSG_GuildInfo(guild, GuildInfo[guild]) to DBSrv
                Log("etc,war_tower1 guild:%d guild_fame:%d")
            DeleteMob(i, 1)
    state = 0
    GTorreGuild = 0
```

---

### Business Rule: BR-07 Ownership reset at war start

**Overview**:
`GTorreGuild` is set to 0 in the countdown branch (`Code/TMSrv/CWarTower.cpp:51`) and in the end branch (`:105`), and nowhere else except the kill hook.

**Detailed description**:
This is the component's entire concept of "no owner". Because `GGenerateMob` only stamps a guild when `GTorreGuild` is non-zero (`:140`), a zero owner means the freshly spawned tower is guildless, which per BR-11/BR-12 makes it attackable by any guild including a guildless player... except that a guildless player is rejected by BR-11 regardless. A guildless tower is therefore attackable by every guild, which is the correct neutral state.

The reset is memory-only. `GTorreGuild` is an ordinary global (`Code/TMSrv/Server.cpp:348`) with no persistence, no journal and no DB packet, so a process restart between two wars reverts ownership to 0. Restarting in the middle of a war loses the current owner while the tower mob itself is also destroyed by the world reboot path, so the war continues with nothing to attack until the next `MobKilled`/`GuildProcess` event.

Note that `GTorreGuild` is written to `pMob[tmob].MOB.Guild`, an `unsigned short` (`Code/Basedef.h:443`), while the global is an `int`. Guild identifiers are bounded by construction (BR-27) so the narrowing is lossless in practice, but the component performs no range validation of its own before indexing `GuildInfo[65536]`.

**Rule workflow**:
```
war start (state 0->1)  -> GTorreGuild = 0
kill with guild         -> GTorreGuild = killer guild
war end   (state 2->0)  -> GTorreGuild = 0
process restart         -> GTorreGuild = 0 (initial value)
```

---

### Business Rule: BR-08 Spawn constraint inside the arena rectangle

**Overview**:
`GenerateMob` refuses to place any NPC whose template segment origin is inside (2440,1845)-(2545,1921) unless the template's `Equip[0].sIndex` is 219, and marks the generator `MinuteGenerate = -1` as a side effect.

**Detailed description**:
The guard is `if (mNPCGen.pList[index].Leader.Equip[0].sIndex != 219 && SegmentListX[0] >= 2440 && <= 2545 && SegmentListY[0] >= 1845 && <= 1921)` → set `MinuteGenerate = -1` and return (`Code/TMSrv/Server.cpp:3060-3064`). Its purpose is to prevent regular field monsters from being spawned inside the PvP arena; 219 is the item index used elsewhere in the same function to mark "summon/guardian"-style mobs whose guild may be inherited (`:3124-3145`). For the tower event this is a hard, undocumented prerequisite: the NPC generator entry 1078 must carry `Equip[0].sIndex == 219`, otherwise the war's spawn silently does nothing.

The rectangle used here (2440..2545 / 1845..1921) is smaller than the rectangle used for the PvP aggression flag in the client-facing handlers (2430..2560 / 1825..1925, BR-24) and slightly smaller than the `ClearArea` rectangle (2445..2546 / 1850..1920). Three different definitions of "the arena" coexist in the codebase, and the module owns only the middle one.

Note also that the guard reads `mNPCGen.pList[index]` — the generator entry, not the instance — so it rejects the generator permanently for the process lifetime by zeroing `MinuteGenerate`, which is a mutation of shared configuration state performed from a generation path.

**Rule workflow**:
```
GenerateMob(index):
    if template.Equip[0].sIndex != 219 and template origin in arena rect:
        template.MinuteGenerate = -1
        return (no mob created)
    ... normal generation ...
```

---

### Business Rule: BR-09 Population cap on tower generation

**Overview**:
Any generate call, including the tower's, is rejected when `mNPCGen.pList[nindex].CurrentNumMob >= MaxNumMob`, and is trimmed when the random group size would exceed the cap.

**Detailed description**:
`GenerateMob` computes a random group size from `MinGroup`/`MaxGroup`, then returns early if the generator is already at `MaxNumMob`, and otherwise clamps the group size to the remaining headroom (`Code/TMSrv/Server.cpp:3030-3048`). For a boss-like single-spawn entry this means the tower can only exist once per generator, which is the intended behaviour, but it also means the spawn is refused while a previous, accidentally orphaned tower instance still counts against the generator. Since `CurrentNumMob` is incremented on every successful generation (`:3209`) and decremented by `DeleteMob(...,1)` (`Code/TMSrv/Server.cpp:7040`), any path that removes a tower slot without `DeleteMob` (or double-deletes it) desynchronises the counter and can lock the event out.

The `CurrentNumMob` counter is also clamped back to zero on `DeleteMob` and on generation when it is negative (`:3206-3207`), so over-deletion is masked rather than reported. In combination with BR-06 (which deletes every matching slot, including dead ones), the counter can be driven to zero while a live tower still exists in a different slot; the next respawn then succeeds and the arena can briefly hold two towers, both flagged `GenerateIndex == GTORRE`.

**Rule workflow**:
```
GenerateMob(index):
    if (CurrentNumMob >= MaxNumMob) return
    if (CurrentNumMob + group > MaxNumMob) group = MaxNumMob - CurrentNumMob
    allocate slot, copy template, ... CurrentNumMob++
```

---

### Business Rule: BR-10 Ownership stamping at generation

**Overview**:
`CWarTower::GGenerateMob` is invoked from the generic generation path for every NPC and overwrites the new instance's guild with `GTorreGuild` when the generator index is the tower and an owner is recorded.

**Detailed description**:
The hook is called after the template `STRUCT_MOB` has been copied into the instance and after the BR guardian guild-inheritance block, but before `pMob[tmob].GenerateIndex = index` is assigned (`Code/TMSrv/Server.cpp:3069-3149`). It sets `pMob[tmob].MOB.Guild = GTorreGuild` and `pMob[tmob].MOB.GuildLevel = 0` (`Code/TMSrv/CWarTower.cpp:140-144`). Two effects follow. First, the tower behaves as a guild member of level 0, which matters for downstream rules that special-call guild leaders (`GuildLevel == 9`) and for the attack predicate which compares guild equality. Second, the tower name and appearance are not touched, so clients still see a neutral mob.

The hook is unconditional with respect to war state: it fires whenever generator 1078 is generated, at any time of day. Concretely, a manual GM spawn of `GTORRE` outside the war window (or a generator-driven respawn) inherits the last recorded owner if `GTorreGuild` is non-zero, i.e. if a previous war was interrupted before its reset. Conversely, when `GTorreGuild` is zero, a tower spawned during a war is guildless and therefore immune to nobody — any guild can damage it, and its first killer becomes the first owner.

`GGenerateMob` accepts `PosX`, `PosY` and does not use them; the parameters exist only for signature symmetry with the other generation hooks in the same region. The same unused-parameter pattern appears in `MobKilled` (`Code/TMSrv/CWarTower.cpp:110`). Neither hook validates `tmob` against `MAX_MOB`, relying entirely on the caller.

**Rule workflow**:
```
GenerateMob(index, ...):
    copy template -> instance
    ... BR guardian guild block ...
    GGenerateMob(index, PosX, PosY, tmob):
        if index == 1078 and GTorreGuild != 0:
            instance.MOB.Guild = GTorreGuild
            instance.MOB.GuildLevel = 0
    instance.GenerateIndex = index
    instance HP = MaxHp (or 3/4 on newbie server below level 120)
```

---

### Business Rule: BR-11 and BR-12 Attack permission: guildless and owner are denied

**Overview**:
`TowerAttack` returns `FALSE` when the attacker has no guild or belongs to the tower's guild (`Code/TMSrv/CWarTower.cpp:152-153`).

**Detailed description**:
The predicate first short-circuits for any non-tower target (`pMob[idx].GenerateIndex != GTORRE → TRUE`), so it costs one integer compare for every normal attack in the game. For a tower target it then applies two denials. The guildless denial means a player without a guild cannot participate in the tower war at all, not even to help a guild by chipping the tower's HP; this is the mechanism that enforces "the tower war is a guild activity". The owner denial means a guild cannot damage the tower it currently holds, which is what prevents a guild from re-claiming its own tower for a repeated claim notice or from resetting the tower's HP.

The predicate does not verify that the attacker is a player: `conn` may be a summoning mob index in `[MAX_USER, MAX_MOB)`. Whether a summon can damage the tower depends on whether the summon's `MOB.Guild` was inherited, which happens only for the 219/220 guardian templates (`Code/TMSrv/Server.cpp:3124-3145`) and only while `RvRState == 0 && GTorreState == 0`. Because BR-25 suppresses that inheritance during the tower war, summons are effectively unable to damage the tower during the war unless their template already carries a guild.

The predicate also does not consult `GTorreState`, `Hp`, the owner's existence, or whether the attacker is in the arena rectangle. Those are enforced elsewhere (damage pipeline, arena eviction, spawn guard).

**Rule workflow**:
```
TowerAttack(conn, idx):
    if pMob[idx].GenerateIndex != 1078   -> TRUE
    if pMob[conn].MOB.Guild == 0         -> FALSE
    if pMob[conn].MOB.Guild == pMob[idx].MOB.Guild -> FALSE
    -> TRUE
```

---

### Business Rule: BR-13 Non-owner guilds may always attack the tower

**Overview**:
Any guild that is neither the owner nor guildless is allowed to damage the tower, regardless of war state, distance, remaining time, or ownership history.

**Detailed description**:
This is the implicit `return TRUE` at the end of the predicate (`Code/TMSrv/CWarTower.cpp:155`). It is what makes the tower a "capture by damage" objective rather than a scripted encounter: there is no challenge registration, no declared opponent, no siege window per guild, and no protection timer after a capture. A second guild may immediately attack the freshly spawned tower claimed by the first guild.

Because the rule ignores `GTorreState`, it also governs the abnormal case of a latched state or an orphaned tower: the tower stays consumable and re-claimable whenever it exists. It also means the ability to attack is not a reward for participating in the event; it is only gated by the two denials of BR-11/BR-12.

There is no cooldown, no damage-cap per hit, and no "last hit" protection beyond the standard `MobKilled` ownership transfer. Any guild can therefore take the tower by arranging the killing blow, including with a single overpowered hit.

**Rule workflow**:
```
owner != 0, attacker guild != owner, attacker guild != 0
    -> TRUE -> damage proceeds through the normal combat pipeline
```

---

### Business Rule: BR-14 Attack enforcement by damage nulling

**Overview**:
Rejection is implemented by clearing `TargetID` and `Damage` of that damage entry and continuing the loop (`Code/TMSrv/_MSG_Attack.cpp:398-403`).

**Detailed description**:
The check is placed after the structural validations (message size, dead target, merchant flags, view-grid distance, damage-marker anti-cheat) and after guild/clan normalisation, but before the arena-attribute and friendly-fire checks (`Code/TMSrv/_MSG_Attack.cpp:331-419`). It is evaluated per damage entry `i`, which matters for multi-target skills: a rejected tower entry is silently dropped while the other entries of the same attack resolve normally. The client is not told why the entry was removed; it simply sees no damage event for that target.

This placement has two consequences. First, the anti-cheat `AddCrackError` path is not triggered by a rejected tower attack, because the damage-marker check has already passed. Second, the rejected entry does not consume the later code paths that compute critical/double-damage random rolls, so the random-number stream is not advanced by a blocked tower attack — server-side RNG consumption differs depending on whether the attack was allowed, which is an observable (if benign) side channel.

Because `TargetID` is zeroed rather than the whole packet being dropped, a crafted client that sends a large `Dam` array gets a partially answered attack: accepted mobs take damage, the tower is listed with id 0. No error code, log line, or counter is produced for rejected tower attacks, so attempts are invisible in logs.

**Rule workflow**:
```
for each damage entry i:
    ... structural + anti-cheat validation ...
    if TowerAttack(conn, idx) == FALSE:
        m->Dam[i].TargetID = 0
        m->Dam[i].Damage = 0
        continue
    ... damage resolution ...
```

---

### Business Rule: BR-15 Kill claim (ownership transfer)

**Overview**:
The tower's death transfers ownership to the killer's guild only when the killer has a guild and the war state is not zero; a guildless killer still triggers eviction and respawn but leaves the owner unchanged.

**Detailed description**:
`MobKilled` reads `pMob[target].GenerateIndex` and requires it to equal `GTORRE` and `GTorreState` to be non-zero (`Code/TMSrv/CWarTower.cpp:112-114`). The state check gives the tower "invulnerability to ownership change" outside the event, without making it unattackable. Inside the state check, a non-zero `pMob[conn].MOB.Guild` sets `GTorreGuild`. Note that the assignment is `GTorreGuild = pMob[conn].MOB.Guild`, reading the *killer instance*, not the summoner: if the killing blow was landed by a summon whose `MOB.Guild` is zero, the claim is skipped even though the owner is obvious to players. Earlier in `MobKilled` the `conn` is reassigned to the summoner only in the mount-experience branch (`Code/TMSrv/MobKilled.cpp:204`), not in the general path.

Reachability is restricted by the caller's structure. The hook is called inside the `else` branch of the clan filter at `Code/TMSrv/MobKilled.cpp:636-639`, so a killer whose `MOB.Clan` is 1, 2, 3, 5 or 6 never reaches the tower-claim code; only clan 0, 4, 7 and 8 do (4 being the summon/creature clan). Since most players have `Clan == 0`, the feature works for the ordinary population, but a player who belongs to one of the other clans can kill the tower without claiming it and without the claim announcement.

The hook also runs *after* `DeleteMob(target, 1)` (`Code/TMSrv/MobKilled.cpp:1990`), so it depends on `DeleteMob` not clearing `GenerateIndex` (it does not — `Code/TMSrv/Server.cpp:7047-7049`). Any future change that zeroes `GenerateIndex` in `DeleteMob` would silently disable the entire tower-claim feature.

**Rule workflow**:
```
CWarTower::MobKilled(target, conn, ...):
    GenerateID = pMob[target].GenerateIndex
    if GenerateID == 1078 and GTorreState != 0:
        if pMob[conn].MOB.Guild != 0:
            announce, GTorreGuild = guild
        ClearArea(...); GenerateMob(1078,0,0)
    (no else: nothing happens, no log)
```

---

### Business Rule: BR-16 Claim announcement

**Overview**:
The claim is broadcast as `_SS_BASEWORKILLTOWER` formatted with the killer's `MobName` and the guild's resolved name.

**Detailed description**:
The name lookup is `BASE_GetGuildName(ServerGroup, usGuild, guildname)` with a 256-byte buffer (`Code/TMSrv/CWarTower.cpp:122-124`). `BASE_GetGuildName` splits the guild id into `Server = id / MAX_GUILD` and `Guild = id - Server * MAX_GUILD`, bounds-checks both, and copies the cached name only if it is non-empty (`Code/Basedef.cpp:1317-1330`); otherwise the buffer is left untouched. Since `guildname` is a local stack array that is not initialised (`char guildname[256];` at `:122`), an unknown or not-yet-initialised guild name results in `sprintf` reading uninitialised stack memory when formatting the notice — undefined behaviour with a garbled name (not a crash, because `sprintf` stops at the first zero byte, but the output is unpredictable). `CWarTower.cpp:79` declares the same pattern in the end-of-war sweep.

The message is delivered through `SendNotice`, which prefixes `not `, appends the raw text to the server log, then sends a `_MSG_MessagePanel` to every `USER_PLAY` socket (`Code/TMSrv/SendFunc.cpp:47-62`). The killer's name is copied into the notice without sanitisation; `MobName` is a fixed 12-byte array with forced terminators applied at generation (`Code/TMSrv/Server.cpp:3071-3086`), and `SendClientMessage` forcibly terminates the last two bytes (`Code/TMSrv/SendFunc.cpp:41-42`), so the practical buffer risk is contained, but the notice is still built into the shared global `temp[4096]` (`Code/TMSrv/Server.h:382`).

There is no announcement when a guildless killer destroys the tower, and no announcement when the owner is cleared at war start or war end.

**Rule workflow**:
```
if killer guild != 0:
    BASE_GetGuildName(ServerGroup, guild, guildname[256])   // may stay uninitialised
    sprintf(temp, _SS_BASEWORKILLTOWER, killer.MobName, guildname)
    SendNotice(temp)                                        // log + all clients
```

---

### Business Rule: BR-17 Arena eviction on tower kill

**Overview**:
When the tower dies, every player inside (2445,1850)-(2546,1920) is recalled to their city/save point, including the attacker who just killed it.

**Detailed description**:
`ClearArea` is called before the respawn (`Code/TMSrv/CWarTower.cpp:132`), with the same rectangle as the spawn phase. `ClearArea` only iterates `i < MAX_USER` and skips slots where `pUser[i].Mode != USER_PLAY` or `pMob[i].Mode == USER_EMPTY`; it revives players at 2 HP and then calls `DoRecall` (`Code/TMSrv/Server.cpp:5754-5775`). `DoRecall` does not merely teleport: it applies the guild-zone spawn, the newbie-city override for `MORTAL` players below `FREEEXP`, and multicasts an action with `Effect = 1` (`Code/TMSrv/Server.cpp:7558-7597`).

Because the eviction is applied to attackers as well, every tower kill resets the PvP encounter: no guild can camp the tower with a full raid and chain-capture it, since the raid is scattered to its city spawns (and the winning guild may even be split across different cities if members have different `Merchant` city bits or newbie status). This is the component's de-facto anti-zerg rule, though it is never documented as such.

The rectangle is strictly smaller than the client-facing aggression rectangle (BR-24): players standing at x in [2430,2444] or [2547,2560], or y in [1825,1849] or [1921,1925], are flagged aggressive but are *not* evicted when the tower dies. Conversely, players between 2440-2444 or 2546-2545 border values may be evicted by BR-17 while sitting just outside the BR-08 spawn guard area.

**Rule workflow**:
```
tower death -> ClearArea(2445,1850,2546,1920)
            -> for each playing player inside: revive to 2 HP if dead, DoRecall
            -> GenerateMob(1078,0,0)
```

---

### Business Rule: BR-18 Immediate respawn

**Overview**:
A killed tower is regenerated immediately and unconditionally while the war state is non-zero, with the new owner stamped in.

**Detailed description**:
The respawn is the last statement of the hook (`Code/TMSrv/CWarTower.cpp:134`), which means the tower is a renewable objective for the whole 53-minute window rather than a one-shot capture. Combined with BR-15, the war becomes a sequence of captures, each of which announces the new owner and scatters the participants. There is no capture lockout, no respawn delay and no HP scaling: the replacement tower comes from the same generator template and therefore has the same statistics.

Because the respawn happens *after* `ClearArea`, and because `GenerateMob` enforces BR-08/BR-09, a failed respawn leaves the arena empty for the remainder of the war with no notice and no retry (the module cannot observe the failure — `GenerateMob` returns `void`). `GTorreState` remains 2, so the end-of-war sweep will still run and simply find no tower to reward.

Note the ordering interaction described in BR-15: the call arrives after the caller already executed `DeleteMob(target, 1)`, so the slot count has been decremented and the killed instance's `Mode` is 0 before `GenerateMob` runs; whether the replacement lands in the same slot depends solely on the `g_EmptyMob` cursor.

**Rule workflow**:
```
GenerateID == 1078 && GTorreState != 0
    -> (optional claim) -> ClearArea -> GenerateMob(1078) -> [BR-08][BR-09][BR-10]
```

---

### Business Rule: BR-19 Fame reward at war end

**Overview**:
Each mob slot with `GenerateIndex == GTORRE` whose `MOB.Guild` is non-zero grants that guild +100 fame; the tower is then deleted.

**Detailed description**:
The code enumerates all NPC slots and, per matching slot, resolves the guild name, increments `GuildInfo[usGuild].Fame` by 100 and immediately builds a `_MSG_GuildInfo` packet with `ID = 0`, `Guild = usGuild` and a copy of the updated structure (`Code/TMSrv/CWarTower.cpp:75-95`). The in-code comment on line 64 states the defending guild receives 50 fame, while line 90 adds 100; the compiled behaviour is 100. `Fame` is a `short` (`Code/Basedef.h:798`), so it saturates/wraps at 32767 rather than growing without bound; no clamping exists in this module.

`usGuild` is assigned from `pMob[i].MOB.Guild` (an `unsigned short`) and used to index `GuildInfo[65536]`, which is safe by construction but unvalidated; `int Server = pMob[i].MOB.Guild / MAX_GUILD` is computed and never used (`:76`), a vestigial line that also exists in the kill hook (`:119`).

The duplicate-award mechanism described under BR-06 is the dominant risk here: because the filter ignores `Mode`, every distinct slot that ever hosted a tower during the war will match, and each will add 100 to whichever guild was recorded in that slot at generation time. A war with five capture cycles can therefore pay 600 instead of 100, spread across several guilds. The per-slot log line `etc,war_tower1 guild:%d guild_fame:%d` is the only audit trail, and it is emitted per award, so multiplicity is visible after the fact in the server log.

**Rule workflow**:
```
for i in [MAX_USER, MAX_MOB):
    if GenerateIndex == 1078:
        if MOB.Guild != 0:
            name lookup (uninitialised buffer risk)
            GuildInfo[guild].Fame += 100
            send _MSG_GuildInfo -> DBSrv
            log "etc,war_tower1"
        DeleteMob(i, 1)
```

---

### Business Rule: BR-20 Fame propagation to DBSrv

**Overview**:
The fame award is transmitted as a single `_MSG_GuildInfo` message (message id 19) to the DB server socket with `ID = 0` and no acknowledgement.

**Detailed description**:
The packet is filled in place (`Type`, `Size`, `ID`, `Guild`, `GuildInfo`) and sent with `DBServerSocket.SendOneMessage` (`Code/TMSrv/CWarTower.cpp:83-94`). DBSrv validates `myguild` in `(0, 65536)`, stores the received structure into its own `GuildInfo[65536]`, re-broadcasts `SendGuildInfo` to every connected game server, and calls `CReadFiles::WriteGuildInfo()` to persist (`Code/DBSrv/CFileDB.cpp:454-481`). The broadcast at the DB layer means every game server in the group observes the new fame immediately, which is how the tower reward becomes visible outside the elected channel.

There is no ordering guarantee, no retry, no sequence number and no acknowledgement. If the TMSrv↔DBSrv link is down at minute 59, the fame award is lost for that tower while the TMSrv-side global still shows the incremented value (TMSrv keeps its own `GuildInfo[65536]` at `Code/TMSrv/Server.cpp:361`), producing divergence until the next synchronisation. `ID = 0` is not a player id, so no per-connection state is affected.

The write is synchronous inside the DB message handler, so a slow disk extends the handler time; nothing in this component depends on the write having completed.

**Rule workflow**:
```
TMSrv: GuildInfo[g].Fame += 100 ; send _MSG_GuildInfo{ID=0,Guild=g,GuildInfo=GuildInfo[g]}
DBSrv: case _MSG_GuildInfo -> GuildInfo[g] = payload -> broadcast -> WriteGuildInfo()
```

---

### Business Rule: BR-21 Weekly fame reset (cross-component constraint)

**Overview**:
DBSrv zeroes every guild's fame at Sunday 00:00:00 and persists the reset.

**Detailed description**:
The DB timer checks `tm_hour == 0 && tm_wday == 0 && tm_min == 0 && tm_sec == 0` and, for every guild with non-zero fame, clears it and pushes a `SendGuildInfo` to each connected server, then writes the guild file (`Code/DBSrv/Server.cpp:2061-2083`). This is the only reset of the value the tower event accumulates. Because the tower war is weekday-only (BR-02), a Friday-evening reward is the one that survives the longest before being wiped, and a Sunday reward is impossible.

The reset is exact-to-the-second. DBSrv's timer must tick inside the `00:00:00` second; if it does not (heavy load, blocked message loop), the weekly reset is skipped entirely and fame accumulates until the following Sunday. Nothing in the tower component compensates for this, and nothing in the tower component reads fame, so a missed reset has no effect on the tower event itself — only on the guild ranking/Fame consumers.

**Rule workflow**:
```
DBSrv minute timer -> Sunday 00:00:00 -> for all guilds with Fame: Fame = 0, broadcast, WriteGuildInfo
Tower war (Mon-Fri) -> adds +100 per matching tower slot at minute 59
```

---

### Business Rule: BR-22 No death experience loss during the tower war

**Overview**:
`GTorreState != 0` forces `AtWar = 1` in the PvP death handler, which suppresses experience loss and PK bookkeeping.

**Detailed description**:
The assignment sits in the PvP `else` branch of `MobKilled`, alongside the castle and RvR equivalents (`Code/TMSrv/MobKilled.cpp:2150-2157`), after the base delta has been computed from level tables and divided by 6 (or 3 on test servers) (`:2131-2137`). Downstream, `AtWar` gates the experience-loss and PK-point blocks (`:2229-2242`, `:2280`). The effect is that the whole map — not just the arena rectangle — becomes a no-experience-loss zone for the duration of the war, because `GTorreState` is global and no coordinate test is applied here.

This is a stronger behaviour than the arena-scoped aggression flag of BR-24 and is not restricted by clan, guild or level. It also applies during the countdown phase (state 1), i.e. before the tower exists, and during any latched state 2, i.e. potentially for a full day after a missed end tick.

Players dying anywhere on the map during the war therefore keep their experience; combined with BR-23 the towers war is effectively a "free" PvP window for everyone, which is a material economy effect that is not described anywhere in the component.

**Rule workflow**:
```
PvP death -> if (GTorreState != 0) AtWar = 1
          -> AtWar suppresses deltaexp loss and PK point changes
```

---

### Business Rule: BR-23 Nyerds protection suspended during the tower war

**Overview**:
The death-protection item (`Equip[13].sIndex == 769`, comment "Nyerds") is not consumed when `GTorreState != 0`.

**Detailed description**:
The guard at the top of `MobKilled` requires `RvRState == 0 && GTorreState == 0 && CastleState == 0` before it decrements the fairy's sanctification and either updates or clears the item (`Code/TMSrv/MobKilled.cpp:65-104`). With the tower war running, the branch is skipped entirely, so the item keeps its charges and the player keeps its effect. This is consistent with the "free PvP window" design of BR-22, and it means the tower war is the only one of the three PvP events (RvR, castle, tower) that shares this specific interaction pattern — the other two are handled by the same single condition, so all three suspend the item.

The rule is global, again without any location test, and it is active from state 1 onward. A war that never ends (missed 59-minute tick) leaves the protection suspended indefinitely, which is an economy-relevant failure mode rather than a cosmetic one.

**Rule workflow**:
```
MobKilled start -> if (Equip[13].sIndex == 769 && RvRState == 0 && GTorreState == 0 && CastleState == 0)
                       decrement/clear fairy, resend item, restore HP, return
                   else -> normal death processing
```

---

### Business Rule: BR-24 Client aggression flag for the arena rectangle

**Overview**:
While `GTorreState != 0`, a player whose position is inside (2430,1825)-(2560,1925) is reported to clients with `state = 1` in `_MSG_PKInfo`, i.e. flagged as an aggressive/PvP target.

**Detailed description**:
Three handlers contain a copy-pasted expression: the `pkmode` handler (`Code/TMSrv/_MSG_PKMode.cpp:51`), the trade-exit handler (`Code/TMSrv/_MSG_QuitTrade.cpp:45`), the whisper/GM handler (`Code/TMSrv/_MSG_MessageWhisper.cpp:549`), and the `SendPKInfo` helper (`Code/TMSrv/SendFunc.cpp:1758`). All four resolve `state` from `guilty || PKMode || (RvRState && rectRvR) || (CastleState && rectCastle) || (GTorreState && rectTorre)`, and all four are bypassed when `NewbieEventServer != 0` (in which case `state` is forced to 1 for everyone). Operator precedence in the expression is `&&` before `||`, so the intended grouping holds, but the expression is written without parentheses and duplicates magic rectangles.

The rectangle (2430..2560, 1825..1925) is 10 grid units wider and 25 higher than the eviction rectangle of BR-17, and 10/5 wider than the spawn guard of BR-08. A player standing at the edge is therefore flagged aggressive to every nearby client while not being evicted when the tower dies and not blocking NPC spawns. Since `SendPKInfo` is only sent on specific events (PK mode toggle, trade exit, certain chat commands) and not continuously, the flag also lags behind the player's actual position.

Because the whole block is skipped when `NewbieEventServer != 0`, on the elected channel every player is reported as state 1 regardless of the tower war — the tower-specific rectangle only matters for non-elected servers, where `GTorreState` can never be non-zero (BR-01). The rectangle test is therefore effectively dead code in a correctly configured group.

**Rule workflow**:
```
NewbieEventServer != 0 -> state = 1 (blanket)
else -> state = guilty || PKMode || (RvR && rect) || (Castle && rect) || (GTorreState && rectTorre)
     -> GridMulticast(_MSG_PKInfo{ID=conn, Parm=state})
```

---

### Business Rule: BR-25 BR guardian guild inheritance suppressed during the war

**Overview**:
Mobs whose template uses `Equip[0].sIndex` 219/220 do not inherit the BR owner's guild while `GTorreState != 0` (or `RvRState != 0`).

**Detailed description**:
The block copies the BR owner's `Equip[15]` effect words and, for index 219, the owner's guild, then calls `GetGuild(tmob)` and demotes a level-9 owner to level 0 (`Code/TMSrv/Server.cpp:3124-3145`). The guard `RvRState == 0 && GTorreState == 0` prevents the inheritance. The practical effect on the tower event is indirect but important: summon/guardian mobs generated during the war do not acquire a guild, and therefore fall under BR-11 (`MOB.Guild == 0 → cannot damage tower`), so the event is protected from BR-guardian interference. Entry 219 is also the flag that BR-08 requires for spawning inside the arena, which ties the tower template to this very block.

The suppression is decided by global state, not by location, so a BR guardian generated in the BR map during the tower hour also loses its guild. Since the two events are scheduled at different hours (`GTorreHour` vs `BRHour`), overlap is possible only through manual GM action or a misconfiguration.

**Rule workflow**:
```
GenerateMob -> if template Equip[0] in {219,220} and BrState valid:
                   if (RvRState == 0 && GTorreState == 0): copy equip, guild, GetGuild
                   else: skip inheritance
               -> GGenerateMob (BR-10)
```

---

### Business Rule: BR-26 Runtime configuration of the war hour

**Overview**:
`GTorreHour` is a mutable global, sourced from `gameconfig.txt` and from the `gtorrehour` GM command, with no validation and no persistence of runtime changes.

**Detailed description**:
The configuration parser is positional and strict: it requires the exact eight tokens on line 13 and aborts the whole configuration read (with a modal error box) if any token is wrong or any value is `-1` (`Code/TMSrv/Server.cpp:1085-1103`). This means a missing or reordered line silently prevents `GTorreHour` (and the seven other values) from being applied, and the process keeps the compile-time default of 22. `DrawConfig` prints the current values to the admin UI (`:1313-1314`), which is the only visibility the operator has.

The GM command form `gtorrehour <n>` (lowercase, as compared with `strcmp`) assigns `ival1` without bounds checking and redraws the config (`Code/TMSrv/imple.cpp:1757-1764`). No permission check is visible in the fragment, and the value is not written back to the file, so the change is lost on restart.

**Rule workflow**:
```
startup -> parse gameconfig.txt line 13 -> GTorreHour = value[2] (or keep default 22 on failure)
runtime -> GM "gtorrehour N" -> GTorreHour = N ; DrawConfig(TRUE)
tick    -> compare tm_hour against GTorreHour (equality)
```

---

### Business Rule: BR-27 Guild identity model

**Overview**:
Guild ids are unsigned 16-bit composites of `ServerIndex * MAX_GUILD(4096) + local guild number`, with 0 reserved for "no guild".

**Detailed description**:
The encoding is defined by `MAX_GUILD 4096` (`Code/Basedef.h:147`), produced at guild creation (`pMob[conn].MOB.Guild = ServerIndex * MAX_GUILD + GuildCounter`, `Code/TMSrv/_MSG_MessageWhisper.cpp:210`), and decoded by `BASE_GetGuildName` as `Server = id / 4096`, `Guild = id - Server * 4096` (`Code/Basedef.cpp:1317-1330`). `STRUCT_MOB.Guild` is `unsigned short` (`Code/Basedef.h:443`), so the theoretical maximum is 65535 and the 16-server × 4096 layout fits exactly.

Every branch of this component treats 0 as "guildless": `TowerAttack` denies it, `GGenerateMob` requires a non-zero owner before stamping, `MobKilled` skips the claim on 0, and the end-of-war sweep skips fame for 0. This shared assumption is the component's most important implicit contract and is not asserted anywhere. Because the value cannot exceed 65535, `GuildInfo[65536]` indexing from `MOB.Guild` is safe, but the same expression is only safe because of the type, not because of a check: `pMob[i].MOB.Guild / MAX_GUILD` and `GuildInfo[usGuild]` would both be wrong if the field were widened.

`GuildLevel` is also part of the contract: the tower is stamped with level 0 (`Code/TMSrv/CWarTower.cpp:143`), and level 9 denotes a guild leader elsewhere in the codebase (`Code/TMSrv/_MSG_War.cpp:33`, `Code/TMSrv/_MSG_InviteGuild.cpp:48`). The tower therefore never satisfies leader-only predicates.

**Rule workflow**:
```
guild creation -> Guild = ServerIndex * 4096 + counter (<= 65535, 0 reserved)
tower logic    -> Guild == 0 means guildless in all four methods
name lookup    -> BASE_GetGuildName splits on / MAX_GUILD and bounds-checks
```

---

### Business Rule: BR-28 Player-visible message contract

**Overview**:
The component depends on exactly four message-table indices; three are used and one is declared but never referenced.

**Detailed description**:
`_DN_CHANNELWAR_BEGIN` (501) is the countdown notice with one integer parameter; `_DN_BASEWORSTART` (425) is the start notice with no parameter; `_SS_BASEWORKILLTOWER` (536) is the claim notice with two string parameters (killer name, guild name); `_DN_BASEWORRESULT` (424) is declared in `Code/TMSrv/Language.h:443` and referenced nowhere in the repository, i.e. an unused message slot that suggests a "war result" announcement that was never wired (or was removed). Indices are within `MAX_STRING 2000` (`Code/Basedef.h:150`).

The table is loaded from `Language.txt` by `BASE_InitializeMessage`, which parses `<index>\t<text>` per line, skips indices outside `(0, MAX_STRING]`, rejects empty or ≥128-character strings with a modal message box, and copies the remainder into `g_pMessageStringTable[index][128]` (`Code/Basedef.cpp:4637-4709`). The file itself is not in the repository, so the actual text, and whether the three used indices are present, cannot be verified statically. If an entry is missing, `sprintf` receives an empty string and the announcement degrades to an empty notice (no crash), while `_SS_BASEWORKILLTOWER` would lose both parameters.

Because the loading routine terminates each string at 128 characters and `sprintf` writes into the 4096-byte global `temp`, the component's formatting is safe for the two-string claim notice as long as the table entries respect the 128-character limit enforced by the loader.

**Rule workflow**:
```
startup -> BASE_InitializeMessage("Language.txt") -> g_pMessageStringTable[1..2000]
war     -> format index 501 (1 int), 425 (none), 536 (2 strings)
```

---

## 4. Component Structure

```
Code/TMSrv/
├── CWarTower.h                  # 37 lines - class declaration, 4 static methods, includes only
│                                #   <stdio.h> and ../Basedef.h (tm comes from Basedef's <time.h>)
│                                #   line 28: CWarTower();  declared, never defined, never instantiated
└── CWarTower.cpp                # 156 lines - full implementation
    ├── includes (lines 19-40)   # Windows, stdio, fcntl, time, math, io, errno,
    │                            # Basedef, CPSock, ItemEffect, Language, CItem, Server,
    │                            # GetFunc, SendFunc, ProcessClientMessage, ProcessDBMessage, CReadFiles
    ├── GuildProcess    (42-108) # 67 lines - 3-phase state machine, end-of-war fame sweep
    ├── MobKilled       (110-136)# 27 lines - claim + announce + evict + respawn
    ├── GGenerateMob    (138-145)#  8 lines - owner stamping hook
    └── TowerAttack     (147-156)# 10 lines - attack permission predicate

Functional boundary (files the module cannot work without):
Code/TMSrv/Server.cpp            # GenerateMob 2995, arena spawn guard 3060, GGenerateMob call 3147,
                                 # globals 310/346-348, ClearArea 5754, GuildProcess 6203-6652,
                                 # GetEmptyNPCMob 6765, DeleteMob 7020, DoRecall 7558, timers 3631-3632
Code/TMSrv/MobKilled.cpp         # call site 2027, clan gate 636-639, AtWar 2156, Nyerds guard 65
Code/TMSrv/_MSG_Attack.cpp       # enforcement 398-403
Code/TMSrv/ProcessSecMinTimer.cpp# minute tick 2019-2314, newbie-server election 2036-2057
Code/TMSrv/SendFunc.cpp          # SendNotice 47-62, SendGuildNotice 167-177, SendPKInfo 1737-1767
Code/TMSrv/Language.h            # indices 424, 425, 501, 536
Code/Basedef.h                   # GTORRE 386, MAX_GUILD 147, MAX_USER 56, MAX_MOB 107,
                                 # STRUCT_GUILDINFO 796-806, MSG_GuildInfo 1325-1333
Code/Basedef.cpp                 # BASE_GetGuildName 1317-1330, BASE_InitializeMessage 4637-4709
Code/DBSrv/CFileDB.cpp           # _MSG_GuildInfo handler 454-481
Code/DBSrv/Server.cpp            # weekly fame reset 2061-2083
```

External, untracked data required at runtime:

```
Language.txt                     # message table (indices used: 501, 425, 536)
NPC generator table (entry 1078) # tower HP/level/stats/sprite/segment; must use Equip[0].sIndex 219
gameconfig.txt (line 13)         # GTorreHour and seven unrelated engine knobs
MobName.txt                      # guild/mob name cache loaded by BASE_InitializeMobname
```

Repository evidence that no test assets exist:

```
Code/release/TMSrv/run/          # contains only TMSrv.exe and TMSrv.pdb
W2PP Code Project.sln            # exactly two projects: DBSrv, TMSrv
```

---

## 5. Dependency Analysis

### Internal Dependencies

```
                     +-------------------------------+
   Win32 WM_TIMER -->| ProcessMinTimer (12 s)        |
                     | ProcessSecMinTimer.cpp:2019   |
                     +---------------+---------------+
                                     |
                                     v
                     +-------------------------------+        +-----------------------+
                     | GuildProcess() [global]       |        | GenerateMob()         |
                     | Server.cpp:6203-6652          |        | Server.cpp:2995       |
                     +---------------+---------------+        +-----------+-----------+
                                     |                                |
                                     v                                v
                          CWarTower::GuildProcess              CWarTower::GGenerateMob
                                     |                                |
        +----------------+-----------+------------+                   |
        |                |                        |                   |
        v                v                        v                   v
  GenerateMob      ClearArea -> DoRecall     SendNotice        pMob[tmob].MOB.Guild
  DeleteMob        (recall/revive)           (SendFunc:47)     = GTorreGuild
  BASE_GetGuildName
  GuildInfo[] + DBServerSocket.SendOneMessage
        |
        v
  DBSrv::_MSG_GuildInfo -> GuildInfo[] -> CReadFiles::WriteGuildInfo

  Client MSG_Attack -> Exec_MSG_Attack -> CWarTower::TowerAttack -> (damage nulled | continue)

  MobKilled(target,conn,...) -> DeleteMob(target,1) -> CWarTower::MobKilled
                                                        -> claim + ClearArea + GenerateMob
```

Dependency edges (source → target, with call-site evidence):

| From | To | Kind | Evidence |
|------|----|------|----------|
| `CWarTower::GuildProcess` | `SendNotice` | outbound projection | `Code/TMSrv/CWarTower.cpp:49,60` |
| `CWarTower::GuildProcess` | `ClearArea` | world mutation | `:56,67` |
| `CWarTower::GuildProcess` | `GenerateMob` | world mutation | `:58` |
| `CWarTower::GuildProcess` | `DeleteMob` | world mutation | `:100` |
| `CWarTower::GuildProcess` | `BASE_GetGuildName` | lookup | `:81` |
| `CWarTower::GuildProcess` | `GuildInfo[]` | shared mutable state | `:90,92,96` |
| `CWarTower::GuildProcess` | `DBServerSocket.SendOneMessage` | IPC | `:94` |
| `CWarTower::GuildProcess` | `Log` | diagnostics | `:97` |
| `CWarTower::MobKilled` | `BASE_GetGuildName`, `sprintf`, `SendNotice`, `ClearArea`, `GenerateMob` | mixed | `:124-134` |
| `CWarTower::GGenerateMob` | `pMob[tmob].MOB.Guild/GuildLevel` | shared mutable state | `:142-143` |
| `CWarTower::TowerAttack` | `pMob[conn/idx].MOB.Guild`, `GenerateIndex` | shared mutable state | `:149-152` |
| `Server.cpp:6651` | `CWarTower::GuildProcess` | scheduled entry | `Code/TMSrv/Server.cpp:6651` |
| `Server.cpp:3147` | `CWarTower::GGenerateMob` | generation hook | `Code/TMSrv/Server.cpp:3147` |
| `MobKilled.cpp:2027` | `CWarTower::MobKilled` | kill hook | `Code/TMSrv/MobKilled.cpp:2027` |
| `_MSG_Attack.cpp:398` | `CWarTower::TowerAttack` | arbitration | `Code/TMSrv/_MSG_Attack.cpp:398` |

### External Dependencies

| Dependency | Type | Purpose | Notes |
|-----------|------|---------|-------|
| `Language.txt` | data file | All player-visible text | Loaded by `BASE_InitializeMessage`; not in repository |
| NPC generator table | data file | Tower template (HP, level, stats, segments, `Equip[0]`) | Not in repository; entry must be 1078 and use `Equip[0].sIndex 219` |
| `gameconfig.txt` | config file | `GTorreHour` | Line 13, positional parse, process aborts config load on mismatch |
| DBSrv process | internal service | Guild fame persistence and group-wide rebroadcast | Custom binary protocol, message id 19 (`_MSG_GuildInfo`) |
| Win32 timer | OS facility | 12,000 ms minute tick that drives the whole event | `Code/TMSrv/Server.cpp:3632` |
| Client protocol | network | Notices (`_MSG_MessagePanel`), PK info (`_MSG_PKInfo`), mob lifecycle (`_MSG_CreateMob`/`_MSG_RemoveMob`) | Emitted indirectly through `SendNotice`, `SendPKInfo`, `DeleteMob`, `GenerateMob` |

No third-party library used by this module other than the MSVC C runtime (`sprintf`, `memset`, `strncpy`) and Win32 (`SetTimer`, `MessageBox` in the config loader).

---

## 6. Afferent and Efferent Coupling

Measured at the C++ class level (`CWarTower` = the component), with per-method detail. "Afferent" counts distinct compilation units that depend on the element; "efferent" counts distinct external symbols/globals the element depends on.

| Component | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Instability Ce/(Ca+Ce) | Critical |
|-----------|------------------------|------------------------|------------------------|----------|
| `CWarTower` (class overall) | 7 semantic dependents (4 call sites + 2 header-only consumers + 1 build entry); 63+ TUs including the header transitively via `ProcessClientMessage.h:43` | 11 (SendNotice, Log, BASE_GetGuildName, ClearArea, GenerateMob, DeleteMob, DBServerSocket, pMob[], GuildInfo[], g_pMessageStringTable[], war globals + mNPCGen via GenerateMob) | 0.61 | High |
| `CWarTower::GuildProcess` | 2 (`Server.cpp:6651` scheduler; enumerated in `ProcessSecMinTimer` include chain) | 8 | 0.80 | Critical |
| `CWarTower::MobKilled` | 1 (`MobKilled.cpp:2027`) | 6 | 0.86 | High |
| `CWarTower::GGenerateMob` | 1 (`Server.cpp:3147`, on every NPC generation) | 1 (`pMob[]`) | 0.50 | High (hot path) |
| `CWarTower::TowerAttack` | 1 (`_MSG_Attack.cpp:398`, per damage entry) | 1 (`pMob[]`) | 0.50 | Medium-High (hot path) |

Afferent detail — who depends on the component:

| Dependent | File:line | Nature |
|-----------|-----------|--------|
| Global `GuildProcess()` scheduler | `Code/TMSrv/Server.cpp:6651` | Only scheduled entry point; called from `ProcessMinTimer` at `Code/TMSrv/ProcessSecMinTimer.cpp:2313` every ~12 s |
| `GenerateMob()` | `Code/TMSrv/Server.cpp:3147` | Hook invoked for every NPC spawn in the world |
| `MobKilled()` | `Code/TMSrv/MobKilled.cpp:2027` | Hook invoked for every PvE kill that passes the clan gate |
| `Exec_MSG_Attack` | `Code/TMSrv/_MSG_Attack.cpp:398` | Arbitration invoked per damage entry |
| `ProcessClientMessage.h` | `Code/TMSrv/ProcessClientMessage.h:43` | Transitive include for 60 handler TUs |
| `Server.cpp` | `Code/TMSrv/Server.cpp:39` | Header include |
| `ProcessSecMinTimer.cpp` | `Code/TMSrv/ProcessSecMinTimer.cpp:38` | Header include |
| `TMSrv.vcxproj` | `Code/TMSrv/TMSrv.vcxproj:99`, `:177` | Build dependency; also a dedicated filter `CWarTower` (`:64`, `:291`, `:344`) |

Coupling observations:

* The class is **stateless but not pure**: it holds no members, yet every method reads and writes shared globals. This is the classic "static utility over a global context" shape; cohesion within each method is high, cohesion of the class as a whole is low (the four methods share only the `GTORRE`/`GTorreGuild`/`GTorreState` triple).
* `GuildProcess` is the single point where mutation, notification and persistence are interleaved; its efferent set (8) covers three subsystems (world/entity, presentation, DB) in one 67-line function, so its instability is maximal.
* `TowerAttack` and `GGenerateMob` are extremely high-frequency (every attack entry, every NPC spawn) and deliberately minimal (one or two integer comparisons), which is the right shape for hot-path predicates, at the cost of hiding domain logic in two-line methods.
* The **efferent edge to `mNPCGen` is indirect** (through `GenerateMob`), which is why the component appears to have Ce=11 rather than 12; the real data dependency includes the generator table because spawn success depends on BR-08/BR-09.

---

## 7. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| DBSrv (`DBServerSocket`) | Internal service | Publish guild fame award; DBSrv persists and rebroadcasts to all game servers | Custom binary TCP (message id 19 `_MSG_GuildInfo`) | Packed `MSG_GuildInfo` {header, Guild:int, STRUCT_GUILDINFO {short Fame, char Clan, Citizen, Sub1..3}} | None in this module: fire-and-forget `SendOneMessage`, no ack, no retry, no return check; DB side validates `(0,65536)` |
| NPC generator (`mNPCGen.pList`) | In-process data table | Tower spawn template and population accounting | Function call (`GenerateMob`) | `STRUCT_NPCGENERATOR` rows loaded from external file | Silent failure: `GenerateMob` returns `void`; caller sets state 2 regardless |
| `Language.txt` | Data file | Player-visible text (indices 501, 425, 536) | File I/O at startup | `index<TAB>text` lines, text ≤127 chars | Loader skips malformed lines with a modal `MessageBox`; missing file leaves the table empty (empty notices) |
| `gameconfig.txt` | Config file | `GTorreHour` and 7 unrelated knobs | File I/O at startup | Positional tokens on line 13 | Strict token/value check, aborts the whole file read and shows a modal error |
| Win32 timer (`TIMER_MIN`) | OS | Drives the whole schedule | `WM_TIMER` at 12,000 ms | n/a | No catch-up logic; a missed tick shifts or loses a phase transition |
| World entity model (`pMob[]`, `pMobGrid`) | In-process shared state | Tower ownership, attack arbitration, eviction, respawn | Direct memory access | `STRUCT_MOB`, `STRUCT_MOB_EXTRA` | No bounds checks in this module; relies on caller-validated indices |
| Guild registry (`GuildInfo[65536]`, `g_pGuildName`) | In-process shared state | Fame accumulation, guild name resolution | Direct memory access | `STRUCT_GUILDINFO`, `char[10][16][4096][12]` | Not initialised by the module; `BASE_GetGuildName` leaves the output buffer untouched on failure |
| Client sockets | Network (indirect) | Notices, PK flag, mob lifecycle | `_MSG_MessagePanel`, `_MSG_PKInfo`, `_MSG_CreateMob`, `_MSG_RemoveMob` | Fixed-size binary packets | `SendNotice` filters `USER_PLAY`; `SendClientMessage` validates `conn` range |
| BR guardian generation | In-process coupling | Guild inheritance suppression while at war | Function-level condition | n/a | None |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Static utility class (namespace idiom in C++) | `class CWarTower` with four `static` methods and no data members; constructor declared and never defined | `Code/TMSrv/CWarTower.h:25-37` | Namespacing the tower-war rules without instantiation; no object lifecycle |
| Implicit state machine | `GTorreState` ∈ {0 idle, 1 countdown, 2 running} with three guarded transitions | `Code/TMSrv/CWarTower.cpp:44-107` | Models the three phases of the event; state is an `int`, not an enum |
| Event hook / callback registration (manual) | Fixed call sites inserted into generic subsystems: `Server.cpp:3147` (spawn), `MobKilled.cpp:2027` (death), `_MSG_Attack.cpp:398` (attack) | see above | Disperses domain logic into the host subsystems instead of an observer/registry |
| Guard clause / early-return predicate | `TowerAttack` returns `TRUE` immediately for non-tower; `MobKilled` returns when index/guild/state conditions fail | `Code/TMSrv/CWarTower.cpp:147-156`, `:112-114` | Keeps hot-path cost minimal |
| Deny-by-mutation | Rejection is implemented by zeroing the damage entry rather than refusing the message | `Code/TMSrv/_MSG_Attack.cpp:398-403` | Fits the existing attack pipeline; avoids a new error protocol |
| Global state / shared registry | `GTorreState`, `GTorreHour`, `GTorreGuild`, `GuildInfo[]`, `pMob[]`, `g_pMessageStringTable[]` | `Code/TMSrv/Server.cpp:346-348`, `:361` | The only persistence-like mechanism available in this codebase |
| Data-driven template instantiation | Tower statistics/ownership applied through the generic `GenerateMob` → template → instance copy → hook | `Code/TMSrv/Server.cpp:3069-3163` | Avoids a bespoke tower constructor; ties the event to generator data |
| Two-tier persistence with write-through broadcast | TMSrv holds `GuildInfo[]`, sends `_MSG_GuildInfo`; DBSrv stores, broadcasts, and calls `WriteGuildInfo` | `Code/TMSrv/CWarTower.cpp:83-94`, `Code/DBSrv/CFileDB.cpp:454-481` | Keeps fame consistent across the server group |
| Hard-coded magic geometry | Three rectangles for "the arena": 2440-2545/1845-1921 (spawn guard), 2445-2546/1850-1920 (eviction), 2430-2560/1825-1925 (aggression) | `Code/TMSrv/Server.cpp:3060`, `Code/TMSrv/CWarTower.cpp:56,67,132`, `Code/TMSrv/_MSG_PKMode.cpp:51` | No constant/name; each usage repeats literals |
| Copy-paste divergence | Four near-identical PK-flag expressions across handlers; two near-identical fame-award blocks (tower and Kefra) | `Code/TMSrv/SendFunc.cpp:1758`, `Code/TMSrv/_MSG_PKMode.cpp:51`, `Code/TMSrv/_MSG_QuitTrade.cpp:45`, `Code/TMSrv/_MSG_MessageWhisper.cpp:549`, `Code/TMSrv/MobKilled.cpp:650-682` | Code-generation artefact; the tower version only adds the `GTorreState` rectangle |

Architectural decisions observable in the code (descriptive, not prescriptive):

1. **Event state belongs to the process, not to a world object.** There is exactly one tower war at a time, one owner, one schedule, so a single global triple suffices. Multi-arena or per-guild siege support would require a data structure that does not exist.
2. **The tower is an ordinary mob with a guild tag.** All consequences (damage pipeline, death pipeline, name plate, HP bar) are inherited from the generic NPC system; the module contributes only the guild tag and the respawn/claim side effects. This gives correct client behaviour for free and is the reason the module is 156 lines.
3. **The event is time-triggered, not state-recovered.** Nothing is recomputed from persistence at boot; `GTorreState` and `GTorreGuild` start at 0. Recovery after a restart is "the war simply does not happen".
4. **Player messaging is synchronous and global.** Every transition uses `SendNotice` (all playing clients, no localisation per player, no per-guild channel except the unused `SendGuildNotice` at `Code/TMSrv/SendFunc.cpp:167`).
5. **Reward flows through the guild-fame channel that already existed** (Kefra boss uses the same `GuildInfo` + `_MSG_GuildInfo` path, `Code/TMSrv/MobKilled.cpp:650-682`), so no new persistence format was introduced.

---

## 9. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | End-of-war sweep (`Code/TMSrv/CWarTower.cpp:69-102`) | Filter checks only `GenerateIndex == GTORRE`, not `Mode`/`Hp`; `DeleteMob` intentionally preserves `GenerateIndex` (`Code/TMSrv/Server.cpp:7047-7049`) | Every toss of the arena that occupied a distinct mob slot re-awards +100 fame at minute 59; reward can be multiplied and credited to a previous owner |
| High | Ownership state (`Code/TMSrv/CWarTower.cpp:129`, `Code/TMSrv/Server.cpp:346-348`) | `GTorreGuild` is a single in-memory `int`; no persistence, no DB packet, no boot recovery | Restart during a war loses the owner and the tower; restart also loses `GTorreState`, leaving no trace of an interrupted event |
| High | State machine (`Code/TMSrv/CWarTower.cpp:44-107`) | End phase keyed on `tm_min == 59` only; no catch-up, no timeout, no reset on boot | A missed tick latches state 2; the next day's countdown and spawn are suppressed and the stale tower is rewarded again before deletion |
| High | Kill hook reachability (`Code/TMSrv/MobKilled.cpp:636-639`, `:2027`) | `CWarTower::MobKilled` is inside the clan-filter `else` branch, so killers with `Clan` 1,2,3,5,6 never reach it | Those players can destroy the tower without claiming it, with no announcement and no owner change — silent behavioural inconsistency |
| High | Reward/state coupling (`Code/TMSrv/CWarTower.cpp:64` vs `:90`) | Comment documents 50 fame; code awards 100 | Design intent is ambiguous; balancing and any external documentation cannot be trusted |
| Medium | Spawn observability (`Code/TMSrv/CWarTower.cpp:58`) | `GenerateMob` returns `void`; state is set to 2 unconditionally | A failed tower spawn (population cap, arena guard, no free slot) produces a broadcast "war started" with no tower and no log entry |
| Medium | Arena geometry (`Code/TMSrv/CWarTower.cpp:56,67,132`, `Code/TMSrv/Server.cpp:3060`, `Code/TMSrv/_MSG_PKMode.cpp:51`) | Three different hard-coded rectangles for the same arena | Players flagged aggressive but not evicted, or blocked from spawning but not flagged; any map change requires editing five files |
| Medium | Name buffer usage (`Code/TMSrv/CWarTower.cpp:79`, `:122`) | `char guildname[256]` uninitialised; `BASE_GetGuildName` leaves it untouched when the name is unknown | Uninitialised stack read in `sprintf` for the claim and reward notices; garbled names, no crash |
| Medium | Global PvP exemptions (`Code/TMSrv/MobKilled.cpp:65`, `:2156`) | `GTorreState != 0` disables death-exp loss and Nyerds consumption map-wide, with no area and no duration bound | A latched state removes the death penalty for everyone indefinitely; economy/balance impact |
| Medium | Generator counter (`Code/TMSrv/Server.cpp:7034-7044`, `:3206-3209`) | `CurrentNumMob` decremented by every `DeleteMob` on a `GenerateIndex`-tower slot, including stale ones, and clamped at 0 | Counter drift can allow a second live tower or block respawn; masked by clamping rather than detected |
| Medium | Hard-coded schedule granularity (`Code/TMSrv/Server.cpp:3632`) | Event driven by a 12,000 ms Win32 timer, while phase checks compare wall-clock minutes exactly | Phase transitions depend on the message loop not being blocked; a stall inside the 59th minute loses the reward |
| Medium | Message-table dependency (`Code/TMSrv/CWarTower.cpp:48,60,126`) | Text comes from untracked `Language.txt`; `_DN_BASEWORRESULT` (424) is declared but unused | Cannot verify the visible strings; a missing index yields empty notices; dead message slot suggests removed functionality |
| Low | Class declaration (`Code/TMSrv/CWarTower.h:28`) | `CWarTower();` declared in the header, never defined, never instantiated | Latent link error if anyone ever instantiates the class; misleading API |
| Low | Narrowing and unused locals (`Code/TMSrv/CWarTower.cpp:76`, `:119`) | `int Server = guild / MAX_GUILD;` computed and never used (twice) | Dead code / compiler warnings; suggests an unfinished per-server routing intent |
| Low | Unused hook parameters (`Code/TMSrv/CWarTower.cpp:110`, `:138`) | `PosX`, `PosY` (and `PosX/PosY` in `GGenerateMob`) unused | Signature implies positional control that does not exist |
| Low | No input validation (`Code/TMSrv/CWarTower.cpp:147-156`) | `conn`/`idx` not range-checked in `TowerAttack` (relies on `_MSG_Attack` validation) | Any future caller must re-validate; the predicate is unsafe in isolation |
| Low | Logging asymmetry (`Code/TMSrv/CWarTower.cpp:96`, `:97`, `:126-127`) | Claim and reward are logged; spawn, state transitions, denied attacks and failed respawns are not | Post-hoc diagnosis of an event relies on inference from other subsystems' logs |
| Low | Duplicate implementation | Tower fame block (`CWarTower.cpp:69-101`) duplicates the Kefra fame block (`MobKilled.cpp:650-682`) with divergent details (`name` buffer init, `SendNotice` usage, log text) | Fixes must be applied twice; the two behaviours already differ |

Security assessment (defensive properties found, no exploit claims):

* The attack predicate operates on server-side guild data only; a client cannot forge the attacker's guild in the damage packet, so tower-damage permission cannot be spoofed through `MSG_Attack`.
* `TowerAttack` is evaluated after the anti-cheat damage-marker validation (`Code/TMSrv/_MSG_Attack.cpp:355-361`), so the early rejection cannot be used to bypass the crack check.
* The two-parameter claim notice formats a player-controlled `MobName` into a 4096-byte global buffer; `MobName` is fixed-width and forcibly terminated on both the generation path and the send path, so overflow requires corrupting the mob structure first.
* `GuildInfo[usGuild]` is indexed by an `unsigned short` read from a server-owned structure, so the 65536-entry array cannot be indexed out of bounds from this component.
* No authentication/authorisation exists on the state machine: any code path that can set `GTorreState` (GM commands, memory corruption) can start or end the event. The `gtorrehour` GM command was observed to assign its argument without bounds checking (`Code/TMSrv/imple.cpp:1757-1764`).
* Denied tower attacks produce no log or counter, so abusive patterns (repeated blocked attempts) are not observable server-side.

---

## 10. Test Coverage Analysis

No automated tests exist for this component, or anywhere in the repository.

Evidence gathered:

| Check | Command / file inspected | Result |
|-------|--------------------------|--------|
| Test file names | recursive search of `Code/**` for `*test*`, `*Test*`, `*TEST*` | No matches |
| Test frameworks | search of all `*.cpp` for `gtest`, `CppUnit`, `TEST_CASE`, `TEST(`, `BOOST_TEST`, `UnitTest` | No matches |
| Assertions | search of all `*.cpp` for `assert(` | No matches |
| Test projects | `W2PP Code Project.sln` project list | Only `DBSrv` and `TMSrv` |
| Test fixtures / data | recursive search for `*.txt`, `*.csv`, `*.ini`, `*.json` outside ignored folders | Only `Code/DBSrv/Release/DBSrv.vcxproj.FileListAbsolute.txt`, `Code/TMSrv/Release/TMSrv.vcxproj.FileListAbsolute.txt`, `license.txt` |
| Runnable artifacts | `Code/release/TMSrv/run/` | `TMSrv.exe`, `TMSrv.pdb` only (no data files, no harness) |
| In-code hooks for testing | search for test-only branches inside the component | Only `TESTSERVER` / `LOCALSERVER` globals used for global engine behaviour, not for the tower event |

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|-----------|-------------------|----------|--------------|
| `CWarTower` (class) | 0 | 0 | 0% (no executable verification exists) | No harness; no mocking of `pMob[]`, `GenerateMob`, `SendNotice`, `DBServerSocket`, `GuildInfo[]` |
| `CWarTower::GuildProcess` | 0 | 0 | 0% | Not testable in isolation: reads `localtime`, mutates world slots, sends global notices, writes a global fame array, and requires external NPC/message data |
| `CWarTower::MobKilled` | 0 | 0 | 0% | Depends on `DeleteMob` side effects (`GenerateIndex` survival) and on the caller's clan gate; no fixture exercises the post-delete read |
| `CWarTower::GGenerateMob` | 0 | 0 | 0% | Only reachable through `GenerateMob`, which requires the generator table and a free mob slot |
| `CWarTower::TowerAttack` | 0 | 0 | 0% (pure predicate, easiest candidate) | No test; three branches, currently only exercised in-game through the attack pipeline |
| BR-01..BR-28 | 0 | 0 | 0% | No rule-level verification of any kind; timing rules (BR-02..BR-06) were never simulated |

Behavioural evidence available instead of tests:

* Server-side log lines are the only runtime assertions: `etc,war_tower1 guild:%d guild_fame:%d` (`Code/TMSrv/CWarTower.cpp:96`) for each reward, and the `not <text>` prefix emitted by `SendNotice` for each announcement (`Code/TMSrv/SendFunc.cpp:51-52`).
* DBSrv persists the guild file on each `_MSG_GuildInfo` (`Code/DBSrv/CFileDB.cpp:480`), so fame changes can be inspected through the persisted guild data.
* There is no way to observe a denied tower attack (no log), no way to observe a failed respawn, and no way to observe a state transition other than the player-visible notices.

Untested rules with the highest latent-impact (list only, no remediation):

1. BR-06 / BR-19 duplicate reward on multi-capture wars.
2. BR-05 / BR-09 silent spawn failure.
3. BR-15 clan-gated claim reachability.
4. BR-22 / BR-23 map-wide PvP exemptions and their duration.
5. BR-24 rectangle coverage gaps between the aggression, eviction and spawn-guard geometries.
6. BR-06 latched state 2 across days.

---

## 11. Ambiguities and Confidence Levels

| Item | Statement | Confidence | Basis |
|------|-----------|------------|-------|
| Tower statistics (HP, level, defence, resistance, sprite) | Not determinable from the repository; loaded from the NPC generator entry 1078 in an untracked data file | High confidence that it is data-driven; unknown values | `Code/TMSrv/Server.cpp:3069-3163` copies template data; no data files tracked |
| Exact notice text | Unknown; indices 501, 425, 536 map to `Language.txt`, which is not in the repository | High confidence in the indirection; unknown text | `Code/Basedef.cpp:4637-4709`, `Code/TMSrv/Language.h:443-549` |
| Duplicate fame award | Multiple tower spawns in distinct slots each pay +100 at minute 59 | High (control flow + `DeleteMob` semantics), not executed | `Code/TMSrv/CWarTower.cpp:69-102`, `Code/TMSrv/Server.cpp:6765-6791`, `:7020-7054` |
| Clan-gated claim hook | Only killers with `Clan` 0, 4, 7, 8 reach `CWarTower::MobKilled` | High (control flow), not executed | `Code/TMSrv/MobKilled.cpp:207-211`, `:636-639`, `:2027` |
| Reward intent (50 vs 100) | Implementation is 100; comment says 50 | High for implementation, ambiguous for intent | `Code/TMSrv/CWarTower.cpp:64`, `:90` |
| `_DN_BASEWORRESULT` purpose | Declared index 424, never referenced | High (no references found); original purpose unknown | `Code/TMSrv/Language.h:443` |
| `Equip[0].sIndex == 219` requirement for the tower template | Inferred from the arena spawn guard and the guardian/BR block | Medium-high (inference from two independent uses of 219) | `Code/TMSrv/Server.cpp:3060-3064`, `:3124-3145` |
| Whether `GuildProcess()` (global) is called once per minute or every 12 s | `TIMER_MIN` is created with a 12,000 ms interval and can be reset by a GM command | High | `Code/TMSrv/Server.cpp:3631-3632`, `Code/TMSrv/imple.cpp:578` |
| Multi-server behaviour | Only the elected "newbie" server runs the event; other servers in the group never set `GTorreState` | High | `Code/TMSrv/CWarTower.cpp:44`, `Code/TMSrv/ProcessSecMinTimer.cpp:2036-2057` |
| Persistence of tower ownership | None found in TMSrv or DBSrv | High (no packet, no file write, no global load at boot) | `Code/TMSrv/CWarTower.cpp:83-94`, `Code/DBSrv/CFileDB.cpp:454-481` |

---

## 12. Summary Table of Evidence Locations

| Concern | Relative path:line |
|---------|--------------------|
| Class contract | `Code/TMSrv/CWarTower.h:25-37` |
| Countdown / spawn / end phases | `Code/TMSrv/CWarTower.cpp:44-107` |
| Fame award (+100) and DB packet | `Code/TMSrv/CWarTower.cpp:90-94` |
| Comment claiming 50 fame | `Code/TMSrv/CWarTower.cpp:64` |
| Claim notice + owner assignment | `Code/TMSrv/CWarTower.cpp:122-130` |
| Eviction + respawn on kill | `Code/TMSrv/CWarTower.cpp:132-134` |
| Owner stamping | `Code/TMSrv/CWarTower.cpp:138-145` |
| Attack predicate | `Code/TMSrv/CWarTower.cpp:147-156` |
| Damage nulling on rejection | `Code/TMSrv/_MSG_Attack.cpp:398-403` |
| Kill hook call site | `Code/TMSrv/MobKilled.cpp:2027` |
| Clan gate around the hook | `Code/TMSrv/MobKilled.cpp:636-639` |
| Death-exp exemption | `Code/TMSrv/MobKilled.cpp:2150-2157` |
| Nyerds suspension | `Code/TMSrv/MobKilled.cpp:65-104` |
| Minute scheduler | `Code/TMSrv/ProcessSecMinTimer.cpp:2019-2314` |
| Tower event scheduling call | `Code/TMSrv/Server.cpp:6651` |
| Scheduler entry chain | `Code/TMSrv/Server.cpp:6203`, `Code/TMSrv/ProcessSecMinTimer.cpp:2313` |
| Generation hook | `Code/TMSrv/Server.cpp:3147` |
| Arena spawn guard | `Code/TMSrv/Server.cpp:3060-3064` |
| Population cap | `Code/TMSrv/Server.cpp:3030-3048` |
| BR guardian inheritance guard | `Code/TMSrv/Server.cpp:3124-3145` |
| `ClearArea` / `DoRecall` | `Code/TMSrv/Server.cpp:5754-5775`, `:7558-7597` |
| `DeleteMob` (keeps `GenerateIndex`) | `Code/TMSrv/Server.cpp:7020-7054` |
| `GetEmptyNPCMob` | `Code/TMSrv/Server.cpp:6765-6791` |
| Timer intervals | `Code/TMSrv/Server.cpp:3631-3632` |
| Global event state | `Code/TMSrv/Server.cpp:310`, `:346-348`, `:353` |
| Guild fame array (TMSrv) | `Code/TMSrv/Server.cpp:361` |
| Guild fame handler (DBSrv) | `Code/DBSrv/CFileDB.cpp:454-481` |
| Weekly fame reset | `Code/DBSrv/Server.cpp:2061-2083` |
| `GTORRE` constant | `Code/Basedef.h:386` |
| `STRUCT_GUILDINFO` (Fame is `short`) | `Code/Basedef.h:796-806` |
| `MSG_GuildInfo` | `Code/Basedef.h:1325-1333` |
| Guild id encoding / name lookup | `Code/Basedef.cpp:1317-1330` |
| Message table loader | `Code/Basedef.cpp:4637-4709` |
| Message indices | `Code/TMSrv/Language.h:424-425`, `:501`, `:536` |
| `GTorreHour` config parse | `Code/TMSrv/Server.cpp:1085-1114` |
| `GTorreHour` GM command | `Code/TMSrv/imple.cpp:1757-1764` |
| PK flag copies | `Code/TMSrv/SendFunc.cpp:1758`, `Code/TMSrv/_MSG_PKMode.cpp:51`, `Code/TMSrv/_MSG_QuitTrade.cpp:45`, `Code/TMSrv/_MSG_MessageWhisper.cpp:549` |
| Build wiring | `Code/TMSrv/TMSrv.vcxproj:99`, `:177`, `:64`, `:291`, `:344` |
| Solution projects | `W2PP Code Project.sln` (DBSrv, TMSrv only) |
