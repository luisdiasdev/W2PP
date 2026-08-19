# Component Deep Analysis Report

**Component Name:** CNPCGene
**Project:** legacy (W2PP C/C++ codebase)
**Component Location:** `legacy/Code/TMSrv/CNPCGene.h`, `legacy/Code/TMSrv/CNPCGene.cpp`
**Analysis Date:** 2026-08-19 17:13:23
**Report Scope:** mob/NPC spawn generators & summons

---

## 1. Executive Summary

**Purpose:** The CNPCGene component is the mob/NPC spawning and summoning subsystem of the W2PP (WYD/PT - "Wyd" private server) game server. It is responsible for (a) loading and parsing the world's static spawn configuration from `NPCGener.txt`, (b) defining the data structures that describe spawn entries (leader/follower pairs, patrol routes, group sizes, spawn timing), (c) loading the base templates for player summons from `BaseSummon`, (d) mapping named world regions to coordinate rectangles from `Regions.txt`, and (e) generating auxiliary drop/level reporting files.

**Role in the system:** CNPCGene acts as the **configuration model and template store** for all procedurally spawned world mobs. It defines the `CNPCGenerator` and `CNPCSummon` classes plus the `NPCGENLIST` and `MAPREGION` structures. The actual spawning of mobs into the game world (`GenerateMob`), the periodic respawn scheduler (`ProcessSecMinTimer`), the on-demand summon creation (`GenerateSummon`), and the runtime de-spawn bookkeeping (`DeleteMob`) all live in other files (`Server.cpp`, `ProcessSecMinTimer.cpp`, `CMob.cpp`) but are tightly coupled to the data structures and templates defined by this component. CNPCGene is therefore the *definitional/data source* for the spawn system; its classes are the passive data containers that other subsystems read at runtime.

**Key findings:**
- The component is primarily a **data-loading and structure-definition** module rather than an active logic processor. Its own `.cpp` implements file parsing (`ReadNPCGenerator`, `ParseString`, `ReadRegion`), template initialization (`CNPCSummon::Initialize`), and report generation (`DropList`, `LevelList`).
- The most important business logic actually *consuming* this component (respawn scheduling, group/formation spawning, patrol routes, segment actions) resides outside the component in `ProcessSecMinTimer.cpp`, `Server.cpp` (`GenerateMob`, `GenerateSummon`), and `CMob.cpp`. This is a significant **cohesion** observation: spawn generation logic is spread across multiple translation units rather than encapsulated with its data model.
- Runtime data files (`NPCGener.txt`, `Regions.txt`, `BaseSummon/*`, `npc/*`) are **not present in the repository**; they are external deployment/runtime assets loaded by absolute relative path at server startup.
- **No test files exist** for this component anywhere in the project. This is a critical coverage gap (see Section 11).
- The code contains **developer/utility functions** (`DropList`, `LevelList`, `ReadRegion`) that appear to be diagnostic tooling (generating drop/level listing text files per region) and are currently **commented out** at their call sites in `Server.cpp` (lines 3646-3648).
- Parsing relies on `sscanf`/`atoi` with minimal bounds/format validation, and fixed-size buffers, presenting robustness and security concerns (see Section 10).

---

## 2. Data Flow Analysis

The data flow for the spawn system that CNPCGene participates in is as follows:

```
1. Server startup (Server.cpp:7144) calls mNPCGen.ReadNPCGenerator()
2. ReadNPCGenerator() opens NPCGener.txt and calls ParseString(i, line) per entry
3. ParseString() parses key:value tokens and populates NPCGENLIST pList[i]
   (timing, group size, leader/follower templates, 5-segment patrol route, actions)
4. Leader/Follower entries trigger ReadMob(&pList[i].Leader/"Follower", "npc")
   which loads the binary STRUCT_MOB template from the ./npc/ directory
5. mSummon.Initialize() (Server.cpp:7162) reads 40 BaseSummon templates from ./BaseSummon/
6. ReadRegion() (commented out at Server.cpp:3646) loads Regions.txt coordinate rectangles
7. Periodic scheduler (ProcessSecMinTimer.cpp:2212) iterates pList[i] each minute:
   - computes mod = i % MinuteGenerate; if MinCounter % MinuteGenerate == mod
   - calls GenerateMob(i, 0, 0)
8. GenerateMob(index) (Server.cpp:2995):
   - copies Leader template into a free NPC mob slot (pMob[tmob])
   - randomizes group size between MinGroup..MaxGroup
   - spawns MinGroup followers using the Follower template
   - applies patrol segment coordinates (SegmentListX/Y + SegmentRange randomization)
   - sets GenerateIndex = index on each spawned mob
   - increments pList[index].CurrentNumMob for each spawned mob
9. Spawned mobs patrol using RouteType/Segment logic in CMob::SetSegment()
   and emit SegmentAction/FightAction/FleeAction/DieAction chat strings
   (ProcessSecMinTimer.cpp:1486, Server.cpp:7248, MobKilled.cpp:631)
10. On mob death, DeleteMob() (Server.cpp:7027) decrements pList[geneidx].CurrentNumMob
11. Summons: GenerateSummon(conn, SummonID, ...) (Server.cpp:2574) reads mSummon.Mob[SummonID]
    template and materializes it into a free NPC slot, applying player-derived stat bonuses
```

**Data ownership:** `pList[]` (the `NPCGENLIST` array) and `Mob[]` (the `STRUCT_MOB` summon array) are the primary data stores owned by this component. They are written once at load/init and read continuously by the spawn, scheduler, and combat subsystems.

---

## 3. Business Rules & Logic

### Overview of the business rules:

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Spawn config entry delimiter `#` starts a new generator entry; comments start with `/` (0x2F) or `#*` | CNPCGene.cpp:71,82 |
| Validation | `Leader:`/`Follower:` value `0` means "no mob" and skips the entry | CNPCGene.cpp:173,189 |
| Validation | Action strings must be shorter than 80 chars | CNPCGene.cpp:155,967 |
| Validation | StartX/StartY must be within `0 < v < MAX_GRIDX` (4096) or a warning is shown | CNPCGene.cpp:216,223 |
| Business Logic | `MinuteGenerate` controls the respawn cadence (minute tick scheduling) | ProcessSecMinTimer.cpp:2214 |
| Business Logic | Spawn interval is dynamically re-rolled into randomized bands once fired | ProcessSecMinTimer.cpp:2228-2253 |
| Business Logic | Certain generator indices (0,1,2,5,6,7,-1) are excluded from automatic spawning | ProcessSecMinTimer.cpp:2206 |
| Business Logic | Group size is randomized between `MinGroup` and `MaxGroup` | Server.cpp:3033-3036 |
| Business Logic | Spawn is capped by `MaxNumMob` via `CurrentNumMob` accounting | Server.cpp:3039-3042 |
| Business Logic | Follower count is clamped to remaining capacity and `MAX_PARTY` (8) | Server.cpp:3039-3042,3229 |
| Business Logic | Patrol route uses 5 waypoints (Start, Segment1-3, Dest) with per-segment range/wait/action | CNPCGene.h:37-41 |
| Business Logic | RouteType drives patrol/ping-pong/idle behavior (0-6) | CMob.cpp:460-560 |
| Business Logic | Special guarded area (2440-2545, 1845-1921) disables respawn of non-219 mobs | Server.cpp:3051-3056 |
| Business Logic | Random relocation: high MinuteGenerate (>=500) entries relocate to a random generator index | Server.cpp:3020-3032 |
| Business Logic | Summon set is a fixed catalog of 40 base beast templates | CNPCGene.cpp:684-962 |
| Business Logic | Summon stats scale with caster Int and Evocation stats | Server.cpp:2690-2701 |
| Business Logic | Drop-list chance values are derived from carry slot index (hardcoded bands) | CNPCGene.cpp:380-408 |
| Business Logic | Region assignment maps spawn start point to a named region or `Zona_Desconhecida` | CNPCGene.cpp:344-355 |

### Detailed breakdown of the business rules:

---

### Business Rule: Spawn configuration file parsing (NPCGener.txt)

**Overview:**
`ReadNPCGenerator()` (CNPCGene.cpp:45) loads the world's static mob-spawn configuration from a text file named `NPCGener.txt`. It also opens a diagnostic output file `NPCGener.new.txt` in write mode and re-emits a normalized version of the input as it parses. This is the entry point that populates the `pList[]` array of `NPCGENLIST` structures that every downstream spawn routine reads.

**Detailed description:**
The parser iterates the file line by line using `fgets` into a 1024-byte buffer. Three line categories are distinguished by the first character. Lines starting with `/` (ASCII 0x2F) are treated as comments and, if their second token does not begin with `*`, are copied verbatim into the normalized output file (CNPCGene.cpp:71-80). Lines starting with `#` begin a new generator entry: the entry index `Num` is incremented, the segment X/Y arrays are zeroed, and a banner comment is written to the output file; a `#*` variant marks the entry differently (CNPCGene.cpp:82-98). Any other non-CR line is passed to `ParseString()` for key/value extraction (CNPCGene.cpp:100-119). If `NPCGener.txt` cannot be opened, a modal `MessageBoxA` is shown and the function returns 0 (CNPCGene.cpp:50-55). After the loop, `NumList = Num + 1` records the total number of generator entries (CNPCGene.cpp:122), which becomes the iteration bound for all downstream consumers. The parser writes a normalized, re-aligned copy of recognized key/value pairs to `NPCGener.new.txt`, preserving only tokens whose first column string length exceeds 8 chars or non-`Í` first tokens (CNPCGene.cpp:107-118).

**Rule workflow:**
```
Open NPCGener.txt (fail -> MessageBox + return 0)
Open NPCGener.new.txt (write)
While lines exist:
  line starts with '/'  -> comment; if 2nd token != '*' write to output
  line starts with '#'  -> new entry; Num++; zero segment X/Y; write banner (#* vs #)
  otherwise             -> ParseString(Num, line)
NumList = Num + 1
close files, return 1
```

---

### Business Rule: Per-entry token parsing (ParseString)

**Overview:**
`ParseString(int i, char *str)` (CNPCGene.cpp:130) is the token-level parser that maps key:value tokens from each `NPCGener.txt` data line onto the `pList[i]` generator entry. It recognizes a fixed vocabulary of configuration keys controlling timing, group sizes, coordinates, ranges, waits, and action strings.

**Detailed description:**
The function uses `sscanf(str, "%s %s %s", str1, str2, str3)` to extract up to three whitespace-delimited tokens (CNPCGene.cpp:144). It rejects comment lines (`str[0]==47`) and empty lines (`str[0]==0`) with `FALSE` (CNPCGene.cpp:146-150). The second token is converted via `atoi` into `value` and the third into `secondvalue` (CNPCGene.cpp:152-153); if the second token exceeds 80 chars the parse is rejected (CNPCGene.cpp:155-156). The key matching is a long if/else-if chain. Numeric keys assigned directly include `MinuteGenerate:`, `MaxNumMob:`, `MinGroup:`, `MaxGroup:`, `RouteType:`, `Formation:`, and a series of segment coordinates/ranges/waits covering Start, Dest, and Segment1-3 (each with X/Y/Range/Wait) (CNPCGene.cpp:159-273). The `Leader:` and `Follower:` keys (CNPCGene.cpp:171-201) are special: if the value is `"0"` the key is skipped (`FALSE`), otherwise the mob name is copied into the respective `STRUCT_MOB` and `ReadMob()` is invoked with the `"npc"` directory to load the binary template; on success `Mode` is set to `MOB_USE`. If `ReadMob` fails, a `MessageBox` is shown with the mob name and an error string. The `*Action:` keys (StartAction, Segment1-3Action) call `SetAct()` to copy the action string into the segment action buffer (CNPCGene.cpp:275-285). Any unrecognized key returns `FALSE` (CNPCGene.cpp:287-288).

**Rule workflow:**
```
sscanf 3 tokens (str1, str2, str3)
if comment or empty -> FALSE
value = atoi(str2); secondvalue = atoi(str3)
if strlen(str2) > 80 -> FALSE
match str1 against known keys:
  numeric keys -> pList[i].field = value
  Leader/Follower -> if "0" skip; else ReadMob(npc) + Mode=MOB_USE
  *Action keys -> SetAct()
  else -> FALSE
return TRUE
```

---

### Business Rule: Respawn cadence scheduling (MinuteGenerate)

**Overview:**
The periodic scheduler in `ProcessSecMinTimer.cpp` (lines 2212-2260) iterates every generator entry each server minute and decides whether to call `GenerateMob(i, 0, 0)`. The decision is driven by the `MinuteGenerate` field parsed by CNPCGene. This is the core rule that turns the static configuration into an active respawn loop.

**Detailed description:**
For each entry `i` from `0` to `NumList`, the scheduler reads `MinuteGenerate`. Entries with index `0,1,2,5,6,7,-1` are skipped entirely (CNPCGene.cpp... ProcessSecMinTimer.cpp:2206) — these reserved indices are never auto-spawned (they correspond to special/summon-derived entries). If `MinuteGenerate <= 0` the entry is skipped (ProcessSecMinTimer.cpp:2218). Otherwise, the rule computes `mod = i % MinuteGenerate` and triggers a spawn when `MinCounter % MinuteGenerate == mod` (ProcessSecMinTimer.cpp:2222-2224). This modulo scheme staggers spawns across the minute counter so not all entries fire simultaneously. Once a spawn fires, the interval is **re-randomized** into a new band so respawns become irregular (anti-farming): bands are 500-999 (`rand()%500+500`), 1000-1999 (`rand()%1000+1000`), 2000-3799 (`rand()%1800+2000`), and >=3800 (`rand()%1000+3800`) (ProcessSecMinTimer.cpp:2228-2253). The >=3800 band additionally triggers a "Dungeon Event" that spawns random prize items from `DungeonItem[]` at a random `DungeonPos[]` location (ProcessSecMinTimer.cpp:2249-2257).

**Rule workflow:**
```
for i in 0..NumList:
  if i in {0,1,2,5,6,7,-1}: skip
  MinuteGenerate = pList[i].MinuteGenerate
  if MinuteGenerate <= 0: skip
  if MinCounter % MinuteGenerate == i % MinuteGenerate:
    GenerateMob(i, 0, 0)
    re-roll MinuteGenerate into random band
    if band >= 3800 and DUNGEONEVENT: spawn dungeon prize items
```

---

### Business Rule: Group and formation spawning (GenerateMob)

**Overview:**
`GenerateMob(int index, int PosX, int PosY)` (Server.cpp:2995) materializes a generator entry into the live mob array. It spawns a **leader** from `pList[index].Leader` and a randomized number of **followers** from `pList[index].Follower`, honoring `MinGroup`/`MaxGroup` and the `MaxNumMob` population cap. This is the primary consumer of the CNPCGene data structures.

**Detailed description:**
First, if any mercenary mob (`pMobMerc[i]`) already carries the same `GenerateIndex`, its carry items are copied back into the leader template (Server.cpp:3001-3007), preserving dropped inventory. If `MinuteGenerate >= 500`, the entry is treated as a *relocation* generator: the segment-0 coordinates are used to derive a random target generator index `reloc = rand() % segx + segy` (Server.cpp:3020-3032); a negative delta logs an error. The group size is computed as `qmob = MaxGroup - MinGroup + 1`, guarded against zero (Server.cpp:3028-3037), then `MinGroup += rand() % qmob`. If the entry is already at `CurrentNumMob >= MaxNumMob`, no spawn occurs (Server.cpp:3039-3040); the group is additionally clamped so `CurrentNumMob + MinGroup` never exceeds `MaxNumMob` (Server.cpp:3041-3042). A special guard (Server.cpp:3051-3056) disables respawns in the coordinate box X[2440,2545], Y[1845,1921] unless the leader's equip index is 219 — effectively a no-spawn zone. The leader is then copied into a free NPC slot, its name cleaned (underscores/`@` to spaces), segment waypoints assigned (randomized within `SegmentRange` if set, else exact coordinates), and patrol fields initialized (`SegmentProgress=0`, `WaitSec`, `RouteType`, `Formation`, `Mode=MOB_PEACE`) (Server.cpp:3060-3170). If `PosX`/`PosY` are supplied, all segment waypoints are overridden to that position (Server.cpp:3117-3123). Special handling for war-tower banners (equip 219/220) copies guild data (Server.cpp:3125-3144), and `CWarTower::GGenerateMob` applies guild ownership for the `GTORRE` banner (Server.cpp:3146, CWarTower.cpp:138). `CurrentNumMob` is incremented (Server.cpp:3199-3201), an optional skill affect from `Leader.BaseScore.MaxMp` is applied (Server.cpp:3203-3207), and a `MSG_CreateMob` broadcast is sent to nearby viewers (Server.cpp:3209-3216). Followers are then spawned in a loop up to `MinGroup` and `MAX_PARTY` (Server.cpp:3229): each follower is copied from the Follower template, positioned using `g_pFormation[i][j][Formation]` offsets around the leader when a segment range is set (Server.cpp:3250-3259), given the same `GenerateIndex`/`RouteType`/`Formation`, and linked to the leader via `PartyList` (Server.cpp:3257,3270). Each follower also increments `CurrentNumMob` (Server.cpp:3356).

**Rule workflow:**
```
sync Leader.Carry from matching pMobMerc (if any)
if MinuteGenerate >= 500: relocate index randomly (log errors on bad bounds)
qmob = MaxGroup - MinGroup + 1 (guard 0)
MinGroup += rand() % qmob
if CurrentNumMob >= MaxNumMob: return
clamp MinGroup to remaining capacity
if leader equip != 219 and within guarded box: MinuteGenerate=-1; return
tmob = GetEmptyNPCMob(); if 0 -> return
copy Leader into pMob[tmob]; set GenerateIndex/RouteType/Formation/Mode
assign segment waypoints (randomized by SegmentRange) or PosX/PosY
apply war-tower banner guild logic
increment CurrentNumMob; apply skill affect; broadcast MSG_CreateMob
for i in 0..MinGroup (cap MAX_PARTY):
  spawn follower from Follower template at formation offsets
  increment CurrentNumMob; broadcast
```

---

### Business Rule: Patrol route and segment waypoint model

**Overview:**
The `NPCGENLIST` structure models a mob's patrol path as **five waypoints** stored in `SegmentListX/Y[5]`: index 0 = Start, indices 1-3 = intermediate Segments 1-3, index 4 = Dest. Each waypoint has an associated `SegmentRange` (spawn scatter radius) and `SegmentWait` (dwell time), plus a `SegmentAction` chat string. The `RouteType` field selects the patrol behavior. This model is defined by CNPCGene (CNPCGene.h:37-47) and consumed by `CMob::SetSegment()` (CMob.cpp:460+).

**Detailed description:**
The five-waypoint model is filled by the parser keys `StartX/Y/Range/Wait`, `Segment1..3X/Y/Range/Wait`, and `DestX/Y/Range/Wait` (CNPCGene.cpp:208-273). At spawn time `GenerateMob` copies each waypoint into the live mob, randomizing the actual X/Y within `±SegmentRange` when a range is set (Server.cpp:3085-3110). The `RouteType` field (0-6) then governs how the mob traverses the waypoints in `CMob::SetSegment()`: `RouteType==6` resets the patrol to waypoint 0 and idles (CMob.cpp:472-481); route types 0-4 move `SegmentProgress` forward or backward depending on `SegmentDirection`, wrapping at the boundaries (`SegmentProgress == 5` for route 0 triggers merchant-mode finalization; `-1` wraps based on route type) (CMob.cpp:483-560). The `SegmentAction[5]` strings are emitted as chat during patrol progression when the segment action is non-empty and the index is within `MAX_SEGMENT` (ProcessSecMinTimer.cpp:1486-1487). Separately, `FightAction[4]`, `FleeAction[4]`, and `DieAction[4]` strings (CNPCGene.h:42-44) are emitted during combat events: `FightAction` on attack (Server.cpp:7248-7249), `FleeAction` when fleeing (ProcessSecMinTimer.cpp:1502-1503), and `DieAction` when a leader dies (MobKilled.cpp:631-632). These actions are the game's "talking mob" flavor/logic mechanism.

**Rule workflow:**
```
Spawn: assign SegmentListX/Y[0..4] (randomized within SegmentRange)
CMob::SetSegment (per tick, RouteType 0-6):
  RouteType 6 -> reset to waypoint 0, idle
  RouteType 0-4 -> advance/retreat SegmentProgress by SegmentDirection, wrap at ends
  dwell per SegmentWait; emit SegmentAction on progression
Combat events:
  on attack -> emit FightAction[rand]
  on flee   -> emit FleeAction[rand]
  on death  -> emit DieAction[rand]
```

---

### Business Rule: Summon base template initialization (CNPCSummon::Initialize)

**Overview:**
`CNPCSummon::Initialize()` (CNPCGene.cpp:678) preloads the fixed catalog of **40 base summon templates** into `Mob[MAX_SUMMONLIST]` (MAX_SUMMONLIST = 50, 40 populated) by calling `ReadMob(&Mob[i], "BaseSummon")` for each named beast. This is the data source for all player summon spells.

**Detailed description:**
The method calls `BASE_InitModuleDir()` to set the working module directory (CNPCGene.cpp:682), then sequentially assigns a hardcoded mob name to each of `Mob[0]` through `Mob[39]` and reads its binary `STRUCT_MOB` template from the `./BaseSummon/` folder (CNPCGene.cpp:684-962). The catalog spans common beasts (Condor, Javali, Lobo, Urso, Tigre, Gorila), fantasy mounts (Dragao_Negro, Dragao, Dragao_Vermelho, Fenrir, FenrirSombra, Unicornio, Pegasus, Unisus, Grifo, Hipogrifo, Grifo_Sangrento, Svadilfire, Sleipnir, Pantera_Negra, Dente_de_Sabre), and stat-variant riding/armor mounts (Sem_Sela_N/B, Fantasma_N/B, Leve_N/B, Equip_N/B, Andaluz_N/B). For each, if `ReadMob` fails, a `MessageBoxA` is shown indicating "Can't read NPC n". The loaded `Mob[]` array is later consumed by `GenerateSummon` (Server.cpp:2574) to materialize summons for players.

**Rule workflow:**
```
BASE_InitModuleDir()
for i in 0..39:
  set Mob[i].MobName = <fixed catalog name>
  ReadMob(&Mob[i], "BaseSummon")
  if fail -> MessageBox "Can't read NPC i"
```

---

### Business Rule: Summon spawning and stat scaling (GenerateSummon)

**Overview:**
`GenerateSummon(int conn, int SummonID, STRUCT_ITEM *sItem, int Num)` (Server.cpp:2574) materializes a player's summon from the `mSummon.Mob[SummonID]` template into the world, applying player-derived stat bonuses and enforcing party/slot constraints. This is the runtime consumer of the `CNPCSummon` catalog.

**Detailed description:**
The function resolves the party leader, obtains a free NPC slot via `GetEmptyNPCMob()` (failing with a "can't create more summons" message if none), validates `SummonID` against `MAX_SUMMONLIST` (Server.cpp:2588-2590), and determines the summon's face item from `Equip[0].sIndex`. It first scans the leader's party to enforce that a party member cannot already own a different summon (returns 0 if a conflicting beast face is found) and counts existing matching summons (Server.cpp:2596-2626); already-summoned beasts near the caster are updated in place rather than duplicated. For each additional summon needed (up to `Num`), it obtains another empty slot, checks the party is not full, copies the template, sets the summon's level to the caster's level (capped at `MAX_LEVEL`), appends a `^` marker and converts underscores to spaces in the name, and clears affects (Server.cpp:2627-2660). Stat scaling uses the `pSummonBonus[SummonID]` table (STRUCT_BEASTBONUS) with the caster's `Int` and `Evocation` (Special[2]) stats: `Damage += Int*Unk/100 + Evoc*Unk2/100`, `Ac += Int*Unk3/100 + Evoc*Unk4/100`, plus HP and other bonuses via Unk5/Unk6 (Server.cpp:2690-2701). The remaining spawn logic (position, action broadcast, party linking) mirrors the normal mob spawn path.

**Rule workflow:**
```
resolve leader; MobEmpty = GetEmptyNPCMob (fail -> message)
validate SummonID in [0, MAX_SUMMONLIST)
scan party: block if a different summon face already owned; count existing
for each needed summon:
  GetEmptyNPCMob; check party capacity
  copy mSummon.Mob[SummonID] template
  level = min(caster level, MAX_LEVEL); clean name
  apply stat bonuses from pSummonBonus using Int and Evocation
  position/broadcast
```

---

### Business Rule: Region assignment and auxiliary report generation (ReadRegion / DropList / LevelList)

**Overview:**
`ReadRegion()` (CNPCGene.cpp:293) loads named coordinate rectangles from `Regions.txt` into `pRegion[]`, and `DropList()` / `LevelList()` (CNPCGene.cpp:319,596) generate region-organized text reports of each generator mob's carried items (with per-slot drop chances) and levels. These are diagnostic/developer utilities; their call sites are currently commented out in `Server.cpp` (lines 3646-3648).

**Detailed description:**
`ReadRegion` reads `Regions.txt` line-by-line, parsing `minX, minY, maxX, maxY = RegionName` into `MAPREGION` entries, incrementing `NumRegion` (CNPCGene.cpp:305-315). `DropList` iterates every generator entry, deduplicating mobs already written (via a `MobList` of names), finds the region containing the entry's Start coordinate (defaulting to `"Zona_Desconhecida"` if none), and appends a `./Drop/<RegionName>.txt` file listing each carried item with a **hardcoded per-slot drop chance** (CNPCGene.cpp:380-408): slots 0-7 = 1/1000, slots 8-11 = 1/4, slots 12-23 = 1/1000, slots 24-55 = 1/1000, slot 56 = 1/1, slot 57 = 1/100, slot 58 = 1/500, slots >=60 = 1/2500, slots 61-62 = 1/5000, else 1/10000. A separate pass filters "special" items (sIndex 412,413,419,420) into a distinct list (CNPCGene.cpp:416-462). The `price < 100000` filter is commented out. `LevelList` similarly writes each mob's name and level to `./LevelList/<RegionName>.txt` (CNPCGene.cpp:641,658). These functions are bounded by `NumList` and `NumRegion`.

**Rule workflow:**
```
ReadRegion: parse Regions.txt rectangles -> pRegion[], NumRegion++
DropList: for each generator entry (dedupe by mob name):
  find region of Start coords (or Zona_Desconhecida)
  write ./Drop/<Region>.txt with item name(sIndex)(1/chance)
  chance from hardcoded carry-slot bands; separate special-item pass
LevelList: write ./LevelList/<Region>.txt with mob name + level
```

---

## 4. Component Structure

```
legacy/Code/TMSrv/
├── CNPCGene.h              # Component header: structures + class declarations + constants
│   ├── #define MOB_FREE 0 / MOB_USE 1      (spawn entry mode)
│   ├── #define MAX_SEGMENT 5               (patrol waypoints)
│   ├── struct NPCGENLIST                   (per-entry spawn config, 2988 bytes)
│   ├── struct MAPREGION                    (named coordinate rectangle)
│   ├── class CNPCSummon                    (summon template catalog holder)
│   └── class CNPCGenerator                 (spawn config + region holder + loaders)
│
└── CNPCGene.cpp            # Component implementation
    ├── CNPCGenerator::CNPCGenerator()      (ctor: zero pList/pRegion)
    ├── CNPCGenerator::~CNPCGenerator()     (dtor)
    ├── CNPCGenerator::ReadNPCGenerator()   (parse NPCGener.txt -> pList[])
    ├── CNPCGenerator::ParseString()        (token-level config parser)
    ├── CNPCGenerator::ReadRegion()         (parse Regions.txt -> pRegion[])
    ├── CNPCGenerator::DropList()           (region drop report generator)
    ├── CNPCGenerator::LevelList()          (region level report generator)
    ├── CNPCSummon::CNPCSummon()/~CNPCSummon() (ctor/dtor)
    ├── CNPCSummon::Initialize()            (load 40 BaseSummon templates)
    └── SetAct()                            (bounded action-string copier)
```

**Dependent translation units (consume CNPCGene structures):**
```
legacy/Code/TMSrv/Server.cpp             (GenerateMob, GenerateSummon, DeleteMob, ReadMob; mNPCGen/mSummon)
legacy/Code/TMSrv/ProcessSecMinTimer.cpp (respawn scheduler, segment/flee actions, secret-room counters)
legacy/Code/TMSrv/CMob.cpp               (patrol RouteType/Segment, leader resist inheritance)
legacy/Code/TMSrv/MobKilled.cpp          (DieAction emission, mercenary regeneration)
legacy/Code/TMSrv/imple.cpp              (mob reload command, leader template export)
legacy/Code/TMSrv/CCastleZakum.cpp       (pList-based mob counters)
legacy/Code/TMSrv/CWarTower.cpp          (GGenerateMob banner guild assignment)
legacy/Code/TMSrv/Server.h               (extern CNPCGenerator mNPCGen; extern CNPCSummon mSummon)
```

---

## 5. Dependency Analysis

### Internal Dependencies (within component):
```
CNPCGenerator::ReadNPCGenerator -> ParseString (per entry)
CNPCGenerator::ParseString      -> ReadMob (external, Server.cpp) for Leader/Follower
CNPCGenerator::ParseString      -> SetAct (for *Action keys)
CNPCSummon::Initialize          -> ReadMob (external, Server.cpp) + BASE_InitModuleDir (Basedef.cpp)
DropList/LevelList              -> g_pItemList (Basedef), pRegion[], pList[]
```

### Internal Dependencies (component consumers -> component):
```
Server.cpp GenerateMob        -> mNPCGen.pList[index] (Leader/Follower/Segment/Formation/RouteType/CurrentNumMob)
Server.cpp GenerateSummon     -> mSummon.Mob[SummonID]
Server.cpp DeleteMob          -> mNPCGen.pList[geneidx].CurrentNumMob--
ProcessSecMinTimer.cpp        -> mNPCGen.NumList, pList[i].MinuteGenerate/CurrentNumMob/SegmentAction/FleeAction
CMob.cpp                      -> mNPCGen.pList[geneidx].Leader.Resist[]; GenerateIndex/RouteType/SegmentProgress
MobKilled.cpp                 -> mNPCGen.pList[GenerateIndex].DieAction
imple.cpp                     -> mNPCGen.pList[i].Leader.MobName/Exp/BaseScore.Level; ReadNPCGenerator
CCastleZakum.cpp              -> mNPCGen.pList[SECRET_ROOM_*].CurrentNumMob
CWarTower.cpp                 -> GTORRE index handling (via GenerateMob)
```

### External Dependencies:
- **Win32 API** — `MessageBoxA`/`MessageBox`, `HWND hWndMain` (modal dialogs for config/load errors)
- **C runtime / stdio** — `fopen/fgets/fscanf/fprintf/fclose`, `sscanf`, `atoi`, `strcmp/strncpy/strcat/strlen/strcpy`
- **STL** — `std::list<char*>` (dedup in DropList/LevelList), plus `<vector>/<map>/<string>` includes
- **Global symbols (Basedef.h/Basedef.cpp)** — `STRUCT_MOB`, `STRUCT_ITEM`, `STRUCT_ITEMLIST`, `g_pItemList`, `g_pFormation`, `STRUCT_BEASTBONUS`/`pSummonBonus` (Server.cpp), `BASE_InitModuleDir`, constants `MAX_SEGMENT/MAX_GRIDX/MAX_CARRY/NAME_LENGTH/MAX_SUMMONLIST/MAX_NPCGENERATOR/MAX_EQUIP/MAX_PARTY/MAX_MOB_MERC`
- **Server.cpp globals/functions** — `ReadMob`, `temp` buffer, `hWndMain`
- **Runtime data files (external, not in repo)** — `NPCGener.txt`, `NPCGener.new.txt`, `Regions.txt`, `./npc/*`, `./BaseSummon/*`, `./Drop/*`, `./LevelList/*`

---

## 6. Afferent and Efferent Coupling

Coupling is measured per class in this C++ component (CNPCGenerator, CNPCSummon, plus the free function SetAct).

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|----------|
| CNPCGenerator | 7 (Server.cpp, ProcessSecMinTimer.cpp, CMob.cpp, MobKilled.cpp, imple.cpp, CCastleZakum.cpp, CWarTower.cpp via GenerateMob) | 6 (ReadMob, BASE_InitModuleDir, g_pItemList, SetAct, Win32, stdio) | High |
| CNPCSummon | 2 (Server.cpp GenerateSummon, Server.h extern) | 3 (ReadMob, BASE_InitModuleDir, Win32 MessageBoxA) | Medium |
| SetAct (free fn) | 1 (CNPCGenerator::ParseString) | 2 (Win32 MessageBox, string.h) | Low |

**Observations:**
- `CNPCGenerator` has **high afferent coupling**: its `pList[]` data is read by seven translation units spanning spawning, scheduling, patrol, death handling, and quest logic. This makes the structure a system-wide shared data contract; any change to `NPCGENLIST` layout ripples broadly.
- The coupling is **data-centric**: most consumers read `pList[index]` fields directly rather than through accessor methods, indicating low encapsulation (open member variables) and high coupling to the struct layout.
- `CNPCSummon` is comparatively low-coupling, used almost exclusively by the summon-spawn path.

---

## 7. Endpoints

The CNPCGene component does **not** expose any network/application endpoints (no REST, gRPC, GraphQL, or message-handler entry points of its own). It is a server-side data model and loader consumed by other subsystems. Its only interaction surfaces are:

- **UI command:** the `IDC_MOBRELOAD` window command in `Server.cpp` (line 4090) triggers `mNPCGen.ReadNPCGenerator()` to hot-reload the spawn config at runtime.
- **Startup hooks:** `mNPCGen.ReadNPCGenerator()` (Server.cpp:7144), `mSummon.Initialize()` (Server.cpp:7162), and `mNPCGen.ReadNPCGenerator()` (imple.cpp:841,1307,1383,1491).
- **Global data consumers:** `mNPCGen.pList[]` and `mSummon.Mob[]` are read by other subsystems (spawning, scheduling, combat).

Since the component exposes no endpoints, this section is informational and no endpoint table is provided.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| NPCGener.txt | Config file | World spawn configuration source | File I/O (fopen/fgets) | Text (key:value, `#`/`/` delimiters) | MessageBox on open failure; silent skip of malformed lines |
| NPCGener.new.txt | Config output | Normalized re-emission of parsed config | File I/O (fprintf) | Text | None (best-effort) |
| ./npc/* (ReadMob) | Binary data files | Leader/Follower `STRUCT_MOB` templates | Binary read (`_open/_read`) | Binary STRUCT_MOB | Returns FALSE + Log on EINVAL/EMFILE/ENOENT/UNKNOWN; MessageBox in ParseString |
| ./BaseSummon/* (ReadMob) | Binary data files | Summon beast templates | Binary read | Binary STRUCT_MOB | Returns FALSE; MessageBox "Can't read NPC n" |
| Regions.txt | Config file | Named region coordinate rectangles | File I/O | Text `minX,minY,maxX,maxY = Name` | MessageBox on open failure |
| ./Drop/*, ./LevelList/* | Output files | Region-based drop/level reports (utility) | File I/O (fprintf) | Text | None |
| g_pItemList (Basedef) | Global data | Item names/prices for drop reports | In-memory array | STRUCT_ITEMLIST | Bounds assumed valid |
| GenerateMob / GenerateSummon / DeleteMob (Server.cpp) | Internal service calls | Consume pList[]/Mob[] to spawn/de-spawn mobs | Direct function calls | In-memory structs | Log + return on empty slots / capacity |
| ProcessSecMinTimer / CMob / MobKilled (TMSrv) | Internal services | Scheduler, patrol, death handling | Direct reads of pList[] | In-memory structs | Guarded bounds checks on GenerateIndex |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Data Container / Struct-of-fields | `NPCGENLIST` struct | CNPCGene.h:29 | Holds all spawn-config fields; public access by consumers |
| Template / Prototype pattern | `pList[].Leader`/`Follower` and `Mob[]` are pre-loaded `STRUCT_MOB` templates cloned at spawn time | CNPCGene.cpp + Server.cpp GenerateMob/GenerateSummon | Copy-on-spawn instead of re-reading files per mob |
| Table-driven configuration | `NPCGener.txt` key:value parsed into fixed array `pList[MAX_NPCGENERATOR]` | CNPCGene.cpp:45 | Externalized world spawn tuning without recompilation |
| Global singleton-ish instances | `extern CNPCGenerator mNPCGen; extern CNPCSummon mSummon;` | Server.h:226-227 | Single shared spawn-data store across subsystems |
| Relocation/randomization | High-`MinuteGenerate` entries relocate to a random generator index; drop chances by slot band | Server.cpp:3020; CNPCGene.cpp:380 | Adds spawn/loot variability |
| Factory method (runtime) | `GenerateMob`/`GenerateSummon` create live mobs from templates | Server.cpp:2995,2574 | Runtime object creation from templates |
| Registry of regions | `MAPREGION pRegion[]` | CNPCGene.h:83 | Named-coordinate lookup for reports |

**Architectural notes:**
- The component follows a **data-centric / structure-oriented** (C-style) design common to legacy game servers: plain structs with public fields, global extern instances, and file-based configuration.
- There is **no abstraction layer** between the data model and consumers; consumers index `pList[i]` directly by hardcoded constants (e.g., `SECRET_ROOM_*`, `RUNEQUEST_*`, `KEFRA_BOSS`, `GTORRE`), which couples specific generator indices to specific quest/dungeon logic (Basedef.h:294+,351+).
- Spawn *behavior* (scheduling, formation, patrol) is not encapsulated in CNPCGene but spread across `Server.cpp`/`ProcessSecMinTimer.cpp`/`CMob.cpp`, indicating the component is a passive data source rather than an active domain service.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | CNPCGene.cpp:52,181,196,689+ | `MessageBoxA`/`MessageBox` modal dialogs on load errors block the server and expose internal paths in a headless server context | Server stalls awaiting UI dismiss; unusable in service mode |
| High | CNPCGene.cpp:76,110,144,312 | Unbounded `sscanf` into fixed `char[128]`/`char[1024]` buffers and `atoi` on unvalidated tokens | Buffer over-reads/undefined behavior on malformed config lines |
| High | CNPCGene.cpp:76,79,110 | `sscanf("%s %s")` writing into `&tmp1`/`&tmp2` (array) with no length limit | Potential stack overflow from long tokens |
| High | CNPCGene.cpp:319-667 | `DropList`/`LevelList` duplicated item-filter/chance blocks (4 near-identical passes) and dead code (commented `price` filter) | Maintenance drift; inconsistent behavior risk |
| Medium | CNPCGene.cpp:330,333,474,607,610,650 | `strncmp(*l, pList[i].Leader.MobName, strlen(pList[i].Leader.MobName))` uses **leader** length for the **follower** comparison | Incorrect deduplication when leader/follower names differ in length |
| Medium | CNPCGene.cpp:45-49 | `ReadNPCGenerator` opens `NPCGener.new.txt` in `"wt"` (truncate) and writes on every reload including hot-reload `IDC_MOBRELOAD` | File churn; partial-write risk if server crashes mid-reload |
| Medium | CNPCGene.h:37-47 / Server.cpp GenerateMob | `pList[index]` indexed by hardcoded magic constants (`SECRET_ROOM_*`, `RUNEQUEST_*`, `KEFRA_BOSS`, `GTORRE`) scattered across consumers | Fragile; reordering `NPCGener.txt` silently breaks quest/dungeon spawns |
| Medium | CNPCGene.cpp:155,967 | `SetAct` checks `strlen >= 79` but arrays are `char[80]`; relies on `MessageBox` then `return` without writing | Correct but rigid; action strings hard-limited to 79 |
| Medium | CNPCGene.h:80 | `NumOld[MAX_NPCGENERATOR]` declared but unused in the component | Dead field |
| Medium | Whole component | No runtime data files (`NPCGener.txt`, `Regions.txt`, `npc/`, `BaseSummon/`) in repo; parsing depends on them | Cannot run/reproduce without external assets |
| Low | CNPCGene.cpp:34-38 | `memset` uses `MAX_NPCGENERATOR` (8192) entries with multi-KB structs | Large static memory footprint (NPCGENLIST ~3KB * 8192 ~ 24MB) |
| Low | CNPCGene.cpp:373,421,491,540 | Unused local `price` variables and commented-out filter code | Code smell; dead code |

---

## 11. Test Coverage Analysis

An exhaustive search was performed across the entire repository (including all test directories) for test files referencing `CNPCGene`, `CNPCGenerator`, `CNPCSummon`, `ReadNPCGenerator`, `NPCGENLIST`, or related symbols.

**Result: No test files exist for this component anywhere in the project.**

The only directories matching `*test*` are within `.opencode/node_modules` (third-party `effect`, `zod`, `isexe` packages), which are outside the legacy codebase and excluded by the ignore-folders parameter. The `legacy/` tree contains no unit, integration, or fixture tests.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| CNPCGenerator (ReadNPCGenerator/ParseString/ReadRegion) | 0 | 0 | 0% | N/A — no tests |
| CNPCSummon (Initialize) | 0 | 0 | 0% | N/A — no tests |
| SetAct | 0 | 0 | 0% | N/A — no tests |
| GenerateMob / GenerateSummon (consumers) | 0 | 0 | 0% | N/A — no tests |

**Test quality assessment:** Not applicable — no test suite exists. The absence of tests is especially risky given the complex parsing logic (token grammar, boundary conditions on the 5-segment route model, group-size randomization, capacity clamping, and the hardcoded drop-chance slot bands). There are no regression guards for the config grammar or the deduplication logic.

---

## 12. Conclusions

- CNPCGene is the **configuration model and template store** for the W2PP mob-spawn and summon system. Its primary value is the `NPCGENLIST` spawn-entry structure, the 40-item summon catalog, and the file parsers that load them.
- The component is **data-centric and low-encapsulation**: consumers across seven translation units access `pList[i]` fields and `Mob[]` directly by hardcoded indices.
- Spawn generation *behavior* (scheduling, formation, patrol, stat scaling) lives **outside** the component in `Server.cpp`, `ProcessSecMinTimer.cpp`, and `CMob.cpp`, creating a cohesion concern: logic and data model are split across files.
- The most impactful risks are **unbounded parsing of external config files**, **modal error dialogs**, and the **complete absence of test coverage**.
- Runtime data files are external assets not present in the repository, so the component cannot be executed or validated from source alone.

---

*This report was produced by static source analysis. It does not modify any project files.*
