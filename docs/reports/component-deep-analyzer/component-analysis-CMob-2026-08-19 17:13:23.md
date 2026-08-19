# Component Deep Analysis Report

**Component:** CMob
**Project scope:** legacy (W2PP C/C++ codebase at `/home/luisdias/dev/github/luisdiasdev/w2pp/legacy`)
**Analysis date:** 2026-08-19
**Files analyzed:**
- `legacy/Code/TMSrv/CMob.h` (123 lines)
- `legacy/Code/TMSrv/CMob.cpp` (1279 lines)
- Supporting: `legacy/Code/Basedef.h`, `legacy/Code/ItemEffect.h`, `legacy/Code/TMSrv/Server.h`, `legacy/Code/TMSrv/CUser.h`, `legacy/Code/TMSrv/ProcessSecMinTimer.cpp`, `legacy/Code/TMSrv/Server.cpp`, `legacy/Code/TMSrv/SendFunc.cpp`, `legacy/Code/TMSrv/GetFunc.cpp`

---

## 1. Executive Summary

CMob is the core in-world entity and artificial-intelligence component of the W2PP game server (TMSrv executable). It models every entity that occupies a slot in the world's mob array `pMob[MAX_MOB]` — a global array of 25,000 `CMob` instances shared by the server. Slots `0..MAX_USER-1` (0..999) represent connected player characters, while slots `MAX_USER..MAX_MOB-1` (1000..24999) represent non-player creatures (mobs) and NPCs. Because both players and mobs are stored in the same structure, the CMob class implements not only monster AI but also the movement, stat, and leveling logic that drives player characters and pets/summons alike.

The component is the AI and simulation heart of the server's world simulation. It provides the state-machine processors (`StandingByProcessor`, `BattleProcessor`) that are driven by the server's second timer (`ProcessSecMinTimer`), the enemy targeting subsystem, the patrol-route (segment) traversal engine, movement/pathfinding helpers, derived-stat computation, and the experience-driven level-up system. It has no direct I/O of its own; it operates entirely over shared global state and delegates all network broadcast, persistence, and combat resolution to the surrounding server modules.

Key findings:

- **Dual role:** The same class is used for players and mobs. Many behaviors branch on the slot index (`idx < MAX_USER`) or on `RouteType`/`Leader` to distinguish player, free mob, patrol mob, and summoned minion.
- **AI is state-machine + bitmask-return based:** Processors return integer bitmasks that the caller (`ProcessSecMinTimer`) interprets to trigger actions (move, teleport, attack, delete).
- **No test files exist anywhere in the project.** Test coverage is zero; the component is validated only at runtime.
- **Significant hardcoded magic values** (face ranges 315..345, fairy item indexes 3900..3913, `KEFRA_BOSS = 396`, affect type 24, `dis = 6/8/12`, etc.) tightly couple behavior to specific game content, making it brittle to content changes.
- **A dead-code / unreachable path:** `BattleProcessor` returns `0x010000` (65536) on the INT-based skill decision, but the caller's combat loop masks only bits `0x20`, `0x1000`, `0x100`, `0x1`, `0x2`, `0x10` — `0x010000` is never handled, so the "cast skill" decision has no observable effect in the combat loop.
- **`ProcessorSecTimer()` is an empty stub** (the increment is commented out), indicating disabled/incomplete per-second processing.
- **Global shared mutable state:** The class reads and writes `pMob[]`, `pUser[]`, `pMobGrid`, `pHeightGrid`, `g_pClanTable`, `g_pItemList`, `mNPCGen`, `g_pNextLevel` directly — tight coupling to the whole server, no encapsulation.

---

## 2. Data Flow Analysis

The CMob component is driven by the server's periodic timer. The primary flow for a mob:

```
1. ProcessSecMinTimer (every second, ProcessSecMinTimer.cpp)
2. Pre-checks: Mode != MOB_EMPTY, HP > 0, ProcessAffect(), RunEQuest cleanup
3. Mode dispatch:
   - MOB_IDLE / MOB_PEACE  -> CMob::StandingByProcessor()  (CMob.cpp:56)
   - MOB_COMBAT            -> CMob::BattleProcessor()      (CMob.cpp:209)
4. Processor returns integer bitmask -> caller interprets:
   - bit 0x1            -> move: GetAction + GridMulticast
   - bit 0x2            -> teleport: DoTeleport
   - bit 0x10           -> flee action: GetAction + GridMulticast
   - bit 0x100          -> chase target: GetTargetPosDistance
   - bit 0x1000         -> attack: GetAttack + damage resolution
   - bit 0x20           -> delete mob (summon/follower cleanup)
   - bit 0x10000000     -> spotted enemy: SetBattle (transition to combat)
   - bit 0x10000        -> route-end delete
5. Movement helpers compute NextX/NextY via BASE_GetRoute + GetEmptyMobGrid
6. State writes back to pMob[index] (TargetX/Y, NextX/Y, Mode, SegmentProgress...)
7. Network broadcast via GridMulticast (external to CMob)
```

Stat recomputation flow (players & mobs):

```
1. Triggered by level up, item change, stat apply, login, etc.
2. CMob::GetCurrentScore(idx)   (CMob.cpp:598)
3. Recompute Resist[0..3] (capped at 100), Rsv cleared
4. Recompute derived bonuses: ExpBonus, DropBonus, ForceDamage, ReflectDamage,
   ForceMobDamage, Accuracy, HpAbs, WeaponDamage, PvPDamage, ReflectPvP
5. Delegate to BASE_GetCurrentScore (Basedef.cpp) for base score derivation
6. Clamp HP/MP to MaxHp/MaxMp
```

Leveling / EXP flow:

```
1. CMob::CheckGetLevel()   (CMob.cpp:1034) — called after combat & item combine
2. Compute 4 EXP segments between current and next level
3. Gate on ClassMaster (MORTAL/ARCH/CELESTIAL/SCELESTIAL) and quest flags
4. If EXP >= next level -> increment Level, HP/MP, SkillBonus, SpecialBonus,
   call BASE_GetBonusScorePoint, then GetCurrentScore(0)
5. Return segment marker (0..4) consumed by callers (e.g. SendFunc.cpp:925)
```

---

## 3. Business Rules & Logic

## Overview of the business rules:

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Lifecycle | Mob slot modes (MOB_EMPTY..MOB_WAITDB) define entity state | CMob.h:26-35 |
| Lifecycle | Empty slot constructor resets persistent fields | CMob.cpp:28-44 |
| AI / State machine | StandingByProcessor drives idle/patrol mobs | CMob.cpp:56 |
| AI / State machine | BattleProcessor drives combat mobs | CMob.cpp:209 |
| Targeting | Enemy list capped at MAX_ENEMY (13), dedup | CMob.cpp:306-378 |
| Targeting | Enemy selection picks nearest valid enemy | CMob.cpp:381-468 |
| Targeting | Vision-based enemy detection via clan hostility | CMob.cpp:1229 |
| Summoning | RouteType 5 / affect 24 defines follower behavior | CMob.cpp:60-130, 220-253 |
| Patrol | Segment route traversal (RouteType 0-6) | CMob.cpp:470-596 |
| Movement | Speed-based pathfinding with obstacle avoidance | CMob.cpp:857-1032, 1154 |
| Stats | Derived stat recomputation with caps | CMob.cpp:598-855 |
| Leveling | 4-segment EXP progression gated by ClassMaster | CMob.cpp:1034-1152 |

## Detailed breakdown of the business rules:

---

### Business Rule: Mob Slot Lifecycle and Mode States

**Overview:**
Every world entity occupies one slot in the global `pMob[MAX_MOB]` array. The `Mode` field is the master state variable determining which processor runs and how the entity is treated by the server. The mode values are defined in `CMob.h:26-35`.

**Detailed description:**
The mode state machine is the outermost business rule of the component. `MOB_EMPTY` (0) means the slot is free and contains no entity; empty slots are skipped by the timer loops and are eligible for reuse when generating mobs. `MOB_USERDOCK` (1) and `MOB_USER` (2) mark player-connected slots that are docking or logged in respectively. `MOB_IDLE` (3) and `MOB_PEACE` (4) are the non-combat standing states where `StandingByProcessor()` runs; the difference is that `MOB_IDLE` re-registers the mob in the grid (`pMobGrid`) each tick, while `MOB_PEACE` runs the full patrol/route and enemy-detection logic. `MOB_COMBAT` (5) is the aggressive state driven by `BattleProcessor()`. `MOB_RETURN` (6), `MOB_FLEE` (7), and `MOB_ROAM` (8) are additional non-combat movement modes referenced by route logic and merchant/teleport behaviors. `MOB_WAITDB` (9) marks an entity waiting on database operations.

The mode determines both the timer branch executed (idle/peace vs. combat) and cleanup behavior. When a combat mob's HP drops to zero, the caller logs an error and deletes the mob. When the mode is invalid or an entity enters a state it cannot leave, the server forcibly deletes the mob to avoid corrupting the world state. The `Mode` field is set both by `CMob` methods (e.g. `BattleProcessor` returns to `MOB_PEACE` when it loses its target) and externally by `SetBattle` (`Server.cpp:7218`) which transitions a mob to `MOB_COMBAT` upon being attacked.

**Rule workflow:**
1. Slot created with `Mode = MOB_EMPTY` in constructor.
2. `GenerateMob` (Server.cpp) assigns a non-empty mode (typically `MOB_PEACE`).
3. Timer loop filters `if (pMob[index].Mode)` and dispatches on mode value.
4. Attack (SetBattle) sets `Mode = MOB_COMBAT`.
5. `BattleProcessor` resets `Mode = MOB_PEACE` when the enemy list empties.
6. HP <= 0 or invalid state triggers `DeleteMob`.

---

### Business Rule: Enemy Targeting — EnemyList Management and Target Selection

**Overview:**
A mob tracks hostile entities in a fixed-size `EnemyList[MAX_ENEMY]` (13 entries, `CMob.h:24`). Three methods maintain this list: `AddEnemyList`, `RemoveEnemyList`, and `SelectTargetFromEnemyList`. The selected target is stored in `CurrentTarget`.

**Detailed description:**
`AddEnemyList` (CMob.cpp:306) enforces several exclusion rules before admitting a target: the target ID must be positive; if the target is a player (`<= MAX_USER`) it is rejected when flagged hidden (`Rsv & RSV_HIDE`) or when it is a merchant; duplicates are rejected; and the list is dropped when full (13 entries). A special Kefra-boss rule (when `GenerateIndex == KEFRA_BOSS`) additionally broadcasts the target into the enemy lists of every nearby player (`pMob[i]`, within a 30-tile box) in the surrounding area, causing the whole surrounding player group to aggro the same target — an area-aggro mechanic. `RemoveEnemyList` (CMob.cpp:365) simply clears the first matching entry.

`SelectTargetFromEnemyList` (CMob.cpp:381) is the core targeting decision. It first resets `CurrentTarget`, then filters each enemy-list entry for validity: entries whose slot is empty, whose HP is <= 0, which are hidden players (`Rsv & 0x10`), or which are high-level non-merchant entities are purged. The engagement distance `dis` is context-sensitive: base 6, extended to 12 for clans 4/7/8, to 8 inside a specific map region (`TargetX/128 < 12 && TargetY/128 > 25`), and to `HALFGRIDX` (16) for the Kefra boss. Only enemies within the `dis`-square of the mob's position remain candidates. Non-player candidates get a +2 distance penalty, biasing selection toward players. The closest surviving candidate becomes `CurrentTarget`. If no candidate survives, `CurrentTarget = 0` and the caller transitions the mob back to `MOB_PEACE`.

**Rule workflow:**
1. `AddEnemyList` validates and inserts a target into the 13-slot list (or area-aggros for Kefra boss).
2. `SelectTargetFromEnemyList` purges dead/hidden/out-of-range entries.
3. Distance is computed per candidate; non-players penalized by +2.
4. Nearest valid candidate becomes `CurrentTarget`; else 0.
5. `BattleProcessor` with `CurrentTarget == 0` returns to `MOB_PEACE`.

---

### Business Rule: Vision-Based Enemy Detection (GetEnemyFromView)

**Overview:**
Non-combat mobs (`Leader == 0`) periodically scan their surroundings to spot aggressors through `GetEnemyFromView` (CMob.cpp:1229), which triggers a transition to combat via the `0x10000000` return bit.

**Detailed description:**
The function defines a sight box centered on the mob: a default 9x9 tile area (`StartX = TargetX - 4`, `Size = 9`), expanded to `HALFGRIDX`/`HALFGRIDY` (16) with offset 6 for clans 7 and 8. It iterates every cell of that box, reading the occupying entity from `pMobGrid`. It skips out-of-bounds cells, its own cell, dead entities (`HP <= 0`), and empty slots. Hidden players (`Rsv & 0x10`) are ignored. Hostility is resolved through the global clan-relations table `g_pClanTable[MOB.Clan][other.Clan]`: if the two clans are enemies (`== 0`), the function returns the spotted mob's ID, which `StandingByProcessor` encodes into its return value (`enemy | 0x10000000`). The caller then runs `SetBattle` on the mob and its entire party, propagating combat to all party members. Bounds checking logs `"err,clan out or range"` if either clan is outside 0..8.

**Rule workflow:**
1. `StandingByProcessor` (Leader == 0 branch) calls `GetEnemyFromView`.
2. Sight box scanned cell-by-cell (9x9, or 16x16 for clans 7/8).
3. Hostile (per `g_pClanTable`) living, non-hidden entity found.
4. Returns mob ID; caller sees `0x10000000` bit and calls `SetBattle`.
5. Combat propagates to the mob's party and the target's party.

---

### Business Rule: Standing (Idle/Patrol) AI — StandingByProcessor

**Overview:**
`StandingByProcessor` (CMob.cpp:56) is the state machine for non-combat entities. It returns an integer bitmask that the caller translates into movement, teleport, enemy-engagement, or cleanup actions.

**Detailed description:**
The method has two top-level branches. The first (`RouteType == 5 || Affect[0].Type == 24`) handles summoned/follower mobs: it requires a valid `Leader` (a party), a valid `Summoner`, confirms the summoner belongs to the leader's party (`_leader` check via `PartyList`), and verifies the summoner is in `USER_PLAY` mode. The follower then keeps within 13 tiles of the summoner: if farther than 13 tiles it moves toward the summoner (returns `0x02`, teleport); if within 4..13 tiles it targets the summoner's position (returns `0x01`). If the leader/summoner relationship breaks, the follower returns `0x01` (which triggers deletion by the caller in some paths) or `0x100`.

The second branch handles free patrol mobs. With `Leader == 0`, it first attempts enemy detection; a spotted enemy within the patrol segment bounds returns `enemy | 0x10000000`. It then manages route traversal: if the mob has reached its current segment target and `RouteType == 6` it returns 0 (stationary). When `SegmentX/Y == TargetX/Y`, it processes segment waits (`SegmentWait[SegmentProgress]`), decrements `WaitSec` by 6 per tick, and advances to the next segment via `SetSegment`. Special handling exists for `RouteType == 3` at `SegmentProgress == 4` (returns `0x10000`, triggering route-end deletion) and for `MOB.BaseScore.AttackRun & 0xF` (returns `0x10`, flee action). If no segment change is needed, it computes the next patrol position via `GetNextPos(0)`.

**Rule workflow:**
1. Dispatch on `RouteType == 5 || Affect[0].Type == 24` (follower) vs. free patrol.
2. Follower: validate Leader/Summoner/party, then follow within 13-tile leash.
3. Free mob: detect enemies; manage segment waits; advance route via SetSegment.
4. Compute next position via GetNextPos(0); return bitmask for the caller to act on.
5. Caller (`ProcessSecMinTimer`) interprets the bitmask (move/teleport/delete/engage).

---

### Business Rule: Combat AI — BattleProcessor

**Overview:**
`BattleProcessor` (CMob.cpp:209) is the combat state machine for `MOB_COMBAT` entities. It selects a target, applies leash/summon rules, and returns a bitmask encoding the combat action.

**Detailed description:**
The method first calls `SelectTargetFromEnemyList`; with no target it sets `Mode = MOB_PEACE` and returns 0. For summoned combat mobs (`RouteType == 5`), it revalidates the leader/summoner relationship: an invalid leader returns `0x20` (which the caller turns into deletion), and an invalid summoner returns `0x100`. If the summoner is more than 20 tiles away, the summoned mob teleports to it (returns 2). This enforces a "pet leash" so summons cannot roam unbounded.

The combat decision uses two derived attributes. First, the mob's Intelligence (`BaseScore.Int`) is compared against a random 0..99 value: if `BaseInt < rand() % 100`, it returns `0x010000`. This is intended as a probabilistic skill-cast decision, but note that the caller's combat loop does not mask this bit, so this decision currently has no observable effect (see Technical Debt). Second, the mob checks range: it computes `Range` (`KEFRA_BOSS` gets a hardcoded 25, otherwise `EF_RANGE` from `BASE_GetMobAbility`) and the distance to its target. If the target lies outside the mob's patrol segment box and the mob is not a summon, it abandons the target, clears the enemy list, returns to `MOB_PEACE`, and heads home (returns 0 or 16). If within range, it rolls a dodge check: `if (Range >= 4 && dis <= 4 && Rand > BaseDex)` returns `0x100` (chase), else returns `0x1000` (attack) when the target is at the mob's position or `0x100` (chase) otherwise. Out of range returns 1 (advance toward target).

**Rule workflow:**
1. `SelectTargetFromEnemyList`; no target -> `MOB_PEACE`, return 0.
2. Summon leash validation (`RouteType == 5`); returns 0x20/0x100/2 accordingly.
3. INT-based skill decision -> return 0x010000 (currently unhandled).
4. Compute range + distance to target.
5. Target outside segment -> disengage, return to peace, head home.
6. Target in range -> dodge check (Dex) -> attack (0x1000) or chase (0x100).
7. Target out of range -> return 1 (advance).

---

### Business Rule: Patrol Route Traversal (SetSegment)

**Overview:**
`SetSegment` (CMob.cpp:470) advances a mob along its configured patrol route, which is defined by up to 5 waypoint pairs (`SegmentListX/Y[5]`), per-segment wait times (`SegmentWait[5]`), ranges, and a direction. The `RouteType` (0-6) dictates the route's looping behavior.

**Detailed description:**
Route types govern traversal semantics. `RouteType 0` is a one-way patrol that stops and enters idle (`Mode = 4`/`MOB_PEACE`) at the final segment, re-derives stats, and, if a `Route` char string is set, extracts a merchant flag digit from the last route byte. `RouteType 1` resets to segment 0 at both ends (a back-and-forth loop restarting at the start). `RouteType 2` is a ping-pong route that reverses direction at segment 5 (sets `SegmentDirection = 1`). `RouteType 3` behaves like a loop that breaks at the boundaries. `RouteType 4` wraps circularly (segment 5 -> -1, continue). `RouteType 6` is a stationary/one-shot route that immediately sets `SegmentProgress = 0` and returns. When `SegmentDirection == 1`, progress decrements; otherwise it increments.

The loop advances `SegmentProgress` (bounded 0..5) and skips empty segment entries (`SegmentListX[SegmentProgress] == 0`). When progress goes out of range (-1 or 5), the route-type-specific recovery logic runs (reset, reverse, or stop). On invalid route types, it logs `"Wrong SetSegment"` or `"SetSegment SegmentProgress -1 but route type ..."`. The final `SegmentX/Y` are set to the chosen waypoint and `WaitSec` is reset to 0. The returned iterator value signals the caller: 1 = ship move (no move), 2 = teleport/move, 0x10 = action.

**Rule workflow:**
1. Handle `RouteType 6` (stationary) and invalid types (log + return).
2. Advance or retreat `SegmentProgress` per `SegmentDirection`.
3. On out-of-range progress, apply route-type-specific recovery.
4. Skip empty waypoints; land on a valid one.
5. Set `SegmentX/Y`, reset `WaitSec`, return iterator flag.

---

### Business Rule: Movement and Pathfinding

**Overview:**
Four movement helpers — `GetNextPos(int battle)`, `GetTargetPos(int tz)`, `GetRandomPos()`, and `GetTargetPosDistance(int tz)` — compute the mob's next position (`NextX/NextY`) while respecting movement speed, terrain height, and occupancy of the world grid.

**Detailed description:**
All four share the same core algorithm. Movement distance derives from speed: `distance = BASE_GetSpeed(&MOB.CurrentScore) * SECBATTLE / 4`, clamped to `MAX_ROUTE - 1` (23). Static/fixed entities (face `Equip[0].sIndex == 219 || 220`, or the Kefra boss in `GetNextPos`) never move and pin `NextX/Y = TargetX/Y`. Entities with `AttackRun & 0xF == 0` also stand still.

`GetNextPos` targets the current `SegmentX/Y` and, in non-battle mode, caps distance by `MOB.BaseScore.Str`. `GetTargetPos` moves toward a specific target slot `tz` (offset by 1 tile) and adds +2 speed. `GetRandomPos` wanders within a small randomized offset (`rand() % 5 - 3`) around the current position. `GetTargetPosDistance` moves toward a target while applying a randomized lateral jitter (`1 + rand() % 2`) so attackers close in with imperfect straight-line movement.

Each helper then performs collision-aware routing: starting from the desired next cell, it walks the distance down toward 0, calling `BASE_GetRoute` (with the height grid) to derive an obstacle-avoiding path and checking `pMobGrid[NextY][NextX] == 0` to confirm the cell is empty. If a candidate cell is occupied, it attempts a nearby empty cell via `GetEmptyMobGrid`. If no valid path is found (`i == -1` or `Route[0] == 0`), the mob falls back to standing at its current position (`NextX/Y = TargetX/Y`) and clears the route.

**Rule workflow:**
1. Determine movement distance from speed (clamped to MAX_ROUTE-1).
2. Apply static-entity and AttackRun early-outs (no movement).
3. Set a candidate destination (segment, target, or random offset).
4. Iteratively route from the last position using BASE_GetRoute + pMobGrid occupancy.
5. Resolve collisions with GetEmptyMobGrid; fall back to standing if no path.

---

### Business Rule: Derived Stat Computation (GetCurrentScore)

**Overview:**
`GetCurrentScore(int idx)` (CMob.cpp:598) recomputes all derived/effective stats of an entity from its base attributes, equipment, learned skills, and active affects. It is invoked on login, level-up, stat application, item/equipment changes, and mob generation.

**Detailed description:**
The method branches on whether the entity is a player (`idx < MAX_USER`) or a mob/NPC. For players below level 2000, elemental resistances are recomputed from `EF_RESIST1..4` via `BASE_GetMobAbility` and hard-capped at 100, the weapon's effect is cleared, and the reserved flags (`Rsv`) are reset. For mobs (`idx >= MAX_USER`), resistances are copied from the NPC generator template (`mNPCGen.pList[geneidx].Leader.Resist[]`) and, if above 100, clamped to 50. `Parry` is derived from `EF_PARRY`. Player attack range is set (and then forcibly overridden to 23). Bonus accumulators (`ExpBonus`, `DropBonus`, `ForceDamage`, `ReflectDamage`, `ForceMobDamage`, `Accuracy`, `HpAbs`) are reset, then `BASE_GetCurrentScore` fills them from base stats and affects.

A large block of item-driven rules follows. Fairy equipment in slot 13 (`Equip[13].sIndex`) grants fixed bonuses by item ID: e.g. Fada Verde 3D (3900) +16 ExpBonus, Fada Azul 3D (3901) +32 DropBonus, Fada Vermelha (3902/3905/3908) +32 Exp/+16 Drop, and several others with intermediate values. The "Concentração" skill (bit 28 of `LearnedSkill`) adds +50 Accuracy. Weapon damage is computed from the two hand slots (`Equip[6]`, `Equip[7]`), halving the off-hand contribution unless the "Pericia do caçador" (bit 10, Class 3) or "Mestre das Armas" (bit 9, Class 0) skills are learned. Class-2 (warrior) rules add `ReflectDamage` from specials and "Escudo do tormento" (bit 19) which adds AC based on a 128-position shield. Weapons with `nPos == 64 || 192` and sanctification >= 9 add +40 WeaponDamage. Per equipment slot, item Grade 5/6/7/8 grant DropBonus/ForceDamage/ExpBonus/ReflectDamage, and gem/sanctification levels scale ForceDamage and ReflectDamage (with Grade 6 gems granting more ForceDamage, Grade 8 granting more ReflectDamage). Finally, PvP multipliers `PvPDamage` and `ReflectPvP` are derived from `EF_HWORDGUILD` and `EF_LWORDGUILD` as `(value + 1) / 10`.

**Rule workflow:**
1. Branch player vs. mob; recompute resistances (caps 100/50) and clear Rsv.
2. Reset all derived bonus accumulators.
3. Call `BASE_GetCurrentScore` for base derivation.
4. Apply fairy-item and skill-specific bonuses (Exp/Drop/Accuracy).
5. Compute WeaponDamage from hands + skills + sanctified weapons.
6. Per-item grade/gem/sanctification bonuses for Drop/Force/Exp/Reflect.
7. Derive PvPDamage/ReflectPvP; clamp HP/MP to maxima.

---

### Business Rule: Experience Segments and Leveling (CheckGetLevel)

**Overview:**
`CheckGetLevel()` (CMob.cpp:1034) determines whether an entity gains a level and tracks progress through four EXP "segments" between levels. It is called after combat kills and after item combination.

**Detailed description:**
The method first returns 0 if the entity is already at `MAX_LEVEL` (399). The maximum attainable level depends on the entity's `ClassMaster`: MORTAL and ARCH cap at `MAX_LEVEL` (399), while CELESTIAL/SCELESTIAL/CELESTIALCS cap at `MAX_CLEVEL` (199). The EXP range between the current and next level is divided into four equal segments via `deltaexp = (nextexp - curexp) / 4`, using either `g_pNextLevel` (mortal/arch) or `g_pNextLevel_2` (celestial) tables. The current segment is determined by comparing accumulated `MOB.Exp` against the segment boundaries.

Two quest gates block advancement: a CELESTIAL entity cannot level at levels 39 or 89 unless the corresponding quest (`QuestInfo.Celestial.Lv40` / `Lv90`) is complete, and an ARCH entity cannot pass levels 354 or 369 without the `Level355`/`Level370` quest flags. When `exp >= nextexp`, the entity levels up: `BaseScore.Level++`, `MaxHp`/`MaxMp` grow by class increments (`g_pIncrementHp[cls]`/`g_pIncrementMp[cls]`), HP/MP are fully restored, and stat bonuses are granted — `SkillBonus += 4` (levels >= 200) or `+= 3` (below), `SpecialBonus += 2`, AC +1, and `BASE_GetBonusScorePoint` is invoked (for MORTAL/ARCH; CELESTIAL only gets the score-point re-derivation and AC). `GetCurrentScore(0)` is then called to refresh derived stats. When only a segment threshold (but not a full level) is crossed, the method records the new `Segment` and restores HP/MP and returns the segment number (1..3). The return value (0..4) is consumed by callers to trigger client-side animations/effects (`SendFunc.cpp:925`, `_MSG_Attack.cpp:1748`, `_MSG_CombineItemEhre.cpp:161`).

**Rule workflow:**
1. Early-out if at level cap.
2. Determine max level by ClassMaster (399 vs 199) and select EXP table.
3. Split the EXP range into 4 segments; locate current segment.
4. Enforce CELESTIAL (39/89) and ARCH (354/369) quest gates.
5. If EXP >= next level: increment level, grow HP/MP, grant stat/skill bonuses, recompute.
6. Else if a segment threshold crossed: record segment, restore HP/MP, return segment.
7. Return 0..4 for caller-driven client feedback.

---

### Business Rule: Summon / Follower Leash

**Overview:**
Entities with `RouteType == 5` (or affect type 24) behave as summoned minions that follow a party leader and a specific summoner, with hard leash-distance constraints.

**Detailed description:**
The follower relationship is defined by `Leader` (the party leader's slot) and `Summoner` (the entity that summoned the minion). Both `StandingByProcessor` (CMob.cpp:60-130) and `BattleProcessor` (CMob.cpp:220-253) validate that the `Summoner` is either the leader itself or a member of the leader's `PartyList`. The summoner must be in `USER_PLAY` mode. If the relationship is invalid, the follower is removed (returns that trigger deletion) or simply cannot act. In standing mode the follower maintains a 13-tile leash: beyond 13 tiles it teleports toward the summoner (return `0x02`); within 4..13 tiles it moves to the summoner's position (return `0x01`). In combat mode the leash is 20 tiles: beyond that the summoned combatant teleports back (return 2). Additionally, `SetBattle` and the damage-resolution code treat summoned Clan-4 mobs specially — damage dealt near the summoner is attributed back, and summon HP is linked to the mount.

**Rule workflow:**
1. Validate Leader, Summoner, party membership, and USER_PLAY.
2. Standing mode: leash 13 tiles -> teleport or move toward summoner.
3. Combat mode: leash 20 tiles -> teleport back to summoner.
4. Invalid relationship -> return codes that delete or stall the minion.

---

## 4. Component Structure

```
legacy/Code/TMSrv/
├── CMob.h                     # Class definition, mode/constants, global pMob[]
└── CMob.cpp                   # Implementation (1279 lines)
    ├── CMob() / ~CMob()       # Constructor / destructor
    ├── ProcessorSecTimer()    # Per-second tick (currently empty stub)
    ├── StandingByProcessor()  # Idle/patrol/follower AI state machine
    ├── BattleProcessor()      # Combat AI state machine
    ├── AddEnemyList()         # Enemy insertion + Kefra area-aggro
    ├── RemoveEnemyList()      # Enemy removal
    ├── SelectTargetFromEnemyList()  # Nearest-target selection
    ├── SetSegment()           # Patrol route traversal
    ├── GetCurrentScore()      # Derived stat computation
    ├── GetTargetPosDistance() # Chase movement with jitter
    ├── GetRandomPos()         # Wandering movement
    ├── GetTargetPos()         # Move toward specific target
    ├── CheckGetLevel()        # EXP segments / level-up
    ├── GetNextPos()           # Patrol/segment movement
    └── GetEnemyFromView()     # Vision-based enemy detection
```

The component is a single class with a large public surface. It is a data-structure-plus-methods design rather than a layered architecture. The state (member fields) and behavior (methods) are co-located, and all world data is shared via global arrays.

---

## 5. Dependency Analysis

**Internal Dependencies (within the class):**
```
StandingByProcessor  -> GetEnemyFromView, SetSegment, GetNextPos, GetTargetPos
BattleProcessor      -> SelectTargetFromEnemyList, GetNextPos, BASE_GetDistance
SelectTargetFromEnemyList -> BASE_GetDistance
SetSegment           -> GetCurrentScore, strlen/Route parsing
GetCurrentScore      -> BASE_GetCurrentScore, BASE_GetMobAbility, BASE_GetItemAbility,
                        BASE_GetItemSanc, BASE_GetItemGem, BASE_GetSpeed
Movement helpers     -> BASE_GetSpeed, BASE_GetRoute, GetEmptyMobGrid, BASE_GetDistance
CheckGetLevel        -> BASE_GetBonusScorePoint, BASE_GetCurrentScore (via GetCurrentScore)
GetEnemyFromView     -> g_pClanTable
```

**External Dependencies (other modules / globals):**
```
- pMob[MAX_MOB]         Global array of all entities (defined in CMob.h:122)
- pUser[]               Player state (TMSrv/CUser.h) — read for USER_PLAY, Range
- pMobGrid[][]          World occupancy grid
- pHeightGrid[][]       Terrain height grid
- g_pClanTable[9][9]    Clan hostility matrix (Basedef.h:2480)
- g_pItemList[]         Item catalog (Basedef.h:2467)
- mNPCGen[]             NPC generator templates (CNPCGene)
- g_pNextLevel[] / g_pNextLevel_2[]   EXP tables (Basedef.h:2454-2455)
- g_pIncrementHp[] / g_pIncrementMp[] Level HP/MP growth tables
- BASE_* functions      Core math/derivation in Basedef.cpp (declared Basedef.h)
- GetEmptyMobGrid       Grid occupancy resolver (Server/GetFunc)
- ProcessSecMinTimer    Timer driver (calls the processors)
- SetBattle             Combat activation (Server.cpp:7218)
- DeleteMob, DoTeleport, GetAction, GridMulticast  Server-side action functions
```

---

## 6. Afferent and Efferent Coupling

Coupling is measured at the class-method level; "components" here are the methods of the CMob class plus the tightly-coupled surrounding server modules.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|----------|
| GetCurrentScore | 20+ (login, level-up, stat apply, item change, mob gen, combat, quests, DB) | ~12 (BASE_GetCurrentScore, BASE_GetMobAbility, BASE_GetItemAbility/Sanc/Gem, BASE_GetSpeed, g_pItemList, mNPCGen) | High |
| CheckGetLevel | 3 (SendFunc, _MSG_Attack, _MSG_CombineItemEhre) | ~7 (BASE_GetBonusScorePoint, GetCurrentScore, g_pNextLevel/_2, g_pIncrementHp/Mp, g_pItemList) | High |
| StandingByProcessor | 1 (ProcessSecMinTimer) | ~7 (GetEnemyFromView, SetSegment, GetNextPos, GetTargetPos, BASE_GetDistance, pMob, pUser) | High |
| BattleProcessor | 1 (ProcessSecMinTimer) | ~5 (SelectTargetFromEnemyList, GetNextPos, BASE_GetDistance, BASE_GetMobAbility, pMob) | High |
| SelectTargetFromEnemyList | 1 (BattleProcessor) | ~3 (BASE_GetDistance, pMob, pMobGrid) | Medium |
| GetEnemyFromView | 1 (StandingByProcessor) | ~3 (g_pClanTable, pMob, pMobGrid) | Medium |
| AddEnemyList | 2+ (SetBattle, attack handlers) | ~2 (pMob, pUser) | Medium |
| SetSegment | 1 (StandingByProcessor) | ~2 (GetCurrentScore, Log) | Medium |
| GetNextPos | 2 (StandingByProcessor, BattleProcessor) | ~3 (BASE_GetSpeed, BASE_GetRoute, GetEmptyMobGrid) | Medium |
| Movement helpers (GetTargetPos/Distance/RandomPos) | 1-2 (ProcessSecMinTimer) | ~3 (BASE_GetSpeed, BASE_GetRoute, GetEmptyMobGrid) | Medium |
| GetCurrentScore | (see above) | (see above) | High |

`GetCurrentScore` is the most coupled method both afferently and efferently, making it the highest-risk change point. The movement helpers and processors share a common high-efferent pattern to grid/pathfinding globals.

---

## 7. Endpoints

The CMob component does not expose any network endpoints (REST, gRPC, etc.) directly. It is an internal simulation component of a game server. Communication with clients occurs indirectly through the surrounding server layer (e.g. `GridMulticast`, `SendScore`, `GetAction`), which are not part of the CMob class. This section is therefore omitted.

---

## 8. Integration Points

The CMob component integrates with the rest of the server exclusively through shared global state and synchronous function calls (no external services, databases, or message queues are touched directly).

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| pMob[] global array | Internal state | All entity data (players + mobs) | Direct memory access | STRUCT_MOB / CMob | Bounds checks via MAX_USER/MAX_MOB |
| pUser[] | Internal state | Player connection/mode state | Direct memory access | CUser | Mode checks (USER_PLAY) |
| pMobGrid / pHeightGrid | Internal state | World occupancy + terrain | Direct memory access | 2D int/char arrays | Bounds checks + GetEmptyMobGrid |
| g_pClanTable | Internal data | Clan hostility relations | Direct memory access | int[9][9] | Range log on OOB clan |
| g_pItemList | Internal data | Item catalog for stat derivation | Direct memory access | STRUCT_ITEMLIST | Index range checks |
| mNPCGen | Internal data | NPC/mob generator templates | Direct memory access | CNPCGene | Index range checks |
| BASE_* functions | Internal API | Math, routing, stat derivation | Function call | Structs/scalars | Return codes / clamping |
| ProcessSecMinTimer | Internal driver | Invokes processors every second | Function call | int bitmask returns | Bitmask dispatch |
| SetBattle / DeleteMob / DoTeleport / GetAction / GridMulticast | Internal API | World actions + client broadcast | Function call | MSG_* structs | Caller-side validation |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| State Machine | Mode field (MOB_EMPTY..MOB_WAITDB) + dispatcher | CMob.h:26-35, ProcessSecMinTimer | Entity lifecycle management |
| State Machine (action) | Bitmask return values from processors | CMob.cpp:56, 209 | Compact action signalling to caller |
| Global Singleton / God Object | Global pMob[] array + stateless methods | CMob.h:122 | Shared world entity state |
| Strategy (route types) | RouteType 0-6 select traversal behavior in SetSegment | CMob.cpp:470 | Patrol route looping strategies |
| Template Method (data-driven) | Derived stats from generator templates (mNPCGen) | CMob.cpp:598 | Mob config driven by data |
| Anti-corruption / helper delegation | Delegate math to BASE_* functions | CMob.cpp | Reuse of core math |
| Leash/Pet pattern | Leader + Summoner validation with distance leash | CMob.cpp:60, 220 | Constrain summoned entities |

The architecture relies on a central mutable global `pMob[]` and bitmask-returning AI methods. This is a legacy single-process game-server design with high cohesion within the class but very tight coupling to surrounding modules through globals.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | BattleProcessor | INT-based skill-cast returns `0x010000` which the caller never handles | Intended skill-cast AI is effectively dead code; combat behavior may diverge from design |
| High | ProcessorSecTimer | Method is an empty stub (increment commented out) | Disabled per-second processing; incomplete feature |
| High | GetCurrentScore | Tight coupling to 20+ call sites; any change risks wide regressions | High blast radius; brittle to stat/equipment changes |
| High | Entire class | Zero test coverage; no unit/integration tests exist anywhere in the project | Behavior validated only at runtime; regressions undetected |
| Medium | Movement / AI | Numerous hardcoded magic values (face 315..345, fairy IDs 3900..3913, dis 6/8/12, KEFRA_BOSS 396, affect type 24) | Content-specific behavior baked into code; brittle to content changes |
| Medium | GetCurrentScore | Resistances clamped to 100 for players but copied to 50 for mobs with inconsistent over-100 handling | Potential stat imbalance / ambiguity |
| Medium | StandingByProcessor | Follower-return logic mixes deletion triggers (0x01/0x100) with movement returns | Hard to reason about follower cleanup |
| Medium | CheckGetLevel | Quest gates (Lv40/Lv90, Level355/Level370) hardcoded to specific levels | ClassMaster progression tightly coupled to content quests |
| Medium | GetEnemyFromView | Non-hostile entities inside view are silently ignored; no explicit fallback | Potential for mobs to ignore valid aggressors |
| Low | Movement helpers | Repeated copy-paste routing loop across 4 functions | Duplicated logic; maintenance burden |
| Low | SetSegment | Logs "Wrong SetSegment" on route type outside 0..4 (RouteType 5/6 handled earlier) | Noisy/incorrect error logs possible |
| Low | Data structures | `Unk5`, `Unk7`, `Tab`, `Snd`, `QuestFlag` fields with unclear purpose | Unclear/unused state increases cognitive load |

---

## 11. Test Coverage Analysis

**Test file locations:** No test files were found anywhere in the project. A project-wide search for directories or files matching `test`, `tests`, `Test`, `Tests`, `spec`, or `Spec` (excluding `.git` and `.opencode`) returned zero results. The W2PP solution contains only the `TMSrv` and `DBSrv` game-server executables; there is no separate test project.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| CMob (all methods) | 0 | 0 | 0% (none) | N/A — no assertions exist |

**Coverage assessment:** The CMob component has no automated test coverage. All business rules documented in Section 3 (AI state machines, targeting, patrol routes, stat derivation, leveling, summon leash) are verified only through manual runtime testing of the live server. Given the component's high coupling, hardcoded magic values, and the presence of dead code (`0x010000` skill cast, empty `ProcessorSecTimer`), the absence of tests represents a significant regression risk. The dependency-auditor report (`docs/reports/dependency-auditor/dependencies-report-2026-08-19 17:13:23.md`) and architectural report (`docs/reports/architectural-analyzer/architectural-report-2026-08-19 17:13:23.md`) similarly contain no test references.

---

## 12. Report Metadata

- **Component analyzed:** CMob
- **Component source:** `legacy/Code/TMSrv/CMob.h`, `legacy/Code/TMSrv/CMob.cpp`
- **Supporting files reviewed:** `legacy/Code/Basedef.h`, `legacy/Code/ItemEffect.h`, `legacy/Code/TMSrv/Server.h`, `legacy/Code/TMSrv/CUser.h`, `legacy/Code/TMSrv/ProcessSecMinTimer.cpp`, `legacy/Code/TMSrv/Server.cpp`, `legacy/Code/TMSrv/SendFunc.cpp`, `legacy/Code/TMSrv/GetFunc.cpp`
- **Folders ignored:** `.git`, `.opencode`
- **Modifications made:** None (analysis only)
