# Component Deep Analysis Report

**Component:** CNPCGene Spawn System
**Primary location:** `Code/TMSrv/CNPCGene.cpp`, `Code/TMSrv/CNPCGene.h`
**Project:** W2PP (decompiled C/C++ Win32 MMORPG server emulator, MSVC v143, MultiByte charset)
**Host process:** `Code/TMSrv/TMSrv.vcxproj` (TMSrv game/world server)
**Analysis date:** 2026-11-15 22:44:15
**Scope:** the component files themselves plus every in-process consumer required to explain their behaviour (spawn driver, scheduler, kill accounting, reload paths). Excluded per instruction: `.agents`, `.codegraph`, `.opencode`, `openspec`.

---

## 1. Executive Summary

The CNPCGene Spawn System is a **content/configuration adapter plus in-memory spawn-parameter registry**. It does not spawn anything itself: it parses the server's NPC generator text file into a fixed array of `NPCGENLIST` records (`mNPCGen.pList`), and every actual spawn decision is executed by `GenerateMob` in `Code/TMSrv/Server.cpp` and scheduled by `ProcessMinTimer` in `Code/TMSrv/ProcessSecMinTimer.cpp`. The component additionally owns a region rectangle table (`MAPREGION pRegion[]`), two **offline content-export tools** (`DropList`, `LevelList`) that write per-region drop/level text files, and a hardcoded **summon base table** (`CNPCSummon`) used by the mount/pet system.

In architectural terms the component is a **passive data provider with a self-contained parser** (`CNPCGenerator`), a **write-only auxiliary data class** (`CNPCSummon`), and two value types (`NPCGENLIST`, `MAPREGION`). It exposes **no network endpoints of any kind** (no REST/GraphQL/gRPC/socket surface); its only externally visible interface is C++ member access on a single global instance, `extern CNPCGenerator mNPCGen` (`Code/TMSrv/Server.h:62`, `:226`; `Code/TMSrv/ProcessClientMessage.h:66`). Its consumers reach directly into the public field `pList[]` rather than through accessor functions, which makes the data layout itself the published contract.

### Key findings

1. **The static footprint is dominated by a fixed 8,192-slot table.** `pList[MAX_NPCGENERATOR]` (`Code/TMSrv/CNPCGene.h:79`) with `MAX_NPCGENERATOR = 8192` (`Code/Basedef.h:109`) and an `NPCGENLIST` of roughly 3.1 KB (1,476-byte header plus two `STRUCT_MOB`, `Code/TMSrv/CNPCGene.h:29-51`) reserves approximately **25.5 MB of zero-initialised BSS**, plus `pRegion[]` (~1.18 MB) and `NumOld[]` (32 KB) — about 26.7 MB reserved at process start regardless of how many generator rows exist (`NumList`, typically a few thousand). The inline comment `// 2988 total` at `Code/TMSrv/CNPCGene.h:29` no longer matches the current `STRUCT_MOB` layout and is stale documentation.
2. **Spawn timing semantics are encoded in a misleadingly named integer.** `MinuteGenerate` is *not* minutes: the scheduler compares it against `MinCounter`, which increments once per 12-second `TIMER_MIN` tick (`Code/TMSrv/Server.cpp:3632`, `Code/TMSrv/ProcessSecMinTimer.cpp:2282`), and a GM command can change that tick at runtime (`Code/TMSrv/imple.cpp:571-578`). A configured `MinuteGenerate: 5` therefore means about one minute only while the tick stays at 12 s.
3. **`MinuteGenerate` is overloaded with four different meanings** — respawn interval, "disabled" sentinel (`-1`), an index-relocation descriptor when `>= 500` (`Code/TMSrv/Server.cpp:3010-3028`), and a jitter seed band after each spawn (`Code/TMSrv/ProcessSecMinTimer.cpp:2228-2251`). This single field drives spawn cadence, event gating, dungeon loot spawns, and generator relocation.
4. **The declared action arrays are partly unreachable.** `FightAction[4][80]`, `FleeAction[4][80]` and `DieAction[4][80]` (`Code/TMSrv/CNPCGene.h:42-44`) have **no parser branch** in `ParseString` (`Code/TMSrv/CNPCGene.cpp:159-288`), so they are always zero; consequently `Code/TMSrv/Server.cpp:7248-7249` (aggro taunt) and `Code/TMSrv/MobKilled.cpp:631-632` (death line) are dead branches, while `SegmentAction[]` and `FleeAction[]` reads at `Code/TMSrv/ProcessSecMinTimer.cpp:1486-1503` only ever produce speech for `SegmentAction`.
5. **Region assignment and both export tools are dormant in the shipping build.** `ReadRegion()`, `DropList()` and `LevelList()` are invoked only from commented-out startup code (`Code/TMSrv/Server.cpp:3646-3648`); no admin command reaches them. As shipped, `NumRegion == 0`, so if they were ever called every mob would be exported into `./Drop/Zona_Desconhecida.txt` / `./LevelList/Zona_Desconhecida.txt`.
6. **There is no automated test coverage of any kind**, and no test infrastructure to add it to: 0 test files, 0 test projects, 0 assertions, 0 frameworks across the repository (evidence in Section 10).
7. **Error handling is interactive and blocking.** A missing `NPCGener.txt` or an unreadable mob file raises a modal `MessageBox` (`Code/TMSrv/CNPCGene.cpp:52`, `:181`, `:197`, `:969`), which parks the server's UI thread — including on a live reload of a running server.
8. **The parser is a silent-failure normaliser.** Unknown keys return `FALSE` and are dropped without any log or diagnostic (`Code/TMSrv/CNPCGene.cpp:287-288`), and re-reading a file neither clears the table nor reports which rows disappeared (Section 3, BR-10).

### Fact table

| Property | Value |
|---|---|
| Files in component | 2 (`Code/TMSrv/CNPCGene.h` 101 lines, `Code/TMSrv/CNPCGene.cpp` 975 lines) |
| Types defined | 2 structs (`NPCGENLIST`, `MAPREGION`), 2 classes (`CNPCSummon`, `CNPCGenerator`) |
| Public methods | 5 on `CNPCGenerator`, 1 on `CNPCSummon`, 1 free function (`SetAct`) |
| Public data members | 5 arrays/counters on `CNPCGenerator`, 1 array on `CNPCSummon` |
| Outbound calls (distinct) | 16 CRT/Win32 functions, `std::list` operations, and 3 project elements (`ReadMob`, `BASE_InitModuleDir`, `g_pItemList`, plus the `hWndMain`/`temp` globals) |
| Inbound dependents | 7 translation units + 3 aggregate headers (Section 6) |
| Runtime data files owned | `NPCGener.txt` (read), `NPCGener.new.txt` (written on every load), `Regions.txt` (read), `./npc/*` (indirect via `ReadMob`), `./BaseSummon/*` (indirect), `./Drop/*.txt` and `./LevelList/*.txt` (written) |
| Threading model | single-threaded Win32 message pump; no locks, no atomics |
| Network endpoints | none |

---

## 2. Data Flow Analysis

### 2.1 Boot / reload pipeline (parse path)

```
1. WinMain startup -> Reboot()                                        (Server.cpp:3505, body at :7127)
2. BASE_InitializeMobname("MobName.txt", 0)                             (Server.cpp:7142)
3. mNPCGen.ReadNPCGenerator()                                           (Server.cpp:7144)
4.   fopen("NPCGener.new.txt","wt")   [output, never checked]           (CNPCGene.cpp:47)
5.   fopen("NPCGener.txt","rt")       [input,  NULL -> modal box]       (CNPCGene.cpp:48-55)
6.   per line: classify by first byte                                   (CNPCGene.cpp:71-119)
       '/'  -> comment/copy-forward, write to .new                       (:71-80)
       '#'  -> new record: Num++, zero SegmentListX/Y[0..4]              (:82-98)
       else -> ParseString(Num, tp)                                      (:102)
7.   ParseString: sscanf "key value value2" -> atoi -> field assignment  (CNPCGene.cpp:144-288)
       "Leader:"/"Follower:" -> ReadMob(&pList[i].X, "npc")              (:178, :194)
                                 -> _open("./npc/<MobName>", O_RDONLY)
                                 -> _read  sizeof(STRUCT_MOB)
                                 -> success => pList[i].Mode = MOB_USE
8.   NumList = Num + 1                                                  (CNPCGene.cpp:122)
9.   fclose(fs); fclose(fp)                                             (CNPCGene.cpp:124-125)
10. mSummon.Initialize()  [40 ReadMob calls from ./BaseSummon/*]        (Server.cpp:7162)
```

### 2.2 Scheduled spawn pipeline (hot path, every 12 s tick)

```
1. WM_TIMER(TIMER_MIN) -> ProcessMinTimer()                            (Server.cpp:3727-3728)
2.   for i in [0, mNPCGen.NumList)                                     (ProcessSecMinTimer.cpp:2212)
3.     MinuteGenerate = mNPCGen.pList[i].MinuteGenerate                (:2214)
4.     skip reserved indices 0,1,2,5,6,7 and intervals <= 0            (:2216-2219)
5.     if (MinCounter % MinuteGenerate == i % MinuteGenerate)          (:2222-2224)
6.       GenerateMob(i, 0, 0)                                          (:2226)
7.         MinuteGenerate >= 500 -> relocate index into [Y, Y+X-1]      (Server.cpp:3010-3028)
8.         merc carry injection from pMobMerc[]                        (Server.cpp:3001-3008)
9.         group size = MinGroup + rand()%(MaxGroup-MinGroup+1)        (Server.cpp:3030-3042)
10.        population gate: CurrentNumMob >= MaxNumMob -> return       (Server.cpp:3044-3048)
11.        restricted-zone gate -> MinuteGenerate = -1 and return      (Server.cpp:3060-3064)
12.        leader instantiated from pList[nindex].Leader               (Server.cpp:3069)
13.        segments 0..4 placed with +-Range jitter                    (Server.cpp:3089-3113)
14.        followers instantiated from pList[nindex].Follower          (Server.cpp:3229-3360)
15.        CurrentNumMob++ for leader and for each follower            (Server.cpp:3209, :3359)
16.        interval jitter re-rolled in one of four bands               (ProcessSecMinTimer.cpp:2228-2276)
17.   MinCounter++                                                     (:2282)
```

### 2.3 Death / accounting pipeline

```
1. MobKilled resolves the kill                                      (MobKilled.cpp)
2.   reads mNPCGen.pList[GenerateID].CurrentNumMob as "am I the last one?"  (:764, :805, ...)
3.   on last kill of a scripted encounter -> respawn / reward branch       (:766-801, :1076-1083)
4. DeleteMob(conn, Type!=0)                                          (Server.cpp:7020)
5.   if conn >= MAX_USER and GenerateIndex in [0, MAX_NPCGENERATOR)  (:7034-7038)
6.     mNPCGen.pList[geneidx].CurrentNumMob--, clamped at 0          (:7040-7043)
7. RebuildGenerator() (GM/quest path)                                (Server.cpp:3363)
8.   recount live mobs per GenerateIndex into NumOld[]               (:3368-3377)
9.   log and overwrite divergent CurrentNumMob values                 (:3379-3389)
```

### 2.4 GM / admin pipeline

```
1. ProcessImple parses chat commands (level gates)                   (imple.cpp)
2.  "generate <idx>"  level>=2, 1..NumList -> GenerateMob(idx, playerX, playerY)   (:660-671)
3.  "reloadnpc"                             -> ReadNPCGenerator()                  (:839-843)
4.  "rebuilgenerator"                       -> RebuildGenerator()                   (:1507-1511)
5.  "waterexp"/"exp"/"nigexp"               -> rewrite ./npc/<Name> from pList data,
                                               then ReadNPCGenerator()               (:1203-1309, :1311-1385, :1387-1493)
6.  "timer <ms>"      level>=3, >=5000 ms   -> SetTimer(TIMER_MIN, ms)
                                               (rescales every spawn interval)       (:571-578)
7.  Menu: Mob > ReloadMob -> ReadNPCGenerator()                                      (Server.cpp:4089-4091)
```

### 2.5 Dormant content-export pipeline

```
1. [disabled] mNPCGen.ReadRegion()  -> ./Regions.txt -> pRegion[], NumRegion   (Server.cpp:3646, commented)
2. [disabled] mNPCGen.DropList()    -> ./Drop/<Region>.txt   (append mode)     (Server.cpp:3647, commented)
3. [disabled] mNPCGen.LevelList()   -> ./LevelList/<Region>.txt (append mode)  (Server.cpp:3648, commented)
```

---

## 3. Business Rules & Logic

## Overview of the business rules

| Rule | Rule Type | Rule Description | Location |
|------|-----------|------------------|----------|
| BR-01 | Parsing / framing | A line whose first byte is `#` opens a new generator record; the record index is the running count of such lines minus one, and only `SegmentListX/Y[0..4]` are reset | `Code/TMSrv/CNPCGene.cpp:82-98` |
| BR-02 | Validation / parsing | Only the 32 hardcoded key names of `ParseString` are accepted; `key value value2` whitespace tokenisation, numeric values via `atoi`, unknown keys silently discarded | `Code/TMSrv/CNPCGene.cpp:144-288` |
| BR-03 | Business logic | A literal `0` after `Leader:` or `Follower:` means "no template" and aborts that key (`return FALSE`) | `Code/TMSrv/CNPCGene.cpp:173-174`, `:189-190` |
| BR-04 | Business logic | Mob templates are resolved from disk at parse time (`./npc/<MobName>`), and a successful read is what marks the record active (`Mode = MOB_USE`) | `Code/TMSrv/CNPCGene.cpp:171-201`, `Server.cpp:6974-7010` |
| BR-05 | Validation | Mob names are limited to `NAME_LENGTH` (16) bytes and are normalised (`_` and `@` to space) when a mob is instantiated | `CNPCGene.cpp:176`, `:192`; `Server.cpp:3079-3086`, `:3253-3260` |
| BR-06 | Validation | `StartX`/`StartY` must be strictly inside `(0, MAX_GRIDX)`; violations only raise a modal warning and the value is still stored | `Code/TMSrv/CNPCGene.cpp:214-226` |
| BR-07 | Business logic / patrol | Movement is a five-node model: start (0), intermediate segments 1-3, destination (4), each with X, Y, Range and Wait | `Code/TMSrv/CNPCGene.h:37-40`, `CNPCGene.cpp:208-273` |
| BR-08 | Business logic | Spawn position per node = configured coordinate +- Range, inclusive on both bounds; `Range == 0` means exact placement | `Code/TMSrv/Server.cpp:3089-3105`, `:3263-3279` |
| BR-09 | Validation | Speech/action strings are single whitespace-delimited tokens of at most 78 characters and only exist for segments 0-3 | `Code/TMSrv/CNPCGene.cpp:275-285`, `:965-975` |
| BR-10 | Lifecycle | `ReadNPCGenerator` is idempotent-ish but non-destructive: it re-bases indices at 0, keeps stale fields and stale tail rows, and rewrites `NPCGener.new.txt` as a side effect | `Code/TMSrv/CNPCGene.cpp:45-128` |
| BR-11 | Business logic / region | A spawn belongs to the first region rectangle (inclusive min/max) containing its start coordinate; otherwise `Zona_Desconhecida` | `Code/TMSrv/CNPCGene.cpp:293-317`, `:344-355`, `:621-632` |
| BR-12 | Business logic / loot export | Per-slot drop chance ladder with a 4-index exclusion list, emitted five-per-line per mob into per-region files | `Code/TMSrv/CNPCGene.cpp:319-594` |
| BR-13 | Business logic / progression export | Per-mob level listing derived from `STRUCT_MOB.BaseScore.Level`, one line per mob, per-region files | `Code/TMSrv/CNPCGene.cpp:596-667` |
| BR-14 | Operations | Region map and both export tools are disabled in shipped code | `Code/TMSrv/Server.cpp:3646-3648` |
| BR-15 | Spawn schedule | `MinuteGenerate` is a count of `TIMER_MIN` ticks (12 s default) and applies per-generator phase offset `i % interval` | `Code/TMSrv/ProcessSecMinTimer.cpp:2212-2226`, `Server.cpp:3632` |
| BR-16 | Spawn schedule | Generator indices 0, 1, 2, 5, 6 and 7 are never auto-spawned | `Code/TMSrv/ProcessSecMinTimer.cpp:2216` |
| BR-17 | Spawn schedule | After each scheduled attempt the interval is re-randomised inside its magnitude band; the top band also rolls dungeon loot when the dungeon event is on | `Code/TMSrv/ProcessSecMinTimer.cpp:2228-2276` |
| BR-18 | Population cap | A spawn is refused when `CurrentNumMob >= MaxNumMob`; the group size is clamped to the remaining capacity | `Code/TMSrv/Server.cpp:3044-3048` |
| BR-19 | Group size | Group size is uniform in `[MinGroup, MaxGroup]`; a non-positive span is logged and coerced to 1 | `Code/TMSrv/Server.cpp:3030-3042` |
| BR-20 | Group size | At most `MAX_PARTY` (12) followers are created per spawn, giving 13 entities per group | `Code/TMSrv/Server.cpp:3229`, `Basedef.h:168` |
| BR-21 | Composition | One leader from `Leader` template plus N followers from the `Follower` template, placed by formation offsets | `Code/TMSrv/Server.cpp:3069`, `:3247-3269` |
| BR-22 | Spawn relocation | `MinuteGenerate >= 500` reinterprets start X/Y as `[baseY, baseY+rangeX-1]` generator index range and spawns a random entry from it | `Code/TMSrv/Server.cpp:3010-3028` |
| BR-23 | Zone restriction | Spawns inside x 2440-2545 / y 1845-1921 are suppressed (permanently disabled) unless the leader wears item 219 | `Code/TMSrv/Server.cpp:3060-3064` |
| BR-24 | Population accounting | `CurrentNumMob` increments per spawned leader/follower, clamps at 0 on decrement, and is only decremented for `DeleteMob` with `Type != 0` on NPC slots | `Code/TMSrv/Server.cpp:3206-3209`, `:3356-3359`, `:7034-7043` |
| BR-25 | Reconciliation | `RebuildGenerator` recounts live mobs per `GenerateIndex` and force-syncs `CurrentNumMob`, logging every divergence | `Code/TMSrv/Server.cpp:3363-3390` |
| BR-26 | Spawn-time mutation | Before instantiation, `Leader.Carry` of the generator row is overwritten from the mercenary table and `GenerateIndex` is stamped on the spawned mob | `Code/TMSrv/Server.cpp:3001-3008`, `:3149`, `:3289` |
| BR-27 | Combat variance | Spawned mobs get skill affect from `Leader.BaseScore.MaxMp`, 75 % HP during the newbie event, and a 10 % clan flip from 1 to 2 | `Code/TMSrv/Server.cpp:3165-3173`, `:3211-3217`, `:3304-3313` |
| BR-28 | Event respawn hook | Consumers respawn a generator when its population drops to 1 (last-one-alive triggers) for scripted encounters | `Code/TMSrv/MobKilled.cpp:762-801`, `:1076-1083`; `ProcessSecMinTimer.cpp:1054-1093` |
| BR-29 | Event gating | Event modules set `MinuteGenerate = -1` to stop a generator and `1` to force it on every tick | `Code/TMSrv/CCastleZakum.cpp:141`, `:150`, `:335`, `:370` |
| BR-30 | Admin control | `generate`, `reloadnpc`, `rebuilgenerator`, `timer`, `exp`/`waterexp`/`nigexp` manipulate generator state and files under privilege gates | `Code/TMSrv/imple.cpp:571-578`, `:660-671`, `:839-843`, `:1203-1493`, `:1507-1511` |
| BR-31 | Static content | `CNPCSummon::Initialize` loads a fixed list of 40 named summons from `./BaseSummon` | `Code/TMSrv/CNPCGene.cpp:678-963` |
| BR-32 | Special-case spawn | Guild tower spawns receive the tower guild; merchant NPC 6 is registered as the zone tax collector | `Code/TMSrv/CWarTower.cpp:138-145`; `Server.cpp:3190-3196` |

## Detailed breakdown of the business rules

---

### Business Rule: BR-01 Record framing and index assignment

**Overview**:
A `#`-prefixed line starts a new generator record; the record's index is the ordinal of that line in the file, and each new record clears only its five start/segment coordinates.

**Detailed description**:
`ReadNPCGenerator` walks `NPCGener.txt` line by line with a 1024-byte `fgets` buffer (`Code/TMSrv/CNPCGene.cpp:59-68`). The first byte decides the line class: `47` (`/`) is a comment, `'#'` is a record delimiter, `13` (carriage return) is ignored, and everything else is a key/value line handed to `ParseString`. On a `#` line the counter `Num` (initialised to `-1`) is incremented and then used as the array index, so the first record in the file is index 0 and file order *is* identity. This is the single most load-bearing rule in the component: every consumer in the server refers to generators by numeric index, and `Code/Basedef.h:278-391` hard-codes a registry of those indices (`SECRET_ROOM_N_SALA1_MOB_1 = 2395`, `RUNEQUEST_LV1_TORRE1 = 5706`, `GTORRE = 1078`, and so on). Inserting or reordering a block in `NPCGener.txt` therefore silently repoints game content.

The reset performed at `Code/TMSrv/CNPCGene.cpp:86-90` is deliberately narrow: only `SegmentListX[0..4]` and `SegmentListY[0..4]` are zeroed. All other fields of that record keep whatever they held before, which matters for reloads (BR-10) and for the "disabled" sentinel write in BR-29. Because the array is a fixed member of a global object (`mNPCGen`, `Code/TMSrv/Server.cpp:266`) and is zero-initialised at load, a first-time parse starts from a clean slate; a parse of a shorter file does not.

Two further consequences follow. First, there is no uniqueness check on indices beyond file order: two blocks cannot share an index, but nothing prevents a block from being empty of keys, producing a record whose `Mode` stays `MOB_FREE` and whose statistics are all zero. Second, the component never validates that `Num < MAX_NPCGENERATOR` before writing `pList[Num]` (`:88`), so a file with more than 8,192 `#` lines writes past the end of a 25 MB member array — an out-of-bounds write reachable purely through content.

**Rule workflow**:

```
line = next line of NPCGener.txt
if line[0] == '/'  -> comment copy-forward
elif line[0] == '#' -> Num += 1
                       zero SegmentListX[0..4], SegmentListY[0..4] of pList[Num]
                       write "#[ index ]" banner to NPCGener.new.txt
elif line[0] == CR -> ignore
else               -> ParseString(Num, line)
```

---

### Business Rule: BR-02 Key whitelist tokenisation

**Overview**:
Each configuration line is `key value [value2]`, split by whitespace; only the 30 keys handled in `ParseString` are recognised, numerics are converted with `atoi`, and anything unrecognised is dropped without a diagnostic.

**Detailed description**:
`ParseString` reads at most three whitespace-delimited tokens into 128-byte buffers (`Code/TMSrv/CNPCGene.cpp:132-153`) and then runs a 32-branch `strcmp` chain. Numeric keys all share one `atoi(str2)` result (`value`); `str3`/`secondvalue` is computed but never used for any recognised key, which means the format is effectively key-plus-one-token despite looking like key-plus-two. The key set covers scheduling (`MinuteGenerate`, `MaxNumMob`, `MinGroup`, `MaxGroup`), templates (`Leader`, `Follower`), behaviour (`RouteType`, `Formation`), and the five segment groups (`Start*`, `Segment1*`-`Segment3*`, `Dest*`, plus `StartAction`/`Segment1Action`-`Segment3Action`).

The rejection path is the risky part. `str[0] == 47` and `str[0] == 0` return early (`:146-150`), and any unrecognised key falls through to `return FALSE` (`:287-288`). In `ReadNPCGenerator` the `FALSE` result causes `continue` (`:104-105`), so an unrecognised line is neither applied *and* not written to `NPCGener.new.txt`, the normalised copy the component emits. Operators therefore lose the only feedback channel the component has: a misspelled key such as `MaxNumMobs:` or `Segement2X:` is silently discarded from both the live table and the artefact that is supposed to reflect it. A second consequence is that `ParseString`'s return value conflates three distinct outcomes — "comment", "unknown key", "intentional `0` placeholder" — which is why no caller can log them separately.

There is also an unreachable length guard at `:155-156` (`strlen(str2) > 80`) placed *after* `atoi` has already run and after the value would in principle be applied; the branch can only trigger for an 81-127 character token, and in that case the key is dropped silently rather than truncated. Dialogue tokens never reach this guard because `SetAct` enforces a stricter 78-character ceiling (BR-09).

**Rule workflow**:

```
tokens = sscanf(line, "%s %s %s")
if line starts with '/' or is empty -> FALSE
value = atoi(tokens[1]); tokens[2] parsed but unused
match tokens[0] against the 30-key table
  hit  -> assign field (Leader/Follower also perform ReadMob)
  miss -> return FALSE  (line dropped from table and from NPCGener.new.txt)
```

---

### Business Rule: BR-03 Zero placeholder for optional templates

**Overview**:
`Leader: 0` and `Follower: 0` are explicit "not configured" markers that abort the key instead of attempting a file read.

**Detailed description**:
Both template branches call `strcmp(str2, "0")` first and return `FALSE` if it matches (`Code/TMSrv/CNPCGene.cpp:173-174`, `:189-190`). This lets a generator row declare only a leader (solo boss) or only a follower template while keeping the file fully populated with the standard key set — useful when a content author copies a template row and wants to blank one side. Because the placeholder returns `FALSE`, the line is also excluded from `NPCGener.new.txt`, so the normalised artefact loses the explicit `0` and shows only the keys that were actually applied.

The rule interacts with the population accounting in a way that is easy to miss: `GenerateMob` always tries to instantiate `Follower` for every group member up to the rolled group size (`Code/TMSrv/Server.cpp:3229-3360`). If the row's `Follower:` key was `0`, `pList[i].Follower` remains a zeroed `STRUCT_MOB` (or, on reload, whatever it held before; see BR-10), and `GenerateMob` will happily stamp that empty template onto up to twelve mob slots, incrementing `CurrentNumMob` for each. There is no guard that checks whether the follower template was actually populated, so the placeholder is only safe when `MaxGroup`/`MinGroup` also prevent followers.

The third effect is that `Mode` stays `MOB_FREE` unless *some* template load succeeded (BR-04). A row with `Leader: 0` and a valid `Follower:` becomes `MOB_USE`, and a row with both `0` stays `MOB_FREE`; nothing else in the codebase reads `Mode`, so this flag is effectively write-only bookkeeping.

**Rule workflow**:

```
parse "Leader: <token>"
  token == "0" -> return FALSE (no read, no assignment)
  else         -> copy name, ReadMob(...)
parse "Follower: <token>" identically
```

---

### Business Rule: BR-04 Disk-resolved mob templates

**Overview**:
A generator row does not contain mob statistics; it contains mob *names*, and the full `STRUCT_MOB` (756+ bytes of stats, equipment and carried items) is loaded from `./npc/<MobName>` while the text file is being parsed.

**Detailed description**:
`ReadMob` (`Code/TMSrv/Server.cpp:6974-7018`) builds `./<dir>/<MobName>` into the global `temp` buffer, opens it binary-read, and `_read`s exactly `sizeof(STRUCT_MOB)` bytes over the struct that was pre-populated with the name. The call site copies the name first with `strncpy(pList[i].Leader.MobName, str2, NAME_LENGTH)` (`Code/TMSrv/CNPCGene.cpp:176`), so the name survives even if the read fails. Success sets `pList[i].Mode = MOB_USE`; failure pops a modal `MessageBox` naming the mob (`:181`, `:197`) and leaves `Mode` untouched.

This design means the generator table is a *join* between two content stores with different formats and different failure modes: a text file that is human-editable and line-ordered, and a directory of fixed-size binary records referenced by name. Content correctness therefore depends on the existence of `./npc/<Name>` for every referenced name, on the binary record being exactly `sizeof(STRUCT_MOB)` (a struct-layout change silently reinterprets every mob file), and on the current working directory being the executable directory (`BASE_InitModuleDir`, `Code/Basedef.cpp:2608-2629`). None of those three preconditions is validated at load; failures surface per-name as blocking dialogs.

The practical consequence is that reloading is only partially safe. A reload with a missing mob file keeps the previously loaded stats for that record (because the struct is not cleared and `ReadMob` may leave it untouched when `_open` fails) while the name has already been overwritten — producing a record whose name and statistics disagree. This is the component's most consequential consistency gap for live operations.

**Rule workflow**:

```
name = token
strncpy(pList[i].X.MobName, name, 16)          # may under-terminate, see BR-05
ok   = ReadMob(&pList[i].X, "npc")
  ok  -> pList[i].Mode = MOB_USE
  !ok -> MessageBox("<name>", "Can't read NPC file in NPCGener.txt")
```

---

### Business Rule: BR-05 Name length and character normalisation

**Overview**:
Mob names are 16-byte fields; the parser copies with `strncpy` without guaranteeing termination, and the spawn path rewrites `_` and `@` to spaces before the name is broadcast to clients.

**Detailed description**:
`NAME_LENGTH` is 16 (`Code/Basedef.h:132`) and `STRUCT_MOB.MobName` is a 16-byte array (`:440`). The parser uses `strncpy(pList[i].Leader.MobName, str2, NAME_LENGTH)` (`Code/TMSrv/CNPCGene.cpp:176`, `:192`) where `str2` can be up to 127 characters. For a name of 16 or more characters the destination receives no NUL terminator, so every later `strlen`/`strncmp`/`%s` on that field (for example the dedupe comparisons in BR-12, or the `sprintf` diagnostics in `Code/TMSrv/imple.cpp:1217`) reads past the field into `Clan`, `Merchant`, `Guild` and beyond. The spawn path partially compensates by forcing `MobName[NAME_LENGTH-1] = 0` after the copy (`Code/TMSrv/Server.cpp:3071`), but the generator table itself remains unterminated.

Normalisation happens twice, at different times and with different rules. At spawn time, `GenerateMob` maps byte 95 (`_`) and byte 64 (`@`) to a space for the leader (`Code/TMSrv/Server.cpp:3079-3086`) and maps `_` to space and `@` to NUL for followers (`:3253-3260`) — note that followers terminate the name at the first `@`, leaders do not. Meanwhile the on-disk convention keeps underscores: the GM exp commands rebuild the file path by replacing spaces with underscores in the table name (`Code/TMSrv/imple.cpp:1219-1225`). So a name read from `NPCGener.txt` must use underscores to be findable on disk, but is displayed with spaces in game.

Because `ReadMob` composes its path from the raw field (`sprintf(temp, "./%s/%s", dir, mob->MobName)`, `Code/TMSrv/Server.cpp:6976`), an unterminated name also produces a path that runs into adjacent struct bytes, yielding the "ReadMob UNKNOW" log class rather than a clear diagnostic.

**Rule workflow**:

```
parse name -> strncpy 16 bytes (no forced terminator)
spawn      -> copy STRUCT_MOB, force MobName[15]=0
           -> replace '_' (95) with ' '
           -> leader: replace '@' (64) with ' '
           -> follower: replace '@' (64) with 0 (terminate)
```

---

### Business Rule: BR-06 Start coordinate validity window

**Overview**:
`StartX` and `StartY` are expected strictly inside `(0, MAX_GRIDX)`; out-of-range values are reported to a human and then stored unchanged.

**Detailed description**:
The parser checks `value <= 0 || value >= MAX_GRIDX` for both axes and raises `MessageBoxA(NULL, str1, pList[i].Leader.MobName, MB_OK)` when violated (`Code/TMSrv/CNPCGene.cpp:214-226`). Two things make this a soft rule rather than validation: the assignment happens unconditionally after the dialog, and `StartY` is compared against `MAX_GRIDX` rather than `MAX_GRIDY`. Both limits are 4096 today (`Code/Basedef.h:100-101`), so the axis mix-up is currently harmless but is a latent defect if the grid ever becomes non-square.

The consequence of storing an invalid start coordinate is deferred to the spawn path, where `SegmentListX[0]`/`Y[0]` become the mob's patrol anchor and the initial grid slot (`Code/TMSrv/Server.cpp:3199-3225`). `GetEmptyMobGrid` reports "No empty mobgrid" and the mob is discarded (`:3177-3187`), which is logged but not attributed back to the generator row that supplied the bad coordinate. In other words, a typo in a content file becomes an intermittent "no empty mobgrid" log line at spawn time, not a load-time error.

A second, undocumented use of these two fields exists: when `MinuteGenerate >= 500` they are reinterpreted as an index range (BR-22). A row that uses that overload must therefore keep plausible grid coordinates *and* a sane numeric pair, which is why the 2440-2545/1845-1921 zone check in BR-23 reads them as a real position as well.

**Rule workflow**:

```
parse StartX -> if (v <= 0 || v >= MAX_GRIDX) MessageBox(leader name)
             -> SegmentListX[0] = v          # stored regardless
parse StartY -> same test against MAX_GRIDX
             -> SegmentListY[0] = v
```

---

### Business Rule: BR-07 Five-node patrol model

**Overview**:
Each generator defines up to five movement nodes — start (0), three intermediates (1-3) and destination (4) — each carrying X, Y, a placement range and a wait time.

**Detailed description**:
The arrays `SegmentListX[5]`, `SegmentListY[5]`, `SegmentRange[5]` and `SegmentWait[5]` (`Code/TMSrv/CNPCGene.h:37-40`, `MAX_SEGMENT = 5` at `:27`) are filled by four key families: `Start*` → index 0 (`Code/TMSrv/CNPCGene.cpp:208-226`), `Segment1*`-`Segment3*` → indices 1-3 (`:239-273`), and `Dest*` → index 4 (`:227-237`). There is no `Segment4*` alias; the fourth intermediate is spelled `Dest`, which is the only naming asymmetry in the schema.

Only index 0 is mandatory in practice. `GenerateMob` treats a zero `SegmentListX[i]` as "this node does not exist" and clears the corresponding mob slots (`Code/TMSrv/Server.cpp:3089-3112`), while a non-zero X with a zero range means "walk exactly here" and a non-zero range means "pick a point within range" (BR-08). `SegmentWait[i]` is copied into the mob's `SegmentWait[i]` and `SegmentWait[0]` additionally becomes the initial `WaitSec` (`:3159`, `:3307`), i.e. the start node's wait doubles as the "stand still on spawn" delay.

Because the component stores only numbers, the behavioural interpretation of the nodes lives entirely in `CMob`'s path state machine; the component's contract is exactly "five parallel arrays, index-aligned, zero means absent". Any node with X set but Y zero will place the mob on the map's north edge rather than being rejected, an inconsistency that no load-time check catches.

**Rule workflow**:

```
Start*    -> idx 0 ; Segment1/2/3* -> idx 1..3 ; Dest* -> idx 4
per index i: X, Y, Range, Wait stored independently
spawn: if X[i] != 0 -> node exists
         Range[i] != 0 -> jitter, else exact
         Wait[i] copied
       else -> node absent (mob gets 0/0)
```

---

### Business Rule: BR-08 Range jitter on placement

**Overview**:
For a node with a non-zero range, the spawn coordinate is the configured coordinate shifted by a uniform random offset in `[-Range, +Range]` on each axis independently.

**Detailed description**:
`GenerateMob` computes `SegmentListX[i] - SegmentRange[i] + rand() % (SegmentRange[i] + 1)` for the leader (`Code/TMSrv/Server.cpp:3095-3099`) and applies the same arithmetic to followers, but relative to the leader's position plus a formation offset (`:3265-3273`). The expression yields values in the closed interval `[base - range, base]`... more precisely, with `rand() % (range+1)` in `[0, range]`, the result spans `[base - range, base]` on the low side through `base` on the high side for X, and identically for Y; the intended symmetric spread is therefore skewed upward by half a range. Both axes draw independent numbers, so the sampled area is a square, not a disc.

A range of 0 is a distinct case: the node is placed exactly (`:3102-3105`). Because the follower branch is gated on the *leader-row* node's range being non-zero (`:3265`) rather than on the follower's own placement, a leader node with `Range: 0` forces every follower onto the exact configured coordinates plus formation delta, collapsing the group into overlapping tiles; `GetEmptyMobGrid` then relocates them one by one, which is why heavily over-crowded spawn points produce a burst of grid-relocation behaviour.

The jitter also interacts with map bounds: there is no clamp, so a node at x=3 with range 5 can produce a negative coordinate, which is passed into `GetEmptyMobGrid` and the grid write at `:3225`. The grid index is `pMobGrid[y][x]` with signed `int` inputs, so a negative coordinate is an out-of-bounds access rather than a wrapped one.

**Rule workflow**:

```
for i in 0..4:
  if X[i] == 0 -> node absent
  elif Range[i] == 0 -> place at (X[i], Y[i])
  else -> place at (X[i] - R + rand()%(R+1), Y[i] - R + rand()%(R+1))
followers: place at (leader[i].x + formation[i][j][F], leader[i].y + formation[...])
GM spawn with PosX/PosY: all five nodes overwritten; followers get +-2 jitter
```

---

### Business Rule: BR-09 Speech/action limits

**Overview**:
Action strings exist for nodes 0-3 only, are single tokens, and are hard-capped at 78 bytes; longer or multi-word values are rejected (with a dialog) or silently truncated at the first space.

**Detailed description**:
`SetAct` (`Code/TMSrv/CNPCGene.cpp:965-975`) rejects any string of length 79 or more with a modal `MessageBox` and leaves the destination untouched, so the effective maximum is 78 characters plus terminator inside an 80-byte field (`Code/TMSrv/CNPCGene.h:41`). Because `ParseString` passes only `str2` — the second whitespace token — any action containing spaces is truncated to its first word before `SetAct` is ever consulted. Both constraints are silent to the author: the dialog appears once at load and the game then speaks the truncated text forever.

Coverage is also asymmetric. `SegmentAction[MAX_SEGMENT][80]` has five slots but only `StartAction`, `Segment1Action`, `Segment2Action` and `Segment3Action` exist as keys (`:275-285`); there is no `DestAction`, so node 4 can never speak. `FightAction`, `FleeAction` and `DieAction` (`Code/TMSrv/CNPCGene.h:42-44`) have no keys at all, so they remain all-zero for every record and every consumer branch that tests `[DieSay][0]` or `[say][0]` is permanently false.

Consumers then index these arrays with a *progress* value derived from the mob's own state: `SegmentAction[Progress]` on segment progression and `FleeAction[Progress]` on flee (`Code/TMSrv/ProcessSecMinTimer.cpp:1486-1503`), both guarded by `progres >= 0 && progres < MAX_SEGMENT`. The speech channel therefore exists and is robustly bounded, but only one quarter of its declared capacity is reachable from content.

**Rule workflow**:

```
parse "StartAction:"|"Segment1Action:"|"Segment2Action:"|"Segment3Action:" -> token
SetAct(dest, token):
   strlen(token) >= 79 -> MessageBox, keep old value
   else               -> strcpy(dest, token)
runtime: SegmentAction[progress] spoken on waypoint advance; Flee/Die/Fight never populated
```

---

### Business Rule: BR-10 Reload semantics and the normalised artefact

**Overview**:
`ReadNPCGenerator` can be invoked repeatedly at runtime (menu, GM command, reboot); each run re-bases indices at 0, rewrites a normalised copy `NPCGener.new.txt`, and leaves fields that the new file omits holding their previous values.

**Detailed description**:
Every invocation starts from `Num = -1` and reuses the same global array, so record 0 of the new file overwrites record 0 of the old one. Only the eight coordinate words are cleared per record (BR-01); `MinuteGenerate`, `MaxNumMob`, `MinGroup`, `MaxGroup`, `SegmentRange`, `SegmentWait`, `SegmentAction`, `Formation`, `RouteType`, `Leader` and `Follower` retain old values unless the new file explicitly restates them. `NumList` is then set to `Num + 1` (`Code/TMSrv/CNPCGene.cpp:122`), which *shrinks* the visible row count immediately, but rows between the new `NumList` and the old one keep their data and remain reachable through hard-coded indices in `Code/Basedef.h` — a state in which a mob can still be spawned at index 3000 while `NumList` reports 2000.

The file side effect is unconditional: `ReadNPCGenerator` opens `NPCGener.new.txt` for write *before* validating the input (`:47-48`) and never checks that handle. If the working directory is read-only the subsequent `fprintf` calls dereference NULL; if `NPCGener.txt` is missing the function opens the output file, shows a dialog, returns 0, and leaks the output handle (`:50-55`). Callers ignore the return value (`Code/TMSrv/Server.cpp:4090`, `:7144`; `Code/TMSrv/imple.cpp:841`, `:1307`, `:1383`, `:1491`), so a failed reload is indistinguishable from a successful one at the operator level — the only signal is the modal dialog itself.

A live reload also has a semantic hazard that is not visible in this file: spawned mobs keep the `STRUCT_MOB` copied at spawn time while `GenerateIndex` still points at the (possibly re-purposed) record, and `CurrentNumMob` for that record is *not* reset by the load. Reloading while the world is populated therefore mixes new definitions with old counters and old instances.

**Rule workflow**:

```
ReadNPCGenerator():
  fp = fopen("NPCGener.new.txt","wt")        # unchecked
  fs = fopen("NPCGener.txt","rt")
  fs==NULL -> MessageBox, return 0           # fp leaked
  Num = -1
  for each line: frame / parse / echo to fp
  NumList = Num + 1
  fclose both; return 1
callers ignore the return value
```

---

### Business Rule: BR-11 Region membership and fallback

**Overview**:
A generator's region is the first rectangle in `Regions.txt` whose inclusive min/max bounds contain its start coordinate; if no rectangle matches, the name `Zona_Desconhecida` is used.

**Detailed description**:
`ReadRegion` (`Code/TMSrv/CNPCGene.cpp:293-317`) reads `Regions.txt` with `sscanf("%d, %d, %d, %d = %s")` into `pRegion[NumRegion]`, incrementing the counter for every line read, without checking the parse result and without any bound against `MAX_NPCGENERATOR`. Blank lines, comments and malformed rows therefore create zero-filled rectangles (`min=max=0`, empty name) and a file longer than 8,192 lines overruns the array. Region lookup (`:344-352`, duplicated at `:621-629`) then takes the first match in file order and compares with `>=`/`<=` on all four edges, so adjacent regions that share a boundary are resolved in favour of whichever appears first, and a point exactly on a shared edge belongs to both.

Because `NumRegion` is only ever incremented by a load and never reset, calling `ReadRegion` twice appends duplicates instead of reloading. `DropList`/`LevelList` handle the no-match case by writing `Zona_Desconhecida` into the 256-byte `filename` buffer (`:354-355`), which is then concatenated into `./Drop/%s.txt` — a file that will only exist if the caller creates the directory.

The lookup is strictly two-dimensional and ignores movement: only `SegmentListX[0]`/`Y[0]` are tested, so a generator whose patrol crosses into a neighbouring region is still attributed to its start region. No code outside the two dormant export functions reads `pRegion` or `NumRegion`.

**Rule workflow**:

```
ReadRegion():
  for each line of Regions.txt:
     sscanf(line, "%d, %d, %d, %d = %s") -> pRegion[NumRegion]
     NumRegion++                                  # unconditional, unbounded
resolve(x, y):
  for j in 0..NumRegion-1: if x>=minX && y>=minY && x<=maxX && y<=maxY -> name
  none -> "Zona_Desconhecida"
```

---

### Business Rule: BR-12 Drop-list export and the per-slot chance ladder

**Overview**:
`DropList` walks every generator record, dedupes mob names through a `std::list<char*>`, resolves the region from the start coordinate, and appends each mob's carried items to `./Drop/<Region>.txt` with a per-slot "1/N" chance.

**Detailed description**:
The function has three separable rules. The **dedupe rule** keeps a `std::list<char*>` of names already emitted and skips a record when both its leader and follower names are present (`Code/TMSrv/CNPCGene.cpp:321-338`, `:472-476`). Two defects live here: the comparison length for the follower is taken from the *leader's* name (`strlen(pList[i].Leader.MobName)`, `:333`, `:474`, `:610`, `:650`), so a follower whose name is longer than the leader's is matched only on the prefix, and names are compared with `strncmp` on a length derived from the *current* record rather than the stored one, so a stored name that is a prefix of the current name is treated as a duplicate. The list also grows to at most two entries per record while each iteration rescans it, giving quadratic behaviour over up to 8,192 records.

The **inclusion rule** splits each mob's 64 carried slots into two passes. The first pass emits every slot whose `sIndex` is non-zero and *not* one of 412, 413, 419, 420 (`:368-371`); the second pass emits only those four indices (`:416-419`). The four excluded indices are therefore not suppressed but segregated into a second paragraph, which preserves whichever currency/quest items they represent while keeping them out of the main list. `price` is fetched from `g_pItemList[...].Price` on both passes but never used; the price filter is present only as a commented-out `if (price < 100000)` (`:373-376`, `:421-424`, `:491-494`, `:540-543`), evidence that a "only valuable drops" policy was tried and abandoned.

The **chance rule** maps the slot index to a denominator: 0-7 → 1000, 8-11 → 4, 12-23 → 1000, 24-55 → 1000, 56 → 1, 57 → 100, 58 → 500, 60 and up → 2500, everything else → 10000 (`:378-408`, repeated three more times at `:426-456`, `:497-527`, `:546-576`). Because the `k >= 60` branch precedes `k == 61 || k == 62`, the latter is unreachable and 61/62 receive 2500; slot 59 falls through to 10000. The quadruple duplication of an eleven-branch ladder is the component's largest copy-paste liability: four identical tables must be edited in lockstep.

**Rule workflow**:

```
for i in 0..NumList-1:
  skip if leader and follower names already in MobList
  region = first rectangle containing (X[0], Y[0]) else "Zona_Desconhecida"
  fp = fopen("./Drop/<region>.txt","a+")            # unchecked
  if leader not seen: emit "MobName: <n>", then
      pass 1: slots with sIndex not in {0,412,413,419,420} -> "name(idx)(1/chance), "
      pass 2: slots with sIndex in {412,413,419,420}       -> same format
  if follower not seen: same two passes
  fclose(fp)
```

---

### Business Rule: BR-13 Level-list export

**Overview**:
`LevelList` writes `MobName: <name> Level:<n>` into `./LevelList/<Region>.txt`, one entry per distinct mob, using the level stored in the template's base score.

**Detailed description**:
The rule is structurally identical to BR-12 minus the item logic: same dedupe list, same region resolution, same `a+` append, same unguarded `fopen` (`Code/TMSrv/CNPCGene.cpp:596-667`). The datum written is `MOB->BaseScore.Level` (`:641`, `:658`), the template's authored level rather than the instance's computed level, so the output reflects content configuration rather than runtime scoring.

The absence of any header, delimiter or sort order makes the produced file append-only and cumulative: repeated runs with `a+` accumulate duplicate blocks, and because no timestamp or marker is written, an operator cannot tell two runs apart. Combined with the `Zona_Desconhecida` fallback this means a run with `Regions.txt` missing produces a single large file whose name asserts a fact ("unknown zone") that is actually an artefact of the missing file.

The function also inherits the follower-name comparison defect described in BR-12 and the unbounded `sprintf`-into-`temp` pattern, though both buffers (256 and 4,096 bytes) are large enough for realistic region names.

**Rule workflow**:

```
for i in 0..NumList-1:
  skip if both names already emitted
  region = first matching rectangle else "Zona_Desconhecida"
  fp = fopen("./LevelList/<region>.txt","a+")       # unchecked
  leader  not seen -> "MobName: <n> Level:<BaseScore.Level>"
  follower not seen -> same
  fclose(fp)
```

---

### Business Rule: BR-14 Content tooling is disabled in the shipped build

**Overview**:
`ReadRegion`, `DropList` and `LevelList` have exactly one call site each, and all three are commented out.

**Detailed description**:
`Code/TMSrv/Server.cpp:3646-3648` contains the only references to these three methods in the entire repository, each prefixed with `//`. No GM command, timer handler, message handler or menu item calls them, and `ProcessImple` has no branch for them (the command table covers `generate`, `reloadnpc`, `rebuilgenerator`, `exp`, `waterexp`, `nigexp`, `timer`, `saveall`, `decay` and others, but nothing region- or list-related). The README advertises standalone tools (`DropTool`, `ExpTool`, `EDITAPNPC`) for these jobs, and those tools are not present in this repository snapshot — only `TMSrv`, `DBSrv`, `ClientPatch_v7662` and `release` exist under `Code/`.

The functional consequence is that `pRegion[]` and `NumRegion` are dead weight at runtime (approximately 1.18 MB of reserved memory plus the read path), and the drop/level export logic is unexercised code. If an operator re-enables the three calls, the behaviour they will observe is the un-fixed one: no directory creation, no `fopen` checks, no dedupe across runs, and — because `Regions.txt` is loaded by the first of the three calls — a hard dependency on call ordering.

This rule therefore matters for analysis rather than gameplay: BR-11, BR-12 and BR-13 describe code that exists and compiles but that no shipping scenario reaches, so any behavioural claim about "drop tables" must be traced to `MobKilled` rather than to this component.

**Rule workflow**:

```
Server startup:
  // mNPCGen.ReadRegion();      # disabled
  // mNPCGen.DropList();        # disabled
  // mNPCGen.LevelList();       # disabled
=> NumRegion stays 0, ./Drop and ./LevelList are never written by TMSrv
```

---

### Business Rule: BR-15 Spawn interval in ticks with per-generator phase

**Overview**:
A generator fires when `MinCounter % MinuteGenerate == i % MinuteGenerate`, where `MinCounter` advances once per `TIMER_MIN` tick (12,000 ms by default).

**Detailed description**:
The scheduler loops `i` over `[0, NumList)` on every `ProcessMinTimer` invocation and, for each visible row, computes `mod = i % MinuteGenerate` then tests `MinCounter % MinuteGenerate == mod` (`Code/TMSrv/ProcessSecMinTimer.cpp:2222-2224`). `MinCounter` is incremented at the end of the same function (`:2282`) and defined as an unsigned `0`-initialised global (`Code/TMSrv/Server.cpp:385`). With a 12-second timer (`SetTimer(hWndMain, TIMER_MIN, 12000, NULL)`, `:3632`), `MinuteGenerate = 5` fires every 5 ticks = 60 s, which explains the field's name; the field is a tick count, not a minute count, and the name is only correct at the default cadence.

The phase offset `i % MinuteGenerate` is what prevents a "thundering herd": two rows with the same interval fire on different ticks, spreading load. It also means the *absolute* offset of a row is a function of its index, so renumbering a block shifts its spawn phase — another way in which file order is semantic. The comparison uses `int` modulo with a value that the scheduler has just checked for positivity (`:2218`), so division by zero is avoided.

Because the tick period is mutable at runtime (`timer <ms>`, `Code/TMSrv/imple.cpp:571-578`, minimum 5,000 ms), every interval in the table is implicitly expressed in units of a value an operator can change while the server runs. Halving the tick halves every spawn period in the world at once, with no clamp and no recomputation, which is a global, unlogged behaviour change.

**Rule workflow**:

```
every TIMER_MIN (default 12000 ms):
  for i in 0..NumList-1:
     interval = pList[i].MinuteGenerate
     if i in {0,1,2,5,6,7} or interval <= 0 -> next
     if MinCounter % interval == i % interval -> GenerateMob(i,0,0) + jitter(interval)
  MinCounter++
```

---

### Business Rule: BR-16 Reserved generator indices

**Overview**:
Rows 0, 1, 2, 5, 6 and 7 are excluded from the automatic scheduler and can only be spawned explicitly.

**Detailed description**:
The exclusion is a literal disjunction at `Code/TMSrv/ProcessSecMinTimer.cpp:2216`: `if (i != 0 && i != 1 && i != 2 && i != 5 && i != 6 && i != 7 && i != -1)`. The `i != -1` term is vacuous because the loop variable is unsigned in effect (`i` is `int` but bounded below by 0). The exclusion is not documented and is not mirrored anywhere else, so these six rows still appear in `NumList`, still have `MinuteGenerate` values, and still accept GM `generate` calls and event-driven `GenerateMob` calls.

Their likely role is to hold the always-present town/service NPCs that the server places through other paths (shops, guards, quest NPCs whose placement is driven by `CreateMob` rather than the generator), but the code contains no comment or assertion to that effect — this is an inference, not a documented rule. What is certain is that the skip is positional: if a content file shifts by one block, a different six rows become "manual only".

Because the check is inside the scheduler and not inside `GenerateMob`, an operator who adds `MinuteGenerate: 30` to row 0 will see no spawns and no diagnostic.

**Rule workflow**:

```
scheduler considers i only if i not in {0,1,2,5,6,7}
all other entry points (GM generate, event handlers, MobKilled respawn) bypass this filter
```

---

### Business Rule: BR-17 Interval jitter bands and dungeon loot side effect

**Overview**:
Immediately after a scheduled spawn attempt, the row's interval is re-rolled inside the band it currently belongs to; when the row is in the highest band and the dungeon event flag is set, loot is also created at random dungeon positions.

**Detailed description**:
Four bands are applied in order after `GenerateMob` returns (`Code/TMSrv/ProcessSecMinTimer.cpp:2228-2251`): `[500,1000) → rand()%500+500`, `[1000,2000) → rand()%1000+1000`, `[2000,3800) → rand()%1800+2000`, `>=3800 → rand()%1000+3800`. The result stays inside the same band, so the band is an invariant chosen by the content author (a "roughly hourly" boss stays roughly hourly) while the exact tick drifts, breaking synchronisation between shards or restarts. Note that the jitter is applied after the spawn *attempt*, so a row whose spawn was refused by the population cap (BR-18) still gets a new interval — the cadence is independent of success.

The highest band carries an unrelated responsibility: `if (DUNGEONEVENT)` the code picks one of 30 `DungeonPos` entries, rolls 5-9 items from `DungeonItem[rand()%10]`, applies `SetItemBonus(...,0,0,0)` and calls `CreateItem` at the chosen position with a random rotation (`:2253-2276`). Spawning loot is therefore a side effect of a *spawn interval band*, not of a kill or an event lifecycle, which means dungeon loot availability is controlled by whichever generator rows happen to be in the ≥3800 band.

The two responsibilities have different natural owners (spawn cadence vs. dungeon loot) and different failure modes (interval drift vs. item spam), yet share one field and one branch. Any change to the band boundaries silently changes dungeon loot frequency.

**Rule workflow**:

```
GenerateMob(i,0,0)
interval' = jitter(interval) within its band
if interval was >= 3800 and DUNGEONEVENT:
   pick DungeonPos[rand()%30]
   for j in 0..(rand()%5+5)-1: CreateItem(pos, DungeonItem[rand()%10], rand()%4)
store interval'
```

---

### Business Rule: BR-18 Population cap and group clamp

**Overview**:
A spawn is refused when the row's live population has reached `MaxNumMob`; otherwise the rolled group size is reduced to the remaining capacity.

**Detailed description**:
Two checks implement the cap (`Code/TMSrv/Server.cpp:3044-3048`). The first returns immediately if `CurrentNumMob >= MaxNumMob`. The second, reached only when capacity remains, clamps `MinGroup` (which by then holds the *rolled* group size, confusingly named) to `MaxNumMob - CurrentNumMob`. Because the leader itself always consumes one slot, a row with `MaxNumMob = 1` spawns a leader and zero followers, and a row with `MaxNumMob = 0` never spawns at all — the zero case is the accidental way to disable a generator without writing `-1`.

The cap is enforced on the record identified by `nindex` (the requested index), while the population counter incremented later uses the same `nindex` (`:3209`, `:3359`) but the `GenerateIndex` stamped on the mob uses `index` (which can differ under BR-22). In the normal (non-relocated) case the two are equal and the invariant "counter tracks live instances" holds; under relocation the counter and the stamped index diverge, so the relocated record's counter rises while its mobs claim a different index.

The check also runs *before* the empty-slot and restricted-zone checks, so a refused spawn does no work at all, while a spawn that passes the cap but fails later still consumes no counter. Population therefore drifts downward whenever spawning fails halfway (for example when `GetEmptyMobGrid` rejects the position), which is exactly the condition `RebuildGenerator` exists to repair (BR-25).

**Rule workflow**:

```
rolled = MinGroup + rand()%(MaxGroup-MinGroup+1)
if CurrentNumMob >= MaxNumMob -> return (no spawn)
if CurrentNumMob + rolled > MaxNumMob -> rolled = MaxNumMob - CurrentNumMob
spawn 1 leader + min(rolled, MAX_PARTY) followers, incrementing CurrentNumMob each time
```

---

### Business Rule: BR-19 Group size range resolution

**Overview**:
Group size is uniform over `[MinGroup, MaxGroup]`; a `MaxGroup` lower than `MinGroup` logs an error and forces a group of one.

**Detailed description**:
`qmob = MaxGroup - MinGroup + 1` is computed, and if it is `<= 0` the code emits `"err,zero divide : <index>"` and sets `qmob = 1` (`Code/TMSrv/Server.cpp:3033-3040`). Since `qmob = 1` yields `MinGroup += rand() % 1` (i.e. unchanged), a reversed range produces a group of exactly `MinGroup` — which, if `MinGroup` is itself large, is the opposite of the intent. The log message names a "zero divide" that is actually a range inversion, so the diagnostic misdirects.

`MinGroup` and `MaxGroup` are plain `atoi` results with no lower-bound validation (`Code/TMSrv/CNPCGene.cpp:165-169`), so negative values are accepted. A negative rolled group makes the follower loop at `:3229` execute zero times, leaving the leader alone, and then the cap arithmetic at `:3047` compares against a negative bound in a way that can let the leader through even at capacity.

The variable naming is itself a hazard: `MinGroup` is overwritten with the rolled size (`:3042`) and then with the clamped size (`:3048`), so by the time the loop reads it the name refers to neither the configured minimum nor a group, but to the follower count that will be attempted.

**Rule workflow**:

```
span = MaxGroup - MinGroup + 1
span <= 0 -> log "zero divide", span = 1
followerCount = MinGroup + rand() % span
followerCount = min(followerCount, MaxNumMob - CurrentNumMob)
create min(followerCount, MAX_PARTY) followers
```

---

### Business Rule: BR-20 Party cap of twelve followers

**Overview**:
No spawn creates more than `MAX_PARTY` (12) followers, so a group is at most one leader plus twelve followers.

**Detailed description**:
The follower loop is `for (int i = 0; i < MinGroup && i < MAX_PARTY; i++)` (`Code/TMSrv/Server.cpp:3229`), with `MAX_PARTY = 12` (`Code/Basedef.h:168`). The cap is a hard structural limit rather than a configurable one: the party list that links followers to the leader (`pMob[tmob2].PartyList[i] = tempmob`, `:3245`) has exactly `MAX_PARTY` slots, so exceeding it would corrupt adjacent fields. The same constant is reused by the player party system, which means content authors cannot raise group size without affecting player parties.

Because the clamp happens after the population cap (BR-18), a row configured with `MaxGroup: 20` will reserve 20 slots of headroom in the cap arithmetic but only ever create 12 — the population counter can therefore never reach `MaxNumMob` for such rows, and the scheduler keeps attempting spawns that always cap at 13 entities.

The interaction with formation offsets (`g_pFormation[5][12][2]`, `Code/Basedef.h:2479`) shows the same constant: the formation table's second dimension is 12, matching `MAX_PARTY`, and is indexed by the follower ordinal `i`. If `MAX_PARTY` were increased without enlarging `g_pFormation`, the indexing at `Code/TMSrv/Server.cpp:3267-3269` would read past the table.

**Rule workflow**:

```
for i in 0..min(followerCount, MAX_PARTY)-1:
   slot = GetEmptyNPCMob()
   PartyList[i] = slot
   copy Follower template, apply formation offset, stamp GenerateIndex, Leader = leader
   CurrentNumMob++
```

---

### Business Rule: BR-21 Leader/follower composition and formation placement

**Overview**:
A group is one leader built from the `Leader` template and N followers built from the `Follower` template, each follower offset from the leader by a formation-specific delta.

**Detailed description**:
The leader copy is `memcpy(&pMob[tmob].MOB, &mNPCGen.pList[nindex].Leader, sizeof(STRUCT_MOB))` (`Code/TMSrv/Server.cpp:3069`) followed by name masking and an `Affect` clear; the follower copy is the analogous `memcpy` of `Follower` (`:3247`) but into `pMob[tempmob]` — note that the leader path copies into `.MOB` while the follower path copies over the whole `CMob`, so follower `Mode`, `GenerateIndex`, `PartyList` and segment fields are then rewritten field by field after the copy. That asymmetry is a decompilation artefact but it has a real effect: the follower's `CMob` is briefly populated with whatever followed `STRUCT_MOB`-shaped data in the source object, before being overwritten.

Placement for followers uses `g_pFormation[i][j][Formation]` as a per-follower, per-segment delta added to the leader's corresponding segment coordinate, gated on the leader row's `SegmentRange[j]` being non-zero (`:3265-3273`). `Formation` comes straight from `atoi` of the `Formation:` key with no range check against the table's second dimension (2) (`Code/TMSrv/CNPCGene.cpp:205-206`), so an authored `Formation: 5` walks off `g_pFormation` into adjacent globals at `Code/TMSrv/Server.cpp:3267`.

When a GM spawns at an explicit position, every segment node of every follower is overwritten with `PosX + rand()%5 - 2` / `PosY + rand()%5 - 2` (`:3280-3287`), discarding both the configured patrol and the formation spread — the mobs land in a 5x5 jittered cluster instead.

**Rule workflow**:

```
leader: copy Leader -> MOB; mask name; Formation, RouteType, Segments[i] with jitter; GenerateIndex = index
followers (i in 0..n-1):
   copy Follower -> CMob; mask name
   for j in 0..4: if Range[j] -> leader[j] + g_pFormation[i][j][Formation], Wait[j]
                  else       -> row's configured X/Y
   Leader = leaderSlot; GenerateIndex = index
```

---

### Business Rule: BR-22 MinuteGenerate overloaded as a relocation descriptor

**Overview**:
When `MinuteGenerate >= 500`, the scheduler spawns a randomly selected generator from the index range encoded in the row's start coordinates instead of spawning the row itself.

**Detailed description**:
`GenerateMob` reads `segx = SegmentListX[0]`, `segy = SegmentListY[0]`, computes `res = segx - segy` (unused), rejects the row when `segx - segy <= 0` with `"err,end index < start index"`, then computes `reloc = rand() % segx + segy` and logs `"err,wrong index relocation"` if the result falls outside `[0, NumList)` — but *keeps using it* regardless (`Code/TMSrv/Server.cpp:3010-3028`). The parameter is not used as a coordinate here but as an encoded range: `[segy, segy + segx - 1]`.

After relocation, two different indices are in play. `nindex` (the original row) supplies the templates, formation, group sizes and the population counter; `index` (the relocated row) supplies the segment coordinates and is stamped into `GenerateIndex` (`:3149`, `:3289`). The net effect is a **hybrid mob**: statistics and group composition from the descriptor row, patrol and death-accounting identity from a random member of the target range. When that mob dies, `DeleteMob` decrements the *relocated* row's counter (`:7040`), which was never incremented for it, so the accounting drifts on both rows.

This overload is the single clearest example of field reuse in the component: an interval field becomes a mode flag by magnitude, and two coordinate fields become integers by convention. It also explains why `MinuteGenerate` bands start at 500 in BR-17: the scheduler must not interpret a relocation row's value as a plain cadence, which is why the top band's jitter keeps it above 3,800 — comfortably clear of the 500 threshold while still being a legitimate interval for a very slow spawn.

**Rule workflow**:

```
interval = MinuteGenerate
if interval >= 500:
   range = SegmentListX[0]; base = SegmentListY[0]
   if range - base <= 0 -> log and return
   idx = rand() % range + base
   if idx out of [0, NumList) -> log (and continue)
else idx = requested index
templates/counters use requested index; segments/GenerateIndex use idx
```

---

### Business Rule: BR-23 Restricted-zone spawn suppression

**Overview**:
A generator whose start coordinate lies inside x 2440-2545 / y 1845-1921 is not spawned unless its leader template wears item 219, and the attempt permanently disables the row by setting `MinuteGenerate = -1`.

**Detailed description**:
The check is a compound condition at `Code/TMSrv/Server.cpp:3060-3064`: `Leader.Equip[0].sIndex != 219` and the start coordinate inside the hard-coded window. When it fires, the row's interval is set to `-1` and the function returns before instantiating anything. The window covers a specific map area (roughly 105x77 tiles) which the code does not name; the item 219 exception marks a particular NPC type that must still spawn there — most plausibly a tower/war objective or a guarded shop, though the code offers no comment, so the intent is an inference.

The rule is one-way and permanent for the process lifetime: `-1` is a disable sentinel (`interval <= 0` skips in the scheduler, BR-15), and nothing in this component restores it. Only external writers restore positivity — the castle quest sets `MinuteGenerate = 1` for its mob range (`Code/TMSrv/CCastleZakum.cpp:150`) and the jitter logic never produces a negative value, so a suppressed row stays suppressed until the next `ReadNPCGenerator` (which, per BR-10, may also leave `-1` in place if the file's value is re-read — it is, because `MinuteGenerate` is an explicit key).

A second, quieter consequence: the check runs *after* the population cap test but *before* any instantiation, so suppression does not disturb the counter. It does, however, run once per scheduled attempt, meaning the log-free suppression happens silently every time the row comes due — an operator sees a row with a plausible interval that never spawns and has no diagnostic to explain it.

**Rule workflow**:

```
if leader.equip[0] != 219 and 2440 <= X[0] <= 2545 and 1845 <= Y[0] <= 1921:
    MinuteGenerate = -1
    return
```

---

### Business Rule: BR-24 Population accounting

**Overview**:
`CurrentNumMob` counts live instances of a generator row: it increments once per created leader and once per created follower, and decrements only when an NPC-slot mob is removed through `DeleteMob` with a non-zero removal type.

**Detailed description**:
Increments happen inside `GenerateMob` at three points — leader (`Code/TMSrv/Server.cpp:3209`) and each follower (`:3359`), each preceded by a `if (CurrentNumMob < 0) CurrentNumMob = 0` normalisation that defends against the counter having been driven negative by earlier decrements. Decrements happen in `DeleteMob` under three conjunctive guards: `Type != 0`, `conn >= MAX_USER`, and `GenerateIndex` inside `[0, MAX_NPCGENERATOR)` (`:7032-7044`), followed by a clamp at zero. `DeleteMob` with `Type == 0` (a "fade out" removal) therefore leaves the counter untouched by design, which is the mechanism behind the drift described next.

The counter is *read* far more widely than it is written. `MobKilled` treats `CurrentNumMob == 1` as "this was the last member of the encounter" and fires reward or respawn logic on that basis (`Code/TMSrv/MobKilled.cpp:764-766`, `:805-807`, and the secret-room sums at `:1089-1342`); `ProcessSecMinTimer` sums groups of counters to detect empty rooms (`:500-503`); `bad` values therefore directly change quest outcomes. `CCastleZakum` and `CWarTower` no longer read it, but the `DeleteMob` path they use writes it.

Drift sources are structural: spawn failures after an increment (the increment happens after the grid placement, so this is mostly avoided), removals with `Type == 0`, entities created through `CreateMob` rather than `GenerateMob` (which never increments but may stamp a `GenerateIndex`), and the relocation path of BR-22 (increment and stamped index differ). `RebuildGenerator` is the designed repair for all of them.

**Rule workflow**:

```
spawn:   if CurrentNumMob < 0 -> 0 ; CurrentNumMob++  (per leader, per follower)
remove:  if Type != 0 and slot >= MAX_USER and 0 <= GenerateIndex < MAX_NPCGENERATOR:
             CurrentNumMob-- ; if < 0 -> 0
repair:  RebuildGenerator() recounts live mobs and overwrites the counter
```

---

### Business Rule: BR-25 Reconciliation by recount

**Overview**:
`RebuildGenerator` recounts live NPC-slot mobs per `GenerateIndex` into `NumOld[]`, logs every row whose counter disagrees, and overwrites `CurrentNumMob` with the recount.

**Detailed description**:
The function zeroes `NumOld[0..NumList)` (`Code/TMSrv/Server.cpp:3365-3366`), scans `pMob[MAX_USER..MAX_MOB)` counting non-empty mobs by `GenerateIndex` when the index is in `[0, NumList)` (`:3368-3377`), then walks `i` from `MAX_USER` to `NumList` and force-syncs any row whose `CurrentNumMob != NumOld[i]`, printing `Idx / Name / Old / New` for each (`:3379-3389`). It is the only writer of `NumOld`, and the only place in the codebase where generator counters are audited.

Three properties limit its usefulness. First, the recount loop starts at `MAX_USER` while the audit loop starts at `MAX_USER` too — but the population array it is auditing is indexed by *generator row*, and generator rows below `MAX_USER` (1,000) exist and are used (the registry in `Code/Basedef.h` starts at index 3). Counters for rows 0-999 are therefore never audited or repaired, even though they are exactly the town NPCs most likely to be re-spawned by hand. Second, `NumOld` is sized `MAX_NPCGENERATOR` but only the first `NumList` entries are cleared, so after a reload that shrinks `NumList`, entries beyond it retain stale counts (harmless because the audit range also shrinks). Third, the recount trust `pMob[i].GenerateIndex`, so any mob whose index was stamped incorrectly (BR-22, or `CreateMob` paths) propagates the error into the "truth" it is compared against.

Operationally the function is exposed as the GM command `rebuilgenerator` (`Code/TMSrv/imple.cpp:1507-1511`) and is called from one water-quest path (`Code/TMSrv/_MSG_UseItem.cpp:1755`), which hints at an observed counter defect that this repair was introduced to contain.

**Rule workflow**:

```
for i in 0..NumList-1: NumOld[i] = 0
for i in MAX_USER..MAX_MOB-1:
   if mob empty -> skip
   idx = mob.GenerateIndex
   if 0 <= idx < NumList -> NumOld[idx]++
for i in MAX_USER..NumList-1:
   if CurrentNumMob != NumOld[i]: log "Idx..Old..New" ; CurrentNumMob = NumOld[i]
```

---

### Business Rule: BR-26 Spawn-time mutation of the template

**Overview**:
Before instantiating, `GenerateMob` overwrites the row's `Leader.Carry[]` from the mercenary stock table, and stamps the spawned entities with `GenerateIndex`.

**Detailed description**:
For each of the `MAX_MOB_MERC` (128) mercenary slots, if `pMobMerc[i].GenerateIndex == index` and the index is non-zero, the mercenary's 64 carried items are copied over the generator row's `Leader.Carry` (`Code/TMSrv/Server.cpp:3001-3008`). This is a **write into the generator table performed by the spawner**, so the component's "configuration" data is mutated by gameplay: once a mercenary has stock, every subsequent spawn of that row inherits it, and the original carried items are lost until the next reload. It also means `DropList` (BR-12), if ever run, would export mercenary stock rather than authored drops depending on runtime history.

`GenerateIndex` stamping is the identity contract that makes all later accounting possible: leader (`:3149`), follower (`:3289`), with `-1` written on failure paths (`:2961`, `:3184`, `:3324`). Every read of `pMob[x].GenerateIndex` — `DeleteMob`'s decrement, `RebuildGenerator`'s audit, `MobKilled`'s encounter logic, `CMob`'s resist copy, `SetBattle`'s taunt, `CCastleZakum`'s cleanup sweep — depends on this stamp being a valid row with `0 <= index < MAX_NPCGENERATOR`, and only some of those readers bound-check it.

The mutation also has a subtle ordering effect: the carry overwrite happens *before* the relocation of BR-22, using the requested index, while the mob is later stamped with the relocated one. Mercenary stock therefore attaches to the descriptor row, not to the row of the mob that actually appears.

**Rule workflow**:

```
for i in 0..MAX_MOB_MERC-1:
   if pMobMerc[i].GenerateIndex == index and index != 0:
        copy pMobMerc[i].Carry -> pList[index].Leader.Carry ; break
... spawn ...
pMob[slot].GenerateIndex = index   (leader and each follower; -1 on failure)
```

---

### Business Rule: BR-27 Spawn-time combat variance

**Overview**:
Every spawned mob receives a skill affect derived from the leader template's `MaxMp`, has its HP reduced to 75 % while the newbie event is active and its level is below 120, and has a 10 % chance to flip clan 1 to clan 2.

**Detailed description**:
`skillnum = mNPCGen.pList[nindex].Leader.BaseScore.MaxMp` is used as a *skill index*, not as a mana pool: if non-zero the spawner calls `SetAffect(tmob, skillnum, 30000, 200)` and `SetTick(tmob, skillnum, 30000, 200)` (`Code/TMSrv/Server.cpp:3211-3217`, followers at `:3338-3344` read the leader's value, not the follower's — a small inconsistency). Content authors therefore encode a permanent aura by writing the skill id into `MaxMp` of the template, an overload that is invisible in the text file.

The newbie-event rule halves nothing but quarters one: `if (NewbieEventServer && CurrentScore.Level < 120) Hp = 3 * Hp / 4` (`:3165-3166`, `:3304-3305`), applied after `GetCurrentScore` and after `Hp = MaxHp`. It is a global event switch multiplied into the spawn path, so toggling the event changes the effective difficulty of every generator row at once without touching content.

The clan flip `if (MOB.Clan == 1 && rand() % 10 == 1) MOB.Clan = 2` (`:3172-3173`, `:3312-3313`) is evaluated per entity, so a single group can split across clans, which is intended for the "chaos"/hostile-clan mechanic but is chosen at spawn time and frozen thereafter.

**Rule workflow**:

```
copy template -> GetCurrentScore()
Hp = MaxHp ; if newbie event and level < 120 -> Hp = 3*Hp/4
if Clan == 1 and rand()%10 == 1 -> Clan = 2
if Leader.BaseScore.MaxMp != 0 -> SetAffect/SetTick(skill = MaxMp, 30000, 200)
```

---

### Business Rule: BR-28 Last-one-alive respawn hooks

**Overview**:
Consumers implement "respawn when the population reaches one" for scripted encounters, using the generator counters as the trigger.

**Detailed description**:
In `MobKilled`, after computing the target's generator id, several branches read `mNPCGen.pList[GenerateID].CurrentNumMob` and compare it to `1` before running reward or respawn logic (`Code/TMSrv/MobKilled.cpp:762-801` for the water rooms, `:803-820` for their antechamber, `:1076-1083` for the nightmare rooms). The pattern relies on `DeleteMob` having already decremented the counter for the mob that just died, so `CurrentNumMob == 1` means "the corpse being processed is the last of its row" — a fragile but consistent convention across the file.

The secret-room and rune-quest branches instead *sum* counters across several rows to decide whether a room is empty (`:1089-1342`) and gate tower availability on individual counters (`:1443-1449`, `:1502`). Because the counter is shared per row, two independent parties in the same row share the trigger, so "last one alive" can fire for a group that is merely the last of one party's contribution.

The respawn itself is an explicit `GenerateMob(id, 0, 0)` call from the consumer (`:1077-1083`, `:1994-2003`, `:2024`), meaning the component is invoked out of band with no regard for `MinuteGenerate`. A row that is simultaneously scheduled can therefore be double-spawned if the timer fires between the death and the respawn call.

**Rule workflow**:

```
kill resolved -> DeleteMob decrements CurrentNumMob
consumer: if pList[id].CurrentNumMob == 1 -> reward / notify / GenerateMob(id,0,0)
```

---

### Business Rule: BR-29 Event gating via the interval sentinel

**Overview**:
Event modules stop a generator by writing `-1` and force it to spawn every tick by writing `1`; the castle quest uses both directions in the same routine.

**Detailed description**:
`CCastleZakum` sweeps the castle map's grid on quest start and, for every NPC-slot mob found, writes `MinuteGenerate = -1` before deleting it (`Code/TMSrv/CCastleZakum.cpp:139-143`), then sets `MinuteGenerate = 1` for every quest mob index except the two bosses and calls `GenerateMob` twice per index (`:147-154`). The cleanup handler repeats the `-1` sweep on quest clear (`:333-337`, `:368-372`). Because `1` is a legal interval (spawn on every tick), "force on" and "every 12 seconds" are the same value.

The rule makes `MinuteGenerate` a three-valued control channel (disabled, scheduled, ASAP) with no encapsulation: any module can flip it, and the sweep writes it based on the `GenerateIndex` of whatever mob happens to be standing on a tile, with no bound check before indexing `pList[generate]` (`:141`, `:335`, `:370`). A mob with `GenerateIndex == -1` (a failed spawn residue) writes to `pList[-1]`.

For analysis purposes, this is the mechanism that reconciles BR-15 with the presence of scripted content: the schedule is the default, and events override it by direct field assignment. There is no counter, no reference count and no restoration path, so overlapping events (castle quest while a GM force-spawns the same row) leave the row in whichever state was written last.

**Rule workflow**:

```
quest start: for mobs on castle tiles: pList[GenerateIndex].MinuteGenerate = -1; DeleteMob
             for quest range: pList[x].MinuteGenerate = 1; GenerateMob twice
quest clear: for mobs on castle tiles: MinuteGenerate = -1; DeleteMob
```

---

### Business Rule: BR-30 Administrative control surface

**Overview**:
Privileged chat commands can spawn arbitrary rows, reload the table, rebuild counters, change the global tick, and rewrite mob experience files followed by a reload.

**Detailed description**:
`generate <idx>` requires level ≥ 2, rejects indices outside `1..NumList`, requires the caller to be a player slot, and spawns at the caller's position (`Code/TMSrv/imple.cpp:660-671`) — note the upper bound uses `NumList` while `GenerateMob` itself accepts up to `MAX_NPCGENERATOR`, so the command is *more* restrictive than the code it calls. `reloadnpc` calls `ReadNPCGenerator` and prints a fixed acknowledgement regardless of the return value (`:839-843`).

`timer <ms>` requires level ≥ 3 and a value of at least 5,000 ms, then re-arms the global `TIMER_MIN` (`:571-578`), which rescales every `MinuteGenerate` in the world (BR-15). `rebuilgenerator` invokes the reconciliation (BR-25, `:1507-1511`). The exp commands (`waterexp`, `exp`, `nigexp`) are the most invasive: they open `./npc/<MobName>` and `_write` a modified `STRUCT_MOB` whose `Exp` field is recomputed from a per-level multiplier, iterating hard-coded index ranges (`:1203-1309`, `:1311-1385`, `:1387-1493`), then call `ReadNPCGenerator` so the live table picks up the change. They read names out of `pList`, so they operate on whatever the last load produced.

These commands are the component's only *operational* interface. They are documented nowhere in the repository, are gated only by an integer level, and two of them (`timer`, exp rewriting) change persistent global state without confirmation.

**Rule workflow**:

```
"generate <n>"        lvl>=2, 1<=n<=NumList -> GenerateMob(n, playerX, playerY)
"reloadnpc"                 -> ReadNPCGenerator() (return ignored)
"rebuilgenerator"           -> RebuildGenerator()
"timer <ms>"          lvl>=3, ms>=5000 -> SetTimer(TIMER_MIN, ms)
"exp"/"waterexp"/"nigexp"   -> rewrite ./npc/* Exp for hard-coded ranges, then ReadNPCGenerator()
```

---

### Business Rule: BR-31 Summon base table

**Overview**:
`CNPCSummon::Initialize` loads a fixed, compile-time list of 40 summon names from `./BaseSummon` into `Mob[MAX_SUMMONLIST]`, and every load failure raises a modal dialog.

**Detailed description**:
The function is 285 lines of straight-line code: for each of 40 indices it `strcpy`s a literal name, calls `ReadMob(&Mob[i], "BaseSummon")` and shows a `MessageBox` naming the index on failure (`Code/TMSrv/CNPCGene.cpp:678-963`). The names run from `Condor`, `Javali`, `Lobo` through mount variants (`Sem_Sela_N`, `Fantasma_N`, `Leve_N`, `Equip_N`, `Andaluz_N`, `_B` equivalents) to high-end summons (`Fenrir`, `Dragao`, `Unicornio`, `Pegasus`, `Svadilfire`, `Sleipnir`). Two entries repeat earlier ones: index 10 is `Porco` (same as index 8) and index 11 is `Javali` (same as index 1) — consistent with a copy-paste artefact, though the summon system's index semantics cannot be confirmed from this component alone.

`MAX_SUMMONLIST` is 50 (`Code/Basedef.h:105`) while only 40 slots are populated, leaving indices 40-49 zeroed. Consumers index `mSummon.Mob[SummonID]` directly (`Code/TMSrv/Server.cpp:2591`, `:2673`), and the only guard observed in this component's consumers is the caller's own range logic, so entries 40-49 are valid indices that yield an empty template.

`Initialize` also calls `BASE_InitModuleDir()` (`:682`) which changes the process working directory to the executable's folder — a global side effect executed from a content loader, and the reason every relative path in this component resolves against the server binary rather than the launch directory.

**Rule workflow**:

```
BASE_InitModuleDir()
for i in 0..39:
    strcpy(Mob[i].MobName, <literal>)
    ok = ReadMob(&Mob[i], "BaseSummon")
    if !ok -> MessageBox("<name>", "Can't read NPC <i>")
```

---

### Business Rule: BR-32 Special-case spawn outcomes

**Overview**:
Two consumers attach identity to specific generator rows: the guild war tower spawn receives the tower guild, and merchant NPCs with `Merchant == 6` are registered as zone tax collectors.

**Detailed description**:
`CWarTower::GGenerateMob` is called from inside `GenerateMob` (`Code/TMSrv/Server.cpp:3147`) and, for `index == GTORRE` with a live `GTorreGuild`, sets the spawned mob's guild and zeroes its guild level (`Code/TMSrv/CWarTower.cpp:138-145`). This is a hook *into* the spawn path keyed on a hard-coded generator index (`GTORRE = 1078`, `Code/Basedef.h:386`), so the generator identity, not the template, decides the mob's allegiance.

The tax-collector rule is implemented in `GenerateMob` itself: `if (MOB.Merchant == 6 && tmob >= MAX_USER)` records the slot in `GuildImpostoID[level]` and assigns the zone's `ChargeGuild` (`Code/TMSrv/Server.cpp:3190-3196`). Here the discriminator is a template field read out of the binary mob file, so content controls it, and the side effect is a global registry indexed by the mob's level.

Both rules show the same architectural shape: `GenerateMob` is the only place where a spawn can be observed before delivery, so every system that needs "when X spawns, also do Y" attaches itself either to the function body or to a hook called from it. The component's data feeds those rules but does not own them; changing a row's index or `Merchant` value silently changes guild-war or taxation behaviour.

**Rule workflow**:

```
after leader instantiation:
   CWarTower::GGenerateMob(index,...) -> if index == GTORRE && GTorreGuild: set guild, level 0
   if MOB.Merchant == 6 && slot >= MAX_USER:
        GuildImpostoID[CurrentScore.Level] = slot
        MOB.Guild = g_pGuildZone[level].ChargeGuild
```

---

## 4. Component Structure

```
Code/TMSrv/CNPCGene.h                     (101 lines)  Component interface and record layouts
├── #define MOB_FREE 0 / MOB_USE 1                       :24-25   record activation flags
├── #define MAX_SEGMENT 5                                 :27      fixed patrol node count
├── struct NPCGENLIST                                     :29-51   one spawn definition (~3.1 KB)
│   ├── Mode, MinuteGenerate, MaxNumMob, CurrentNumMob    :31-34   lifecycle + schedule + population
│   ├── MinGroup, MaxGroup                                :35-36   group size range (also group cap inputs)
│   ├── SegmentListX/Y/Range/Wait[5]                      :37-40   patrol nodes
│   ├── SegmentAction[5][80]                              :41      waypoint speech (0-3 reachable)
│   ├── FightAction/FleeAction/DieAction[4][80]           :42-44   never populated (dead fields)
│   ├── Formation, RouteType, Offset                      :45-47   placement/AI hints; Offset unused
│   └── STRUCT_MOB Leader / Follower                      :49-50   full templates copied from ./npc/*
├── struct MAPREGION                                      :53-61   rectangle + 128-byte name
├── class CNPCSummon                                      :63-73   summon base table (40 of 50 slots)
│   └── STRUCT_MOB Mob[MAX_SUMMONLIST]                    :66
└── class CNPCGenerator                                   :75-96   the component's main object
    ├── NPCGENLIST pList[MAX_NPCGENERATOR]                :79      ~25.5 MB BSS
    ├── int NumOld[MAX_NPCGENERATOR]                      :80      reconciliation scratch (32 KB)
    ├── int NumList                                       :81      visible row count
    ├── MAPREGION pRegion[MAX_NPCGENERATOR]               :83      ~1.18 MB BSS, dormant
    ├── int NumRegion                                     :84
    ├── ReadNPCGenerator() / ParseString()                :91-92   text -> table
    ├── ReadRegion()                                      :93      Regions.txt -> pRegion (dormant)
    └── DropList() / LevelList()                          :94-95   table -> per-region exports (dormant)
└── void SetAct(char*, char*)                             :98      bounded string copy with dialog

Code/TMSrv/CNPCGene.cpp                   (975 lines)  Implementation
├── ctor / dtor                                           :32-43   zeroes ~26.7 MB at static init
├── CNPCGenerator::ReadNPCGenerator                       :45-128  line classifier + normalised echo
├── CNPCGenerator::ParseString                            :130-291 32-branch key dispatch
├── CNPCGenerator::ReadRegion                             :293-317 Regions.txt reader
├── CNPCGenerator::DropList                               :319-594 item/chance export
├── CNPCGenerator::LevelList                              :596-667 level export
├── CNPCSummon ctor / dtor                                :670-676
├── CNPCSummon::Initialize                                :678-963 40 fixed ReadMob calls
└── SetAct                                                :965-975 78-byte cap + dialog
```

### Record layout arithmetic

`NPCGENLIST` begins with 1,476 bytes of scalars and coordinates (`int Mode` through `int Offset`, `Code/TMSrv/CNPCGene.h:31-47`: 6 ints = 24, four `int[5]` = 80, `char[5][80]` = 400, three `char[4][80]` = 960, three ints = 12), followed by two `STRUCT_MOB` templates. `STRUCT_MOB` (`Code/Basedef.h:438-481`) is approximately 812 bytes with natural alignment: 24-byte identity block, `Coin` 4, `Exp` 8, two shorts 4, two `STRUCT_SCORE` at 48 each, `Equip[16]` (16 x 8) 128, `Carry[64]` (64 x 8) 512, then `LearnedSkill`, `Magic`, three `unsigned short` bonuses, three bytes of flags, `GuildLevel`, two `Regen` shorts and `Resist[4]`. The resulting record is ~3.1 KB and the table ~25.5 MB; the `// 2988 total` comment at `Code/TMSrv/CNPCGene.h:29` corresponds to an older `STRUCT_MOB` size of ~756 bytes and should be treated as stale.

### Boundaries

The component boundary is clean at the *file* level (only `CNPCGene.h/.cpp` implement it) but porous at the *state* level: `pList[]` is public, so server code writes into it (`Code/TMSrv/Server.cpp:3006`, `:3062`; `Code/TMSrv/CCastleZakum.cpp:141`, `:150`, `:335`, `:370`; `Code/TMSrv/imple.cpp:1234`, `:1267`, `:1300`, `:1352`, `:1376`, `:1418`, `:1451`, `:1484`) with no mediation, no validation and no notification.

---

## 5. Dependency Analysis

### Internal dependencies (outbound)

```
CNPCGenerator::ReadNPCGenerator
   └─> CNPCGenerator::ParseString
         ├─> CNPCGenerator::SetAct
         └─> ReadMob()                         [Server.cpp:6974]
               ├─> _open / _read / _close      [CRT]
               ├─> Log()                       [Server.cpp, external]
               └─> temp[4096]                  [Server.cpp:277]

CNPCGenerator::ReadRegion
   ├─> temp[4096]  (used as the fgets buffer)
   └─> MessageBoxA / fopen / fgets / sscanf

CNPCGenerator::DropList / LevelList
   ├─> pRegion[] / NumRegion
   ├─> pList[].Leader / .Follower
   ├─> g_pItemList[]                         [Basedef.h:2467]  (DropList only)
   └─> temp[4096] as the output path buffer

CNPCSummon::Initialize
   ├─> BASE_InitModuleDir()                  [Basedef.cpp:2608]
   └─> ReadMob() -> ./BaseSummon/*
```

### Inbound dependencies (consumers)

```
Server.cpp (266, 2995-3390, 3646-3648, 4089-4091, 7020-7054, 7127-7144, 7218-7252, 9378-9390)
   reads:  NumList, pList[].Leader/Follower/Segment*/Formation/RouteType/MinuteGenerate/
           MaxNumMob/CurrentNumMob/MinGroup/MaxGroup/FightAction
   writes: pList[].MinuteGenerate (:-1), pList[].Leader.Carry (:3006), pList[].CurrentNumMob

MobKilled.cpp (628-650, 716, 762-820, 1076-1090, 1089-1342, 1443-1502, 1994-2024)
   reads:  pList[].DieAction, pList[].CurrentNumMob
   writes: none (calls GenerateMob)

ProcessSecMinTimer.cpp (500-503, 1040-1093, 1339-1349, 1486-1503, 2212-2280)
   reads:  NumList, pList[].MinuteGenerate/SegmentAction/FleeAction/CurrentNumMob
   writes: pList[].MinuteGenerate (jitter bands)

imple.cpp (571-578, 660-671, 839-843, 1203-1493, 1507-1511)
   reads:  NumList, pList[].Leader/Follower/MobName/BaseScore.Level/Exp
   writes: pList[].Leader.Exp, pList[].Follower.Exp (then reloads)

CMob.cpp (630-649)
   reads:  pList[geneidx].Leader.Resist[0..3]

CCastleZakum.cpp (139-154, 333-372)
   reads:  pMob[].GenerateIndex -> pList[] index
   writes: pList[].MinuteGenerate (+1 / -1)

Server.h (62, 226), ProcessClientMessage.h (66), ProcessDBMessage.h (39)
   declare `extern CNPCGenerator mNPCGen` (Server.h also declares `extern CNPCSummon mSummon` at :227)
```

### External dependencies

| Dependency | Type | Purpose | Interface | Failure behaviour |
|---|---|---|---|---|
| `NPCGener.txt` | Content file (text) | Generator definitions | `fopen`/`fgets`/`sscanf`, 1024-byte lines | Modal dialog, `return 0`, leaked output handle |
| `NPCGener.new.txt` | Output artefact | Normalised copy of the parsed file | `fopen("wt")`/`fprintf` | Handle unchecked; NULL deref if unwritable |
| `Regions.txt` | Content file (text) | Region rectangles for export tools | `fopen("rb")` + `fgets` + `sscanf` | Modal dialog, `return`, `NumRegion` unchanged |
| `./npc/<MobName>` | Binary records | Mob templates (`STRUCT_MOB`) | `_open`/`_read` via `ReadMob` | Modal dialog per mob, record left stale |
| `./BaseSummon/<Name>` | Binary records | Summon templates | `_open`/`_read` via `ReadMob` | Modal dialog per index (40 possible) |
| `./Drop/<Region>.txt`, `./LevelList/<Region>.txt` | Output files | Content export | `fopen("a+")`/`fprintf` | Unchecked; NULL deref if directory absent |
| `g_pItemList[MAX_ITEMLIST]` | In-process table | Item names and prices | direct array read | No bounds check on `sIndex` |
| `g_pFormation[5][12][2]` | In-process table | Follower placement deltas | direct array read | `Formation` unvalidated |
| `hWndMain`, `temp[4096]` | Globals | Dialog parent, path/line buffer | direct use | `temp` shared with every other module (aliasing) |
| Win32 / CRT / STL | Platform | `MessageBox*`, stdio, `atoi`, `str*`, `std::list` | linked | Modal blocking on error paths |

### Important absence

No content data, no `npc/`, no `BaseSummon/`, no `Regions.txt`, no `NPCGener.txt`, no `Drop/` or `LevelList/` directory exists in the repository (verified by path glob across the whole tree). The component can therefore be **statically** analysed only; its input contract is inferred from the parser, and no data-driven validation is possible from this snapshot.

---

## 6. Afferent and Efferent Coupling

### Class / type level

| Component element | Kind | Ca (dependent files) | Ce (distinct external elements) | Instability I = Ce/(Ca+Ce) | Criticality |
|---|---|---|---|---|---|
| `CNPCGenerator` (class) | class | 8 | 6 (CRT, Win32, STL, Basedef data, Server globals, `ReadMob`) | 0.43 | High |
| `NPCGENLIST` (record) | struct | 7 | 1 (`STRUCT_MOB`) | 0.13 | High |
| `MAPREGION` | struct | 1 | 0 | 0.00 | Low |
| `CNPCSummon` | class | 1 | 3 (`ReadMob`, `BASE_InitModuleDir`, `STRUCT_MOB`) | 0.75 | Medium |
| `CNPCGenerator::ReadNPCGenerator` | method | 2 files / 6 call sites | 8 | 0.80 | High |
| `CNPCGenerator::ParseString` | method | 1 | 10 | 0.91 | High |
| `CNPCGenerator::ReadRegion` | method | 0 (commented) | 5 | 1.00 | Low |
| `CNPCGenerator::DropList` | method | 0 (commented) | 9 | 1.00 | Low |
| `CNPCGenerator::LevelList` | method | 0 (commented) | 8 | 1.00 | Low |
| `CNPCSummon::Initialize` | method | 1 (`Reboot`) | 4 | 0.80 | Medium |
| `SetAct` | free function | 1 | 3 | 0.75 | Low |

### File level (measured occurrences)

| File | References | Direction | Notes |
|---|---|---|---|
| `Code/TMSrv/Server.cpp` | 69 | afferent (read + write) | Spawn driver, accounting, reconciliation, menu, `ReadMob` |
| `Code/TMSrv/MobKilled.cpp` | 42 | afferent (read) | Encounter/last-kill logic, hard-coded index ranges |
| `Code/TMSrv/imple.cpp` | 30 | afferent (read + write + reload) | GM commands, exp rewriting |
| `Code/TMSrv/ProcessSecMinTimer.cpp` | 14 | afferent (read + write) | Scheduler, jitter, speech |
| `Code/TMSrv/CNPCGene.cpp` | 17 | self | implementation |
| `Code/TMSrv/CNPCGene.h` | 11 | self | declarations |
| `Code/TMSrv/CCastleZakum.cpp` | 4 | afferent (write) | Event gating |
| `Code/TMSrv/CMob.cpp` | 4 | afferent (read) | Resist derivation |
| `Code/TMSrv/Server.h` | 3 | afferent (declare) | `extern` aggregation |
| `Code/TMSrv/ProcessClientMessage.h` | 1 | afferent (declare) | Duplicate `extern` |

### Compile-time amplification

`CNPCGene.h` is included by three aggregate headers — `Code/TMSrv/Server.h:40`, `Code/TMSrv/ProcessClientMessage.h:40`, `Code/TMSrv/ProcessDBMessage.h:39` — and `Server.h` (directly or via `ProcessClientMessage.h`) is included by **76 of 80** translation units in `Code/`. A change to `NPCGENLIST` therefore recompiles essentially the entire server, and because `pList` is an inline member array defined in the header, any layout change also silently changes the meaning of every binary mob file read by `ReadMob`.

### Cohesion assessment (LCOM-style)

`CNPCGenerator` groups three disjoint field clusters with no method that touches more than two of them:

- `pList[]` + `NumList` — used by `ReadNPCGenerator`, `ParseString`, and (read-only) `DropList`/`LevelList`;
- `pRegion[]` + `NumRegion` — used only by `ReadRegion` and the two export methods;
- `NumOld[]` — not touched by any member function at all; it is mutated exclusively by `RebuildGenerator` in `Code/TMSrv/Server.cpp:3366-3388`.

Lack of cohesion (LCOM) is therefore high: the class is a bundle of three unrelated data sets plus loaders, not a cohesive service. This is not a defect in itself for a content adapter, but it explains the very different criticality of the parts (the spawn table is hot, the region table is dead).

---

## 7. Integration Points

| Integration | Type | Purpose | Protocol / mechanism | Data format | Error handling |
|---|---|---|---|---|---|
| `NPCGener.txt` | File system (content) | Generator definitions | stdio text read, 1024-byte lines | Plain text, `key value` lines, `#` framing | Modal `MessageBoxA`, `return 0`, output handle leaked |
| `NPCGener.new.txt` | File system (artefact) | Normalised/echoed copy of the parsed file | stdio text write | Plain text | `fopen` result never checked |
| `Regions.txt` | File system (content) | Region rectangles | stdio "rb" read + `sscanf` | `minX, minY, maxX, maxY = Name` | Modal dialog, silent return, counter not incremented |
| `./npc/<MobName>` | File system (binary content) | `STRUCT_MOB` templates | `_open`/`_read` (`ReadMob`) | Fixed-size binary struct | `MessageBox` per mob; `Log("err,ReadMob ...")`; record left stale |
| `./BaseSummon/<Name>` | File system (binary content) | Summon templates | `_open`/`_read` (`ReadMob`) | Fixed-size binary struct | `MessageBox` per index |
| `./Drop/`, `./LevelList/` | File system (output) | Content export (dormant) | stdio append | Plain text | None (unchecked `fopen`, NULL deref risk) |
| `g_pItemList[]` | In-process table | Item names/prices for the drop export | Direct array indexing | `STRUCT_ITEMLIST` | No index validation against `MAX_ITEMLIST` |
| `g_pFormation[]` | In-process table | Follower placement | Direct array indexing | `char[5][12][2]` | `Formation` value unvalidated |
| `GenerateMob()` | In-process API (consumer) | The actual spawner | Direct C++ call | `pList[]` fields + `rand()` | Log lines `err,no emptymob ...`, `err,No empty mobgrid ...` |
| `RebuildGenerator()` | In-process API (consumer) | Counter reconciliation | Direct call | `NumOld[]` | Logs each divergence |
| Win32 message loop | Platform | Reload trigger, timer | `WM_COMMAND` (menu), `WM_TIMER` | n/a | none |
| GM chat interpreter | In-process (privileged) | `generate`, `reloadnpc`, `rebuilgenerator`, `timer`, exp commands | Direct call from `ProcessImple` | text commands | Chat acknowledgement strings; return values ignored |

**Endpoints:** this component exposes no network endpoint of any protocol (no socket, no HTTP, no RPC surface). Its only remote-reachable effects are indirect, through `GenerateMob` calls made by message handlers owned by other components. The Endpoints section is therefore omitted, per the reporting scope.

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / assessment |
|---|---|---|---|
| Table-driven configuration (registry) | `NPCGENLIST pList[8192]` indexed by file order | `CNPCGene.h:79`, `CNPCGene.cpp:82-98` | Content as data; index identity is asserted through `Basedef.h:278-391` |
| Parse-and-normalise (ETL) | `ReadNPCGenerator` echoes a cleaned copy to `NPCGener.new.txt` while parsing | `CNPCGene.cpp:45-128` | Intended operator feedback; actually a silent filter for bad keys |
| Lazy materialisation / join at load | Names in text resolved to binary `STRUCT_MOB` during parsing | `CNPCGene.cpp:171-201` | Keeps text editable; couples load to a directory layout |
| Global singleton | `CNPCGenerator mNPCGen` defined once, `extern` in three headers | `Server.cpp:266`, `Server.h:62`, `ProcessClientMessage.h:66` | Simplest access for 76 TUs; duplicate declarations are an ODR risk |
| Aggregate object / record bundle | One class holding table + region map + scratch | `CNPCGene.h:75-96` | Low cohesion (Section 6); region map is unrelated to spawn |
| Sentinel value / overloaded field | `MinuteGenerate` as interval, disable flag, relocation descriptor, jitter band | `Server.cpp:3010-3028`, `CCastleZakum.cpp:141`, `ProcessSecMinTimer.cpp:2228-2251` | Compact but unreadable; drives four unrelated behaviours |
| Strategy by magnitude | Behaviour switches on numeric thresholds rather than explicit mode | `Server.cpp:3010`, `ProcessSecMinTimer.cpp:2228` | No named modes; undocumented |
| Snapshot + reconciliation | `NumOld[]` recount and force-sync | `Server.cpp:3363-3390` | Self-healing counter; covers up to index 999 gap |
| Template copy (prototype) | Spawns are `memcpy` of template records into live entities | `Server.cpp:3069`, `:3247` | Fast, but conflates template and instance state |
| Hook / extension point | `CWarTower::GGenerateMob` called from inside the spawner | `Server.cpp:3147` | Invasive extension point; only hook of its kind |
| Two-phase initialisation | Constructor zeroes, explicit `ReadNPCGenerator`/`Initialize` loads | `CNPCGene.cpp:32-39`, `:45`, `:678` | Load result not propagated to callers |
| Fail-fast via UI | `MessageBox` on any content error | `CNPCGene.cpp:52`, `:181`, `:197`, `:969` | Blocks the server thread; unsuitable for headless operation |
| Magic-number registry | Hard-coded generator ids in a shared header | `Basedef.h:278-391` | Content positions become compile-time constants |

**Architectural decisions observable from the code (documented as observations, not recommendations):**

1. **The component is a data owner, not a behaviour owner.** All spawning, scheduling and accounting logic lives in `Server.cpp` and `ProcessSecMinTimer.cpp`; the component supplies arrays and a loader. The afferent/efferent profile matches this: high afferent on data, minimal outbound logic.
2. **The public field is the API.** There are no getters/setters; 163 reference occurrences spread across six translation units read or write `pList[i].<field>` directly (69 in `Server.cpp`, 42 in `MobKilled.cpp`, 30 in `imple.cpp`, 14 in `ProcessSecMinTimer.cpp`, 4 each in `CCastleZakum.cpp` and `CMob.cpp`), and four of those files write. Encapsulation is nominal (a `class` with `public:` members only).
3. **Content identity is positional.** Indices are assigned by file order and then hard-coded across the server, so the file is effectively a binary database with a text syntax.
4. **Configuration is mutable at runtime by any module.** `MinuteGenerate`, `CurrentNumMob` and `Leader.Carry` are written from four different translation units without mediation or notification.
5. **Operational ergonomics are not automated.** Diagnostics are modal dialogs and free-text `Log` lines; there is no structured status surface, no metrics, no dry-run mode, and no validation report.

---

## 9. Technical Debt & Risks

### Complexity and size metrics (derived from source inspection)

| Function | Lines | Approx. cyclomatic complexity | Notes |
|---|---|---|---|
| `ReadNPCGenerator` | 84 | ~14 | 4-way line classification, unchecked I/O, duplicated formatting branches |
| `ParseString` | 162 | ~34 | 32-branch `strcmp` chain; one shared `atoi` |
| `ReadRegion` | 25 | ~3 | no parse validation |
| `DropList` | 276 | ~42 | 4 copies of an 11-branch chance ladder, 2 near-identical mob blocks |
| `LevelList` | 72 | ~12 | same structure as `DropList` minus items |
| `CNPCSummon::Initialize` | 286 | ~41 | 40 repeated copy/read/if blocks |
| `SetAct` | 11 | ~2 | size cap + copy |
| Component total | 920 code lines | ~146 | 2 files, zero tests |

Duplication: the chance ladder appears four times (`CNPCGene.cpp:380-408`, `:428-456`, `:499-527`, `:548-576`), identical byte for byte; the mob-export block appears twice for leader and twice for follower (`:360-470`, `:478-590`) with only the blacklist predicate negated.

### Risk register

| Risk | Level | Component area | Issue | Impact | Evidence |
|---|---|---|---|---|---|
| R-01 | High | `ReadNPCGenerator` | No bound check that the parsed record count stays below `MAX_NPCGENERATOR` before writing `pList[Num]` | Out-of-bounds write from a content file; memory corruption of ~25 MB of adjacent BSS | `CNPCGene.cpp:82-90` vs `CNPCGene.h:79` |
| R-02 | High | `ReadNPCGenerator` | Output file opened before input validation and never checked; handle leaked on the early return | NULL deref or handle exhaustion on a read-only/invalid deployment | `CNPCGene.cpp:47-55` |
| R-03 | High | `ReadNPCGenerator` | No per-record clear and no table clear; omitted keys keep previous values | A reduced or reordered file produces a table that mixes old and new definitions; `NumList` shrinks while hard-coded indices remain reachable | `CNPCGene.cpp:86-90`, `:122` |
| R-04 | High | Load error path | `MessageBox` on every content failure, including live reload | Blocks the server's UI/message thread; a reload with many bad mobs requires dozens of clicks | `CNPCGene.cpp:52`, `:181`, `:197`, `:969` |
| R-05 | High | `ParseString` | Unbounded `strncpy` into 16-byte `MobName` without forced terminator | Non-terminated strings; `strlen`/`strncmp` read into adjacent struct fields and into neighbouring records | `CNPCGene.cpp:176`, `:192` |
| R-06 | High | `DropList` / `LevelList` | `fopen` result never checked before `fprintf` | NULL-pointer dereference if `./Drop`/`./LevelList` do not exist (they do not exist in this repository) | `CNPCGene.cpp:358`, `:635` |
| R-07 | High | Scheduler / `MinuteGenerate` | Interval, disable flag, relocation descriptor and jitter band share one field | Cannot reason about a row from its configuration alone; four subsystems change behaviour when one value crosses 500 or 3800 | `Server.cpp:3010-3028`; `ProcessSecMinTimer.cpp:2228-2251` |
| R-08 | High | `GenerateMob` + BR-22 | Relocation increments the descriptor's counter but stamps the relocated index on the mob | Population counters diverge on both rows; last-kill triggers and room-empty checks fire incorrectly | `Server.cpp:3012-3028`, `:3209`, `:3149` |
| R-09 | Medium | `RebuildGenerator` | Audit loop starts at `MAX_USER` (1000) so rows 0-999 are never reconciled | Town/service generators keep drifting counters that no repair path fixes | `Server.cpp:3379` |
| R-10 | Medium | `ReadRegion` | `sscanf` result unchecked, `NumRegion++` unconditional, no `MAX_NPCGENERATOR` bound | Malformed or long `Regions.txt` creates bogus rectangles or overflows the array | `CNPCGene.cpp:305-315` |
| R-11 | Medium | `ReadRegion` / exports | `NumRegion` never reset; exports opened `a+` with no header or dedupe across runs | Repeated runs accumulate duplicates; region load twice duplicates rectangles | `CNPCGene.cpp:314`, `:349`, `:357`, `:626`, `:634` |
| R-12 | Medium | `DropList` / `LevelList` | Follower dedupe compares with `strlen(leader name)` | Followers with longer names are matched on a prefix; missed or spurious dedupe | `CNPCGene.cpp:333`, `:474`, `:610`, `:650` |
| R-13 | Medium | `DropList` chance ladder | `k >= 60` tested before `k == 61 || k == 62`, making the 5000 tier unreachable; slot 59 falls to 10000 | Exported chances do not match the documented intent of the ladder | `CNPCGene.cpp:401-408` (and 3 identical copies) |
| R-14 | Medium | Action arrays | `FightAction`, `FleeAction`, `DieAction` have no parser branch | Consumers' speech branches are permanently dead; content authors cannot set aggro/death lines | `CNPCGene.h:42-44`; `Server.cpp:7248`; `MobKilled.cpp:631` |
| R-15 | Medium | Action parsing | `SetAct` receives only the first whitespace token; 78-byte cap enforced with a dialog | Multi-word dialogue is impossible; over-long text silently retained as the old value | `CNPCGene.cpp:275-285`, `:965-975` |
| R-16 | Medium | Spawn placement | Range jitter is not clamped to the grid | Negative coordinates reach `pMobGrid[y][x]` | `Server.cpp:3095-3099`, `:3225` |
| R-17 | Medium | Template copy | `Formation` used to index `g_pFormation[5][12][2]` without validation | Out-of-bounds read of a global table for any `Formation` outside 0..1 | `CNPCGene.cpp:205-206`; `Server.cpp:3267` |
| R-18 | Medium | Spawn driver | Generator table mutated by the spawner (merc carry) | Authored drop data is silently replaced at runtime and never restored | `Server.cpp:3001-3008` |
| R-19 | Medium | Event gating | `pList[generate]` indexed from `pMob[].GenerateIndex` with no bound check in three sweeps | Write to `pList[-1]` or beyond when a mob carries an invalid index | `CCastleZakum.cpp:141`, `:335`, `:370` |
| R-20 | Medium | Scheduler | Global tick mutable via `timer <ms>` | `MinuteGenerate` silently changes meaning at runtime; all world spawn rates shift together | `imple.cpp:571-578` |
| R-21 | Medium | `CNPCSummon::Initialize` | 40 hard-coded names, two duplicated, `MAX_SUMMONLIST` 50 | Slots 40-49 are empty templates; duplicated names suggest a content defect | `CNPCGene.cpp:754-766` (indices 8/10, 1/11) |
| R-22 | Low | Memory | ~26.7 MB of BSS reserved for 8,192 rows and the dormant region map | Wasted footprint; slower construction (`memset` of ~26 MB at static init) | `CNPCGene.h:79`, `:83`; `CNPCGene.cpp:37-38` |
| R-23 | Low | `NPCGENLIST` | `Offset` field never read or written; `Mode` written but never read | Dead data members mislead readers | `CNPCGene.h:47`; `CNPCGene.cpp:184`, `:200` |
| R-24 | Low | Diagnostics | Error strings are mixed-language and mixed-encoding (`"NPCGener.txt¦í ¥"`, `"Não foi possivel..."`, `"Zona_Desconhecida"`) | Unreadable logs on non-Latin code pages | `CNPCGene.cpp:52`, `:301`, `:355` |
| R-25 | Low | `ParseString` | `secondvalue` parsed for every line, never used | Suggests a two-value grammar that does not exist | `CNPCGene.cpp:142`, `:153` |
| R-26 | Low | In-file documentation | `// 2988 total` no longer matches `STRUCT_MOB`'s size | Misleading capacity planning | `CNPCGene.h:29` |
| R-27 | Low | Global buffer reuse | `temp[4096]` used as the `fgets` buffer, path buffer and log formatter | Aliasing with every other module; a re-entrant call would corrupt paths | `CNPCGene.cpp:307`, `:357`, `:634`; `Server.cpp:277` |

### Security assessment

The component has no authentication, no network input and no privilege checks of its own; its exposure is entirely through **content files** and the **privileged command path**. The realistic attack surfaces are therefore: a writable `NPCGener.txt`/`npc/` directory (arbitrary stats, arbitrary `Carry` items, index-based content substitution), a writable working directory (arbitrary overwrite via `./Drop`/`./LevelList` if the dormant tools are enabled), and the unvalidated indices `R-01`, `R-17`, `R-19` which convert malformed content into memory writes/reads. There is no path traversal guard in `ReadMob` (`sprintf("./%s/%s", dir, mob->MobName)`) — a `MobName` containing `../` reads an arbitrary file of `sizeof(STRUCT_MOB)` bytes.

### Resilience assessment

There is no retry, no fallback content, no transactional load (partial tables remain live), no timeout and no health signal. The only recovery primitives are an interactive dialog, an ignored return code, and an operator-initiated full reload. `RebuildGenerator` is the single self-healing routine and it covers only indices ≥ 1000.

---

## 10. Test Coverage Analysis

### Evidence of test infrastructure

| Check | Command / method | Result |
|---|---|---|
| Test files by name | recursive listing of `Code/` for `*test*`, `*Test*`, `*spec*` | 0 files |
| Test projects | inspection of `W2PP Code Project.sln`, `Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj` | 2 projects, both `Application`; no test project |
| Test frameworks | content grep for `gtest`, `catch2`, `CppUnit`, `TEST_CASE`, `TEST_F`, `ASSERT_` | 0 matches in `Code/` |
| Assertions | content grep for `assert(` | 0 matches in `Code/` |
| CI / automation | recursive listing for `*.yml`, `*.yaml`, `CMakeLists.txt`, `Makefile`, `*.ps1`, `*.bat` | none outside `.vs/` IDE state |
| Fixtures / mocks for this component | search for `NPCGENLIST`, `MAPREGION`, `mNPCGen` initialisation in test-shaped code | none |
| Sample content data | search for `NPCGener.txt`, `Regions.txt`, `Drop/`, `LevelList/`, `npc/` anywhere in the tree | none |

The absence is repository-wide, not specific to this component: the only files matching "test" anywhere under `Code/` are none, and the README's "Fix all bugs / Create documentation / Re-create the project" checklist (`README.md:30-37`) confirms the project is a decompilation without a verification harness.

### Coverage table

| Component / function | Unit tests | Integration tests | Coverage | Test quality |
|---|---|---|---|---|
| `CNPCGenerator::ReadNPCGenerator` | 0 | 0 | 0 % | No tests. Parser branches (`/`, `#`, CR, key, unknown key, missing file) are entirely unverified; the only executable check is the modal dialog. |
| `CNPCGenerator::ParseString` | 0 | 0 | 0 % | No tests. 32 of 32 key branches unverified, including the `Leader: 0` placeholder and the unreachable `strlen > 80` guard. |
| `CNPCGenerator::ReadRegion` | 0 | 0 | 0 % | No tests. Malformed-line and overflow behaviour unverified. |
| `CNPCGenerator::DropList` | 0 | 0 | 0 % | No tests. Chance ladder (4 copies), 4-index blacklist, dedupe and region fallback unverified; `fopen` failure untested (would crash). |
| `CNPCGenerator::LevelList` | 0 | 0 | 0 % | No tests. |
| `CNPCSummon::Initialize` | 0 | 0 | 0 % | No tests. 40 load paths unverified; duplicate names unremarked. |
| `SetAct` | 0 | 0 | 0 % | No tests. The 79-byte boundary is untested. |
| Spawn scheduling (`ProcessMinTimer` loop) | 0 | 0 | 0 % | No tests. Phase arithmetic `MinCounter % interval == i % interval`, the reserved-index skip and the jitter bands are unverified. |
| Spawn execution (`GenerateMob`) | 0 | 0 | 0 % | No tests. Cap clamping, relocation overload, restricted zone, follower formation and counter increments are unverified. |
| Population accounting / `RebuildGenerator` | 0 | 0 | 0 % | No tests. |
| Module aggregate | 0 | 0 | 0 % | No coverage tooling is configured in either `.vcxproj`. |

### Risk statement

Every business rule in Section 3 is verified only by observation of the decompiled implementation. The rules with **no** executable evidence are those whose correctness cannot even be inferred from data, because the content files are absent from the repository: the entire spawn schedule (BR-15 to BR-18), the relocation encoding (BR-22), the restricted zone (BR-23) and the region map (BR-11) have neither tests nor fixtures nor sample inputs. The nearest thing to a specification is the code itself, plus the hard-coded index registry in `Code/Basedef.h:278-391` and the assumptions recorded in sibling component reports (for example `docs/reports/component-deep-analyzer/component-analysis-TMSrv-Server-Core-2026-11-15_22-08-08.md:494`, which independently notes that `MinuteGenerate` counts 12-second ticks).

The only practical verification artefacts that exist are runtime: the `Log("err,...")` lines emitted by `GenerateMob`/`RebuildGenerator` (`Code/TMSrv/Server.cpp:3019`, `:3025`, `:3037`, `:3054`, `:3179`, `:3237`, `:3319`, `:3384`) and the normalised `NPCGener.new.txt` the loader writes. Both are diagnostic side effects, not tests, and neither is asserted against anything.

---

## 11. Code Quality Metrics

| Metric | Value | Interpretation |
|---|---|---|
| Files / lines | 2 / 1,076 (975 `.cpp` + 101 `.h`) | Small surface, large data |
| Functions | 10 (7 member, 1 free, 2 lifecycle) | Few units of behaviour |
| Mean function length | ~92 lines | Skewed by `Initialize` (286) and `DropList` (276) |
| Approx. total cyclomatic complexity | ~146 | Concentrated in `ParseString` (~32), `Initialize` (~41), `DropList` (~42) |
| Maximum function complexity | ~42 (`DropList`) | Above the usual review threshold of 10-15 |
| Duplicated blocks | 4 x chance ladder, 2 x mob export block, 40 x summon load block | ~62 % of `DropList`, ~70 % of `Initialize` is repetition |
| Public data members | 5 arrays + 2 counters on `CNPCGenerator`, 1 array on `CNPCSummon` | No encapsulation; layout is the contract |
| Afferent coupling (files) | 9-10 | High (7 TUs + 3 aggregate headers) |
| Efferent coupling (class) | 6 categories | High (platform + project + data) |
| Instability of the class | 0.43 | Balanced, but its data is depended upon asymmetrically |
| Cohesion (LCOM-style) | High lack of cohesion | Three disjoint field clusters; one field cluster (`NumOld`) has no member function |
| Reserved memory | ~26.7 MB | 8,192-slot table (25.5 MB) + region map (1.18 MB) + scratch (32 KB) |
| Global mutable state touched | 8 objects | `pList`, `pRegion`, `NumList`, `NumRegion`, `NumOld`, `temp`, `hWndMain`, `g_pItemList` |
| Error-handling styles in one file | 3 | Modal dialogs, `Log` lines, silent `return`/`continue` |
| Dead code / fields | 5 | `FightAction`, `FleeAction`, `DieAction`, `Offset`, `NumOld` (unused in-component) |

---

**Analysis limitations**

- The component's content inputs (`NPCGener.txt`, `Regions.txt`, `npc/`, `BaseSummon/`, `Drop/`, `LevelList/`) are not present in this repository, so field-level defaults, real index assignments and typical interval values could not be inspected. All statements about content semantics are derived from the parser and from the hard-coded registry in `Code/Basedef.h:278-391`.
- `STRUCT_MOB`/`NPCGENLIST` byte sizes in Section 4 are computed from field arithmetic with default MSVC alignment; no build was executed to confirm `sizeof`. The `// 2988 total` comment disagrees with that arithmetic and is flagged as stale rather than resolved.
- Behaviour of the spawned mobs after instantiation (patrol execution, aggro, flee) belongs to `CMob` and was only traced far enough to establish how generator data is consumed.
- No file in the repository was modified during this analysis.
