# Component Deep Analysis Report — CMob Entity and AI

**Project:** W2PP (decompiled C/C++ Win32 MMORPG server emulator, MSVC, 32-bit)
**Component:** `CMob` — per-entity record plus mob AI state machine, enemy list, path-segment engine and per-tick processors
**Primary location:** `Code/TMSrv/CMob.cpp` (1,279 lines), `Code/TMSrv/CMob.h` (123 lines)
**Analysis date:** 2026-11-15 22:44:18
**Scope:** entire project root `E:\wyd\test-setup\w2pp`
**Excluded:** `.agents`, `.codegraph`, `.opencode`, `openspec`
**Architectural role (from architecture report):** Domain Entity — Ca ≈ 67, Ce ≈ 4

---

## 1. Executive Summary

`CMob` is the single entity representation of the whole TMSrv world. It is not a "monster class" in the narrow sense: the same class instance backs player characters, NPCs, pets/summons, war towers, castle doors and monsters. The world is one contiguous static array `CMob pMob[MAX_MOB]` (`Code/TMSrv/Server.cpp:270`, `CMob.h:122`) of 25,000 slots, where indices `1..999` are player sessions (`MAX_USER = 1000`) and `1000..24999` are non-player entities (`Code/Basedef.h:56,107`). A connection index `conn` is simultaneously the socket index, the `pUser[conn]` session index and the `pMob[conn]` entity index — slot identity aliasing is therefore a first-class architectural property of this component.

`CMob` has three distinct responsibilities that are physically co-located in one class:

1. **Entity record** — `STRUCT_MOB MOB` (persisted character/monster sheet), `STRUCT_MOBEXTRA extra`, `Affect[32]`, and derived combat caches (`WeaponDamage`, `Parry`, `DropBonus`, `ExpBonus`, `ForceDamage`, `ReflectDamage`, `PvPDamage`, `ReflectPvP`, `Accuracy`, `HpAbs`).
2. **AI state machine** — a 10-value `Mode` enumeration resolved by two mutually exclusive per-tick processors (`StandingByProcessor` for out-of-combat, `BattleProcessor` for combat), each returning a bitmask of *behaviours* rather than performing side effects itself.
3. **Navigation engine** — a 5-waypoint segment list (`SegmentListX/Y`, `SegmentWait`, `SegmentRange`) driven by `SegmentProgress`/`SegmentDirection`, plus six movement primitives (`GetNextPos`, `GetTargetPos`, `GetTargetPosDistance`, `GetRandomPos`, `SetSegment`, and the shared `BASE_GetRoute` height-aware pathfinder).

Key findings:

* The documented state machine (idle/peace/combat/return/flee/roam) is **not** what the code implements. `MOB_IDLE`, `MOB_RETURN`, `MOB_FLEE`, `MOB_ROAM` and `MOB_USERDOCK` appear exactly once each in the entire tree — inside their own `#define`. Only `MOB_EMPTY`, `MOB_USER`, `MOB_PEACE`, `MOB_COMBAT` and `MOB_WAITDB` are ever assigned. The effective machine is `EMPTY -> PEACE <-> COMBAT` with `WAITDB` as a terminal persistence state.
* Behaviour is encoded in **overloaded integer bitmasks** with at least one value collision (`0x10000` = despawn in the standing-by path, `0x010000` = "suppress action" in the combat path) and several unreachable branches.
* `BattleProcessor` contains a provably dead branch (`CMob.cpp:291-299`): `bx=tx; by=ty;` are assigned immediately before the comparison `tx==bx && ty==by`, so the function always returns `4096` (attack) and never the `256` alternative.
* `SetSegment` contains an impossible guard (`CMob.cpp:483`: `RouteType <= 0 && RouteType > 4`), an out-of-bounds read of `SegmentListX[5]` / `SegmentListX[-1]` (`CMob.cpp:591-592`) reachable for route types 1 and 3, and an out-of-bounds read of `Route[strlen(Route)]` (`CMob.cpp:544`).
* The leash rule is anchored to the **current segment waypoint**, not the spawn point, so the "aggro boundary" travels with the patrol path.
* There are **no tests of any kind** in the repository (no test project, no test framework references, no test directory). This is verified in section 10 with evidence.
* `GetCurrentScore` is the highest-afferent member: 60 call sites across 10 translation units, and it mutates `pMob[idx].MOB.Clan = 0` for player indices, which is a side effect unrelated to scoring.

---

## 2. Data Flow Analysis

### 2.1 Out-of-combat tick (peace / idle)

```
1. ProcessSecTimer() fires every second (TIMER_SEC → ProcessSecMinTimer.cpp:40)
2. Partition: Sec6 = SecCounter % 6; loop index = Sec6 + MAX_USER; index += 6
   (ProcessSecMinTimer.cpp:1353-1357) → every mob is visited once per 6 seconds
3. Read Mode, Clan from pMob[index] (1359-1360)
4. Mode == MOB_IDLE  → HP<=0 ? DeleteMob(index,1) : ProcessAffect + re-register pMobGrid (1364-1374)
5. Mode == MOB_PEACE → HP<=0 ? DeleteMob(index,1) (1376-1384)
6. Quest/event despawn zone check → DeleteMob(index,3) (1388-1398)
7. Sanitize SegmentProgress into [0, MAX_SEGMENT] (1399-1405)
8. CMob::StandingByProcessor() (CMob.cpp:56) → returns behaviour bitmask
9. RouteType==5 || Affect[0].Type==24  → summon/pet branch
       Leader/Summoner/party validation → 0x100 (delete), 1 (invalid), 2 (teleport), 0x01 (walk to owner)
   else                                 → patrol branch
       Leader==0 && no target in segment box → GetEnemyFromView() → return enemy|0x10000000
       at waypoint? → wait timers (WaitSec -= 6) → SetSegment()
10. Caller decodes bitmask: 0x10000000 → SetBattle(index, target) + party propagation (1408-1462)
                           1 → GetAction + GridMulticast (MSG_Action) (1465-1495)
                           0x10 → SendSay(FleeAction) (1496-1505)
                           0x100 → DeleteMob(index,3) (1508-1512)
                           0x1000 → GetRandomPos + action (1513-1532)
                           2 → DoTeleport (1533-1534)
                           0x10000 → DeleteMob(index,3) (1536-1546)
```

### 2.2 Combat tick

```
1. Sec4 = SecCounter % 4; loop index = Sec4 + MAX_USER; index += 4
   (ProcessSecMinTimer.cpp:1554-1560) → every combat mob acts once per 4 seconds
2. Mode != MOB_COMBAT → skip
3. HP<=0 → DeleteMob(index,1) (1567-1576)
4. ProcessAffect every 16th slot (1578-1579)
5. Quest/event despawn zone check → DeleteMob(index,3) (1581-1591)
6. Same-leader / same-guild reconciliation → RemoveEnemyList(Target) (1593-1619)
7. CMob::BattleProcessor() (CMob.cpp:209)
       SelectTargetFromEnemyList() → CurrentTarget = 0 or nearest valid enemy
       CurrentTarget==0 → Mode = MOB_PEACE, return 0
       RouteType==5 → pet validation (Leader, Summoner, party membership)
       Int gate: BaseInt < rand()%100 → return 0x010000 (do nothing this tick)
       Segment leash: outside SegmentX/Y ±16 → clear target+enemies, Mode=MOB_PEACE, GetNextPos(1)
       Range check: dis <= EF_RANGE (KEFRA_BOSS = 25)
       Dex gate: ranged mob, dis<=4, rand>Dex → return 256 (reposition)
       otherwise return 4096 (attack)
8. Caller decodes: 0x20 → DeleteMob(index,3)
                  0x1000 → GetAttack + damage math + MobKilled on kill (1629-1942)
                  0x100 → GetTargetPosDistance + MSG_Action (1955-1973)
                  1 → GetTargetPos + MSG_Action (1975-1993)
                  2 → DoTeleport (1995-1996)
                  0x10 → MSG_Action continuation (1998-2014)
9. Damage path: parry roll → clan-4 damage scaling → SetBattle both ways → party set-battle
                → affect type 18 mana shield → fairy 786/1936/1937 damage reduction
                → HP subtraction → ReqHp update → GridMulticast(_MSG_AttackOne)
                → HP<=0 → MobKilled(Target, index) (1936-1941)
```

### 2.3 Spawn flow (producer side of the entity)

```
GenerateMob(index, PosX, PosY)   (Server.cpp:2995)
  → generator relocation for MinuteGenerate >= 500 (3010-3028)
  → quota check CurrentNumMob vs MaxNumMob (3044-3048)
  → GetEmptyNPCMob() linear scan from g_EmptyMob (Server.cpp:6765)
  → memcpy STRUCT_MOB from mNPCGen.pList[nindex].Leader (3069)
  → SegmentListX/Y from generator, jittered by SegmentRange (3089-3113)
  → GenerateIndex / Formation / RouteType / Mode = MOB_PEACE (3149-3154)
  → GetCurrentScore(MAX_USER)  (3162)
  → full HP, newbie-event 75% HP (3163-3166)
  → GetEmptyMobGrid placement, pMobGrid[py][px] = tmob (3175-3225)
  → followers: PartyList[i] = tempmob, Leader = tmob2, formation offsets (3229-3314)
```

### 2.4 Death flow (consumer side)

```
HP <= 0  →  MobKilled(target, killer, x, y)  (MobKilled.cpp:41)
  → level >= 1000 → invulnerable: restore MaxHp, re-broadcast MSG_CreateMob, return (50-63)
  → fairy 769 (Nyerds) → decrement sanc instead of dying (65-104)
  → castle spawn point → teleport out (105-109)
  → pet-kill mount XP accrual (113-171)
  → exp / coin / drop distribution, party split (206+)
  → caller then DeleteMob(target, 1) from the tick loop or attack handler
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Data model / invariant | World entity identity: slots `1..999` are players, `1000..24999` are non-players; one static `CMob` array backs both | `Code/TMSrv/CMob.h:37-100,122`; `Code/TMSrv/Server.cpp:270`; `Code/Basedef.h:56,107` |
| BR-02 | State machine | Ten `Mode` constants declared; only `MOB_EMPTY/USER/PEACE/COMBAT/WAITDB` are ever assigned | `Code/TMSrv/CMob.h:26-35` |
| BR-03 | Scheduling | Out-of-combat entities tick every 6 s, combat entities every 4 s; `WaitSec` decrements by 6 per visit | `Code/TMSrv/ProcessSecMinTimer.cpp:1353-1357,1554-1560`; `CMob.cpp:153,168` |
| BR-04 | Validation / lifecycle | HP<=0 while idle or in peace → delete entity with reason 1 ("death") | `ProcessSecMinTimer.cpp:1366-1371,1378-1384,1567-1576` |
| BR-05 | Aggro acquisition | Aggro only via `GetEnemyFromView()` scan (9x9, or 16x16 for clan 7/8) filtered by `g_pClanTable` hostility | `CMob.cpp:1229-1279`; `Basedef.cpp:189-202` |
| BR-06 | Aggro commitment | `SetBattle` requires the pair to be within ±23 grid units, target must be a non-player or a playing user; sets `MOB_COMBAT` and appends to enemy list | `Server.cpp:7218-7252` |
| BR-07 | Collection bounds | Enemy list holds at most 13 entries; duplicates suppressed; hidden users (`RSV_HIDE`) and merchants (`Merchant & 1`) are immune | `CMob.h:24`; `CMob.cpp:306-363` |
| BR-08 | Target selection | Nearest valid enemy inside a radius of 6 / 12 (clan 4,7,8) / 8 (specific map band) / 16 (KEFRA boss), mobs penalised +2 | `CMob.cpp:381-468` |
| BR-09 | Leash / disengage | Target dropped and enemies cleared when the mob leaves `SegmentX/Y ± HALFGRID(16)`; returns to peace and paths home | `CMob.cpp:268-283` |
| BR-10 | Action throttle | Mob skips its combat action when `BaseInt < rand()%100` (probability of acting ≈ Int/100) | `CMob.cpp:255-260` |
| BR-11 | Standoff discipline | Ranged mob (`Range>=4`) that is within 4 tiles of the target and fails a `rand%Dex` roll repositions instead of attacking | `CMob.cpp:284-289` |
| BR-12 | Attack resolution | Attack only when `dis <= EF_RANGE` (boss override 25); actual damage, parry, shields, reflect and death are resolved by the caller | `CMob.cpp:262-301`; `ProcessSecMinTimer.cpp:1629-1942` |
| BR-13 | Pet / summon contract | `RouteType==5` or `Affect[0].Type==24` binds the entity to `Leader`+`Summoner`; invalid owner → despawn; distance ≥13 → teleport behind; pet lifetime from affect timer | `CMob.cpp:60-129,220-253`; `Server.cpp:2707-2731,2807-2816,5388-5389` |
| BR-14 | Path-segment model | 5 waypoints, per-waypoint wait, per-waypoint jitter range, forward/backward direction, route type 0..6 semantics | `CMob.h:55-66`; `CMob.cpp:470-596`; `CNPCGene.h:27-46` |
| BR-15 | Route type 0 | Cyclic patrol; on wrap re-derives appearance `Merchant` nibble from the `Route` string and recomputes scores | `CMob.cpp:534-561` |
| BR-16 | Route type 3 | One-shot route; at final waypoint with `WaitSec<=0` the entity is despawned (`0x10000`) | `CMob.cpp:148-156`; `ProcessSecMinTimer.cpp:1536-1546` |
| BR-17 | Route type 6 | Fixed-anchor route: no waiting at all; arrival short-circuits to idle return | `CMob.cpp:143-144,157,472-481` |
| BR-18 | Movement pacing | Speed = `AttackRun & 0xF` clamped to 1..6; step budget = `speed * SECBATTLE(8) / 4`; immobilised when `AttackRun & 0xF == 0` | `Basedef.cpp:1210-1223`; `CMob.cpp:867-871,931-935,991-995,1170-1174` |
| BR-19 | Occupancy / terrain | Target tile must be free of entities and not height `127`; search escalates 1→2→3→4 tiles; blocked path cancels the move | `GetFunc.cpp:1901-2001`; `CMob.cpp:894-918,1197-1226` |
| BR-20 | Derived stats | `GetCurrentScore` recomputes resist caps, HP/MP clamps, weapon damage, fairy/grade/gem/sanc bonuses, PvP attack/defence, parry, accuracy | `CMob.cpp:598-855` |
| BR-21 | Progression | `CheckGetLevel` level-up with class-master caps (399 / 199), quest gates at 40/90 and 355/370, 4-tier EXP segments | `CMob.cpp:1034-1152`; `Basedef.h:117-118` |
| BR-22 | Spawn quotas | Generator enforces `MaxNumMob`, `MinGroup..MaxGroup`, formation offsets, jitter, full HP (75% during newbie event), 10% clan variance | `Server.cpp:2995-3330` |
| BR-23 | Death guard | Entities with level ≥ 1000 are unkillable (HP restored); Nyerds fairy 769 absorbs death by consuming sanctification | `MobKilled.cpp:50-104` |
| BR-24 | Event despawn | Rune-track / castle / pista zones forcibly remove specific generator ranges, with counters incremented | `ProcessSecMinTimer.cpp:1388-1398,1490-1494,1536-1546,1581-1591` |
| BR-25 | Social filter | Same leader or same guild (with `GuildDisable` forcing guild 0) removes the target from the enemy list during combat | `ProcessSecMinTimer.cpp:1593-1619` |

---

### 3.2 Detailed breakdown of the business rules

---

#### Business Rule: BR-01 — World entity identity and slot partitioning

**Overview**
Every living thing in TMSrv is a `CMob` in one static array. The index encodes the entity kind; there is no type tag, no subtype field and no separate container for players.

**Detailed description**
The array `CMob pMob[MAX_MOB]` is declared at `Code/TMSrv/Server.cpp:270` and re-declared `extern` in two headers (`CMob.h:122`, `Server.h:230`). `MAX_USER` is defined as 1000 (`Basedef.h:56`) and the comment states it is both "Max users on CUser pUser" and "starting index of npcs and mobs"; `MAX_MOB` is 25000 (`Basedef.h:107`). Consequently, a connection index `conn` is a valid index into `pUser[conn]`, `pMob[conn]` and the outbound packet `ID` field simultaneously. The component leans on this constantly: `GetCurrentScore(int idx)` branches on `idx < MAX_USER` versus `idx >= MAX_USER` to decide whether it is recomputing a player sheet or a monster sheet (`CMob.cpp:600-663`), `AddEnemyList` treats `TargetId <= MAX_USER` as "this is a player", and `SelectTargetFromEnemyList` adds a +2 distance penalty for `myenemy > MAX_USER` and skips player-only filters with `goto LABEL_51` when `myenemy >= MAX_USER` (`CMob.cpp:421-443`).

The non-player half of the array is allocated by a monotonically advancing cursor `g_EmptyMob` inside `GetEmptyNPCMob()` (`Server.cpp:6765-6791`), which wraps at `MAX_MOB` back to `MAX_USER` and performs a linear scan for `Mode == MOB_EMPTY`. This is an O(n) allocation that degrades as the NPC population grows; there is no free-list. Slots are released only through `DeleteMob(conn, Type != 0)` which sets `MOB.CurrentScore.Hp = 0`, `Mode = 0`, clears `pMobGrid[TargetY][TargetX]` and calls `RemoveParty(conn)` (`Server.cpp:7020-7054`).

Because slot 0 is never a valid user (`for (int i = 1; i < MAX_USER; i++)` is the universal player iteration idiom) but *is* a valid `pMob` index, an entity reference of `0` is used as the "no entity" sentinel throughout the component: `CurrentTarget == 0`, `EnemyList[i] == 0`, `Leader == 0`, `Summoner == 0`. The sentinel is not type-safe; `CMob.cpp:1257-1262` reads `pMobGrid[y][x]` straight into `pMob[tmob]` without checking `tmob != 0`, relying on `pMob[0].Mode` being zero.

**Rule workflow**
```
conn (1..999)  → pUser[conn] + pMob[conn]  (player entity)
index (1000..24999) → pMob[index] only     (NPC / mob / pet / tower / door)
GetEmptyNPCMob() → scan from g_EmptyMob, wrap at MAX_MOB, return first Mode==MOB_EMPTY
DeleteMob(idx, Type!=0) → Hp=0, Mode=0, pMobGrid[y][x]=0, RemoveParty(idx), generator quota--
```

---

#### Business Rule: BR-02 — Mode enumeration versus implemented state machine

**Overview**
`CMob.h` declares ten modes; the running system implements five, of which only three participate in AI.

**Detailed description**
The declarations are `MOB_EMPTY 0`, `MOB_USERDOCK 1`, `MOB_USER 2`, `MOB_IDLE 3`, `MOB_PEACE 4`, `MOB_COMBAT 5`, `MOB_RETURN 6`, `MOB_FLEE 7`, `MOB_ROAM 8`, `MOB_WAITDB 9` (`CMob.h:26-35`). A whole-tree scan (`.cpp` + `.h`, excluding the un-built `ClientPatch_v7662`) returns exactly one occurrence — the `#define` itself — for `MOB_USERDOCK`, `MOB_RETURN`, `MOB_FLEE` and `MOB_ROAM`. `MOB_IDLE` appears twice: the `#define` and the `if (Mode == MOB_IDLE)` test at `ProcessSecMinTimer.cpp:1364`, but nothing ever assigns `MOB_IDLE`. Assignment sites are `MOB_PEACE` (11 sites, e.g. `Server.cpp:2712,2931,3154,3294`, `CMob.cpp:215,275`, `_MSG_Attack.cpp:718`), `MOB_COMBAT` (`Server.cpp:7236` only), `MOB_USER` (`_MSG_CharacterLogin.cpp:77`, `ProcessDBMessage.cpp:782,1211`), `MOB_WAITDB` (`_MSG_Quest.cpp:856`) and literal `0` / `MOB_EMPTY` (`DeleteMob`, `Server.cpp:7048`).

Therefore the effective machine is: `MOB_EMPTY` → (spawn) `MOB_PEACE` → (`SetBattle`) `MOB_COMBAT` → (`BattleProcessor` finds no target or leash) `MOB_PEACE` → (`DeleteMob`) `MOB_EMPTY`. `MOB_USER` marks an authenticated-but-not-spawned player entity (set during login/DB load) so that `SetBattle` will accept it as a target only when `pUser[target].Mode == USER_PLAY`. `MOB_WAITDB` marks a character mid-quest whose DB save is in flight (`_MSG_Quest.cpp:838,856`) and is a persistence barrier, not an AI state.

The "return" and "flee" behaviours described in the architecture report are real but are **not states**: return-to-anchor is performed inside `MOB_PEACE` by `GetNextPos(1)` after a leash break (`CMob.cpp:277`), and the closest thing to flee is the `rt | 0x10` signal emitted while a runner-type mob is waiting at a waypoint (`CMob.cpp:171-177`), which the caller turns into an `mNPCGen.FleeAction` chat line (`ProcessSecMinTimer.cpp:1496-1505`). Roaming is also stateless: `Processor & 0x1000` calls `GetRandomPos()` from inside peace handling (`ProcessSecMinTimer.cpp:1513-1532`).

**Rule workflow**
```
MOB_EMPTY --GenerateMob/CreateMob--> MOB_PEACE
MOB_PEACE --SetBattle(mob,target) within ±23--> MOB_COMBAT
MOB_COMBAT --CurrentTarget==0--> MOB_PEACE (CMob.cpp:215)
MOB_COMBAT --outside SegmentX/Y±16--> MOB_PEACE + clear EnemyList (CMob.cpp:270-282)
MOB_COMBAT --HP<=0--> DeleteMob(...,1) --> MOB_EMPTY
MOB_PEACE  --HP<=0--> DeleteMob(...,1) --> MOB_EMPTY
MOB_USER / MOB_WAITDB : no AI participation
(never assigned: MOB_USERDOCK, MOB_IDLE, MOB_RETURN, MOB_FLEE, MOB_ROAM)
```

---

#### Business Rule: BR-03 — Tick partitioning and cadence

**Overview**
Entity AI is not driven by a per-entity timer. It is a strided sweep over the whole 25,000-slot array, partitioned by `SecCounter % N` so that the work is spread across seconds.

**Detailed description**
`ProcessSecTimer()` (`ProcessSecMinTimer.cpp:40`) runs once per second. Two sweeps matter here. The out-of-combat sweep computes `int Sec6 = SecCounter % 6` and iterates `for (int index = Sec6 + MAX_USER; index < MAX_MOB; index += 6)` (1353-1357). The combat sweep computes `Sec4 = SecCounter % 4` and iterates `for (int index = Sec4 + MAX_USER; index < MAX_MOB; index += 4)` (1554-1560). Both start at `MAX_USER`, so **player entities never run `StandingByProcessor` or `BattleProcessor`**; their AI-adjacent work (regen, affects, quest recall) is done by separate, explicitly range-checked loops.

The consequence is a cadence contract baked into the timers: a monster is re-evaluated for patrol every 6 seconds and for combat every 4 seconds. `WaitSec` is expressed in seconds but decremented by `6` in the patrol path (`CMob.cpp:153,168`) precisely because each entity is visited once per 6 seconds. A `SegmentWait` of 30 therefore produces five visits of waiting. Any change to the stride silently rescales every authored NPC pause in the content files.

Also note `CMob::ProcessorSecTimer()` (`CMob.cpp:51-54`) is called for every playing user, once per second, from `ProcessSecMinTimer.cpp:223-224`, and its entire body is a commented-out `ProcessorCounter++`. The method and its `unsigned int ProcessorCounter` field (`CMob.h:100`) exist purely as vestigial state.

**Rule workflow**
```
every second:
  Sec6 = SecCounter % 6
  for index in [Sec6+1000, 25000) step 6:      # 4000 entities per second
      if Mode == MOB_PEACE: ProcessAffect; StandingByProcessor; decode bitmask
  Sec4 = SecCounter % 4
  for index in [Sec4+1000, 25000) step 4:      # 6000 entities per second
      if Mode == MOB_COMBAT: BattleProcessor; decode bitmask; attack
  for bb in [1,1000): if pUser[bb].Mode==USER_PLAY: pMob[bb].ProcessorSecTimer()  # no-op
```

---

#### Business Rule: BR-04 — HP-zero validation before AI

**Overview**
An entity whose current HP is zero must never be processed. Both sweeps enforce this before touching AI, and the enforced action is a *hard delete* with reason `1`, not a state transition.

**Detailed description**
In the patrol sweep, `MOB_IDLE` and `MOB_PEACE` each check `pMob[index].MOB.CurrentScore.Hp <= 0` and, if true, emit `Log("err,standingby processer delete hp zero mob")` and call `DeleteMob(index, 1)` (`ProcessSecMinTimer.cpp:1366-1371, 1378-1384`). In the combat sweep the same check exists at 1567-1576 but is more verbose: it forces `Hp = 0`, logs `err, battleprocessor delete hp 0 idx:%d leader:%d fol0:%d fol1:%d` with the leader and first two followers, then deletes.

The delete is `DeleteMob(index, 1)` with `Type == 1`, which per the protocol comment means "1 : morte" (`Basedef.h:1587`). `DeleteMob` broadcasts `_MSG_RemoveMob` with `RemoveType = 1` to the surrounding grid *before* mutating the entity, decrements the generator's `CurrentNumMob` (floored at zero) for non-player slots, zeroes HP, sets `Mode = 0`, clears the grid cell and calls `RemoveParty(conn)` (`Server.cpp:7020-7054`). Note the ordering: the broadcast uses `pMob[conn].TargetX/TargetY`, so position must still be valid at call time — which is why every delete call site happens before any teleport in the same tick.

This rule is a defensive invariant, not a gameplay rule: normal death goes through `MobKilled` (`MobKilled.cpp:41`), which sets `Hp = 0` and lets the caller decide on `DeleteMob`. The validation exists because the tick loop can observe a slot that was zeroed by an asynchronous path (a client attack, a scripted kill, a recall) between the `Mode` read and the processor call.

**Rule workflow**
```
read Mode
  idle/peace:  Hp<=0 ?  Log(err,...) -> DeleteMob(index,1) -> continue
  combat:      Hp<=0 ?  Hp=0 -> Log(err,battleprocessor...) -> DeleteMob(index,1) -> continue
otherwise: proceed to ProcessAffect + processor
```

---

#### Business Rule: BR-05 — Aggro acquisition from view window

**Overview**
A monster decides to enter combat only if it *sees* a hostile, and "seeing" is a rectangular scan over `pMobGrid` bounded by a hard-coded window and filtered by a static 9x9 clan relation matrix.

**Detailed description**
`GetEnemyFromView()` (`CMob.cpp:1229-1279`) defaults to `SizeX = SizeY = 9` with `StartX = TargetX - 4`, `StartY = TargetY - 4` — an inclusive 9x9 box centred on the mob. For `MOB.Clan == 7 || MOB.Clan == 8` the window becomes `SizeX = SizeY = HALFGRIDX/Y = 16` starting at `-6`, giving a -6..+9 box (asymmetric; the intent appears to be a 16-wide box anchored slightly behind). The scan iterates `y` then `x`, skips out-of-grid coordinates, reads `tmob = pMobGrid[y][x]`, skips its own tile, requires `pMob[tmob].MOB.CurrentScore.Hp > 0 && pMob[tmob].Mode`, skips hidden players (`tmob < MAX_USER && Rsv & 0x10`), and returns the **first** candidate whose clan relation is hostile.

Hostility is `g_pClanTable[MOB.Clan][pMob[tmob].MOB.Clan] == 0` (`Basedef.cpp:189-202`). The matrix is 9x9 and is not symmetric in row 0 vs others? It is symmetric in the data given, with `0` marking hostility: clan 1 is hostile to 0,3,7,8; clan 5 is hostile to 0,3,4,6,7,8; clan 8 is hostile to 1,5,7; clan 0 is hostile only to 1 and 5. Both clans are range-checked against `0..8` and a violation logs `err,clan out or range` and returns `0` (no aggro, no exception).

The return value is a raw entity index, which the caller turns into `enemy | 0x10000000` (`ProcessSecMinTimer.cpp:138-139`). That is the only place in the component where a target index is smuggled through the high bits of a behaviour mask. Because `0` is the "no enemy" sentinel and `0x10000000` alone would be `268435456`, the encoding is unambiguous in practice but undocumented.

**Rule workflow**
```
window = clan in {7,8} ? [-6,+9]^2 : [-4,+4]^2
for y in window, x in window:
   tmob = pMobGrid[y][x]
   if outside grid or (x,y)==self: continue
   if Hp<=0 or Mode==0: continue
   if tmob<MAX_USER and (Rsv & RSV_HIDE): continue
   if clan out of [0,8]: Log(err,clan out or range); return 0
   if g_pClanTable[self.Clan][other.Clan] == 0: return tmob
return 0
```

---

#### Business Rule: BR-06 — Aggro commitment (`SetBattle` contract)

**Overview**
Seeing is necessary but not sufficient. Combat starts only through `SetBattle`, which adds a proximity gate of ±23 grid units and a target-legality gate, then flips `Mode` and appends to the enemy list.

**Detailed description**
`SetBattle(int mob, int target)` (`Server.cpp:7218-7252`) begins with null/range guards (`mob <= 0 || target <= 0 || mob >= MAX_MOB || target >= MAX_MOB`), rejects `MOB_EMPTY` on either side, and rejects self-targeting. It then requires `mob >= MAX_USER && (target >= MAX_USER || pUser[target].Mode == USER_PLAY)`. That expression encodes two rules at once: only non-players can be the aggressor, and a player is only a legal victim if they are fully in the world (`USER_PLAY == 22`, `CUser.h:36`) — so a player still loading, trading into another state, or waiting on the DB cannot be aggroed.

The proximity gate is a Chebyshev box of ±23 (`7234`), a deliberate superset of the ±16 aggro window and of the ±16 leash box, presumably a safety factor for the 500 ms/1 s tick skew. On success the function sets `pMob[mob].Mode = MOB_COMBAT`, calls `pMob[mob].AddEnemyList(target)`, and then with 25% probability (`rand() % 4`) emits one of the four `mNPCGen.pList[generateindex].FightAction[say]` lines via `SendChat`, but only if `!pMob[mob].Leader` — i.e. only pack leaders talk. A generator index outside `[0, MAX_NPCGENERATOR)` returns early after the mode change, silently skipping the chat.

`SetBattle` is the universal entry point: the player attack handler calls it on both sides of a hit (`_MSG_Attack.cpp`), the mob attack path calls it on both sides (`ProcessSecMinTimer.cpp:1777-1801`), and the aggro path calls it for the mob plus every >`MAX_USER` party member of both sides (`ProcessSecMinTimer.cpp:1408-1462`). Summons and clan-4 entities are special-cased asymmetrically: `if (pMob[attacker].MOB.Clan != 4) SetBattle(attacker, TargetLeader)` — clan-4 (summon) entities do not reciprocate aggro, they are only ever the target of it.

**Rule workflow**
```
SetBattle(mob, target):
  guards: 0 < mob,target < MAX_MOB; both Mode != MOB_EMPTY; mob != target
  require: mob >= MAX_USER
  require: target >= MAX_USER || pUser[target].Mode == USER_PLAY
  require: |dx| <= 23 && |dy| <= 23
  -> Mode = MOB_COMBAT
  -> AddEnemyList(target)
  -> if generator valid and rand()%4 selects a line and Leader==0: SendChat(FightAction)
```

---

#### Business Rule: BR-07 — Enemy list collection semantics

**Overview**
The enemy list is a fixed 13-slot array of entity indices with implicit LRU-free ordering, duplicate suppression, and two immunity classes evaluated only for player targets.

**Detailed description**
`AddEnemyList(short TargetId)` (`CMob.cpp:306-363`) rejects `TargetId <= 0`; for `TargetId <= MAX_USER` it rejects targets with `MOB.Rsv & RSV_HIDE` (`0x10`, invisible) and targets with `MOB.Merchant & 1` (shop/marker NPCs flagged as merchants — the check is applied to the player-index range, so it protects player-slots repurposed as vendors, e.g. the item-shop avatars). It then scans for an existing identical entry and returns (no reordering, no recency update), scans for the first zero slot, and returns without inserting if the list is full. Insertion is therefore append-to-first-hole; **there is no eviction policy**, so once 13 enemies are recorded the 14th is silently ignored until entries are removed.

`RemoveEnemyList(short TargetId)` (`CMob.cpp:365-379`) nulls the first matching slot and returns; there is no compaction, so the list fragments.

The `GenerateIndex == KEFRA_BOSS` (`396`, `Basedef.h:388`) block (`CMob.cpp:334-362`) is intended to propagate the boss's enemy to nearby players so they share the encounter. As written it is broken: the loop over `i` in `[0, MAX_USER)` never uses `i` in the assignment — every iteration writes `EnemyList[c] = TargetId`, so the block can only ever re-add the same single target, and it `return`s immediately if that target is already present. The block performs no observable work beyond occasionally duplicating the boss's own target into a second slot.

Consumers prune the list: in `ProcessSecMinTimer.cpp:1593-1619` the combat sweep removes a target that shares the mob's `Leader` or shares its guild (`GuildDisable` forcing guild id 0, with both zeroed guilds normalised to `-1` to avoid false matches). In `_MSG_Attack.cpp:790-805` an invisibility spell retargets every enemy-list entry equal to the caster to the spell's `conn`.

**Rule workflow**
```
AddEnemyList(id):
  id<=0 -> return
  if id<=MAX_USER and (Rsv & RSV_HIDE) -> return
  if id<=MAX_USER and (Merchant & 1)   -> return
  if id already present -> return
  find first slot == 0; if none -> return (list full, no eviction)
  EnemyList[slot] = id

SelectTargetFromEnemyList (pruning pass, CMob.cpp:402-448):
  invalid index / MOB_EMPTY / Hp<=0 -> slot := 0
  player and (Rsv & 0x10) -> slot := 0
  player and Level > MAX_LEVEL and !(Merchant & 1) -> slot := 0
  outside radius -> slot := 0
```

---

#### Business Rule: BR-08 — Target selection policy

**Overview**
Among retained enemies, the current target is the **nearest by `BASE_GetDistance`**, inside a radius that varies by clan, map band and boss identity, with monsters made artificially less attractive than players.

**Detailed description**
`SelectTargetFromEnemyList()` (`CMob.cpp:381-468`) first clears `CurrentTarget`, initialises a parallel `Enemy[13]` array to `MAX_USER` (999, used as "infinity") and picks `dis = 6`. Three overrides follow: `dis = 12` when `MOB.Clan` is 4, 7 or 8 (summon, and the two large-view clans); `dis = 8` when `(TargetX / 128) < 12 && (TargetY / 128) > 25` — a 128x128 map-cell band, i.e. a specific authored region; and `dis = HALFGRIDX = 16` for `GenerateIndex == KEFRA_BOSS`. The overrides are not exclusive in intent but the order makes them so: KEFRA wins, then the map band, then clan.

For each slot the function drops dead references (index out of range, `MOB_EMPTY`, `Hp <= 0`), branches around the player-only filters for non-players, drops invisible players and drops players whose `BaseScore.Level > MAX_LEVEL` **unless** they are `Merchant & 1` (a deliberate exception allowing vendors/GM avatars to be attacked). It then applies the radius as an axis-aligned box (`>= X-dis && <= X+dis`, same for Y) and, if inside, stores `BASE_GetDistance(...)` as the ranking key, adding **+2** for `myenemy > MAX_USER` so that monsters are deprioritised relative to players at equal distance. Entries outside the box are erased from the list.

The final selection uses `if (NoTarget >= Enemy[i])`. Because the comparison is `>=`, ties resolve to the **last** slot in index order, not the first — the list order therefore determines tie-breaking, and since `AddEnemyList` appends to the first hole, ties favour the most recently appended hole, which is roughly insertion order. `NoTarget` stays at `MAX_USER` only if no slot was ever scored, in which case `CurrentTarget = 0` and `BattleProcessor` immediately drops the mob to `MOB_PEACE` (`CMob.cpp:213-218`).

**Rule workflow**
```
radius = 6
  clan in {4,7,8} -> 12
  (X/128)<12 and (Y/128)>25 -> 8        # overrides clan
  GenerateIndex == KEFRA_BOSS -> 16     # overrides both
for each of 13 slots:
   drop invalid/dead
   players: drop Rsv&0x10; drop Level>399 unless Merchant&1
   if inside ±radius box: score = distance + (isMonster ? 2 : 0)
   else: erase slot
pick slot with smallest score, ties -> last slot
no score -> CurrentTarget = 0 -> Mode = MOB_PEACE
```

---

#### Business Rule: BR-09 — Leash / disengage boundary

**Overview**
A mob that strays more than 16 grid units from its current segment waypoint abandons the fight, erases its enemy list and walks back — but the anchor is the waypoint, not the spawn point.

**Detailed description**
The rule lives in `BattleProcessor` at `CMob.cpp:268-283`. The guard is `RouteType != 5 && (TargetX > SegmentX + HALFGRIDX || TargetX < SegmentX - HALFGRIDX || TargetY > SegmentY + HALFGRIDY || TargetY < SegmentY - HALFGRIDY)`, i.e. a ±16 Chebyshev box around the *segment waypoint*. On trigger it sets `CurrentTarget = 0`, zeroes all 13 enemy slots, sets `Mode = MOB_PEACE`, calls `GetNextPos(1)` (the battle variant), and returns `0` if the computed next position already equals the current position, else `16` (which the caller treats as "send a continuation action message", `ProcessSecMinTimer.cpp:1998-2014`).

The critical nuance is that `SegmentX/SegmentY` is the waypoint the mob is currently walking toward, not the spawn tile. `GenerateMob` sets `SegmentX = SegmentListX[0]` at spawn (`Server.cpp:3199`) and `SetSegment` advances both `SegmentProgress` and `SegmentX/Y` as the patrol proceeds (`CMob.cpp:591-592`). Where `SegmentX/Y` is redefined, the leash moves with it: for summons, `StandingByProcessor` overwrites `SegmentX/Y` with the owner's position (`CMob.cpp:120-121`), which is how pets get an owner-relative leash; and for `RouteType == 6` the anchor is fixed because `SetSegment` immediately restores waypoint 0 (`CMob.cpp:472-480`). Consequently a patrolling mob has a **rolling** engagement boundary, and a long patrol route can effectively chase a player across the map as long as it is within 16 tiles of the next waypoint. No rule ties the entity back to `GenerateIndex`'s authored origin.

There is a second, independent disengage path: if `SelectTargetFromEnemyList` yields `CurrentTarget == 0`, the mode drops to peace without any leash arithmetic (`CMob.cpp:213-218`). That is the normal exit when all enemies died or left the radius.

**Rule workflow**
```
after target selection:
  if RouteType != 5 and mob outside (SegmentX±16, SegmentY±16):
      CurrentTarget = 0
      EnemyList[0..12] = 0
      Mode = MOB_PEACE
      GetNextPos(battle=1)
      return (NextX==TargetX && NextY==TargetY) ? 0 : 16
```

---

#### Business Rule: BR-10 — Intelligence-based action throttle

**Overview**
`BaseScore.Int` is not only a magic-damage stat: it is the probability, in percent, that a monster takes any combat action at all on a given combat tick.

**Detailed description**
`CMob.cpp:255-260` reads `int BaseInt = MOB.BaseScore.Int;` and returns `0x010000` when `BaseInt < rand() % 100`. `rand() % 100` is uniform on `[0, 99]`, so the mob acts with probability approximately `Int / 100` and skips with `1 - Int/100`. An `Int` of 0 gives a 1-in-100 chance of acting; an `Int` of 99 gives 99-in-100; an `Int` of 100+ always acts. Because the return value `0x010000` shares no bits with any behaviour the caller decodes in the combat loop (`0x20`, `0x1000`, `0x100`, `1`, `2`, `0x10`), the effect is a clean "do nothing this tick".

This is the component's model of reaction speed and stupidity, and it interacts multiplicatively with the 4-second combat cadence: an `Int`-10 monster effectively attacks once per ~40 seconds, while an `Int`-90 monster attacks nearly every 4 seconds. It also gates the leash check? No — the leash check happens *before* the Int gate at line 268, so a low-Int monster still disengages promptly; but it is placed *after* the pet validation block, so pets with low `Int` will also idle in place while their owner fights.

The rule has no upper clamp and no configuration surface: it reads the persisted `BaseScore.Int` directly from the entity sheet, so NPC balance is a content-data concern, not a code constant. Note that `Int` is also consumed by summon scaling in `GenerateSummon` (`Server.cpp:2690`), so rebalancing one changes the other.

**Rule workflow**
```
BaseInt = MOB.BaseScore.Int
if BaseInt < rand()%100:
     return 0x010000        # no behaviour bits -> caller does nothing
proceed to leash check, range check, attack
```

---

#### Business Rule: BR-11 — Dexterity-based standoff

**Overview**
A ranged attacker that lets a target close inside 4 tiles may step back instead of firing, with the decision probability driven by `BaseScore.Dex`.

**Detailed description**
Inside the range branch (`CMob.cpp:284-289`) the code draws `int Rand = rand() % 100;` and then: `if (Range >= 4 && dis <= 4 && Rand > BaseDex) return 256;`. `Range` is `EF_RANGE` (effect id 27, `ItemEffect.h:66`) for normal mobs or hard-coded `25` for the KEFRA boss. `256` is `0x100`, which the caller maps to `GetTargetPosDistance(CurrentTarget)` followed by an action broadcast (`ProcessSecMinTimer.cpp:1955-1973`) — that primitive moves the mob **away** from the target (it inverts the delta on both axes, `CMob.cpp:878-887`), so the behaviour is a retreat/kite step, not an approach.

The probability is inverted from intuition: `Rand > BaseDex` means a **high** Dex monster retreats **less** often, i.e. Dex models "discipline" / willingness to stand and shoot rather than panic. A Dex of 100+ never retreats. The rule only applies when the mob actually has range (≥4), so melee monsters are unaffected.

This is also the only place where the `256` return value can be produced by the range branch; the branch immediately below it (`291-299`) computes `bx = tx; by = ty;` then tests `tx == bx && ty == by`, which is tautologically true, so the function always returns `4096` (attack) and the `else return 256` is unreachable. The commented-out `BASE_GetHitPosition` call at line 294 shows that a hit-position adjustment used to exist and was removed, leaving the comparison dead.

**Rule workflow**
```
if dis <= Range:
    Rand = rand()%100
    if Range >= 4 and dis <= 4 and Rand > BaseDex:
        return 256                 # 0x100 -> GetTargetPosDistance (retreat step)
    # dead code: bx/ty copies, always equal
    return 4096                    # 0x1000 -> attack
return 1                           # 0x01 -> approach via GetTargetPos
```

---

#### Business Rule: BR-12 — Attack resolution and cadence

**Overview**
`BattleProcessor` never deals damage. It only decides "attack now"; the caller performs the full hit pipeline, including parry, shields, affects, aggro propagation and death.

**Detailed description**
The attack signal is the single value `4096` (`0x1000`). The caller (`ProcessSecMinTimer.cpp:1629-1942`) then: guards that `Target` is in range and alive; writes a diagnostic file entry when a non-player attacks a player inside a village whose map attribute lacks bits `0x4`/`0x40` (`1650-1657`); calls `GetAttack(index, Target, &sm)` to build the `_MSG_AttackOne` payload (mob skill selection is driven by `MOB.SkillBar[0..3]` with distance determining which pair of slots is used, `GetFunc.cpp:1307-1365`); verifies `GetInHalf` and aborts otherwise; applies skill affects for skills where `kind = (skill % MAX_SKILL / 8) + 1` is in `1..3`; computes parry from `GetParryRate(&pMob[Target].MOB, pMob[Target].Parry, attackdex, pMob[index].MOB.Rsv)` with `attackdex = Dex/5` plus `+100` for learned skill bit 24 and `+500` for `Rsv & 0x40`, plus a 70% reduction for skills 79 and 22; applies a 40% damage reduction for clan-4 attackers; calls `SetBattle` in both directions on positive damage; propagates battle to every `>MAX_USER` party member of the target's leader; applies affect type 18 (mana shield) which converts damage into MP at a 50% rate with an 80-divisor rescale; applies fairy 786 (half per 2 sanc), 1936 (per 20 sanc), 1937 (per 2000 sanc) damage divisors; clamps HP to zero; broadcasts the attack; updates `ReqHp`; and finally calls `MobKilled` when HP reaches zero.

Cadence is therefore `4 seconds × (1 / P(Int gate)) × (1 / P(Dex gate when applicable))`. There is no explicit attack cooldown field, no "next attack time" timestamp and no per-target attack timer in `CMob`; the only cadence control is the stride of the loop and the two random gates. `LastTime` (`CMob.h:48`) is written at spawn (`Server.cpp:3160`) but never read anywhere in the component.

**Rule workflow**
```
return 4096 from BattleProcessor
  -> caller: GetAttack -> skill selection -> parry roll -> clan-4 scaling
  -> SetBattle(target, attacker); SetBattle(attacker, target) unless attacker.Clan==4
  -> party propagation (both directions, >MAX_USER members only)
  -> affect 18 mana shield -> fairy 786/1936/1937 divisors -> HP -= damage
  -> GridMulticast(_MSG_AttackOne) -> ReqHp update
  -> HP <= 0 -> MobKilled(Target, index, 0, 0)
```

---

#### Business Rule: BR-13 — Pet / summon ownership contract

**Overview**
An entity is a pet if `RouteType == 5` or if its first affect slot is type `24`. Pets are bound to a `Leader` and a `Summoner` and are deleted the moment that binding is invalid.

**Detailed description**
`GenerateSummon` (`Server.cpp:2572` onward) sets `RouteType = 5`, `Formation = 5`, `Leader = Leader`, `GenerateIndex = -1`, `Mode = MOB_PEACE`, and — only when no summon item is involved (`sItem == 0`) — sets `Affect[0].Type = 24` with `Time = 20` (or `2000000000` for summon ids 28..37, i.e. effectively permanent). The affect is decremented in the generic affect sweep and, when it reaches zero, `if (Affect[i].Type == 24 && idx >= MAX_USER) DeleteMob(idx, 3)` (`Server.cpp:5388-5389`). So pets have a lifetime of 20 affect ticks; permanent summons bypass expiry by using a 2-billion-second timer.

`StandingByProcessor` treats either marker identically (`CMob.cpp:60`). If `Leader == 0`, it returns `0x100` → `DeleteMob` (orphan pet). If `Leader` is outside `[1, MAX_USER)`, it returns `1` (invalid; no action). When the marker is present it validates the chain: `Summoner` must be a valid user index; `Leader == Summoner` **or** `Summoner` appears in `pMob[Leader].PartyList[0..11]`; and `pUser[Summoner].Mode == USER_PLAY`. Any failure returns `1` (no action, but not deletion), except the `Summoner` range failure which also returns `1`. Only then does distance resolution happen: `>= 13` → teleport the pet to the owner's tile (`NextX/NextY = owner` and return `0x02`), `<= 4` → idle (`return rt` with `rt` possibly already `0x100`), otherwise set `SegmentX/Y` to the owner's position and `GetTargetPos(summoner)` with return `0x01`.

`BattleProcessor` repeats a near-identical validation (`CMob.cpp:220-253`) but with different failure codes: an invalid `Leader` returns `32` → the caller deletes the pet (`ProcessSecMinTimer.cpp:1623-1627`); an invalid `Summoner`, non-party `Summoner`, or failed membership returns `256` → the pet repositions toward its target instead of being deleted; and distance `>= 20` from the owner makes the pet teleport to the owner and return `2`. The asymmetry between the two processors (delete vs. reposition, 13 vs. 20) is a genuine inconsistency in the contract.

`MobKilled` further special-cases clan-4 killers: it resolves `Summoner` from the killer, and if the chain is broken it broadcasts the kill and deletes the target without awarding anything (`MobKilled.cpp:190-205`).

**Rule workflow**
```
isPet = (RouteType == 5) || (Affect[0].Type == 24)
peace tick:
   Leader==0 -> 0x100 (delete)
   Leader invalid -> 1
   marker present:
       Summoner invalid -> 1
       Leader==Summoner or Summoner in Leader.PartyList -> ok
       Summoner not USER_PLAY -> 1
       dist >= 13 -> Next = owner, 0x02 (teleport)
       dist <= 4  -> idle
       else       -> Segment = owner pos; GetTargetPos(owner); 0x01
combat tick:
   Leader invalid -> 32 (delete)
   Summoner invalid / not in party -> 256 (reposition)
   dist >= 20 -> Next = owner, 2 (teleport)
affect expiry (Type 24) -> DeleteMob(idx,3)
```

---

#### Business Rule: BR-14 — Path-segment model and route types

**Overview**
Navigation is a five-waypoint scripted tour with per-waypoint dwell time, per-waypoint spawn jitter and three direction disciplines, selected by a `RouteType` integer authored per NPC generator.

**Detailed description**
`CMob` carries `Segment int`, `SegmentX/Y`, `SegmentListX[5]`, `SegmentListY[5]`, `SegmentWait[5]`, `SegmentRange[5]`, `SegmentDirection`, `SegmentProgress` (`CMob.h:58-66`). `MAX_SEGMENT` is 5 (`CNPCGene.h:27`) and the generator's `NPCGENLIST` holds the same five slots plus per-slot `SegmentAction` strings (`CNPCGene.h:37-44`). `GenerateMob` copies each waypoint, jittering both axes by `-SegmentRange[i] .. +SegmentRange[i]` when a range is authored (`Server.cpp:3089-3113`), and copies `SegmentWait[i]`. Followers get `g_pFormation[i][j][Formation]` offsets added to the leader's waypoint (`3267-3269`), so a formation is a rigid translation of the leader's route.

`SegmentProgress` is the index of the waypoint being approached; `SegmentDirection` is `0` for forward and non-zero for backward. `SetSegment` (`CMob.cpp:470-596`) is the advance function and is a hand-rolled state machine over `SegmentProgress` (`-1`, `0..4`, `5`). It advances the index by ±1, then handles the three edge positions: `-1` (underflow past waypoint 0), `5` (overflow past waypoint 4), and the normal case (skip empty waypoints by `continue`ing when `SegmentListX[SegmentProgress] == 0`). The return value is overloaded: `0` = move to the new segment, `1` = stop/idle, `2` = cycle boundary reached (caller emits a `SegmentAction` chat line and continues).

Route semantics as implemented: type 0 = cyclic with an appearance refresh at wrap (BR-15); type 1 = cyclic, stop at boundary; type 2 = ping-pong (direction flips at `5`); type 3 = one-shot, despawn at `-1`; type 4 = cyclic, skips to `-1` at `5`; type 5 = summon (handled before the segment machine ever runs); type 6 = fixed anchor, always resets to waypoint 0 with `WaitSec = 0`. The guard at `483` (`if (RouteType <= 0 && RouteType > 4)`) is unsatisfiable — a range check written with `&&` instead of `||` — so an out-of-range route type is never rejected and falls through to the loop with no matching `if`, spinning until it finds a non-empty waypoint.

Three out-of-bounds conditions exist in this function. For `RouteType == 1` reaching `SegmentProgress == 5`, the loop breaks with `iterator = 2` and `< 5` while `SegmentProgress` is still `5`, then reads `SegmentListX[5]`/`SegmentListY[5]` at lines 591-592. For `RouteType == 3` reaching `-1`, the loop breaks the same way and reads index `-1`. For `RouteType == 0` at `5` the code correctly rewinds to `4` before falling through. `ProcessSecMinTimer.cpp:1401` re-sanitises with `Progress < 0 || Progress > MAX_SEGMENT` — but `MAX_SEGMENT` is 5 and the valid maximum is 4, so `5` is allowed through and can reach `SegmentWait[5]` at `CMob.cpp:159`.

**Rule workflow**
```
SetSegment():
  RouteType == 6 -> SegmentProgress=0, Direction=0, Segment=List[0], WaitSec=0, return 0
  loop:
    Progress += (Direction==0 ? +1 : -1)
    Progress == -1:
        type0 -> Progress=0, Direction=0, iterator=2, Log(...), break
        type1 -> Progress=0, Direction=0, continue reading forward
        type2 -> Progress=0, Direction=0, continue
        type3 -> iterator=2, break            # OOB read follows
        type4 -> Log(...), break              # OOB read follows
    Progress == 5:
        type0 -> Progress=4, Mode=4, Merchant rewrite, GetCurrentScore(MAX_USER), iterator=1, break
        type1 -> iterator=2, break            # OOB read follows
        type2,3 -> Progress=4, Direction=1, continue
        type4 -> Progress=-1, continue
    ListX[Progress]==0 -> continue            # skip empty waypoint
    iterator=0, break
  SegmentX = ListX[Progress]; SegmentY = ListY[Progress]; WaitSec = 0
  return iterator
```

---

#### Business Rule: BR-15 — Route type 0 appearance rewrite

**Overview**
At the end of a type-0 cycle the mob recomputes its derived stats and mutates its own `Merchant` byte using a nibble decoded from the ASCII `Route` string — a content-authored cosmetic/vendor-state side channel.

**Detailed description**
When `SegmentProgress` reaches `5` with `RouteType == 0` (`CMob.cpp:534-561`) the code rewinds to waypoint 4, assigns the raw literal `Mode = 4` (i.e. `MOB_PEACE`, but written as a magic number rather than the constant used everywhere else), copies `MOB.Merchant` into `MOB.BaseScore.Merchant`, and then reads `int len = strlen(Route)` and `rt = Route[len]`. That read is one byte past the NUL terminator — the buffer is `char Route[MAX_ROUTE]` (24 bytes) and the read is within the array as long as `len <= 23`, but the value is undefined for short strings because it is the byte following the terminator. It then computes `rt = (rt & 0xFF) - 48` and, if the result is in `1..9`, shifts it left by four and ORs it into `MOB.Merchant`.

So the low nibble of `Merchant` survives and the high nibble becomes a digit taken from the byte after the route string. `Merchant` is used elsewhere as a role discriminator (`Merchant == 2` for warp NPCs in `_MSG_TradingItem.cpp:54`, `Merchant == 6` for tax collectors in `Server.cpp:3190`, `Merchant & 1` immunity in `AddEnemyList`), so this rule can flip an NPC's role at runtime. Finally `GetCurrentScore(MAX_USER)` recomputes all derived stats with the mob branch.

Because `RouteType == 0` is also the only type that stops at the cycle end (`iterator = 1` → the caller stops moving instead of continuing), a type-0 NPC is effectively a stationary spawn that periodically rebuilds its sheet. The rule is content-driven and undocumented in the source; no README or spec describes the `Route`-string nibble protocol.

**Rule workflow**
```
at Progress==5 and RouteType==0:
   Progress = 4; Mode = 4 (MOB_PEACE); BaseScore.Merchant = Merchant
   len = strlen(Route)
   if len > 0:
       c = Route[len]            # byte after NUL, undefined-by-construction
       d = (c & 0xFF) - 48
       if 1 <= d <= 9: Merchant |= ((d << 4) & 0xFF)
   GetCurrentScore(MAX_USER)
   return 1                      # caller stops
```

---

#### Business Rule: BR-16 — Route type 3 one-shot despawn

**Overview**
A one-shot route terminates the entity. When the last waypoint is reached and the dwell timer has expired, `StandingByProcessor` signals `0x10000`, which the caller turns into `DeleteMob(index, 3)`.

**Detailed description**
The rule is split across two sites. In `StandingByProcessor`, when the mob is standing exactly on its current segment (`SegmentX == TargetX && SegmentY == TargetY`), the first sub-branch is `if (SegmentProgress == 4 && RouteType == 3)`: if `WaitSec <= 0` return `0x10000`; otherwise decrement `WaitSec` by 6 and return `0` (`CMob.cpp:148-156`). In the caller, `if (Processor & 0x10000)` increments the pista-6 mob counter when the entity belongs to the `RUNEQUEST_LV6_MOB_INITIAL..END` range and the counter is between 1 and 99, then `DeleteMob(index, 3)` and `continue` (`ProcessSecMinTimer.cpp:1536-1546`).

`Type == 3` is the "scripted removal" reason (as opposed to 1 = death, 2 = logout, per `Basedef.h:1587`). It still decrements the generator quota, so a one-shot NPC's slot is returned to its spawner. The counter increment is the quest bookkeeping that decides whether the rune-track encounter is complete; it is guarded by `MobCount != 0` so that a disabled pista (count 0) is never accrued.

Note the interaction with `0x010000` from `BattleProcessor`: both values are numerically adjacent and both are returned from the same two-processor family, but they live in different decode contexts (peace loop vs combat loop). Reading `0x10000` as a combat return would be `65536 & 0x1000 = 0` → silent no-op, not a delete.

**Rule workflow**
```
peace tick, standing on waypoint:
   if SegmentProgress==4 and RouteType==3:
        WaitSec <= 0 -> return 0x10000
        else         -> WaitSec -= 6; return 0
caller:
   if Processor & 0x10000:
        if GenerateIndex in RUNEQUEST_LV6 range and Pista[6].Party[0].MobCount in (0,100): MobCount++
        DeleteMob(index, 3); continue
```

---

#### Business Rule: BR-17 — Route type 6 fixed-anchor (return) behaviour

**Overview**
`RouteType == 6` is the only route type that disables waiting entirely and snaps the entity to waypoint 0 as soon as a segment advance is requested — the closest implementation of a "return home" discipline.

**Detailed description**
Three sites cooperate. `StandingByProcessor` short-circuits when the mob is already on its segment: `if (RouteType == 6 && TargetX == SegmentX && TargetY == SegmentY) return 0;` (`CMob.cpp:143-144`) — no waiting, no advance. The wait branch treats `RouteType == 6` as if `WaitSec` were already expired (`CMob.cpp:157`), so the dwell timer never applies to it. And `SetSegment` begins with `if (RouteType == 6) { SegmentProgress = 0; SegmentDirection = 0; SegmentX = SegmentListX[0]; SegmentY = SegmentListY[0]; WaitSec = 0; return 0; }` (`CMob.cpp:472-481`).

The practical effect is a guard-post entity that never leaves waypoint 0, never pauses, and simply re-establishes waypoint 0 whenever the segment machine runs. Because `SegmentX/Y` is also the leash anchor, a route-6 entity has a genuinely fixed engagement boundary — the only entity class in the component with that property (type 0 is fixed only between cycles; type 5 follows the owner; types 1-4 roll with the patrol).

`CreateMob` supports this directly: `pMob[tmob].WaitSec = Type == 0 ? 10 : 40;` and `pMob[tmob].SegmentProgress = Type == 0 ? 0 : 4;` (`Server.cpp:2936,2945`), so a hand-placed NPC created with `Type == 6` starts at progress 4 and immediately resets to 0 on its first segment call.

**Rule workflow**
```
peace tick:
   if on waypoint and RouteType==6: return 0
   if RouteType==6: treat WaitSec as expired
   advance -> SetSegment() -> progress=0, direction=0, segment=list[0], wait=0
```

---

#### Business Rule: BR-18 — Movement budget and immobilisation

**Overview**
Whether a mob can move at all, and how far per tick, is derived from the low nibble of `CurrentScore.AttackRun`; the derived value is clamped to 1..6 and multiplied by `SECBATTLE / 4`.

**Detailed description**
All five movement primitives begin with the same two guards. First, `if (MOB.Equip[0].sIndex == 219 || MOB.Equip[0].sIndex == 220)` in `GetTargetPos` and `GetNextPos` (and `GenerateIndex == KEFRA_BOSS` additionally in `GetNextPos`, `CMob.cpp:975-980, 1156-1161`) forces `NextX/NextY = TargetX/TargetY` — the entity is **immobile** if it wears face 219 or 220 (castle gate / war tower per the architecture report) or is the Kefra boss. Second, `if ((MOB.BaseScore.AttackRun & 0xF) == 0)` forces the same no-op — `AttackRun` low nibble zero means "stationary". `GetTargetPosDistance` and `GetRandomPos` implement only the second guard (`859-865`, `923-929`).

Otherwise `BASE_GetSpeed(&MOB.CurrentScore)` returns `AttackRun & 15` clamped into `[1,6]` (`Basedef.cpp:1210-1223`) and the per-tick budget is `distance = speed * SECBATTLE / 4` where `SECBATTLE` is `8` (`Basedef.h:55`, and the code comment at `CMob.cpp:868` says "SECBATTLE(?) = 8"). With `speed = 1..6` the budget is `2, 4, 6, 8, 10, 12` tiles per call. It is capped at `MAX_ROUTE - 1 = 23` (never binding, since the max is 12). `GetTargetPos` adds a flat `+2` to speed before the calculation (`991`), i.e. it is the "chase" primitive and always moves faster than `GetNextPos` (the "patrol" primitive) for the same entity — but the `+2` is added before the clamp, so a speed-6 entity gets `distance = 8*8/4 = 16`, exceeding the 12 of a normal move.

`GetNextPos` contains a dead computation: in the `battle` branch it computes `str = Str + 1; res = str; if (res > distance) res = distance;` and then never uses `res` (`CMob.cpp:1176-1183`). The `else` branch does bind: `if (distance > MOB.BaseScore.Str) distance = MOB.BaseScore.Str;` (`1186-1187`), so out of combat a mob's step is capped by its Strength — a low-Str NPC shuffles, a high-Str NPC strides. Because `distance` can become `0` there, `BASE_GetRoute(..., distance=0, ...)` is called with a zero budget and returns immediately, leaving `NextX/NextY` at the segment target; the subsequent `NextX != TargetX` test in the caller then reports "no movement".

**Rule workflow**
```
immobile if: face 219/220, or KEFRA_BOSS (GetNextPos), or (AttackRun & 0xF) == 0
speed = clamp(AttackRun & 0xF, 1, 6)
distance = speed * 8 / 4                # 2..12
GetTargetPos:        speed += 2 first   # up to 16
GetNextPos(0):       distance = min(distance, BaseScore.Str)
GetNextPos(1):       (dead Str clamp, no effect)
if distance >= MAX_ROUTE: distance = MAX_ROUTE - 1
```

---

#### Business Rule: BR-19 — Occupancy and terrain passability

**Overview**
A move is only emitted to the client if the destination cell is free of entities and not marked as impassable height `127`; if the direct route is blocked the search escalates outward, and if every attempt fails the move is cancelled.

**Detailed description**
All four navigation primitives call `GetEmptyMobGrid(0, &NextX, &NextY)` before pathing and again inside the retry loop. `GetEmptyMobGrid` (`GetFunc.cpp:1901-2001`) first rejects out-of-range coordinates with `Log("GetEmptyMobGridOut of range")`, then accepts if the cell already holds `mob` (self), or if `pMobGrid == 0 && pHeightGrid != 127`. Otherwise it scans square rings of radius 1, then 2, 3 and 4 (three separate nested loops, not a loop over radius), returning the first free cell and mutating `tx/ty` in place; if all rings fail it returns `FALSE`. Ring scanning is row-major within the ring, so displacement is biased toward the top-left of the ring.

On top of that, each primitive runs `for (i = distance; i >= 0; i--) { BASE_GetRoute(LastX, LastY, &NextX, &NextY, Route, i, pHeightGrid); if (pMobGrid[NextY][NextX] == 0) break; ... }` — i.e. it asks the height-aware pathfinder for a path of length `i`, and if the resulting endpoint is occupied it retries with a fresh empty-grid resolution and the same `i`, and otherwise decrements `i`. If the loop exhausts (`i == -1`) or the path string is empty (`Route[0] == 0`), the primitive sets `NextX/NextY` back to the mob's current `TargetX/TargetY` and clears `Route[0]`, which the caller interprets as "did not move".

`BASE_GetRoute` (`Basedef.cpp:5538-5826`) is a greedy 8-direction stepper constrained by `MH = 8` height tolerance: at each step it reads the nine height samples around the current tile and only takes a direction whose neighbour height is within `(cul - MH, cul + MH)`. It writes direction characters `'1'..'9'` (numeric keypad layout) into `Route`, stops at the target or on an out-of-bounds heightmap access, and returns `1` if it moved at all. A directional asymmetry is visible in the branch ladder: `tx > x && ty < y` is tested for north-east twice (at 5584 and again at 5651), and several combinations (`tx < x && ty > y` first with `sw`, later with `w` and `s`) are duplicated with different neighbour constraints — the ladder is a decompiler-reconstructed decision tree, not a clean algorithm.

**Rule workflow**
```
NextX/NextY = desired destination
GetEmptyMobGrid(0, &NextX, &NextY)   # 0, self, or first free cell in rings 1..4
if GetEmptyMobGrid == FALSE -> caller: continue (no move emitted)
for i = distance downto 0:
    BASE_GetRoute(LastX, LastY, &Next, Route, i, pHeightGrid)
    if pMobGrid[NextY][NextX] == 0: break
    if i != distance: re-resolve empty grid; retry same i
if i == -1 or Route[0] == 0: Next = current Target; Route[0] = 0
caller: if Next == Target -> continue (no action packet)
        else GetAction + GridMulticast(_MSG_Action)
```

---

#### Business Rule: BR-20 — Derived stat recomputation (`GetCurrentScore`)

**Overview**
One function derives every combat cache for both players and monsters, with sharply different branches and several hard-coded item-index bonus tables.

**Detailed description**
For player indices (`idx < MAX_USER`) the function first zeroes `pMob[idx].MOB.Clan` — a side effect on a *different* concern (clan membership) executed as part of scoring — then, only when `BaseScore.Level < 2000`, clamps each of the four resists to `min(value, 100)` (the clamp is applied twice, once via the ternary and once via four explicit `if` blocks at 610-620), clears `Equip[0]`'s first effect, and zeroes `MOB.Rsv`. For mob indices it reads the four resists from `mNPCGen.pList[GenerateIndex].Leader.Resist[]` and — notably — sets any value **greater than 100 to 50**, not to 100, so an over-capped mob resist becomes a mid-range resist. Then `MOB.Rsv = 0`.

The shared part sets `Parry = EF_PARRY` (40), and for players writes `pUser[idx].Range` twice — first from `EF_RANGE`, then unconditionally to `23` (`661-662`), making the first assignment dead. It resets the seven bonus accumulators and calls the monolithic `BASE_GetCurrentScore(...)` with the player/mob flag as `idx >= MAX_USER`; then it applies six hard-coded fairy bonuses keyed on `Equip[13].sIndex` (3900 = +16 exp; 3901 = +32 drop; 3902/3905/3908 = +32 exp +16 drop; 3903/3906/3911/3912/3913 = +16 exp; 3904/3907 = +32 exp), a +50 accuracy for learned skill bit 28 ("Concentração"), negative-resist flooring for players, and HP/MP clamps to max.

Weapon damage uses dual-wield averaging: `w1`, `w2` from `Equip[6]`/`Equip[7]` `EF_DAMAGE`, halved into `fw1`/`fw2`, and then `WeaponDamage = max-side + half-of-other-side` (`751-754`). Two class skills restore the full off-hand value instead of the half: skill bit 10 for class 3 ("Perícia do Caçador") and skill bit 9 for class 0 ("Mestre das Armas"). Class 2 gets reflect damage `(Special[3]+1)/6` on skill bit 17 and an AC bonus `(EF_AC+1)/7` on skill bit 19 when `Equip[7]` has `nPos == 128`. Weapons with `nPos` 64 or 192 and sanctification ≥ 9 add a flat `+40` each (the guard `if (idx1 >= 0 || idx1 < MAX_ITEMLIST)` is always true — `||` should be `&&`).

For all 16 equipment slots the loop applies grade bonuses (`Grade 5` +8 drop, `Grade 6` force damage, `Grade 7` +2 exp, `Grade 8` +20 reflect) and gem bonuses (`gem 0` +8 drop; `gem 1` force damage `(Grade==6 ? 80 : 40) * isanc`; `gem 2` +2 exp; `gem 3` reflect `(Grade==8 ? 80 : 40) * isanc`), where `isanc` maps `REF_10..REF_15` (10/12/15/18/22/27, `Basedef.h:196-201`) to `1..6`. Note `if (ItemId <= 0 || ItemId > MAX_ITEMLIST) continue;` is checked *before* indexing `g_pItemList`, so the bound is `> MAX_ITEMLIST` rather than `>=`, a one-past-the-end read at exactly `MAX_ITEMLIST`.

Finally PvP values: `PvPDamage = (EF_HWORDGUILD + 1) / 10` and `ReflectPvP = (EF_LWORDGUILD + 1) / 10`.

**Rule workflow**
```
if idx < MAX_USER: Clan = 0
if idx < MAX_USER and BaseScore.Level < 2000:
    Resist[i] = min(EF_RESISTi, 100); Equip[0].effect0 = 0; Rsv = 0
else if idx >= MAX_USER: Resist[i] = generator.Leader.Resist[i]; if >100 -> 50; Rsv = 0
Parry = EF_PARRY
if idx < MAX_USER: pUser[idx].Range = 23
reset ExpBonus/DropBonus/ForceDamage/ReflectDamage/ForceMobDamage/Accuracy/HpAbs
BASE_GetCurrentScore(MOB, Affect, extra, &ExpBonus, &ForceMobDamage, isMob, &Accuracy, &HpAbs, &ForceDamage)
apply fairy-index bonuses, skill bit 28 accuracy, resist flooring, HP/MP clamps
WeaponDamage = dual-wield formula (+40 per anced nPos 64/192)
for 16 slots: grade bonus, gem bonus * sanc tier
PvPDamage / ReflectPvP from EF_HWORDGUILD / EF_LWORDGUILD
```

---

#### Business Rule: BR-21 — Level-up and EXP segmentation (`CheckGetLevel`)

**Overview**
Progression is driven by a four-segment EXP ladder per level, with a separate ceiling for class-master tiers and hard quest gates that silently block level-up.

**Detailed description**
`CheckGetLevel` (`CMob.cpp:1034-1152`) returns `0` for no change, `1..3` when the character crosses a quarter-segment (used to emit the level-up *effect* and to refill HP/MP), and `4` when a level is actually gained. It first refuses if `BaseScore.Level >= MAX_LEVEL` (399) and then computes `max_level` from `extra.ClassMaster`: `MORTAL` (2) or `ARCH` (1) → 399; `CELESTIAL` (3), `SCELESTIAL` (5) or `CELESTIALCS` (4) → 199 (`MAX_CLEVEL`, `Basedef.h:118`). If the current level is already at that ceiling it returns 0.

The EXP ladder is built from two tables, `g_pNextLevel` for the 399-tier and `g_pNextLevel_2` for the 199-tier: `curexp = table[cur]`, `nextexp = table[cur+1]`, `delta = (nextexp - curexp) / 4`, and three intermediate boundaries at `curexp + delta`, `+2*delta`, `+3*delta`. Note `g_pNextLevel[cur + 1]` is read without a bound check, so at the top of each tier the read is one past the intended entry.

Two hard gates run before the level grant: `CELESTIAL` characters are blocked at level 39 unless `QuestInfo.Celestial.Lv40` is set, and at 89 unless `Lv90` is set; `ARCH` characters are blocked at 354 unless `Arch.Level355` and at 369 unless `Arch.Level370`. These return `0` with no message — the block is silent from this component's perspective.

On level grant the function increments `BaseScore.Level`, adds the per-class increments `g_pIncrementHp[cls]` / `g_pIncrementMp[cls]` to max HP/MP (note: max, and then current HP/MP are set to the new max), grants `SkillBonus += (Level >= 200 ? 4 : 3)` and `SpecialBonus += 2` for `MORTAL` and `ARCH`, calls `BASE_GetBonusScorePoint`, increments `BaseScore.Ac`, recomputes the sheet with `GetCurrentScore(0)`, sets `Segment = 0` and returns 4. `CELESTIAL` tiers skip the skill/special bonus but still get the bonus score point and AC.

The four segment returns are idempotent by design: `else if (exp >= Segment3 && Segment < 3)` guards against re-emitting the same effect, because `Segment` persists on the entity.

**Rule workflow**
```
Level >= 399 -> 0
max = (ClassMaster in {MORTAL(2), ARCH(1)}) ? 399 : (CELESTIAL/SCELESTIAL/CELESTIALCS ? 199 : 0)
cur >= max -> 0
curexp/nextexp/delta -> Segment1..4
CELESTIAL at 39 without Lv40 / at 89 without Lv90 -> 0
ARCH at 354 without Level355 / at 369 without Level370 -> 0
exp >= nextexp      -> Level++, MaxHp/Mp += class inc, HP/MP = max, bonuses, GetCurrentScore, Segment=0, return 4
exp >= Segment3     -> return 3
exp >= Segment2     -> return 2
exp >= Segment1     -> return 1
each of 1..3: HP/MP = max; GetCurrentScore(0)
```

---

#### Business Rule: BR-22 — Spawn quotas, groups and formation

**Overview**
Content generators own the population. `GenerateMob` enforces per-generator caps, randomises the group size inside an authored range, and stamps the whole follower formation from the leader's route.

**Detailed description**
`GenerateMob(index, PosX, PosY)` (`Server.cpp:2995`) has a relocation mode: when `MinuteGenerate >= 500` it treats the generator's first waypoint as a numeric range (`segx - segy` must be positive), picks `reloc = rand() % segx + segy`, and reassigns `index = reloc` after logging if the relocation is out of `NumList`. Otherwise `index` stays. It always reads formation/group from the **original** index (`nindex`) — so a relocated spawn still uses the owning generator's formation and group range.

Quota handling is exact: if `CurrentNumMob >= MaxNumMob` the function returns without spawning; the requested group size `MinGroup += rand() % (MaxGroup - MinGroup + 1)` is then clamped so the total does not exceed `MaxNumMob` (`3044-3048`). A `qmob <= 0` (MaxGroup < MinGroup) logs `err,zero divide` and is coerced to 1. `CurrentNumMob` is incremented only after successful grid placement (`3206-3209`), so a failed placement does not leak quota. `DeleteMob(..., Type != 0)` decrements it with a floor of zero.

Waypoint jitter, follower formation (`g_pFormation[i][j][Formation]` added to the leader's waypoint), explicit `PosX/PosY` override, clan-4/face-215..345 tower linkage, the tax-collector registration (`Merchant == 6` → `GuildImpostoID[level] = tmob`), the `Type != 0` skill-affect seed (`SetAffect(tmob, MaxMp, 30000, 200)`), the 75% HP reduction for level < 120 during the newbie event, the `RUNEQUEST` castle-map exclusion, and the 10% clan 1→2 variance are all part of this rule. The newbie HP reduction at `3165-3166` is the only spawn-time HP modifier.

`CreateMob` (`2865`) is the hand-placed counterpart and differs: `GenerateIndex = -1`, `Formation = 0`, `RouteType = Type` (the caller's parameter, so an author can select any of 0..6), `WaitSec = Type == 0 ? 10 : 40`, `SegmentProgress = Type == 0 ? 0 : 4`, and waypoints jittered ±2 unless the face is 219/220/358.

**Rule workflow**
```
if MinuteGenerate >= 500: relocate index within [segy, segx)
if CurrentNumMob >= MaxNumMob: return
group = MinGroup + rand()%(MaxGroup-MinGroup+1); clamp to MaxNumMob - CurrentNumMob
tmob = GetEmptyNPCMob(); if 0 -> return
copy leader MOB, sanitize name ('_'->space, '@'->NUL), clear affects
copy 5 waypoints (jitter by SegmentRange) and SegmentWait
Mode = MOB_PEACE; RouteType = generator.RouteType; GenerateIndex = index
GetCurrentScore(MAX_USER); Hp = MaxHp  (-25% if NewbieEvent and Level<120)
place via GetEmptyMobGrid; register pMobGrid; quota++
spawn `group` followers with formation offsets, Leader = tmob2
```

---

#### Business Rule: BR-23 — Death guards (unkillable entities and the Nyerds fairy)

**Overview**
Not everything that reaches zero HP dies. Level-1000+ entities are restored, and a specific fairy (`sIndex 769`) converts death into sanctification loss.

**Detailed description**
`MobKilled` (`MobKilled.cpp:41`) validates the (killer, target) pair and rejects `pMob[target].Mode == USER_EMPTY`. It then applies two escape hatches before any reward logic. First, `if (pMob[target].MOB.CurrentScore.Level >= 1000)`: HP is restored to max, `SetReqHp` is called for players, and a fresh `MSG_CreateMob` is multicast so clients re-render the entity; the function returns without granting anything (`50-63`). Level 1000 is the sentinel for "admin/event/boss entity" — `MAX_LEVEL` is 399, so this is unreachable for normal progression and is a pure scripted-invulnerability flag. Second, `if (pMob[target].MOB.Equip[13].sIndex == 769 && RvRState == 0 && GTorreState == 0 && CastleState == 0)`: the fairy's sanctification (`BASE_GetItemSanc`) is decremented (clearing the item at zero), the item is re-sent, an emotion is played, the entity is restored to full HP and re-broadcast (`65-104`). The four world-state conditions mean this protection is disabled during RvR, tower and castle events.

A third guard teleports rather than kills: `CastleState > 1 && TargetX == 1046 && TargetY == 1690 && target in player range` sends the player to `(1066, 1717)` instead of resolving the death (`105-109`). That is the castle throne-room ejection.

Note the helper's parameter names: `MobKilled(int target, int conn, ...)` where `conn` is the **killer**, not a connection. `target` is the victim. The naming is inverted relative to the rest of the codebase and is a recurring source of confusion in downstream logic (e.g. `pMob[conn].Leader` at 185 is the killer's leader).

**Rule workflow**
```
guards: 0<conn,target<MAX_MOB; target.Mode != USER_EMPTY
victim.Level >= 1000            -> restore MaxHp, re-broadcast, return
victim fairy 769 and no event   -> sanc--, restore MaxHp, re-broadcast, return
castle throne room and player   -> DoTeleport(1066,1717), return
pet-kill mount growth
exp/coin/drop/party resolution
Hp = 0 ; caller deletes
```

---

#### Business Rule: BR-24 — Event and quest despawn zones

**Overview**
Hard-coded coordinate boxes forcibly remove mobs, independent of AI state. This is content logic embedded in the entity tick loop.

**Detailed description**
Three categories exist. Rune-track boxes: in the peace sweep, any mob not the `RUNEQUEST_LV6_MOB_BOSS` whose position falls in `X ∈ [3423,3442], Y ∈ [1492,1511]` is deleted with reason 3, and if it belongs to `RUNEQUEST_LV6_MOB_INITIAL2..END2` the pista-6 counter is incremented first (`ProcessSecMinTimer.cpp:1388-1398`). A second rune box at `X ∈ [3373,3407], Y ∈ [1420,1453]` for `RUNEQUEST_LV2_MOB_INITIAL..END` triggers after a movement action (`1490-1494`). The same LV6 box is re-checked in the combat sweep (`1581-1591`), so a fighting mob inside it is also removed.

The remaining boxes are player-oriented and live in the player loop, not in `CMob` itself: `DoRecall` at five quest areas (2379..2426 / 2076..2133, 2228..2257 / 1700..1728, 459..497 / 3887..3916, 658..703 / 3728..3762, 1312..1348 / 4027..4055), each gated by `QuestFlag != n` and `BaseScore.Level < 1000` (`1192-1209`). These depend on `CMob::QuestFlag` (`CMob.h:97`) but are not implemented in `CMob.cpp`.

Because the boxes are literals in the tick loop rather than data, they cannot be re-authored without a rebuild, and they run before `StandingByProcessor`, so a mob can be deleted in the same tick it would have acted.

**Rule workflow**
```
peace/combat tick, per entity:
    if GenerateIndex != RUNEQUEST_LV6_MOB_BOSS and pos in LV6 box:
        if GenerateIndex in LV6_INITIAL2..END2 and 0 < MobCount < 100: MobCount++
        DeleteMob(index, 3); continue
    (peace only) if GenerateIndex in LV2 range and pos in LV2 box: DeleteMob(index, 3)
```

---

#### Business Rule: BR-25 — Social combat filter (leader and guild reconciliation)

**Overview**
Before the combat processor runs, the entity strips targets that share its party leader or its guild, so allied units cannot remain locked onto each other.

**Detailed description**
`ProcessSecMinTimer.cpp:1593-1619` reads `Target = pMob[index].CurrentTarget`, resolves `leader = index` when `Leader == MOB_EMPTY`, resolves the target's leader the same way, then resolves guilds with `GuildDisable` forcing the value to `0`. The normalisation `if (indexguild == 0 && targetguild == 0) indexguild = -1;` prevents two guild-less entities from being treated as allies. If `leader == targetLeader || indexguild == targetguild` it calls `pMob[index].RemoveEnemyList(Target)`.

The removal only clears the enemy-list slot; it does **not** clear `CurrentTarget`. Since `BattleProcessor` immediately calls `SelectTargetFromEnemyList()` and that function recomputes `CurrentTarget` from scratch, the effect is applied one step later in the same tick — `RemoveEnemyList` prunes the list, then the selection pass sees the pruned list. That ordering is essential and fragile: moving the reconciliation after `BattleProcessor` would leave the stale target active for one full tick.

`RemoveParty(conn)` is called by `DeleteMob`, which is how a dissolved party eventually stops being mutually filtered. The guild comparison uses the persisted `MOB.Guild`, so guild changes take effect on the next tick without any explicit invalidation.

**Rule workflow**
```
Target = CurrentTarget
indexLeader  = Leader ? Leader : index
targetLeader = pMob[Target].Leader ? pMob[Target].Leader : Target
indexGuild   = GuildDisable ? 0 : MOB.Guild ; targetGuild likewise
if indexGuild == 0 && targetGuild == 0: indexGuild = -1
if indexLeader == targetLeader || indexGuild == targetGuild:
    RemoveEnemyList(Target)
BattleProcessor() -> SelectTargetFromEnemyList() recomputes CurrentTarget
```

---

## 4. Component Structure

```
Code/TMSrv/CMob.h                                    123 lines
├── include guard __CMOB__                            :19-20
├── #include "../Basedef.h"                           :22
├── MAX_ENEMY 13                                      :24
├── Mode constants MOB_EMPTY..MOB_WAITDB (0..9)       :26-35
└── class CMob                                        :37-120
    ├── Entity record
    │   ├── STRUCT_MOB MOB                            :40   persisted sheet (796-800 B)
    │   ├── STRUCT_AFFECT Affect[32]                  :41   buffs / pet marker
    │   ├── STRUCT_MOBEXTRA extra                     :84   class master, quest flags, saves
    │   └── char Tab[36], Snd[96], Unk5[12], Unk7     :57,71,86,87   opaque/padding
    ├── AI state
    │   ├── int Mode                                  :42   AI state (BR-02)
    │   ├── short CurrentTarget                       :68   selected victim
    │   ├── short EnemyList[13]                       :69   candidate victims (BR-07)
    │   └── short PartyList[12]                       :70   party/follower links
    ├── Navigation state
    │   ├── Segment, SegmentX/Y                       :58-60 current waypoint
    │   ├── SegmentListX/Y[5], SegmentWait[5],
    │   │   SegmentRange[5]                           :61-64 route (BR-14)
    │   ├── SegmentDirection, SegmentProgress         :65-66 direction discipline
    │   ├── TargetX/Y, LastX/Y, NextX/Y               :46-53 positions
    │   ├── char Route[24]                            :55   emitted path string
    │   ├── int RouteType, Formation, WaitSec         :45,44,56 route discipline (BR-14..17)
    │   └── int LastTime, LastSpeed                   :48-49 written at spawn, never read
    ├── Social / ownership
    │   ├── Leader, Summoner                          :43,73 pet + party chain (BR-13)
    │   ├── int GenerateIndex                         :67   generator back-reference
    │   ├── GuildDisable, LastReqParty, QuestFlag     :76,99,97
    │   └── PotionCount, Parry, MaxCarry              :74,75,96
    ├── Derived combat caches (BR-20)
    │   ├── WeaponDamage, Accuracy, HpAbs             :72,90,91
    │   ├── DropBonus, ExpBonus                       :77,78
    │   ├── ForceDamage, ReflectDamage                :79,80
    │   ├── ForceMobDamage, PvPDamage, ReflectPvP     :89,93,94
    │   └── unsigned int ProcessorCounter             :100  vestigial
    └── Methods                                        :106-119
        ├── void ProcessorSecTimer()                  dead (body commented out)
        ├── int  StandingByProcessor()                out-of-combat processor   (152 lines)
        ├── int  BattleProcessor()                    combat processor          (96 lines)
        ├── void AddEnemyList(short)                  list insert
        ├── void RemoveEnemyList(short)               list erase
        ├── void SelectTargetFromEnemyList()          target ranking
        ├── int  SetSegment()                         waypoint advance state machine
        ├── void GetCurrentScore(int idx)             stat derivation (258 lines)
        ├── void GetTargetPosDistance(int tz)         retreat step
        ├── void GetRandomPos()                       roam step
        ├── void GetTargetPos(int tz)                 chase step
        ├── int  CheckGetLevel()                      progression
        ├── void GetNextPos(int battle)               patrol step
        └── int  GetEnemyFromView()                   aggro scan

Code/TMSrv/CMob.cpp                                  1279 lines, 13 function definitions
├── ctor / dtor                                       :28-49
├── ProcessorSecTimer (no-op)                         :51-54
├── StandingByProcessor                               :56-207
│   ├── summon/pet branch                             :60-130
│   └── patrol branch                                 :132-205
├── BattleProcessor                                   :209-304
│   ├── pet validation                                :220-253
│   ├── Int gate                                      :255-260
│   ├── leash check                                   :268-283
│   └── range / Dex / attack decision                 :284-301
├── AddEnemyList                                      :306-363
├── RemoveEnemyList                                   :365-379
├── SelectTargetFromEnemyList                         :381-468
├── SetSegment                                        :470-596
├── GetCurrentScore                                   :598-855
├── GetTargetPosDistance                              :857-919
├── GetRandomPos                                      :921-971
├── GetTargetPos                                      :973-1032
├── CheckGetLevel                                     :1034-1152
├── GetNextPos                                        :1154-1227
└── GetEnemyFromView                                  :1229-1279

Collaborating files owned by other components (read-only for this analysis)
├── Code/TMSrv/ProcessSecMinTimer.cpp   :1353-2016  the two tick sweeps that drive CMob
├── Code/TMSrv/Server.cpp               :270        pMob[] definition
│                                       :2572-2860 summon creation (RouteType 5, Affect 24)
│                                       :2865-2993 CreateMob
│                                       :2995-3330 GenerateMob / quota / formation
│                                       :7020-7054 DeleteMob
│                                       :7218-7252 SetBattle
├── Code/TMSrv/MobKilled.cpp            :41-2469   death resolution
├── Code/TMSrv/GetFunc.cpp              :1239      GetAction (movement packet)
│                                       :1275      GetAttack (mob skill selection)
│                                       :1564      GetAttackArea (Kefra AoE)
│                                       :1901-2001 GetEmptyMobGrid
├── Code/TMSrv/CNPCGene.h               :27-51    MAX_SEGMENT, NPCGENLIST (route authoring)
├── Code/Basedef.h                      :54-124   SECSTANDINGBY, SECBATTLE, MAX_USER, MAX_MOB,
│                                                 HALFGRID, MAX_ROUTE, MAX_SEGMENT-related,
│                                                 MAX_LEVEL, MAX_CLEVEL, MAX_PARTY
│                                       :183-202  RSV_* flags, REF_10..15
│                                       :388      KEFRA_BOSS 396
│                                       :438-481  STRUCT_MOB
│                                       :483-591  STRUCT_MOBEXTRA
│                                       :593-599  STRUCT_AFFECT
│                                       :2480     extern g_pClanTable[9][9]
├── Code/Basedef.cpp                    :189-202  g_pClanTable values
│                                       :1210-1223 BASE_GetSpeed
│                                       :5538-5826 BASE_GetRoute
│                                       :5828-5849 BASE_GetDistance
└── Code/ItemEffect.h                   :66,80,89-97 EF_RANGE 27, EF_PARRY 40,
                                                  EF_RESIST1..4 49, EF_HWORDGUILD 56,
                                                  EF_LWORDGUILD 57
```

Dimensional facts worth recording: `MAX_USER` 1000, `MAX_MOB` 25000, `MAX_PARTY` 12, `MAX_ENEMY` 13, `MAX_TARGET` 13 (`Basedef.h:173`), `MAX_AFFECT` 32, `MAX_ROUTE` 24, `HALFGRIDX/Y` 16, `VIEWGRIDX/Y` 33, `MAX_GRIDX/Y` 4096, `MAX_NPCGENERATOR` 8192, `MAX_SEGMENT` 5, `SECBATTLE` 8, `MAX_LEVEL` 399, `MAX_CLEVEL` 199, `KEFRA_BOSS` 396.

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (outbound, compile- and link-time)

```
CMob.h
 └── Basedef.h            (STRUCT_MOB, STRUCT_AFFECT, STRUCT_MOBEXTRA, MAX_* constants, RSV_*)

CMob.cpp
 ├── <Windows.h>                              (rand, memset, strlen)
 ├── ../ItemEffect.h                          (EF_RANGE, EF_PARRY, EF_RESIST*, EF_HWORDGUILD,
 │                                             EF_LWORDGUILD, EF_DAMAGE, EF_AC)
 ├── CMob.h
 ├── Server.h            → pMob[], pUser[], Log, DeleteMob, SetBattle, DoTeleport,
 │                        GetAttack, GetAttackArea, MobAttack, GetParryRate, SendScore,
 │                        SetAffect, SetTick, SendClientMessage, g_pItemList, CurrentTime
 ├── GetFunc.h           → GetEmptyMobGrid, GetAction, GetAttribute, BASE_GetSpeed(no),
 │                        SendScore, DoTeleport, BASE_GetDistance, BASE_GetRoute,
 │                        BASE_GetMobAbility, BASE_GetItemAbility, BASE_GetItemSanc,
 │                        BASE_GetItemGem, BASE_GetCurrentScore
 └── (transitively via Server.h) CNPCGene.h  → mNPCGen.pList[], MAX_SEGMENT, MAX_NPCGENERATOR
                                 CUser.h    → pUser[], USER_PLAY, USER_EMPTY
                                 CWarTower.h, CCastleZakum.h, CReadFiles.h
```

Runtime (global) dependencies actually touched by `CMob.cpp`:

| Global | Read/Write | Purpose |
|--------|-----------|---------|
| `pMob[MAX_MOB]` | R/W | own entity and every other entity (sensor, target, leader, party, summoner) |
| `pUser[MAX_USER]` | R | `USER_PLAY` gating for summoners and targets |
| `pMobGrid[4096][4096]` | R/W | occupancy reads in all pathing; written indirectly by callers |
| `pHeightGrid[4096][4096]` | R | terrain passability (`127` = blocked) and height tolerance |
| `g_pClanTable[9][9]` | R | hostility matrix (BR-05) |
| `g_pItemList[6500]` | R | `nPos`, `Grade` for weapon/equip bonuses (BR-20) |
| `mNPCGen.pList[8192]` | R | generator `Leader.Resist[]` for mob resist derivation, route/segment data |
| `g_pIncrementHp/Mp[]` | R | per-class level-up increments (BR-21) |

### 5.2 External dependencies

| Dependency | Type | Purpose in this component | Notes |
|------------|------|---------------------------|-------|
| MSVC C runtime (`rand`, `memset`, `strlen`) | Library | RNG for all gates, zeroing, route-string length | `rand()` is used unseeded-quality, shared globally; determinism cannot be reproduced without the same call ordering |
| Win32 (`Windows.h`) | OS API | Type definitions only in this TU | no Win32 call in `CMob.cpp` |
| `heightmap.dat` (runtime file, read elsewhere) | Data file | Loaded into `pHeightGrid`, consumed here read-only | File path resolved in `CReadFiles`/`Basedef`; failure mode is an all-zero height grid → every path accepted |
| No network, no DB, no filesystem access | — | `CMob` performs no I/O | All persistence is delegated to `MobKilled`/`CReadFiles` |

### 5.3 Key call chains

```
ProcessSecTimer (1 Hz)
  ├─> CMob::StandingByProcessor   -> GetEnemyFromView, GetTargetPos, GetNextPos, SetSegment
  │      -> BASE_GetDistance, BASE_GetRoute, GetEmptyMobGrid, BASE_GetSpeed
  ├─> SetBattle                    -> CMob::AddEnemyList -> SendChat
  ├─> DeleteMob                    -> RemoveParty, GridMulticast(_MSG_RemoveMob)
  ├─> GetRandomPos / DoTeleport / SendSay / GetAction / GridMulticast
  └─> CMob::BattleProcessor       -> SelectTargetFromEnemyList
         -> BASE_GetDistance, BASE_GetMobAbility
         -> GetTargetPosDistance / GetTargetPos -> BASE_GetRoute
         -> GetAttack -> GetParryRate -> SetAffect/SetTick -> MobKilled

GenerateMob -> GetEmptyNPCMob -> CMob::GetCurrentScore -> BASE_GetCurrentScore
            -> GetEmptyMobGrid -> GetCreateMob -> GridMulticast
```

---

## 6. Afferent and Efferent Coupling

Measured at translation-unit granularity by counting distinct call/field sites (whole tree, `ClientPatch_v7662` excluded because it has no sources).

| Component (unit under analysis) | Afferent coupling (Ca) | Efferent coupling (Ce) | Instability I = Ce/(Ca+Ce) | Criticality |
|---------------------------------|------------------------|------------------------|----------------------------|-------------|
| `CMob` (class, aggregate) | 67 (65 `.cpp`/`.h` files reference `pMob[`, + `CMob.h`, `Server.h`) | 4 (Basedef / GetFunc / Server / ItemEffect) | 0.06 | Critical |
| `CMob::GetCurrentScore` | 60 call sites in 10 files | 6 (`BASE_GetCurrentScore`, `BASE_GetMobAbility`, `BASE_GetItemAbility`, `BASE_GetItemSanc`, `BASE_GetItemGem`, `g_pItemList`/`g_pIncrement*`) | — | Critical |
| `CMob::CheckGetLevel` | 11 call sites in 5 files | 3 (`g_pNextLevel*`, `BASE_GetBonusScorePoint`, `GetCurrentScore`) | — | High |
| `CMob::GetTargetPos` | 2 (1 external, 1 internal) | 3 | — | Medium |
| `CMob::StandingByProcessor` | 1 | 8 | — | Medium |
| `CMob::BattleProcessor` | 1 | 6 | — | Medium |
| `CMob::GetTargetPosDistance` | 1 | 3 | — | Medium |
| `CMob::GetRandomPos` | 1 | 3 | — | Medium |
| `CMob::AddEnemyList` | 1 (`SetBattle`) | 0 | — | Low |
| `CMob::RemoveEnemyList` | 1 (tick loop) | 0 | — | Low |
| `CMob::SetSegment` | 0 external / 2 internal | 2 (`Log`, `GetCurrentScore`) | — | Medium |
| `CMob::SelectTargetFromEnemyList` | 0 external / 1 internal | 1 (`BASE_GetDistance`) | — | Medium |
| `CMob::GetEnemyFromView` | 0 external / 1 internal | 2 (`g_pClanTable`, `Log`) | — | Medium |
| `CMob::GetNextPos` | 0 external / 2 internal | 4 | — | Medium |
| `CMob::ProcessorSecTimer` | 1 | 0 (empty) | — | Low |
| `STRUCT_MOB` / `STRUCT_MOBEXTRA` (via `CMob`) | ~95 files (architecture report figure for Basedef contract) | 0 | — | Critical |
| `pMob[]` global array | 65 files, 3,913 references | 1 (CMob) | — | Critical |

Highest-reference consumers of the `pMob[]` surface (proxy for afferent pressure):

| File | `pMob[` references |
|------|--------------------|
| `Code/TMSrv/Server.cpp` | 802 |
| `Code/TMSrv/_MSG_UseItem.cpp` | 574 |
| `Code/TMSrv/_MSG_Quest.cpp` | 491 |
| `Code/TMSrv/MobKilled.cpp` | 269 |
| `Code/TMSrv/_MSG_Attack.cpp` | 244 |
| `Code/TMSrv/ProcessSecMinTimer.cpp` | 213 |
| `Code/TMSrv/_MSG_MessageWhisper.cpp` | 149 |
| `Code/TMSrv/GetFunc.cpp` | 142 |
| `Code/TMSrv/SendFunc.cpp` | 122 |
| `Code/TMSrv/imple.cpp` | 99 |

Interpretation. `CMob` is a **stable, maximally depended-upon** component (I ≈ 0.06): almost nothing in it depends on the rest of the world through abstractions, while everything depends on it through a global array. The class-level cohesion is low: entity persistence (`STRUCT_MOB`), AI state machine, navigation, stat derivation and player progression (`CheckGetLevel`) are four unrelated concerns sharing one object. Efferent coupling is nominally 4 but the *effective* fan-out is much larger because `Server.h` is a transitively-included wiring hub — `CMob.cpp` can reach `mNPCGen`, `pUser`, `g_pItemList`, `CCastleZakum` and `CReadFiles` without naming them in its own includes.

---

## 7. Integration Points

`CMob` exposes no protocol surface of its own: it neither registers a message handler nor opens a socket. All of its observable effects are emitted through other components' helpers, which makes the following table the real integration contract.

| Integration | Type | Purpose | Protocol / mechanism | Data format | Error handling |
|-------------|------|---------|----------------------|-------------|----------------|
| `ProcessSecTimer` tick sweep | Internal scheduler | Drives both processors at 6 s / 4 s cadence | Direct method call from `ProcessSecMinTimer.cpp:1406,1621` | Return bitmask (`int`) | Bits not decoded are silently ignored; `0x010000` and `0x10000` collision resolved only by call-site context |
| `SetBattle` / `AddEnemyList` | Internal domain service | Aggro commitment and list insertion | Direct call (`Server.cpp:7218`) | Entity index | Guards + silent return; no error propagation |
| `GetEnemyFromView` → `0x10000000 \| index` | Internal signal channel | Aggro candidate transfer to caller | Bit-packed `int` | index in low 28 bits | `Log("err,clan out or range")` then `return 0` |
| `GetAction` + `GridMulticast(_MSG_Action)` | Outbound client | Movement animation broadcast | Packet fan-out to nearby players | `MSG_Action` (`Basedef.h:1696`) | Caller skips emission when `NextX==TargetX && NextY==TargetY` or `GetEmptyMobGrid` returns `FALSE` |
| `GetAttack` + `GridMulticast(_MSG_AttackOne)` | Outbound client | Melee/ranged attack broadcast | Packet fan-out | `MSG_AttackOne` | `GetInHalf` failure aborts and appends to `Gethalf.txt` |
| `GetAttackArea` + `MobAttack` | Outbound client | Kefra boss area attack | Packet fan-out | `MSG_Attack` (13 targets) | None |
| `SendSay` / `SendChat` | Outbound client | NPC dialogue: `SegmentAction`, `FleeAction`, `FightAction` | Packet | `mNPCGen` strings (80 bytes) | Guarded by generator index range |
| `DoTeleport` | Internal service | Pet recall, return-step teleport, castle ejection | Direct call | Coordinates | None |
| `DeleteMob(idx, type)` | Internal lifecycle | Despawn with reason 1/2/3 | Direct call | `type` int | No bounds check on `pMobGrid[pMob[conn].TargetY][pMob[conn].TargetX]` |
| `MobKilled` | Internal domain service | Death resolution handoff | Direct call (`ProcessSecMinTimer.cpp:1940`) | victim/killer indices | Extensive guards; `Log` + early return |
| `pMobGrid` / `pHeightGrid` | Shared memory state | Occupancy and terrain | Raw array reads | `unsigned short` / `char` | `GetEmptyMobGrid` logs and returns `FALSE` out of range |
| `g_pClanTable` | Static config | Hostility relations | 9x9 int matrix | compile-time table | Bounds-checked before indexing in `GetEnemyFromView`; **not** bounds-checked in `AddEnemyList`'s KEFRA block (no index use) |
| `mNPCGen.pList[]` | Runtime config | Route waypoints, waits, actions, resist seeds | Struct array read | `NPCGENLIST` (2,988 bytes) | `Log("err,rotate index out of range")`; several call sites check `GenerateIndex` against `MAX_NPCGENERATOR` but `GetCurrentScore` does so only for the top branch |
| `g_pItemList[]` | Runtime config | Equipment grade / `nPos` lookups | Struct array read | `STRUCT_ITEMLIST` | One-past-end read at exactly `MAX_ITEMLIST` (see BR-20) |
| `Log(...)` | Observability | Diagnostics for impossible states | Text to server log | `char*` | fire-and-forget |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / assessment |
|---------|----------------|----------|----------------------|
| **State machine (implicit)** | `int Mode` with symbolic constants and two processors | `CMob.h:42`; dispatch at `ProcessSecMinTimer.cpp:1362-1550,1559` | Half-implemented: 10 declared states, 5 assigned, 3 active. Dispatch is an untagged `if`-chain over magic ints, not a `switch` |
| **Behaviour bitmask return ("command object" collapsed to bits)** | `StandingByProcessor` / `BattleProcessor` return an `int` whose bits mean actions | `CMob.cpp:56-304`; decode at `ProcessSecMinTimer.cpp:1408-1546,1955-2014` | Intentional separation of decision from side effects, but the encoding is undocumented and collides (`0x10000` vs `0x010000`) |
| **Template Method / fixed pipeline** | `ProcessSecTimer` orchestrates a fixed sequence per entity: validate → affect → processor → decode → emit | `ProcessSecMinTimer.cpp:1357-1551,1557-2015` | Duplicated between the two sweeps (peace vs combat) with slightly different validation and different failure semantics |
| **Entity-Component-in-one (fat entity / active record)** | One class holds persistence, AI, navigation, derived stats | `CMob.h:37-120` | Deliberate data-oriented choice for a 25,000-slot world; costs cohesion |
| **Singleton global array** | `CMob pMob[MAX_MOB]`, `extern` in 2 headers | `Server.cpp:270`; `CMob.h:122`; `Server.h:230` | Global mutable world state; enables O(1) index access and zero-copy persistence, at the cost of total coupling |
| **Object pool with cursor** | `g_EmptyMob` cursor + linear scan in `GetEmptyNPCMob` | `Server.cpp:6765-6791` | Pooled allocation without a free list; O(n) worst case, monotone cursor gives good locality in the common case |
| **Scripted route / waypoint tour** | `SegmentListX/Y[5]` + `SegmentWait` + `SegmentProgress`/`Direction` + `RouteType` | `CMob.h:58-66`; `CMob.cpp:470-596` | Data-driven AI authored per NPC generator; 7 route disciplines overloaded onto one int |
| **Strategy-by-int** | `RouteType` selects patrol semantics | `CMob.cpp:499-581` | Effectively a strategy pattern flattened into an `if`-ladder; the `&&` guard bug at 483 removes validation |
| **Greedy local steering (no global pathfinding)** | `BASE_GetRoute` 8-direction height-tolerant stepper | `Basedef.cpp:5538-5826` | O(distance) per move, no A*, no caching, no waypoint graph; produces the `'1'..'9'` keypad strings in `Route` |
| **Retry-with-degradation** | Path length decremented from `distance` toward 0, re-resolving occupancy | `CMob.cpp:894-918,946-962,1007-1023,1202-1218` | Best-effort movement; if all attempts fail the move is cancelled rather than reported |
| **Guarded sequential escalation** | `GetEmptyMobGrid` rings 1→2→3→4 | `GetFunc.cpp:1919-1997` | Bounded local search; bias toward top-left in ring order |
| **Null-object sentinel** | index `0` = "no entity"; `MAX_USER` (999) used as "infinity" in distance ranking | `CMob.cpp:387-388,450-463` | Two different sentinels for two different meanings, both untyped |
| **Side-effect-on-read anti-pattern** | `GetCurrentScore` mutates `MOB.Clan`, `MOB.Rsv`, `pUser[idx].Range`, `MOB.CurrentScore.Hp/Mp` | `CMob.cpp:600-663,715-719` | A "getter" that writes three subsystems; the clan reset (`601`) is the most surprising |
| **Hard-coded content constants in engine code** | Rune-track boxes, KEFRA_BOSS id, fairy item indices 3900-3913, 769, 786, 1936, 1937, faces 219/220/315-345 | `ProcessSecMinTimer.cpp:1388-1391,1581-1591`; `CMob.cpp:265,334,677-697,975,1156`; `MobKilled.cpp:65,114` | Content cannot be re-authored without a rebuild |
| **Fail-silent error handling** | Guards `return 0` / `return rt` with no log in most branches | throughout | Only "impossible" states log; expected failures (invalid leader, no target) are silent |
| **Layered architecture (nominal only)** | `_MSG_*` → `GetFunc/SendFunc` → `Basedef` → `CPSock` | — | `CMob` is reached directly by 65 files, bypassing the layer boundary |

---

## 9. Technical Debt & Risks

| Risk | Area | Issue | Evidence | Impact |
|------|------|-------|----------|--------|
| High | `CMob::SetSegment` | Out-of-bounds reads `SegmentListX[5]`/`[−1]` reachable for route types 1 and 3 | `CMob.cpp:563-567,518-522,591-592` | Undefined behaviour on every cycle boundary; corrupt `SegmentX/Y` propagates into the leash anchor and aggro box |
| High | `CMob::SetSegment` | `Route[strlen(Route)]` reads one byte past the NUL and mutates `MOB.Merchant` | `CMob.cpp:539-555` | NPC role/immunity flips unpredictably; depends on adjacent stack/heap bytes |
| High | `DeleteMob` | `pMobGrid[pMob[conn].TargetY][pMob[conn].TargetX] = 0` without bounds check | `Server.cpp:7049` | Any corrupted position writes 4 bytes outside the 32 MB grid (memory corruption, remote-reachable via movement) |
| High | State machine | 5 of 10 declared states are never assigned; `MOB_IDLE` branch is unreachable | `CMob.h:26-35`; whole-tree occurrence count = 1 each for USERDOCK/RETURN/FLEE/ROAM; `MOB_IDLE` never assigned | Documented AI (return/flee/roam) does not exist; maintainers reading the header are misled |
| High | `BattleProcessor` | Tautological comparison makes `else return 256` unreachable; hit-position adjustment was deleted | `CMob.cpp:291-299` (with commented call at 294) | Dead code; `0x100` (retreat) only reachable through the Dex gate |
| High | `StandingByProcessor` | `rt = rt \|= 1` on line 128 — assignment of the OR back to `rt` via `\|=` idiom is a no-op-on-no-op; more importantly the branch returns `1` for a route-5 entity with any marker but no leader resolution | `CMob.cpp:128` | Confusing control flow; the only reachable outcome is "no action" |
| High | Test coverage | Zero tests for a 1,279-line AI state machine with bitmask contracts | section 10 | Every change above is unverified by automation |
| Medium | Bitmask contract | `0x10000` (despawn) and `0x010000` (suppress) differ by one nibble, returned from sibling functions, decoded in different loops | `CMob.cpp:151,259`; `ProcessSecMinTimer.cpp:1536,1623` | A refactor that unifies the decoders would delete mobs instead of skipping a tick |
| Medium | `AddEnemyList` KEFRA block | Loop writes `EnemyList[c] = TargetId` regardless of loop index; cannot add distinct nearby players | `CMob.cpp:334-361` | Boss encounter sharing is non-functional; 13-slot list can be doubly filled with one target |
| Medium | `SelectTargetFromEnemyList` | `>=` tie-breaking selects the *last* slot; list order is first-hole insertion | `CMob.cpp:456`; `306-332` | Target choice among equidistant enemies is an artefact of insertion history, not a rule |
| Medium | Leash anchor | Uses `SegmentX/Y` (current waypoint), not spawn point | `CMob.cpp:268`; `3199`, `591-592` | Mobs can be kited arbitrarily far along long routes; no spawn-point return exists |
| Medium | `GetCurrentScore` | Mutates `MOB.Clan = 0`, `MOB.Rsv = 0`, `pUser[idx].Range`, HP/MP on a "getter" | `CMob.cpp:600-601,625,654,661-662,715-719` | Calling it at the wrong moment wipes clan/Rsv flags (stealth, parry, haste) |
| Medium | `GetCurrentScore` | Mob resist over-cap maps to `50`, not `100` | `CMob.cpp:641-651` | Content authored with resist >100 silently becomes mid-range |
| Medium | `GetCurrentScore` | `pUser[idx].Range = BASE_GetMobAbility(...)` immediately overwritten by `23` | `CMob.cpp:661-662` | Weapon range stat is ignored for players |
| Medium | `GetCurrentScore` | `if (idx1 >= 0 \|\| idx1 < MAX_ITEMLIST)` always true; `> MAX_ITEMLIST` bound allows index `MAX_ITEMLIST` | `CMob.cpp:761,775,793` | One-past-end read of `g_pItemList` |
| Medium | `GetNextPos` | Str clamp computed into `res` and discarded in the battle branch | `CMob.cpp:1176-1183` | Battle movement ignores Strength; patrol movement is silently throttled |
| Medium | `ProcessorSecTimer` | Called every second per player; body fully commented out | `CMob.cpp:51-54`; `ProcessSecMinTimer.cpp:223-224` | Up to 1,000 no-op virtual calls per second plus a dead `ProcessorCounter` field |
| Medium | `LastTime`, `LastSpeed` | Written at spawn, never read | `Server.cpp:3160,3277`; `CMob.h:48-49` | Dead state; suggests a removed per-mob timer feature |
| Medium | Scheduling | Peace stride 6 s vs combat stride 4 s; `WaitSec -= 6` couples authored seconds to the stride | `ProcessSecMinTimer.cpp:1353-1357,1554-1560`; `CMob.cpp:153,168` | Any stride change rescales all authored NPC pauses and attack rates |
| Medium | Bounds check | `Progress > MAX_SEGMENT` permits `5` for a 5-element array | `ProcessSecMinTimer.cpp:1401`; `MAX_SEGMENT` = 5 | `SegmentWait[5]` read at `CMob.cpp:159` |
| Medium | `SetSegment` | `if (RouteType <= 0 && RouteType > 4)` is unsatisfiable | `CMob.cpp:483` | No validation of out-of-range route types; route 4 can spin when all waypoints are zero |
| Medium | `GetEnemyFromView` | Clan 7/8 window is `[-6,+9]` (16 tiles) — asymmetric, not centred | `CMob.cpp:1237-1243` | Large-view aggro is biased forward-left; undocumented |
| Medium | `GetEnemyFromView` | `pMobGrid[y][x]` can be `0`; `pMob[0]` is then probed for HP/Mode | `CMob.cpp:1257-1262` | Depends on slot 0 never being a live entity; a latent sentinel bug |
| Medium | Aggro asymmetry | Peace path returns `256`/`32` for invalid summon bindings; combat path deletes/teleports with different thresholds (13 vs 20) | `CMob.cpp:109,222-251` | Pets near their owner can oscillate between teleport and reposition across the two ticks |
| Medium | Memory | `pMob` is 25,000 constructed `CMob` objects with `STRUCT_MOB` + `STRUCT_MOBEXTRA` + `Affect[32]` + 144 bytes of opaque padding; the ctor runs 25,000 times at startup | `Server.cpp:270`; `CMob.h:40-100`; architecture report: ~50 MB | Fixed 32-bit address-space ceiling; ctor cost on boot |
| Medium | `memset` over class objects | `memset(&pMob[i].PartyList, ...)`, `memset(pMob[tmob].Affect, ...)`, `memset(&pMob[tmob].SegmentX, 0, sizeof(SegmentX))` | `Server.cpp:2798,3087,2720-2721,3170` | `memset(&SegmentX, 0, sizeof(SegmentX))` zeroes only `SegmentX` (4 bytes), not the segment block — misleading intent |
| Medium | `MobKilled` naming | Parameters `(target, conn)` where `conn` is the killer | `MobKilled.cpp:41` | Inverted naming relative to the rest of the codebase; invites off-by-one target/killer errors |
| Low | `CMob` ctor | Initialises only 10 of ~45 fields; `Route`, `Segment*`, `Target*`, `Next*` rely on BSS zeroing | `CMob.cpp:28-44` | Safe only because `pMob` is a global; a heap/stack `CMob` would be uninitialised |
| Low | `char Tab[36]`, `char Snd[96]`, `Unk5[12]`, `Unk7` | Opaque fields with no producer or consumer in the component | `CMob.h:57,71,86,87` | Undocumented; unknown whether persistence depends on them |
| Low | `Enemy[MAX_ENEMY] = { 0, 0, 0, 0 }` | Braced init of 4 of 13 elements | `CMob.cpp:385` | Harmless (loop follows) but signals a resize that was not propagated |
| Low | `Mode = 4` and `if (Mode == 5)` equivalents | Magic literals where constants exist | `CMob.cpp:537`; `Server.cpp:4491,4496` | Symbols bypassed; searchability and safety reduced |

---

## 10. Test Coverage Analysis

### 10.1 Result: no tests exist

| Component | Unit tests | Integration tests | Coverage | Test quality |
|-----------|-----------|-------------------|----------|--------------|
| `CMob` (all 13 methods) | 0 | 0 | 0% (no harness exists) | Not applicable — no test code, no fixtures, no mocks, no golden data |
| `CMob::StandingByProcessor` | 0 | 0 | 0% | Untested state machine with an overloaded return contract |
| `CMob::BattleProcessor` | 0 | 0 | 0% | Untested RNG gates (`Int`, `Dex`) and leash arithmetic |
| `CMob::SelectTargetFromEnemyList` | 0 | 0 | 0% | Untested radius overrides and tie-breaking |
| `CMob::SetSegment` | 0 | 0 | 0% | Untested route-type matrix; the OOB paths would be trivially caught by a bounds-checked test build |
| `CMob::GetCurrentScore` | 0 | 0 | 0% | 60 call sites, zero assertions |
| `CMob::GetEnemyFromView` | 0 | 0 | 0% | Untested clan matrix and window geometry |

### 10.2 Evidence

1. **No test project.** `W2PP Code Project.sln` contains exactly two projects — `Code/TMSrv/TMSrv.vcxproj` and `Code/DBSrv/DBSrv.vcxproj`. There is no test project, no `tests/` directory and no test configuration.
2. **No test framework references.** A case-sensitive scan of all 80 `.cpp` and 24 `.h` files for `TEST_F`, `TEST(`, `TEST_CASE`, `CppUnit`, `gtest`, `catch2`, `NUnit`, `BOOST_AUTO_TEST`, `doctest` returns **zero matches**.
3. **No test-named files.** A recursive glob for `**/*{test,Test,TEST,spec,Spec}*` across the repository (excluding the ignored folders) returns **zero files**.
4. **No fixture or mock data.** There is no `fixtures/`, `mocks/`, `data-test/` or equivalent. The only binary data files are production runtime artifacts (`ItemList.bin`, `SkillData.bin`, `InitItem.bin`, `heightmap.dat`, `AttributeMap.dat`) consumed directly by the server.
5. **No assertions in production code.** No `assert`, `static_assert` or debug-only validation exists in `CMob.cpp`; the only defensive output is `Log(...)` for states the authors considered impossible.
6. **No CI configuration.** No `.github/`, `.gitlab-ci.yml`, `azure-pipelines.yml`, `Jenkinsfile` or build script beyond the MSVC solution.
7. **OpenSpec has no specifications.** `openspec/specs/` contains only `.gitkeep`; `openspec/changes/archive/` likewise. There is no written behavioural specification that could substitute for tests.

### 10.3 What exist instead of tests

| Substitute | Location | What it verifies | Limitation |
|------------|----------|------------------|------------|
| `Log("err,standingby processer delete hp zero mob")` | `ProcessSecMinTimer.cpp:1368,1381` | Survivability of the HP-zero invariant | Observational only; fires after the fact |
| `Log("err,clan out or range")` | `CMob.cpp:1269` | Clan table bounds | Returns `0` (safe) but hides the corrupting write |
| `Log("Wrong SetSegment")` / `Log("SetSegment SegmentProgress -1 but route type 0/4")` | `CMob.cpp:485,504,526` | Route-type anomalies | The surrounding guard is dead code |
| `AttackDieTeste.txt` diagnostic dump | `ProcessSecMinTimer.cpp:1650-1657` | Records non-player attacks on players inside villages without PvP map flags | Appends unboundedly; a forensic artifact, not a test |
| `Gethalf.txt` diagnostic dump | `ProcessSecMinTimer.cpp:1672-1705` | Records `GetInHalf` failures with attacker/target/grid context | Same; also writes an entire grid window per failure |
| Production log analysis | `./Log/*.txt` | Post-incident reconstruction | Requires an incident |

### 10.4 Coverage risk statement

The component's most dangerous properties — out-of-bounds `SegmentList` reads, the unreachable `256` branch, the unsatisfiable route-type guard, the `0x10000`/`0x010000` collision, the `GetCurrentScore` clan reset, and the tautological attractor — are all detectable by a single bounds-checked debug build with a handful of seeded `rand()` cases and a mocked `pMob` array. None of them is covered by any executable check in the repository. Test coverage of the CMob component is 0%.

---

## 11. Configuration and Runtime Tunables

`CMob` itself reads no configuration file. Its behaviour is parameterised by compile-time constants and by content data owned by other components.

| Parameter | Source | Current value | Effect on CMob |
|-----------|--------|---------------|----------------|
| `MAX_USER` | `Basedef.h:56` | 1000 | Player/NPC index boundary used in ~40 branches |
| `MAX_MOB` | `Basedef.h:107` | 25000 | World capacity; array size and sweep length |
| `MAX_PARTY` | `Basedef.h:168` | 12 | Pet/leader membership loops, party propagation |
| `MAX_ENEMY` | `CMob.h:24` | 13 | Enemy list capacity (no eviction) |
| `MAX_AFFECT` | `Basedef.h:122` | 32 | `Affect[0]` pet marker, expiry sweep |
| `MAX_ROUTE` | `Basedef.h:124` | 24 | `char Route[24]`; caps `distance` to 23 |
| `HALFGRIDX/Y` | `Basedef.h:97-98` | 16 | Leash box, large-view aggro window, KEFRA target radius |
| `SECBATTLE` | `Basedef.h:55` | 8 | Movement budget divisor (`speed * 8 / 4`) |
| `SECSTANDINGBY` | `Basedef.h:54` | 8 | Declared; not referenced by `CMob.cpp` |
| `MAX_SEGMENT` | `CNPCGene.h:27` | 5 | Waypoint count; the `> MAX_SEGMENT` check is off by one |
| `MAX_LEVEL` | `Basedef.h:117` | 399 | `CheckGetLevel` ceiling, target-selection filter |
| `MAX_CLEVEL` | `Basedef.h:118` | 199 | Celestial-tier ceiling |
| `KEFRA_BOSS` | `Basedef.h:388` | 396 | Immobility, 25-tile attack range, 16-tile target radius, aggro fan-out |
| `MH` | `Basedef.h:134` | 8 | Height tolerance in `BASE_GetRoute` |
| `AttackRun & 0xF` | entity sheet | per mob | Speed 1..6, or 0 = immobile |
| `BaseScore.Int` | entity sheet | per mob | Combat action probability (BR-10) |
| `BaseScore.Dex` | entity sheet | per mob | Standoff discipline (BR-11) |
| `BaseScore.Str` | entity sheet | per mob | Patrol step cap (BR-18) |
| `RouteType` | `mNPCGen.pList[i].RouteType` ("RouteType:" in the generator file, `CNPCGene.cpp:202-203`) | 0..6 | Patrol discipline (BR-14..17) |
| `SegmentWait[5]` | generator file | per NPC | Dwell time, decremented by 6 per visit |
| `SegmentRange[5]` | generator file | per NPC | Spawn jitter radius |
| `MinuteGenerate` | generator file | per NPC | `>= 500` selects the index-relocation spawn mode |
| `MaxNumMob` / `MinGroup` / `MaxGroup` | generator file | per NPC | Population quota and group size |
| `Formation` | generator file | per NPC | Follower offset table `g_pFormation[12][5][Formation]` |
| `Face` (`Equip[0].sIndex`) | mob sheet | per NPC | 219/220 = immobile; 315..345 = summon/pet family |
| Fairy indices 3900-3913 | mob sheet | per NPC | Exp/drop bonuses applied in `GetCurrentScore` |
| Item grade / gem / sanc | item data | per item | Weapon damage, force/reflect/drop/exp bonuses |
| `g_pClanTable` | `Basedef.cpp:189-202` | static 9x9 | Hostility relations |

Environment variation: none. There is no per-environment override, no feature flag, no environment variable and no configuration reload. `mNPCGen.pList[]` is loaded at boot from the NPC generator file and can be reloaded via the `IDC_MOBRELOAD` menu command (architecture report), which re-reads content but not `CMob` state.

---

## Appendix A — Behaviour bitmask reference (as implemented)

| Return value | Emitted by | Decoded in | Meaning |
|--------------|-----------|-----------|---------|
| `0` | both | both | No action |
| `1` (`0x01`) | `StandingByProcessor`, `BattleProcessor` | peace: move via `GetAction`; combat: chase via `GetTargetPos` | Advance one step |
| `2` (`0x02`) | both | both | `DoTeleport(NextX, NextY)` |
| `0x10` (`16`) | both | peace: `SendSay(FleeAction)`; combat: `GetAction` continuation | Move-while-waiting / flee chatter |
| `0x20` (`32`) | `BattleProcessor` | combat | `DeleteMob(index, 3)` — pet with invalid leader |
| `0x100` (`256`) | both | peace: `DeleteMob(index,3)`; combat: `GetTargetPosDistance` | Peace: self-delete; combat: retreat step |
| `0x1000` (`4096`) | `BattleProcessor` | combat | Attack |
| `0x10000` | `StandingByProcessor` | peace | `DeleteMob(index, 3)` — one-shot route end |
| `0x010000` | `BattleProcessor` | combat | Suppress action (Int gate failed) |
| `0x10000000 \| index` | `StandingByProcessor` | peace | `SetBattle(index, index)` + party propagation |

## Appendix B — `CMob` field inventory with roles

| Field | Type | Role | Written by | Read by |
|-------|------|------|-----------|---------|
| `MOB` | `STRUCT_MOB` | Persisted sheet, source of all stats | spawn, DB load, item use | entire codebase |
| `Affect[32]` | `STRUCT_AFFECT` | Buffs; slot 0 doubles as pet marker (type 24) | `SetAffect`, `GenerateSummon` | `GetCurrentScore`, processors |
| `Mode` | int | AI state | spawn, `SetBattle`, processors, `DeleteMob` | tick sweeps |
| `Leader` | int | Party/follower/pet owner | `GenerateMob`, `GenerateSummon` | processors, `MobKilled` |
| `Formation` | int | Follower offset selector | spawn | spawn only |
| `RouteType` | int | Patrol discipline (0..6) | spawn / `CreateMob` | processors, `SetSegment` |
| `LastX/LastY` | int | Previous position for path origin | movement primitives | movement primitives |
| `LastTime` | int | Spawn timestamp | spawn | never |
| `LastSpeed` | int | Unused | never | never |
| `TargetX/TargetY` | int | Current position | movement, teleport, DB load | everything |
| `NextX/NextY` | int | Proposed next position | movement primitives | tick sweeps, `DoTeleport` |
| `NextAction` | int | Unused in component | never | never |
| `Route[24]` | char | Emitted direction string | `BASE_GetRoute` | movement primitives, `SetSegment` (type 0) |
| `WaitSec` | int | Remaining dwell seconds | `SetSegment`, spawn | `StandingByProcessor` |
| `Segment` | int | EXP segment for `CheckGetLevel` | `CheckGetLevel` | `CheckGetLevel` |
| `SegmentX/Y` | int | Current waypoint / leash anchor | `SetSegment`, spawn, pet logic | processors |
| `SegmentListX/Y[5]` | int | Authored waypoints | spawn | `SetSegment`, `StandingByProcessor` |
| `SegmentWait[5]` | int | Authored dwell | spawn | `StandingByProcessor` |
| `SegmentRange[5]` | int | Authored jitter | spawn | spawn only |
| `SegmentDirection` | int | Forward/backward | `SetSegment`, spawn | `SetSegment` |
| `SegmentProgress` | int | Waypoint index (−1..5) | `SetSegment`, spawn | `StandingByProcessor`, tick sweep |
| `GenerateIndex` | int | Generator back-reference | spawn | processors, `GetCurrentScore`, despawn zones |
| `CurrentTarget` | short | Selected victim | `SelectTargetFromEnemyList`, handlers | combat sweep, `MobAttack` |
| `EnemyList[13]` | short | Candidate victims | `AddEnemyList`, handlers | `SelectTargetFromEnemyList` |
| `PartyList[12]` | short | Followers / party members / summons | spawn, party handlers | processors, `MobKilled` |
| `WeaponDamage` | int | Derived dual-wield damage | `GetCurrentScore` | combat math |
| `Summoner` | int | Actual owner of a pet | `GenerateSummon` | processors, `MobKilled` |
| `PotionCount` | int | Pet potion bookkeeping | legacy | legacy |
| `Parry` | int | `EF_PARRY` cache | `GetCurrentScore` | `GetParryRate` |
| `GuildDisable` | int | Forces guild id 0 in combat filter | handlers | combat sweep |
| `DropBonus` / `ExpBonus` | int | Derived reward multipliers | `GetCurrentScore` | `MobKilled` |
| `ForceDamage` / `ReflectDamage` | int | Derived damage bonuses | `GetCurrentScore` | combat math |
| `ForceMobDamage` | int | Derived PvM bonus | `GetCurrentScore` | combat math |
| `Accuracy` | int | Derived accuracy | `GetCurrentScore` | `GetAttack` |
| `HpAbs` | int | HP absorption | `GetCurrentScore` | combat math |
| `PvPDamage` / `ReflectPvP` | int | Derived PvP attack/defence | `GetCurrentScore` | PvP math |
| `MaxCarry` | int | Inventory cap cache | item logic | item logic |
| `QuestFlag` | int | Quest-area recall exclusion | quest logic | player tick loop |
| `LastReqParty` | int | Party request throttle | party logic | party logic |
| `ProcessorCounter` | unsigned | Vestigial | never incremented | never |
| `extra` | `STRUCT_MOBEXTRA` | Class master, quest flags, celestial saves | DB, quests | `CheckGetLevel`, summoning |
| `Tab[36]`, `Snd[96]`, `Unk5[12]`, `Unk7` | char/short | Opaque | unknown | unknown |
