# Component Deep Analysis Report — RankingSystem

- **Component**: `RankingSystem` (with its owned entity container `GrindRanking` and value type `GrindRankingConnId`)
- **Primary location**: `Code/DBSrv/CRanking.cpp`, `Code/DBSrv/CRanking.h`
- **Host executable**: `DBSrv` (`Code/DBSrv/DBSrv.vcxproj`, listed at `Code/DBSrv/DBSrv.vcxproj` `<ClCompile Include="CRanking.cpp" />`)
- **Project**: W2PP — decompiled C++ (Win32 / MSVC) MMORPG server emulator for W2
- **Analysis date**: 2026-11-15 22:54:49
- **Scope**: entire project root (`E:\wyd\test-setup\w2pp`), excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
- **Language / paradigm**: C++ with a C-style procedural core; two small classes, raw pointer arrays, Win32 API file walking

---

## 1. Executive Summary

`RankingSystem` is the DBSrv-side owner of the persistent **grind ranking** (experience leaderboard) of the W2PP server. It is a singleton domain service (`extern RankingSystem rankingSystem;`, `Code/DBSrv/CRanking.h:88`, instantiated at `Code/DBSrv/CRanking.cpp:29`) that holds a fixed-capacity, in-memory, order-maintaining table of 500 `STRUCT_RANKING` records and pushes change notifications back to the game servers (`TMSrv`) over the DBSrv message protocol.

The component has two collaborating classes:

- `GrindRanking` (`Code/DBSrv/CRanking.h:50-66`) — a fixed-size container of 500 heap-allocated `STRUCT_RANKING*` slots (`m_GrindRank[MAX_RANK_INDEX]`) plus a parallel array of connection identifiers (`m_GrindRankConnId[MAX_RANK_INDEX]`) used to route notifications to the online session that owns each ranked character. It provides name lookup, slot get/set, and connection-id get/set.
- `RankingSystem` (`Code/DBSrv/CRanking.h:68-86`) — the service that loads the ranking from the per-account files on disk, applies the insertion/eviction/ascension algorithms, answers queries, and emits `MSG_SendExpRanking` packets to the sessions that are affected by a change.

Role in the system: the ranking is written once per 10 minutes (`Code/DBSrv/Server.cpp:2041-2046`) and rebuilt from scratch on every DBSrv start (`Code/DBSrv/CRanking.cpp:94-111`). It is never used as a source of truth for character data — the authoritative experience value is the character record inside the account file; the ranking is a derived, lossy, best-effort projection (it drops the worst entry when full, drops entries of inactive accounts at load time, and never persists slot 0).

Key findings (details and evidence in later sections):

1. **Ordering is an invariant maintained by adjacent recomputation, not by a global sort.** Every mutation (insert, value update) writes to a single slot and then bubbles that element upward one slot at a time until the pairwise comparison fails (`Code/DBSrv/CRanking.cpp:257-268`, `:379-388`).
2. **Class tier dominates experience entirely.** The comparison operator is `(A.Value > B.Value && A.ClassMaster >= B.ClassMaster) || A.ClassMaster > B.ClassMaster`, so a strictly higher class tier always leapfrogs regardless of experience.
3. **Class-master values are normalized on write** (`MORTAL 2 → 1`, `ARCH 1 → 2`, `Code/DBSrv/CRanking.cpp:224-228`, `:367-371`), because the raw `#define`s (`Basedef.h:178-179`) are in the opposite order to the required precedence.
4. **The two update paths use different, mutually inconsistent comparison formulations** (`tryInsertInRanking` uses `>=`, `increaseRankingElementValue` uses `<=` and compares the element's class against the *incoming parameter* instead of against the neighbour, `Code/DBSrv/CRanking.cpp:381`), and the caller-side gate in `CFileDB.cpp:2033` compares a **raw** `ClassMaster` against a **normalized** stored one. For `MORTAL` characters this gate degenerates to "always true", which makes `increaseRankingElementValue`'s unconditional assignment (`Code/DBSrv/CRanking.cpp:377`) able to **lower** a stored experience value.
5. **Two confirmed memory leaks**: 500 `STRUCT_RANKING` objects are allocated with `new` and never released (`Code/DBSrv/CRanking.cpp:36`, no destructor in `Code/DBSrv/CRanking.h:50-66`), and every character/account login leaks one `new STRUCT_RANKING(...)` (`Code/DBSrv/CFileDB.cpp:851`, `:1091`).
6. **Connection ids are registered but never revoked** (`Code/DBSrv/CFileDB.cpp:847`, `:1087` are the only writers of a live id; nothing clears them on logout), so notification routing can target a stale `(TMSrvId, PlayerId)` pair.
7. **No automated tests of any kind exist in the project** for this component or anywhere else (evidence in Section 13).

---

## 2. Data Flow Analysis

### 2.1 Startup / load path

```
1. Static initialization of `RankingSystem rankingSystem` (CRanking.cpp:29)
2. RankingSystem::RankingSystem() (CRanking.cpp:94) -> loadRanking() (CRanking.cpp:99)
3. GrindRanking member already constructed (CRanking.cpp:32-39): 500 x new STRUCT_RANKING(),
   all slots {Name="", Value=0, ClassMaster=0, Level=0, Class=0}; all conn ids (-1,-1)
4. GetTickCount() start mark (CRanking.cpp:101)
5. readAccountsInDir("./account/*") (CRanking.cpp:102 -> :118)
   5.1 FindFirstFile("./account/*") then FindNextFile loop (CRanking.cpp:127, :135)
   5.2 skip entries starting with "." (CRanking.cpp:139)
   5.3 directories -> recurse into "./account/<name>/*" (CRanking.cpp:142-146)
   5.4 files: FileTimeToSystemTime(ftLastWriteTime) vs GetLocalTime (CRanking.cpp:149-152)
   5.5 activity window filter: drop if write-year differs, or write-month < current month - 1 (:155-156)
   5.6 path assembly into tmp[1024] (CRanking.cpp:158-160)
   5.7 fopen_s "rb"; size check 7500..7600 or == sizeof(STRUCT_ACCOUNTFILE) (CRanking.cpp:165-175)
   5.8 fread full STRUCT_ACCOUNTFILE into a zeroed stack buffer (CRanking.cpp:177-180)
   5.9 for each of MOB_PER_ACCOUNT (4) characters (CRanking.cpp:182):
       - skip empty MobName (CRanking.cpp:184)
       - skip CurrentScore.Level >= 1000 (CRanking.cpp:187)
       - tryInsertInRanking(OUT_OF_RANK, MobName, Exp, mobExtra.ClassMaster,
                            CurrentScore.Level, Class, NULL, NULL, NULL) (CRanking.cpp:192)
       - entryCount++ unconditionally (CRanking.cpp:194)
6. Log("Read %d ranking entries in %d.%d seconds") (CRanking.cpp:107-108)
7. CReadFiles::WriteRanking() (CRanking.cpp:110) -> regenerates ../../Common/Ranking.txt
```

### 2.2 Runtime update path (per experience gain)

```
1. TMSrv: mob killed, exp granted (TMSrv/MobKilled.cpp:335)
2. TMSrv builds STRUCT_RANKING(MobName, Exp, extra.ClassMaster, CurrentScore.Level, Class)
   and MSG_UpdateExpRanking (TMSrv/MobKilled.cpp:337-342) and sends on DBServerSocket
   (4 identical emission sites: MobKilled.cpp:338, :432, :525, :619)
3. DBSrv CFileDB::ProcessMessage case _MSG_UpdateExpRanking (CFileDB.cpp:1990)
4. Lookup by name: grindRanking.getElementIndex(Name) (CFileDB.cpp:1998 -> CRanking.cpp:57)
5a. NOT FOUND (rankId == OUT_OF_RANK):
    - snapshot the current LAST element and its conn id into locals (CFileDB.cpp:2005-2007)
    - tryInsertInRanking(...) (CFileDB.cpp:2009 -> CRanking.cpp:222)
      * normalize ClassMaster (CRanking.cpp:224-228)
      * gate against LAST slot (CRanking.cpp:232)
      * snapshot evicted LAST into olderLastRank/olderLastConnId (CRanking.cpp:238-242)
      * install conn id at LAST (CRanking.cpp:244-247)
      * allocate new STRUCT_RANKING, setElement(LAST, newRank, deleteOlder=true) (CRanking.cpp:249-251)
      * bubble from 0 upward while superior (CRanking.cpp:257-268) -> returns final rankId
    - if accepted: notify evicted owner with OUT_OF_RANK (CFileDB.cpp:2018-2019)
    - notify the displacement range (CFileDB.cpp:2023 -> CRanking.cpp:321)
5b. FOUND (rankId in [0,499]):
    - gate: Value/ClassMaster vs stored Value/ClassMaster (CFileDB.cpp:2033)
    - increaseRankingElementValue(rankId, (int)Value, ClassMaster) (CFileDB.cpp:2035 -> CRanking.cpp:363)
      * normalize classvalue (CRanking.cpp:367-371)
      * unconditional thisRank->Value = newValue (CRanking.cpp:377)
      * bubble upward (CRanking.cpp:379-388) -> returns final rankId
    - notify the displacement range (CFileDB.cpp:2038)
6. Notification: sendUpdateRangeRank(initId, endId) (CRanking.cpp:321)
   -> for i = initId-1 .. endId+1, clamped to [0,499]:
      read conn id; validate ranges; sendUpdateIndividualRank(i, TMSrvId, PlayerId, element)
7. sendUpdateIndividualRank (CRanking.cpp:279) builds MSG_SendExpRanking:
   RankPosition, PlayerAbove, PlayerRank, PlayerUnder; then
   pUser[tmsrvId].cSock.SendOneMessage(...) if pUser[tmsrvId].cSock.Sock != 0
8. CPSock::SendOneMessage -> AddMessage + SendMessageA (Code/CPSock.cpp:686-693)
9. TMSrv ProcessDBMessage case _MSG_GrindRankingData (TMSrv/ProcessDBMessage.cpp:1284-1297):
   forwards to the client only if Mode == USER_PLAY and PlayerRank.Name matches the
   receiving character's name
10. Client receives the ranking panel data
```

### 2.3 Login / session-binding path

```
1. DBSrv case _MSG_DBAccountLogin (CFileDB.cpp:843-852) and case _MSG_DBCharacterLogin (CFileDB.cpp:1083-1092)
2. getElementIndex(MobName)
3. if found in [0,499]: setElementConnId(rankId, GrindRankingConnId(conn, sm.ID))  (CFileDB.cpp:847, :1087)
4. sendUpdateIndividualRank(rankId, conn, sm.ID, new STRUCT_RANKING(...))           (CFileDB.cpp:851, :1091)
   -> heap object allocated per login and never freed
5. No code path ever resets the conn id (no revocation on logout/save)
```

### 2.4 Persistence path

```
1. DBSrv ProcessSecTimer (Server.cpp:2013), driven by SetTimer(hWndMain, TIMER_SEC, 1000) (Server.cpp:613)
   inside the Win32 single-threaded message pump (Server.cpp:617-621)
2. every SecCounter % 600 == 0 (Server.cpp:2041) -> CReadFiles::WriteRanking()
3. CReadFiles::WriteRanking (CReadFiles.cpp:1043)
   fopen(RANKING_PATH = "../../Common/Ranking.txt", "wt")   (CReadFiles.cpp:58, :1047)
   for i = FIRST(499) down to i > LAST(0), pos starting at 1:
       if Name[0] != 0 && Level < 1000:
           fprintf(fp, "%d %s %s %s\n", pos, Name, classmaster[ClassMaster], classe[Class])
4. fclose  (note: file truncated by "wt"; no temp-file + rename)
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Domain invariant | Ranking has exactly 500 slots; index 499 = best (`FIRST`), index 0 = worst (`LAST`), `-1` = `OUT_OF_RANK` sentinel | `Code/DBSrv/CRanking.h:25-30`; `Code/Basedef.h:61` |
| BR-02 | Ordering | Slot order is descending by a two-key comparison: class tier first, experience second; equal keys never swap | `Code/DBSrv/CRanking.cpp:259`, `:381` |
| BR-03 | Validation (normalization) | `ClassMaster` is rewritten on every insertion/update: `MORTAL(2) -> 1`, `ARCH(1) -> 2`, others unchanged | `Code/DBSrv/CRanking.cpp:224-228`, `:367-371` |
| BR-04 | Eligibility / admission | A non-ranked character may enter only if its experience exceeds slot 0's experience and its class tier is at least slot 0's, or its class tier strictly exceeds slot 0's | `Code/DBSrv/CRanking.cpp:232` |
| BR-05 | Eviction | Admission always replaces (and destroys) the occupant of slot 0; the evicted record and conn id are snapshotted first so the evicted player can be notified | `Code/DBSrv/CRanking.cpp:238-251` |
| BR-06 | Ascension | After being written to slot 0 an accepted element bubbles upward one adjacent swap at a time until the comparison against the next slot fails | `Code/DBSrv/CRanking.cpp:257-268`; swap at `:342-356` |
| BR-07 | Update of ranked player | A ranked player's stored experience is overwritten and the element bubbles upward; the write is unconditional once the caller-side gate passes | `Code/DBSrv/CRanking.cpp:363-392`; gate at `Code/DBSrv/CFileDB.cpp:2033` |
| BR-08 | Update gate (caller side) | Update is attempted only if `(newValue > storedValue && rawClass <= storedClass) || rawClass > storedClass` | `Code/DBSrv/CFileDB.cpp:2033` |
| BR-09 | Identity | Character identity in the ranking is the character name string, compared case-sensitively with `strcmp` over 500 slots; names are copied into a fixed 32-byte field | `Code/DBSrv/CRanking.cpp:57-74`; `Code/Basedef.h:822,829` |
| BR-10 | Startup reconstruction | The ranking is rebuilt from `./account/**` files at construction time; only "active" accounts (write year == current year and write month >= current month - 1) are considered | `Code/DBSrv/CRanking.cpp:102`, `:149-156` |
| BR-11 | Load eligibility (per character) | Per account, up to 4 characters are considered; a character is skipped if its name is empty or its level is >= 1000 | `Code/DBSrv/CRanking.cpp:182-190` |
| BR-12 | Load file validation | An account file is parsed only if its size is in [7500, 7600] or exactly `sizeof(STRUCT_ACCOUNTFILE)` | `Code/DBSrv/CRanking.cpp:175` |
| BR-13 | Persistence format | `../../Common/Ranking.txt` is a 4-column text file `position name classmasterLabel classLabel`, one row per non-empty slot with `Level < 1000`, positions starting at 1 | `Code/DBSrv/CReadFiles.cpp:1043-1066` |
| BR-14 | Persistence scope | Only indices 499 down to 1 are exported, so slot 0 (`LAST`) is never persisted | `Code/DBSrv/CReadFiles.cpp:1055` |
| BR-15 | Persistence cadence | The file is regenerated every 10 minutes and immediately after startup load | `Code/DBSrv/Server.cpp:2041`; `Code/DBSrv/CRanking.cpp:110` |
| BR-16 | Notification fan-out | Every element in `[start-1, end+1]` of a displacement range is re-notified, but only if it has a validated conn id | `Code/DBSrv/CRanking.cpp:321-337` |
| BR-17 | Notification content | Each packet carries `RankPosition` plus up to three neighbour records (`PlayerAbove`, `PlayerRank`, `PlayerUnder`) depending on the boundary case | `Code/DBSrv/CRanking.cpp:289-310` |
| BR-18 | Notification addressing | A packet is emitted only when `pUser[tmsrvId].cSock.Sock != 0` and the ranges `tmsrvId in [0, MAX_SERVER]`, `playerId in [1, MAX_USER]` hold | `Code/DBSrv/CRanking.cpp:283`, `:312-313` |
| BR-19 | Session binding | A ranked character's conn id is bound at login and is never unbound | `Code/DBSrv/CFileDB.cpp:847`, `:1087` |
| BR-20 | Memory ownership | The container owns its 500 slots; `setElement(..., deleteOlder=true)` destroys the replaced record, swaps use `deleteOlder=false` to transfer ownership without destruction; accepted input pointers become container-owned | `Code/DBSrv/CRanking.cpp:41-47`, `:347-349`; `Code/DBSrv/CRanking.h:63` |
| BR-21 | Eviction notification contract | The evicted player is notified with `RankPosition = OUT_OF_RANK` and `PlayerRank` = the snapshot of the record it used to hold | `Code/DBSrv/CFileDB.cpp:2018-2019`; `Code/DBSrv/CRanking.cpp:289-292` |
| BR-22 | Empty-slot semantics | Fresh slots are `{Value 0, ClassMaster 0}`, so any character with positive experience is admissible while the table is not full | `Code/DBSrv/CRanking.cpp:36`; `Code/Basedef.h:811-818` |

### Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Fixed capacity with inverted index convention

**Overview**:
The ranking is a fixed-capacity array of 500 entries whose index axis is inverted relative to the domain notion of position: index 499 is rank #1 and index 0 is rank #500.

**Detailed description**:
`MAX_RANK_INDEX` is defined as 500 (`Code/Basedef.h:61`). The `RankPos` enumeration in `Code/DBSrv/CRanking.h:25-30` names the two ends of the array: `FIRST = MAX_RANK_INDEX - 1` (499) and `LAST = 0`, plus a separate sentinel `OUT_OF_RANK = -1` that denotes "this character is not in the ranking at all". Because the array is never resized or reallocated, capacity is a hard structural limit: when 500 characters are present, the only way for a 501st character to appear is to displace the occupant of slot 0. All algorithms in the component assume the array is already densely ordered and that `LAST` is the worst entry, which is what makes a single-slot insert plus a bubble pass sufficient.

The inverted convention leaks into every consumer. `sendUpdateRangeRank` iterates `initId - 1` to `endId + 1` and clamps to `[LAST, FIRST]` explicitly because the arithmetic can produce `-1` or `500` at the boundaries (`Code/DBSrv/CRanking.cpp:325-328`, with the explanatory comment at `:327`). `CReadFiles::WriteRanking` iterates downward from `FIRST` and assigns text positions upward from 1 (`Code/DBSrv/CReadFiles.cpp:1055`). `sendUpdateIndividualRank` decides which neighbour to attach based on whether the position equals `FIRST`, `LAST`, or is in between (`Code/DBSrv/CRanking.cpp:289-305`). Any code that mixes the two conventions (array index vs. display position) is therefore silently off by one; for example, the persisted text position for index 499 is 1, so a persisted "position 1" is array index 499.

A subtle consequence is that `OUT_OF_RANK = -1` is a legal sentinel but adjacent to the valid range. Several guards are written as compound conditions that tolerate `-1` by checking it explicitly first (`Code/DBSrv/CRanking.cpp:281`, `:289`), and `getElement` returns `NULL` for any index outside `[0,499]` (`Code/DBSrv/CRanking.cpp:49-55`). Because `RankingSystem` never checks that return value before dereferencing (`Code/DBSrv/CRanking.cpp:232`, `:291`), correctness depends entirely on the caller's index discipline rather than on the accessor.

**Rule workflow**:
```
capacity = 500 (fixed)
index 499 -> displayed position 1   (FIRST, best)
index 0   -> displayed position 500 (LAST, worst)
index -1  -> character not ranked   (OUT_OF_RANK)
writes enter at LAST(0) then bubble upward to a higher index
reads/notifications clamp every index into [0, 499]
```

---

### Business Rule: BR-02 — Two-key ordering: class tier dominates, experience breaks ties

**Overview**:
Two entries are compared with the predicate `(A.Value > B.Value && A.ClassMaster >= B.ClassMaster) || A.ClassMaster > B.ClassMaster`, which means class tier is the primary key and experience the secondary key.

**Detailed description**:
The predicate appears twice with different spellings: in the insertion bubble loop (`Code/DBSrv/CRanking.cpp:259`) it compares the moving element against the next element using `>=` on class; in the value-update bubble loop (`Code/DBSrv/CRanking.cpp:381`) it uses `<=` on class and compares against the incoming `classvalue` parameter rather than against the neighbour. In both cases the second disjunct — "A's class is strictly greater than B's class" — makes class an absolute override: an entry with a strictly higher class tier is considered superior **regardless of experience**, so a celestial-class character with 1 experience point outranks a mortal character with the maximum possible experience.

Experience is compared with strict `>` in the first conjunct. That means equal experience values never cause a swap, and equal experience with equal class also never causes a swap; ordering among fully tied entries is therefore whatever order the sequence of insertions produced (a stable, insertion-order tie-break, though the order among ties is the *reverse* of arrival in the sense that a newly arrived tied entry stays below the incumbent). This is a deliberate-looking property of `>` versus `>=` and is what keeps the bubble algorithm terminating: a comparison that could swap on equality would make the loop non-terminating or oscillating.

Because the ordering is enforced locally (adjacent pairs) and never re-verified globally, the invariant "the array is sorted according to the predicate" holds only if every mutation bubbles until the predicate fails. This is true for insertions and value updates, but is **not** true for class-tier changes: `increaseRankingElementValue` does not rewrite the stored `ClassMaster` (it only rewrites `Value`, `Code/DBSrv/CRanking.cpp:377`), so a character whose class tier changed keeps the tier captured at insert time and its relative order will not reflect the new tier. The persisted output likewise reflects whatever tier was stored at insert time.

**Rule workflow**:
```
superior(A, B) := (A.Value > B.Value AND A.ClassMaster >= B.ClassMaster)
                  OR (A.ClassMaster > B.ClassMaster)

if superior(moving, next):        swap(moving, next); continue
else:                             stop the bubble
=> no swap on full equality (stable insertion-order tie-break)
=> strictly higher class tier always wins (experience irrelevant)
```

---

### Business Rule: BR-03 — Class-master tier normalization (MORTAL/ARCH swap)

**Overview**:
The raw `ClassMaster` encoding defines `ARCH = 1` and `MORTAL = 2` (`Code/Basedef.h:178-179`), which is the inverse of the desired precedence. Both mutation entry points therefore remap these two values before storing or comparing.

**Detailed description**:
`tryInsertInRanking` begins by decrementing `classvalue` when it equals `MORTAL` and incrementing it when it equals `ARCH` (`Code/DBSrv/CRanking.cpp:224-228`). `increaseRankingElementValue` performs exactly the same remap on its parameter (`Code/DBSrv/CRanking.cpp:367-371`). The effect is that the tier ordering used by the ranking becomes `0 (no class master) < 1 (mortal) < 2 (arch) < 3 (celestial) < 4 (celestial CS) < 5 (super celestial)`, a monotone scale suitable for the `>` comparison in the ordering predicate. The remap is applied to the value that is **stored** in `STRUCT_RANKING::ClassMaster` on the insertion path (`Code/DBSrv/CRanking.cpp:249`), so the in-memory representation of `ClassMaster` is normalized, not raw.

This normalization has three visible consequences. First, the persistence layer indexes a six-element label table `{"SEM CLASSMASTER", "M", "A", "C", "C", "SC"}` with the stored value (`Code/DBSrv/CReadFiles.cpp:1052`, `:1060`), which is consistent with the normalized scale and confirms the intent. Second, an unnormalized value written into the struct by any other route (for example the value constructed directly in `CFileDB.cpp:851` and `:1091` from `mobExtra[Slot].ClassMaster`) is raw, so the same struct type carries two different encodings depending on which producer built it. Third, the caller-side update gate at `Code/DBSrv/CFileDB.cpp:2033` compares the **raw** incoming value against the **normalized stored** value, which produces the asymmetry described in BR-08.

The remap is not range-checked. Any value outside `{1,2}` is passed through untouched, so a corrupted account file with `ClassMaster = 7` becomes a stored tier of 7, which would then index past the end of the six-entry label table when the ranking file is written (see the risk table). The same holds for the `Level` and `Class` fields, which are copied verbatim.

**Rule workflow**:
```
normalize(x) := x - 1   if x == MORTAL (2)
                x + 1   if x == ARCH   (1)
                x       otherwise
applied in tryInsertInRanking (CRanking.cpp:224) and increaseRankingElementValue (CRanking.cpp:367)
stored ClassMaster is normalized on the insertion path only
```

---

### Business Rule: BR-04 — Admission gate against the worst-ranked slot

**Overview**:
A character that is not currently ranked may only enter the table if it beats the occupant of slot 0 (`LAST`) under the same two-key predicate, evaluated with the incoming class value against slot 0's stored class value.

**Detailed description**:
`tryInsertInRanking` receives `rankId` and only does work when `rankId == OUT_OF_RANK` (`Code/DBSrv/CRanking.cpp:230`). The gate is:

```
value > getElement(LAST)->Value && classvalue >= getElement(LAST)->ClassMaster
|| classvalue > getElement(LAST)->ClassMaster
```

which is the ordering predicate of BR-02 applied against slot 0 rather than against an adjacent neighbour. If the gate fails, the function returns the unchanged `rankId` (`-1`) and no state changes; the caller then skips notification because `newRankId` is not in `[0,499]` (`Code/DBSrv/CFileDB.cpp:2011`). If the gate passes, the character is admitted and immediately overwrites slot 0 (BR-05) and bubbles upward (BR-06).

Because the class tier is the dominant key, admission is gated primarily by class, not by experience: a mortal character cannot displace a celestial character even with vastly more experience, whereas a celestial character can displace a mortal character with 1 experience point. This makes the leaderboard partitioned by class tier in practice whenever the table is full and the lowest tier present at slot 0 is above the applicant's tier. Note also that "empty" slots are `{Value 0, ClassMaster 0}`, so while the table is not yet full any character with strictly positive experience passes the first conjunct (`value > 0 && classvalue >= 0`) — the table fills from the bottom up during startup.

`value` is `long long` while `getElement(LAST)->Value` is also `long long`, so this comparison is not truncated. The truncation occurs later, on the update path (BR-07).

**Rule workflow**:
```
insert(rankId = -1, name, value, classvalue, level, class, connId, ...)
  classvalue = normalize(classvalue)
  if value > slot0.Value and classvalue >= slot0.ClassMaster
     or classvalue > slot0.ClassMaster:
        admit  -> snapshot slot0 -> set conn id -> new record at slot0 -> bubble
        return final index
  else:
        return -1 (unchanged, no notification)
```

---

### Business Rule: BR-05 — Unconditional eviction of the worst-ranked entry

**Overview**:
Admission always destroys the occupant of slot 0, after snapshotting both the record and its connection id so that the displaced player can be told it fell out of the ranking.

**Detailed description**:
The eviction is implemented before the new record exists: `olderLastRank` receives a by-value copy of slot 0's record and `olderLastConnId` receives slot 0's conn id, both guarded by NULL checks because the load-time caller passes NULLs (`Code/DBSrv/CRanking.cpp:238-242`, load call at `:192`). The conn-id slot is then overwritten with the incoming `connId`, or reset to the default `(-1,-1)` when the caller supplies NULL (`Code/DBSrv/CRanking.cpp:244-247`). Finally a heap record is created with the five-field constructor and installed at slot 0 with `deleteOlder = true`, which `delete`s the previous occupant (`Code/DBSrv/CRanking.cpp:249-251`, `:41-47`).

There is no check of whether the table is actually full. Because empty slots are `{Value 0, ClassMaster 0}`, "evicting" an empty slot is a no-op in business terms, and the bubble pass then places the new record above all remaining empty slots. This means the algorithm never needs to track a count of live entries, at the cost of an invariant that must be re-derived by scanning (`Name[0] == 0`, used by `WriteRanking` at `Code/DBSrv/CReadFiles.cpp:1057`). The consequence is that "the ranking has N entries" is never explicitly known; the log line "Read %d ranking entries" reports the number of *candidate* characters examined, not the number admitted.

The eviction is also the only place where a ranked character loses its position without any action of its own; the caller responds to it by emitting an `OUT_OF_RANK` notification to the evicted owner using the snapshot (BR-21). Since the snapshot is taken *before* the delete, the notification payload is valid even though the original object no longer exists.

**Rule workflow**:
```
if admitted:
    olderLastRank   <- copy of slot0 record          (if pointer non-NULL)
    olderLastConnId <- copy of slot0 conn id         (if pointer non-NULL)
    slot0.connId    <- incoming connId or (-1,-1)
    newRecord = new STRUCT_RANKING(name, value, classvalue, level, class)
    setElement(0, newRecord, deleteOlder = true)     -> delete previous slot0 object
    bubble upward from 0
```

---

### Business Rule: BR-06 — Ascension by adjacent swap (insertion-sort bubble)

**Overview**:
After an accepted entry is written to slot 0, it is moved upward one position at a time by `riseRankingElement` while it is superior to the entry immediately above it, and the loop stops at the first entry it is not superior to.

**Detailed description**:
The loop is `for (int i = rankId + 1; i < MAX_RANK_INDEX; i++)` with `rankId` updated to `i` after each successful swap (`Code/DBSrv/CRanking.cpp:257-268`). Because `i` increments by exactly one and `rankId` is set to `i`, the moving element is compared only with the immediately next slot, so the loop is a classic adjacent-swap insertion pass. `riseRankingElement` performs the swap by exchanging the two pointers with `deleteOlder = false` (so neither object is destroyed) and then exchanging the two conn ids (`Code/DBSrv/CRanking.cpp:342-356`). The guard `rankId >= LAST && rankId < FIRST` prevents accessing slot 500.

The algorithm terminates because each successful swap strictly increases the index and the loop bounds the index at 499. Its cost is O(k) swaps per insertion where k is the number of positions gained, and each swap is O(1), so a full-table worst case is 499 pointer exchanges. Because the array is already sorted and the element starts at the bottom, the first failing comparison means every higher entry is also not inferior (transitivity of the predicate within a class tier); the loop therefore does not need to scan the rest of the table. This property only holds while the predicate is consistent — which it is not across the two mutation paths (BR-07), so a corrupted order caused by an inconsistent update is never repaired by later insertions except along the path a new element traverses.

A detail worth noting is that the conn id travels with the record: because both arrays are swapped in the same operation, the `(TMSrvId, PlayerId)` binding always follows the character it belongs to. Any future change that swaps only one of the two arrays would silently misroute every subsequent notification.

**Rule workflow**:
```
rankId = 0
for i = 1 .. 499:
    if superior(element[rankId], element[i]):
        swap(element[rankId], element[i], delete=false)
        swap(connId[rankId], connId[i])
        rankId = i
    else:
        break
return rankId
```

---

### Business Rule: BR-07 — Value update for an already-ranked character

**Overview**:
When the incoming character name is already present, the service overwrites the stored experience with the incoming value and then bubbles the element upward using a slightly different predicate than the insertion path.

**Detailed description**:
`increaseRankingElementValue` receives `(rankId, newValue, classvalue)`, normalizes the class value, verifies the index range, obtains a mutable pointer to the slot, and executes `thisRank->Value = newValue;` **before** any comparison (`Code/DBSrv/CRanking.cpp:363-377`). The write is therefore unconditional: the function has no way to refuse a decrease, and it never compares `newValue` with the current value. Callers rely on their own gate to avoid decreases (`Code/DBSrv/CFileDB.cpp:2033`), but that gate is not reliable (BR-08). Additionally the parameter type is `int` while the stored field is `long long` and the packet field is `long long` (`Code/Basedef.h:830`), and the caller casts explicitly: `(int)m->RankInfo.Value` (`Code/DBSrv/CFileDB.cpp:2035`). Any experience value above `INT_MAX` is truncated — possibly to a negative number — and then written to the `long long` field, which both corrupts the stored value and can make the element bubble in the wrong direction.

The bubble loop uses

```
if (thisRank->Value > getElement(i)->Value && thisRank->ClassMaster <= classvalue
    || thisRank->ClassMaster > classvalue)
```

(`Code/DBSrv/CRanking.cpp:381`). Two deviations from the insertion path are visible. The class comparison in the first conjunct is `<=` where the insertion path uses `>=`, which weakens the requirement (an element can swap past an equal-class neighbour with equal-or-greater experience only in the first conjunct — in practice indistinguishable for equal classes, but inconsistent in intent). The second disjunct compares the element's own stored class with the *incoming parameter* instead of with the neighbour's class: when the stored tier differs from the incoming tier, `thisRank->ClassMaster > classvalue` is a constant for the whole loop, so either every comparison passes (element bubbles to slot 499 unconditionally, regardless of experience) or the loop degrades to the first conjunct alone. This can only happen when the character's class tier changed since the record was inserted, because normally both sides originate from the same record.

**Rule workflow**:
```
update(rankId, newValue, classvalue):
    classvalue = normalize(classvalue)
    if rankId not in [0,499]: return rankId
    thisRank->Value = newValue            (unconditional, int-truncated)
    for i = rankId+1 .. 499:
        if (thisRank->Value > element[i].Value && thisRank->ClassMaster <= classvalue)
           || thisRank->ClassMaster > classvalue:
              rise(rankId); rankId = i
        else: break
    return rankId
```

---

### Business Rule: BR-08 — Caller-side update gate and its normalization mismatch

**Overview**:
Before calling the update path, the DBSrv message handler decides whether an update is worth attempting using a predicate that mixes the raw incoming class tier with the normalized stored tier.

**Detailed description**:
The gate is `m->RankInfo.Value > thisRank->Value && m->RankInfo.ClassMaster <= thisRank->ClassMaster || m->RankInfo.ClassMaster > thisRank->ClassMaster` (`Code/DBSrv/CFileDB.cpp:2033`), evaluated with `&&` binding tighter than `||`. `thisRank->ClassMaster` is the normalized value produced at insertion (BR-03); `m->RankInfo.ClassMaster` comes straight from the `MSG_UpdateExpRanking` payload, which the TMSrv fills from `extra.ClassMaster` (`Code/TMSrv/MobKilled.cpp:338`) — the raw encoding. For mortal characters the raw value is 2 while the stored value is 1, so the second disjunct `2 > 1` is always true and the gate degenerates to "always pass". For arch characters the raw value is 1 and the stored value 2, so the gate behaves as intended (`Value > storedValue`). For celestial and above the two encodings agree, so the gate also behaves as intended.

The practical effect is confined to mortal characters: every exp tick from a mortal character reaches `increaseRankingElementValue`, which then unconditionally overwrites the stored value with the incoming one — including a lower one. Because exp normally only increases, this is rarely observable, but it becomes observable on: (a) `long long -> int` truncation, which can turn a large exp into a smaller or negative number; (b) any server-side exp correction or rollback; (c) duplicate or replayed `MSG_UpdateExpRanking` packets carrying stale values. In those cases a mortal character's ranking value can regress while arch and higher characters are protected by the gate.

There is a second, structural asymmetry: the gate reads `m->RankInfo.ClassMaster` (raw) but `increaseRankingElementValue` normalizes that same field again before comparing it against the stored normalized tier (BR-07), so the gate and the algorithm do not share a definition of class. This is the clearest instance in the component of the "two encodings in one struct" problem introduced by BR-03.

**Rule workflow**:
```
ranked player, incoming (value_raw, class_raw)
pass := value_raw > stored.Value && class_raw <= stored.ClassMaster
        || class_raw > stored.ClassMaster
if pass: increaseRankingElementValue(rankId, (int)value_raw, class_raw)
         -> normalizes class_raw -> may raise to top / may overwrite with a lower value
else:    no change, no notification
```

---

### Business Rule: BR-09 — Character name as ranking identity

**Overview**:
A character's identity inside the ranking is its name string, matched case-sensitively by `strcmp` across all 500 slots; names are stored in a fixed 32-byte array copied with `strncpy`.

**Detailed description**:
`GrindRanking::getElementIndex` returns `OUT_OF_RANK` for a NULL name and otherwise performs a linear scan of all 500 slots comparing `m_GrindRank[i]->Name` with the argument (`Code/DBSrv/CRanking.cpp:57-74`). The first match wins, and the scan does not stop early on empty slots, so the cost is always up to 500 `strcmp` calls per lookup. Name comparison is byte-exact, so "Knight" and "knight" are different characters from the ranking's point of view even if the rest of the server normalizes names elsewhere.

Storage is a fixed array `char Name[32]` inside a `#pragma pack(push, 1)` struct (`Code/Basedef.h:808-835`), filled with `strncpy(Name, name, sizeof(Name))` (`Code/Basedef.h:822`). `strncpy` does not null-terminate when the source is 32 bytes or longer, so a 32-character name produces a completely unterminated field. Every later `strcmp` in `getElementIndex`, every `strcpy`/`strcmp` in the TMSrv forwarding filter (`Code/TMSrv/ProcessDBMessage.cpp:1293`), and every `fprintf("%s")` in the persistence layer (`Code/DBSrv/CReadFiles.cpp:1059`) then reads past the field boundary. Whether this is reachable depends on the character-name length validation performed at character creation, which is outside this component; the ranking component itself performs no length validation.

Uniqueness is not enforced by the container. `tryInsertInRanking` never asks whether the name is already present; it trusts its caller to have done the lookup (`Code/DBSrv/CFileDB.cpp:1998`). The startup loader calls the insertion path blindly for every character in every account file with a single argument that ignores the return value (`Code/DBSrv/CRanking.cpp:192`), so two account files containing the same character name produce two ranked entries for the same identity, both of which will answer to the same name in a subsequent lookup — and the first one found will be the one updated, leaving the duplicate frozen at its stale value.

**Rule workflow**:
```
index = OUT_OF_RANK
for i = 0 .. 499:
    if strcmp(element[i].Name, name) == 0: index = i; break
return index                      # -1 when not found

insert path does not check uniqueness; uniqueness is a caller responsibility
```

---

### Business Rule: BR-10 — Startup reconstruction from account files with activity window

**Overview**:
The ranking is not loaded from its own persisted file; it is recomputed from the per-account binary files, and only accounts whose file was written in the current year and no earlier than the previous month are considered.

**Detailed description**:
`RankingSystem`'s constructor calls `loadRanking()` immediately (`Code/DBSrv/CRanking.cpp:94-97`), which calls `readAccountsInDir("./account/*")` and then `CReadFiles::WriteRanking()` (`Code/DBSrv/CRanking.cpp:102`, `:110`). The recursive walker skips entries whose name starts with "." (which covers "." and ".." and any dotfile), recurses into directories by rebuilding a path from the hard-coded prefix `"./account/%s/*"` rather than from the `dir` argument (`Code/DBSrv/CRanking.cpp:144`), and treats everything else as an account file.

The activity filter converts `ftLastWriteTime` to a `SYSTEMTIME`, obtains `GetLocalTime()`, and skips the file when `fWriteTime.wYear != sysTime.wYear || fWriteTime.wMonth < sysTime.wMonth - 1` (`Code/DBSrv/CRanking.cpp:149-156`). This is a coarse "active in the last ~2 months" rule expressed with calendar fields rather than elapsed time. It has three notable behaviours: files timestamped in a future year are skipped (year inequality) and files timestamped in a past year are skipped; in January the expression `wMonth - 1` evaluates to 0, so the second condition is never true and only the year test applies; and month arithmetic ignores year boundaries, so in February a December file of the previous year is already rejected by the year test while a December file of the *current* year would pass.

Because the on-disk ranking file is never read, characters belonging to inactive accounts are permanently absent from the rebuilt ranking and are then erased from the persisted ranking file, which is regenerated immediately after loading. The ranking is therefore a rolling snapshot of recently active accounts rather than a historical record, and the only durable source of truth remains the account files themselves.

**Rule workflow**:
```
loadRanking():
    t0 = GetTickCount()
    n = readAccountsInDir("./account/*")
    log "Read n ranking entries in <m>.<s> seconds"
    WriteRanking()          # regenerate ../../Common/Ranking.txt
```

---

### Business Rule: BR-11 — Per-character load eligibility

**Overview**:
Each account file contributes at most `MOB_PER_ACCOUNT` (4) candidates; a character is skipped if its name is empty or its level is >= 1000.

**Detailed description**:
The loop `for (int i = 0; i < MOB_PER_ACCOUNT; i++)` examines the four character slots of `STRUCT_ACCOUNTFILE` (`Code/DBSrv/CRanking.cpp:182`, with `MOB_PER_ACCOUNT` at `Code/Basedef.h:71`). An empty `MobName[0] == '\0'` marks an unused slot and is skipped (`:184`). A character whose `BaseScore.Level` is >= 1000 is skipped (`:187`), which is the mechanism used to exclude non-player or corrupted records (the same `Level < 1000` test is repeated in the persistence filter at `Code/DBSrv/CReadFiles.cpp:1057`). Note that the eligibility test reads `BaseScore.Level` while the value stored in the ranking is `CurrentScore.Level` (`Code/DBSrv/CRanking.cpp:192`), so the two levels serve different purposes: base level gates eligibility, current (buffed) level is displayed.

Accepted characters are inserted with `(Exp, mobExtra[i].ClassMaster, CurrentScore.Level, Class)` and a NULL conn id, NULL eviction snapshots (`Code/DBSrv/CRanking.cpp:190-192`). The `entryCount` counter is incremented for every examined character regardless of whether the insertion succeeded (`:194`), so the startup log's "Read N ranking entries" is an upper bound, not the number of ranked characters.

No validation is applied to `Exp`, `Class`, or the raw `ClassMaster` before insertion. A negative or zero `Exp` simply fails the admission gate and is discarded; a `Class` outside `[0,3]` is stored and later used to index the four-entry `classe` label table during persistence.

**Rule workflow**:
```
for slot in 0..3:
    if MobName[0] == 0: continue
    if BaseScore.Level >= 1000: continue
    tryInsertInRanking(-1, MobName, Exp, mobExtra.ClassMaster, CurrentScore.Level, Class,
                       NULL, NULL, NULL)     # return value ignored
    entryCount++                             # counted even if not admitted
```

---

### Business Rule: BR-12 — Account-file size validation and parse policy

**Overview**:
An account file is only interpreted when its size falls in `[7500, 7600]` bytes or equals exactly `sizeof(STRUCT_ACCOUNTFILE)`; the whole structure is read in a single `fread` into a zeroed buffer.

**Detailed description**:
After `fopen_s(&fs, tmp, "rb")` the code seeks to the end, records `ftell`, seeks back to the start, and evaluates `(fsize >= 7500 && fsize <= 7600) || fsize == sizeof(STRUCT_ACCOUNTFILE)` (`Code/DBSrv/CRanking.cpp:165-175`). The wide window exists because historical builds produced slightly different structure sizes; the exact-equality clause keeps the check valid if the structure evolves beyond the window. When the check fails, the branch contains only a `// TODO: file don't have the right size` comment (`:199`) — no log, no counter, no diagnostic.

Parsing allocates a `STRUCT_ACCOUNTFILE` on the stack, zeroes it, and reads one whole structure (`Code/DBSrv/CRanking.cpp:177-180`). The `fread` return value is not checked, so a short read leaves the tail zeroed, which the per-character loop then treats as unused slots and skips. A partially readable file therefore contributes only the characters it actually delivered, silently.

`STRUCT_ACCOUNTFILE` is a large aggregate (`Code/Basedef.h:840-856`) containing `Info`, 4 x `STRUCT_MOB`, cargo, coin, skills, 4 x affects, 4 x `STRUCT_MOBEXTRA`, donate, and a temp key. Only three regions are actually consumed by this component: `Char[i].MobName`, `Char[i].Exp`, `Char[i].CurrentScore.Level`, `Char[i].Class`, `Char[i].Class`, and `mobExtra[i].ClassMaster`. The rest is parsed and discarded, which is why the size tolerance can be loose without changing ranking semantics.

**Rule workflow**:
```
fsize in [7500,7600] or fsize == sizeof(STRUCT_ACCOUNTFILE)
    -> read one STRUCT_ACCOUNTFILE, iterate 4 character slots
else
    -> ignore file (silent TODO branch)
fread result unchecked; short reads degrade to empty slots
```

---

### Business Rule: BR-13 — Persistence format and label tables

**Overview**:
The ranking is exported as a four-column, whitespace-separated text file (`position name classMasterLabel classLabel`), rebuilding the file from scratch on each write.

**Detailed description**:
`CReadFiles::WriteRanking` opens `RANKING_PATH`, which is defined as `"../../Common/Ranking.txt"` (`Code/DBSrv/CReadFiles.cpp:58`), with mode `"wt"` (`:1047`). If the open fails the function returns silently. It then walks indices from `FIRST` down to (exclusive) `LAST` maintaining a `pos` counter starting at 1, and for each slot whose `Name[0] != 0` and `Level < 1000` writes `"%d %s %s %s\n"` with the position, the name, a class-master label, and a class label (`Code/DBSrv/CReadFiles.cpp:1055-1064`).

The labels are produced by indexing two static tables: `classmaster[6] = {"SEM CLASSMASTER","M","A","C","C","SC"}` and `classe[4] = {"TK","FM","BM","HT"}` (`Code/DBSrv/CReadFiles.cpp:1052-1053`). Both are indexed by values read straight from the in-memory struct, with no bounds check: a stored `ClassMaster` outside `[0,5]` or a stored `Class` outside `[0,3]` reads past the table. Given that `ClassMaster` can arrive from `MSG_UpdateExpRanking` (a network packet), the only protection is the trust relationship with the TMSrv peer and the `Class` validation performed at character creation (`Code/DBSrv/CFileDB.cpp:873`), which does not cover the class-master field.

The write is not atomic: `"wt"` truncates the existing file before the first `fprintf`, and the handle is closed at the end (`:1065`). A crash or a full disk leaves a truncated ranking file. Since nothing reads the file back, the consequence is limited to external consumers (website/panel tooling) rather than to server correctness.

**Rule workflow**:
```
open "../../Common/Ranking.txt" in mode "wt"           (truncate; silent failure)
pos = 1
for i = 499 downto 1:
    if element[i].Name[0] != 0 and element[i].Level < 1000:
        write "<pos> <Name> <classmasterLabel[ClassMaster]> <classLabel[Class]>"
        pos++
close
```

---

### Business Rule: BR-14 — Persistence omits the worst-ranked slot

**Overview**:
The export loop stops before index 0, so the 500th ranked character is never present in the persisted file.

**Detailed description**:
The loop header is `for (int i = RankPos::FIRST, pos = 1; i > RankPos::LAST; i--)` (`Code/DBSrv/CReadFiles.cpp:1055`). `FIRST` is 499 and `LAST` is 0, and the condition is strict `>`, so the last iteration runs at `i == 1` and index 0 is never visited. The persisted file therefore contains at most 499 rows even when 500 characters are ranked, and the missing row is precisely the entry that the eviction rule (BR-05) is about to destroy next — the least valuable record in the table.

Whether this is intentional is not documented in the code. The consequences are asymmetric: an external consumer that treats the file as the leaderboard cannot display the current 500th place, and an operator comparing the file to the in-memory table will consistently see a one-row difference whenever the table is full. Internally nothing depends on the file, so the server's own behaviour is unaffected.

Note that this is separate from, and additional to, the `Name[0] != 0` / `Level < 1000` filters applied inside the loop, and separate from the maximum 499 rows implied by the loop bounds. In a partially filled ranking the number of rows equals the number of populated slots from index 1 to 499, and the two effects are indistinguishable from the file alone.

**Rule workflow**:
```
loop bound: i in [1, 499]  =>  499 candidate slots
rows written = candidates with non-empty name and Level < 1000
slot 0 (LAST) is never exported
```

---

### Business Rule: BR-15 — Persistence cadence

**Overview**:
The ranking file is regenerated every 600 seconds from the DBSrv one-second timer, and once immediately after the startup load.

**Detailed description**:
`ProcessSecTimer` (`Code/DBSrv/Server.cpp:2013`) is invoked by the Windows timer installed with `SetTimer(hWndMain, TIMER_SEC, 1000, NULL)` (`Code/DBSrv/Server.cpp:613`) inside the single-threaded message pump (`Code/DBSrv/Server.cpp:617-621`). Inside it, `if (SecCounter % 600 == 0) { CReadFiles::WriteRanking(); }` (`Code/DBSrv/Server.cpp:2041-2046`) triggers the export ten minutes apart. The `#pragma region Gerador de Ranking` around the call marks it as a deliberate periodic generator.

This design makes persistence a pure function of in-memory state with no dirty tracking: the file is fully rewritten on a fixed period whether or not anything changed, and there is no export on shutdown, so up to 600 seconds of ranking changes can be lost on an abrupt stop. There is also no export triggered by an individual ranking change; the interval is the only policy.

Because the write happens synchronously inside the timer handler on the same thread that services all network messages, it briefly stalls message processing while the file is written (at most 499 formatted lines). This is bounded and small, but it is I/O inside an event handler, which is a performance attribute worth recording rather than a defect.

**Rule workflow**:
```
WM_TIMER(TIMER_SEC, 1000ms) -> ProcessSecTimer
   SecCounter++
   if SecCounter % 600 == 0: WriteRanking()      (blocking, same thread)
plus WriteRanking() once at construction (CRanking.cpp:110)
```

---

### Business Rule: BR-16 — Notification fan-out over a displacement range

**Overview**:
After a mutation, every slot in `[initId - 1, endId + 1]` (clamped to `[0, 499]`) is re-notified, because each of those players' `PlayerAbove`/`PlayerUnder` neighbours may have changed.

**Detailed description**:
`sendUpdateRangeRank(initId, endId)` first validates `initId >= LAST && endId <= FIRST` (`Code/DBSrv/CRanking.cpp:323`), then iterates `i = initId - 1 .. endId + 1` inclusive and re-checks `i >= LAST && i <= FIRST` before using it (`:325-328`). The extra two positions come from the packet's neighbour semantics (BR-17): if a player moved from position p to q, then the players at p-1, p, q, and q+1 all have at least one changed neighbour, and re-sending is cheaper than computing the exact set. The component never computes the exact minimal set; it always over-notifies the boundary slots.

For each candidate slot the conn id is read, validated against `TMSrvId in [0, MAX_SERVER]` and `PlayerId in [1, MAX_USER]`, and then `sendUpdateIndividualRank` is called with the slot's own record as `PlayerRank` (`Code/DBSrv/CRanking.cpp:330-333`). Slots with a default conn id `(-1,-1)` fail the validation and are skipped, which is how offline characters avoid generating traffic. The fan-out is therefore proportional to the distance a character travelled: a single insertion in a full table that rises from slot 0 to slot 499 triggers 500 individual packet constructions and socket writes.

The range call sites are the two mutation outcomes in the message handler: insertion with `initId = LAST` (0) and `endId = newRankId` (`Code/DBSrv/CFileDB.cpp:2015`, `:2023`), and update with `initId = oldRankId` and `endId = newRankId` (`Code/DBSrv/CFileDB.cpp:2038`). Because the insertion call resets `rankId = LAST` before notifying, an insertion that rises high will notify the entire span from 0 to `newRankId + 1`, including every player the newcomer passed.

**Rule workflow**:
```
on mutation from oldPos to newPos:
    for i = oldPos-1 .. newPos+1, clamped to [0,499]:
        cid = connId[i]
        if cid.TMSrvId in [0,MAX_SERVER] and cid.PlayerId in [1,MAX_USER]:
            sendUpdateIndividualRank(i, cid.TMSrvId, cid.PlayerId, element[i])
```

---

### Business Rule: BR-17 — Notification payload composition (boundary cases)

**Overview**:
Each notification carries the recipient's rank position and up to three records: the player above, the recipient's own record, and the player below, selected according to whether the recipient is at the top, bottom, or middle of the table.

**Detailed description**:
`sendUpdateIndividualRank` builds `MSG_SendExpRanking(playerId)`, whose constructor sets `Type = _MSG_GrindRankingData` and `Size = sizeof(MSG_SendExpRanking)` (`Code/Basedef.h:2197-2215`), and assigns `RankPosition = rankId` (`Code/DBSrv/CRanking.cpp:285-287`). It then fills neighbours by case: for `OUT_OF_RANK` it copies slot 0 into `PlayerAbove` (the player is below the entire table); for `FIRST` it copies slot `rankId - 1` into `PlayerUnder`; for `LAST` it copies slot `rankId + 1` into `PlayerAbove`; and for any middle slot it fills both (`Code/DBSrv/CRanking.cpp:289-305`). `PlayerRank` is filled only when the caller supplies a non-NULL record (`:307-310`), which is the case for all mutation notifications but not for the "you fell out" notification path where the evicted snapshot is supplied instead.

The three-record layout means one packet is self-sufficient for a client panel that shows only a three-line window around the player, which matches the client's ranking panel. The cases are mutually exclusive and exhaustive over the valid domain `{-1} ∪ [0,499]`, with the outer guard at `:281` admitting exactly that domain. A consequence is that for the boundary cases one of the two neighbour fields remains default-initialized (`Name[0]='\0'`, all zeroes), and the TMSrv forwarding filter at `Code/TMSrv/ProcessDBMessage.cpp:1293` relies on `PlayerRank.Name[0] != '\0'` to decide whether the packet concerns the receiving character, so an empty `PlayerRank` acts as an implicit "do not forward" marker.

**Rule workflow**:
```
packet.RankPosition = rankId
rankId == -1  -> PlayerAbove = slot[0]
rankId == 499 -> PlayerUnder = slot[498]
rankId == 0   -> PlayerAbove = slot[1]
else          -> PlayerUnder = slot[rankId-1]; PlayerAbove = slot[rankId+1]
if playerRank != NULL: packet.PlayerRank = *playerRank
send only if pUser[tmsrvId].cSock.Sock != 0
```

---

### Business Rule: BR-18 — Notification addressing rules and their off-by-one edges

**Overview**:
A notification is emitted only to a live DB connection whose index and player id pass range checks; the binding between a ranked character and a live session is explicit state that is never invalidated.

**Detailed description**:
The outer guard admits `rankId == OUT_OF_RANK` or any index in `[LAST, FIRST]` (`Code/DBSrv/CRanking.cpp:281`), then checks `tmsrvId >= 0 && tmsrvId <= MAX_SERVER && playerId >= 1 && playerId <= MAX_USER` (`:283`). `pUser` is declared as `extern CUser pUser[MAX_SERVER]` (`Code/DBSrv/CRanking.cpp:25`, definition at `Code/DBSrv/Server.cpp:82`), with `MAX_SERVER` equal to 10 (`Code/Basedef.h:48`), so valid array indices are 0..9 while the check admits 10. `MAX_USER` is 1000 (`Code/Basedef.h:56`) and the player id is only copied into the packet, not used as an index here, so that bound is a protocol-range check rather than a memory-safety one.

The check is also subtly duplicated: `sendUpdateRangeRank` validates with `PlayerId >= 1 && PlayerId <= MAX_USER` (`Code/DBSrv/CRanking.cpp:332`) while the eviction-notification call site validates with `PlayerId > 0 && PlayerId < MAX_USER` (`Code/DBSrv/CFileDB.cpp:2018`). The two agree on the lower bound and differ on the upper bound by one, so a conn id of exactly 1000 would be notified by the range path but not by the eviction path. Since the stored conn ids originate from the DBSrv connection index and the TMSrv session id, both are normally well inside the range, and the discrepancy is latent rather than active.

The most consequential aspect of this rule is that the conn id is *state*, not a lookup. It is written at login (`Code/DBSrv/CFileDB.cpp:847`, `:1087`) and at insertion (`Code/DBSrv/CRanking.cpp:244-247`), and nothing in the codebase ever resets it — there is no logout handler in this component and no `setElementConnId(rankId, GrindRankingConnId())` call on disconnect. A character that logs out keeps a conn id pointing at a slot that will later be reassigned to another session; the packet may then be addressed to a session that no longer owns the character. The TMSrv-side name check (`Code/TMSrv/ProcessDBMessage.cpp:1293`) prevents the wrong *client* from seeing the panel data, because the forwarding requires `PlayerRank.Name` to match the receiving character's name, but the packet still crosses the wire to the wrong session's TMSrv context before being discarded.

**Rule workflow**:
```
emit(rankId, tmsrvId, playerId):
    require rankId == -1 or rankId in [0,499]
    require tmsrvId in [0, MAX_SERVER]        (MAX_SERVER == 10, pUser has 10 elements)
    require playerId in [1, MAX_USER]
    require pUser[tmsrvId].cSock.Sock != 0
    -> SendOneMessage
conn ids are never cleared on logout
```

---

### Business Rule: BR-19 — Session binding lifecycle

**Overview**:
Binding a ranked character to a live session happens only at login and at insertion; there is no unbinding, no rebinding validation, and no persistence of the binding.

**Detailed description**:
Both login paths perform `getElementIndex(MobName)` followed, when the index is in range, by `setElementConnId(rankId, GrindRankingConnId(conn, sm.ID))` (`Code/DBSrv/CFileDB.cpp:843-848`, `:1083-1088`). `conn` is the DBSrv connection index of the TMSrv and `sm.ID` is the TMSrv-side session id, so the pair `(conn, playerId)` is exactly what `sendUpdateIndividualRank` needs to address a socket write and a packet `ID`. `setElementConnId` validates the index before writing (`Code/DBSrv/CRanking.cpp:76-82`), and `getElementConnId` returns a default `(-1,-1)` for an out-of-range index (`:84-90`), so lookups are safe even at the boundaries.

A character that is not in the ranking at login is not bound at all; it becomes bound only when it is first admitted to the ranking, at which point the insertion path installs the conn id supplied by the message handler (`Code/DBSrv/CRanking.cpp:244-247`, caller at `Code/DBSrv/CFileDB.cpp:2007-2009`). If the admission is instead triggered by the startup loader, the conn id is explicitly reset to `(-1,-1)` (`:246-247`), which is correct because no session exists yet. That reset is also the only place where a conn id is ever cleared.

The lifecycle gap is therefore between "character goes offline" and "the slot is overwritten": the binding survives logout, character deletion, and even transfer between servers, until the same slot is either evicted or rebound by the same name logging in again. Because the binding is invisible to the ranking file (BR-13) and to the ranking struct (the conn id lives in a parallel array, `Code/DBSrv/CRanking.h:54`), it can never be reconstructed after a restart — which is consistent with the fact that the ranking itself is rebuilt from account files at startup.

**Rule workflow**:
```
login(conn, playerId, MobName):
    rankId = getElementIndex(MobName)
    if rankId in [0,499]: connId[rankId] = (conn, playerId)
    sendUpdateIndividualRank(...)
insert accepted:
    connId[0] = incoming connId  or  (-1,-1)
never: connId[i] = (-1,-1) on logout
```

---

### Business Rule: BR-20 — Pointer ownership, deletion policy and swap semantics

**Overview**:
`GrindRanking` owns an array of 500 `STRUCT_RANKING*`, destroys the replaced record when a slot is assigned with `deleteOlder = true` (the default), and transfers ownership without destruction during swaps.

**Detailed description**:
The constructor allocates all 500 slots (`Code/DBSrv/CRanking.cpp:32-39`) so every slot is always dereferenceable and callers never need a NULL check — an approach that trades a 500-object upfront cost (500 x 52 bytes = 26,000 bytes plus allocator overhead) for the elimination of null states. `setElement(int index, STRUCT_RANKING* target, bool deleteOlder = true)` (`Code/DBSrv/CRanking.h:63`) deletes the current occupant when `deleteOlder` is true and then installs `target` (`Code/DBSrv/CRanking.cpp:41-47`). The container has no destructor and no copy control (`Code/DBSrv/CRanking.h:50-66`), so the 500 allocations are never released; in practice that only matters at process exit, but it also means `GrindRanking` is unsafe to copy, since a default copy would duplicate 500 owning raw pointers.

Ownership transfer semantics are used in exactly one place: `riseRankingElement` swaps two slots with `deleteOlder = false` (`Code/DBSrv/CRanking.cpp:347-349`), which exchanges the two pointers without destroying either object, and then swaps the two conn ids so the binding follows the character (`:352-354`). Every other assignment uses the default and therefore destroys the previous object. The insertion path relies on this: it snapshots slot 0 into a local variable *before* calling `setElement(LAST, newRank)` so that the value copy survives the delete (`:238-251`).

Ownership also flows inward from callers. `tryInsertInRanking` allocates the new record itself (`:249`) and hands it to `setElement`, so the container takes ownership of a pointer the caller never sees again. Conversely, `sendUpdateIndividualRank` takes a `STRUCT_RANKING*` that it only reads; the two login call sites pass a freshly `new`-allocated record to that read-only parameter (`Code/DBSrv/CFileDB.cpp:851`, `:1091`), so those allocations are leaked by the caller, not by the component. The asymmetry between "the container owns what you pass to `setElement`" and "the notifier does not own what you pass to `sendUpdateIndividualRank`" is not documented anywhere in the header.

**Rule workflow**:
```
ctor:                m_GrindRank[i] = new STRUCT_RANKING()  for all i
setElement(i,t,true):  delete m_GrindRank[i]; m_GrindRank[i] = t
setElement(i,t,false): m_GrindRank[i] = t                  (ownership transfer)
swap:                setElement(a, element[b], false); setElement(b, element[a], false)
no destructor        -> 500 objects retained for process lifetime
```

---

### Business Rule: BR-21 — Eviction notification contract

**Overview**:
A player displaced by an insertion is notified with `RankPosition = OUT_OF_RANK` and `PlayerRank` set to the snapshot of the record it held, and only that player receives this specific message.

**Detailed description**:
The evicted record and conn id are captured before the container destroys them (`Code/DBSrv/CRanking.cpp:238-242`) and returned through two out-parameters of `tryInsertInRanking` (`Code/DBSrv/CRanking.h:81`). The caller then validates the captured conn id and calls `sendUpdateIndividualRank(OUT_OF_RANK, olderLastConnId.TMSrvId, olderLastConnId.PlayerId, &olderLastPlayer)` (`Code/DBSrv/CFileDB.cpp:2018-2019`). The `-1` position selects the `OUT_OF_RANK` branch in the notifier, which fills `PlayerAbove` with the current slot 0 — i.e. the player who just took its place — and `PlayerRank` with the snapshot (`Code/DBSrv/CRanking.cpp:289-292`, `:307-310`).

This is the only path where `PlayerRank` and `RankPosition` describe different things: `RankPosition` says "you are no longer ranked" while `PlayerRank` still carries the character's own last known record, which the client needs in order to render the panel with the character's own stats. It is also the only notification that is targeted individually rather than through the range fan-out, because the evicted entry no longer exists in the array and therefore cannot be reached by index.

Two edge behaviours follow. First, when the eviction happens during the startup load, both out-parameters are NULL, so no snapshot is taken (guarded at `:238` and `:241`) and, since the caller is the loader, no notification is attempted — correct, because no sessions exist. Second, when the evicted slot was an empty slot (`Name[0] == 0`, conn id `(-1,-1)`), the snapshot is an empty record and the conn-id validation fails upstream (`Code/DBSrv/CFileDB.cpp:2018`), so nothing is sent. The rule therefore only produces observable traffic when the table is full and the displaced occupant is online.

**Rule workflow**:
```
insertion accepted:
    olderLastRank / olderLastConnId captured before delete
    if olderLastConnId is a valid live pair:
        sendUpdateIndividualRank(-1, tmsrvId, playerId, &olderLastRank)
            -> RankPosition = -1, PlayerAbove = new slot0, PlayerRank = snapshot
    sendUpdateRangeRank(0, newRankId)
```

---

### Business Rule: BR-22 — Empty-slot semantics

**Overview**:
Unoccupied slots are ordinary records with an empty name, zero value, and zero class tier, and are distinguished from occupied slots only by `Name[0] == '\0'`.

**Detailed description**:
The default constructor sets `Name[0] = '\0'`, `Value = 0`, `ClassMaster = 0`, `Level = 0`, `Class = 0` (`Code/Basedef.h:811-818`), and the container pre-allocates all 500 slots with that state (`Code/DBSrv/CRanking.cpp:36`). Because zero value and zero class tier are the weakest possible combination under the ordering predicate, empty slots naturally sort to the bottom and are the first to be displaced — which is what allows BR-05 to evict unconditionally without maintaining a live-entry count.

The absence of a separate occupancy flag has two consequences. First, an occupied slot with a name but zero experience is indistinguishable in the ordering logic from an empty slot (both have `Value 0`, `ClassMaster 0`), though it is distinguishable in persistence (`Name[0] != 0`). Second, `getElementIndex` will match an empty name against an empty slot, so a lookup with an empty name argument would return index 0 rather than `OUT_OF_RANK`; in practice the only NULL guard is for the pointer, not for the string content (`Code/DBSrv/CRanking.cpp:59-60`).

The persistence filter mirrors the same convention: `Name[0] != 0 && Level < 1000` (`Code/DBSrv/CReadFiles.cpp:1057`). But an in-memory record with a non-empty name and `Level >= 1000` is invisible to persistence *and* still participates in ordering and notifications — so a record can occupy a slot, receive notifications, and never appear in the exported file.

**Rule workflow**:
```
empty slot := { Name = "", Value = 0, ClassMaster = 0, Level = 0, Class = 0 }
occupied  := Name[0] != '\0'
ordering treats empty slots as weakest -> fill-from-bottom behaviour
persistence additionally requires Level < 1000
```

---

## 4. Component Structure

```
Code/DBSrv/
├── CRanking.h                              # Public interface: RankPos enum, GrindRankingConnId,
│                                           # GrindRanking, RankingSystem, extern singleton
│   ├── enum RankPos (25-30)                # FIRST = MAX_RANK_INDEX-1, LAST = 0, OUT_OF_RANK = -1
│   ├── struct GrindRankingConnId (32-48)   # {TMSrvId, PlayerId}, defaults (-1,-1), 2 ctors
│   ├── class GrindRanking (50-66)          # 500 x STRUCT_RANKING* + 500 x conn id
│   └── class RankingSystem (68-86)         # public `grindRanking` member + 8 operations
│
├── CRanking.cpp                            # Implementation (393 lines)
│   ├── globals (25-29)                     # extern pUser/pAdmin; RankingSystem rankingSystem
│   ├── pragma region GrindRanking (31-91)
│   │   ├── ctor (32-39)                    # 500 x new STRUCT_RANKING()
│   │   ├── setElement (41-47)              # assign with optional delete of the old record
│   │   ├── getElement (49-55)              # bounds-checked slot accessor (returns NULL outside range)
│   │   ├── getElementIndex (57-74)         # linear name lookup -> index or OUT_OF_RANK
│   │   ├── setElementConnId (76-82)        # bounds-checked write
│   │   └── getElementConnId (84-90)        # bounds-checked read, default (-1,-1)
│   └── pragma region RankingSystem (93-393)
│       ├── ctor (94-97)                    # calls loadRanking()
│       ├── loadRanking (99-111)            # timed rebuild + immediate WriteRanking
│       ├── readAccountsInDir (118-215)     # recursive Win32 account-file scan and insert
│       ├── tryInsertInRanking (222-274)    # normalize, gate, evict, install, bubble
│       ├── sendUpdateIndividualRank (279-316) # build MSG_SendExpRanking and send
│       ├── sendUpdateRangeRank (321-337)   # fan-out over [initId-1, endId+1]
│       ├── riseRankingElement (342-356)    # adjacent swap of record + conn id
│       └── increaseRankingElementValue (363-392) # overwrite value, bubble upward
│
├── CReadFiles.h                            # declares static RANKING_PATH + WriteRanking()
├── CReadFiles.cpp                          # RANKING_PATH (58), WriteRanking (1043-1066)
│
├── CFileDB.cpp                             # consumers: ranking section on login (842-852, 1082-1092),
│                                           # _MSG_UpdateExpRanking handler (1990-2042)
├── Server.cpp                              # singleton definition site (82-83); 600s WriteRanking (2041-2046)
└── Server.h                                # extern pUser/pAdmin declarations (106-107)

Code/Basedef.h                              # shared contracts used by the component
├── MAX_RANK_INDEX 500 (61), MAX_SERVER 10 (48), MAX_USER 1000 (56), MOB_PER_ACCOUNT 4 (71)
├── MORTAL 2 / ARCH 1 / CELESTIAL 3 / CELESTIALCS 4 / SCELESTIAL 5 (178-182)
├── STRUCT_MOB.Exp long long (450), .Class unsigned char (444)
├── STRUCT_MOBEXTRA.ClassMaster short (485)
├── STRUCT_RANKING packed 1-byte, 52 bytes (808-835)
├── STRUCT_ACCOUNTFILE (840-856)
├── _MSG header macro, 12 bytes (925-930)
├── MSG_SendExpRanking / _MSG_GrindRankingData, 172 bytes (2196-2215)
└── MSG_UpdateExpRanking / _MSG_UpdateExpRanking, 64 bytes (2217-2233)

Code/TMSrv/                                 # producers/consumers outside the component boundary
├── MobKilled.cpp                           # 4 x STRUCT_RANKING + MSG_UpdateExpRanking emission (338, 432, 525, 619)
└── ProcessDBMessage.cpp                    # _MSG_GrindRankingData forwarding with name filter (1284-1297)
```

Derived sizes (from `#pragma pack(push,1)` and the `_MSG` macro at `Code/Basedef.h:925-930`): `STRUCT_RANKING` = 32 + 8 + 4 + 4 + 4 = 52 bytes; `MSG_SendExpRanking` = 12 + 4 + 3 x 52 = 172 bytes; `MSG_UpdateExpRanking` = 12 + 52 = 64 bytes.

Declared but never defined: `GrindRanking::updateElement(int, STRUCT_RANKING*)` (`Code/DBSrv/CRanking.h:61`) has no definition anywhere in the project — using it would fail at link time. Declared but unused: `extern CUser pAdmin[MAX_ADMIN]` in `Code/DBSrv/CRanking.cpp:26` is never referenced in this translation unit.

---

## 5. Dependency Analysis

### Internal dependencies

```
RankingSystem
 ├── GrindRanking (composition, by value: `GrindRanking grindRanking;` CRanking.h:71)
 │    ├── STRUCT_RANKING* x500        (Basedef.h:809)
 │    └── GrindRankingConnId x500     (CRanking.h:32)
 ├── CReadFiles::WriteRanking        (CReadFiles.h:53 -> CReadFiles.cpp:1043)
 │    └── CReadFiles::RANKING_PATH   (CReadFiles.cpp:58)
 ├── Log(char*, char*, int)          (CRanking.cpp:108; global logger)
 └── pUser[MAX_SERVER] + CPSock::SendOneMessage   (CRanking.cpp:25, :313)
      └── CPSock::AddMessage / SendMessageA       (CPSock.cpp:686-693)

External consumers of the component
CFileDB::ProcessMessage
 ├── case _MSG_DBAccountLogin      -> getElementIndex, setElementConnId, sendUpdateIndividualRank (CFileDB.cpp:843-851)
 ├── case _MSG_DBCharacterLogin    -> getElementIndex, setElementConnId, sendUpdateIndividualRank (CFileDB.cpp:1083-1091)
 └── case _MSG_UpdateExpRanking    -> getElementIndex, tryInsertInRanking, sendUpdateIndividualRank,
                                      sendUpdateRangeRank, increaseRankingElementValue (CFileDB.cpp:1998-2038)
ProcessSecTimer (Server.cpp:2013)  -> CReadFiles::WriteRanking (Server.cpp:2044)
                                  -> reads rankingSystem.grindRanking (indirect, via WriteRanking)
Static initialization of rankingSystem (CRanking.cpp:29) -> RankingSystem() -> loadRanking()
```

### External dependencies

| Dependency | Kind | Purpose | Notes |
|------------|------|---------|-------|
| Win32 file API (`FindFirstFile`/`FindNextFile`/`FindClose`) | OS | Enumerate `./account/**` | `Code/DBSrv/CRanking.cpp:127-212`; recursive walk |
| Win32 time API (`FileTimeToSystemTime`, `GetLocalTime`, `GetTickCount`) | OS | Activity filter and load timing | `Code/DBSrv/CRanking.cpp:101`, `:149-152` |
| CRT file I/O (`fopen_s`, `fseek`, `ftell`, `fread`, `fclose`) | Runtime | Parse account files and write `Ranking.txt` | `Code/DBSrv/CRanking.cpp:165-202`, `Code/DBSrv/CReadFiles.cpp:1047-1065` |
| `DBSrv` message protocol (`MSG_UpdateExpRanking` in, `MSG_SendExpRanking` out) | Internal network | Ranking update input, notification output | `_MSG` types at `Code/Basedef.h:2196`, `:2217` |
| `TMSrv` (`MobKilled.cpp`, `ProcessDBMessage.cpp`) | Internal peer process | Produces updates, forwards notifications to clients | `Code/TMSrv/MobKilled.cpp:338-342`; `Code/TMSrv/ProcessDBMessage.cpp:1284-1297` |
| Winsock via `CPSock` | Runtime | Outbound notification transport | `Code/CPSock.cpp:686-693` |
| `Log` facility | Internal | Startup diagnostic | `Code/DBSrv/CRanking.cpp:108` |

Notably **absent** dependencies: no database, no configuration file, no feature flag, no mutex/critical section, and no compression or serialization library. The component reads its configuration entirely from `#define`s in `Code/Basedef.h` and from the file system layout.

---

## 6. Afferent and Efferent Coupling

C++ classes are the unit of decomposition (per the project's paradigm: two classes plus a POD struct).

| Component | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Critical | Notes |
|-----------|------------------------|------------------------|----------|-------|
| `RankingSystem` | 3 — `CFileDB` (3 distinct `case` blocks), `Server.cpp` (`ProcessSecTimer` via `WriteRanking`), static-init (`CRanking.cpp:29`) | 5 — `GrindRanking`, `CReadFiles`, `STRUCT_RANKING`, `Log`, `pUser`/`CPSock` | High | Public `grindRanking` member makes `GrindRanking` reachable without going through the service |
| `GrindRanking` | 6 — `RankingSystem` (8 call sites), `CReadFiles::WriteRanking` (4 sites), `CFileDB` (`getElementIndex` x3, `setElementConnId` x2, `getElement` x1) | 2 — `STRUCT_RANKING`, `GrindRankingConnId` | Medium | Container is a pure data structure with no outbound calls; high fan-in because its member is public |
| `GrindRankingConnId` | 3 — `GrindRanking`, `RankingSystem`, `CFileDB` | 0 | Low | Value object, no behaviour beyond constructors |
| `STRUCT_RANKING` | 8 — `GrindRanking`, `RankingSystem`, `CFileDB`, `CReadFiles`, `TMSrv/MobKilled`, `Basedef` message structs (x2), `TMSrv/ProcessDBMessage` | 0 | High | Shared across process boundaries; schema-coupled to the wire and to the account file |
| `WriteRanking` (static op on `CReadFiles`) | 2 — `RankingSystem::loadRanking`, `ProcessSecTimer` | 3 — `RankingSystem` (via global), CRT file I/O, label tables | Medium | Reaches back into the service through the global singleton, so the dependency is bidirectional |

Cohesion assessment: `RankingSystem` is a mixed-grain class — it performs file-system walking (`readAccountsInDir`), domain algorithms (`tryInsertInRanking`, `increaseRankingElementValue`, `riseRankingElement`), and network notification (`sendUpdateIndividualRank`, `sendUpdateRangeRank`). Three concerns in one class means LCOM is high: the load path and the notify path share no fields and no helpers, and only the `grindRanking` member binds them. Functionally it is a coherent "ranking subsystem", but structurally it is three responsibilities that happen to live in one file.

Coupling direction is mostly sane, with two boundary violations: `CReadFiles::WriteRanking` (a file-I/O utility) depends on the `RankingSystem` global to read ranking state instead of receiving it as a parameter, and `CFileDB` (the message dispatcher) contains the ranking *update policy* — the admission gate at `Code/DBSrv/CFileDB.cpp:2033` — that logically belongs with the ordering predicate it must agree with. That second violation is the direct cause of the inconsistent comparison semantics documented in BR-08.

---

## 7. Endpoints

The component exposes no REST, GraphQL, or gRPC surface. Its contract is the DBSrv binary message protocol; the following message types cross the component boundary.

| Endpoint (message) | Direction | Code | Size | Description |
|--------------------|-----------|------|------|-------------|
| `_MSG_UpdateExpRanking` (value `2 \| FLAG_NEW \| FLAG_DB2GAME \| FLAG_GAME2DB \| FLAG_GAME2CLIENT`) | TMSrv -> DBSrv | `Code/Basedef.h:2217-2233`; handled at `Code/DBSrv/CFileDB.cpp:1990` | 64 bytes | Inbound ranking update: one `STRUCT_RANKING` (`Name`, `Value`, `ClassMaster`, `Level`, `Class`) for a character. Triggers insert-or-update logic. |
| `_MSG_GrindRankingData` / `MSG_SendExpRanking` (value `1 \| FLAG_NEW \| FLAG_DB2GAME \| FLAG_GAME2CLIENT`) | DBSrv -> TMSrv -> client | `Code/Basedef.h:2196-2215`; produced at `Code/DBSrv/CRanking.cpp:285-313`; forwarded at `Code/TMSrv/ProcessDBMessage.cpp:1284-1297` | 172 bytes | Outbound notification: `RankPosition` plus `PlayerAbove`, `PlayerRank`, `PlayerUnder`. |

In-process operations exposed to other DBSrv translation units (not network endpoints):

| Operation | Signature | Callers |
|-----------|-----------|---------|
| `loadRanking` | `void loadRanking()` | `RankingSystem::RankingSystem` (`CRanking.cpp:96`) only |
| `readAccountsInDir` | `int readAccountsInDir(char* dir)` | `loadRanking` (`CRanking.cpp:102`) plus self-recursion (`:145`) |
| `getRankingIndexFromName` | `int getRankingIndexFromName(char* charName)` | **no callers** — declared at `Code/DBSrv/CRanking.h:80`, never defined in `Code/DBSrv/CRanking.cpp` |
| `tryInsertInRanking` | `int tryInsertInRanking(int rankId, char* name, long long value, int classvalue, int Level, int cls, GrindRankingConnId* connId, STRUCT_RANKING* olderLastRank, GrindRankingConnId* olderLastConnId)` | `loadRanking`/`readAccountsInDir` (`:192`), `CFileDB.cpp:2009` |
| `sendUpdateIndividualRank` | `void sendUpdateIndividualRank(int rankId, int tmsrvId, int playerId, STRUCT_RANKING* playerRank)` | `CFileDB.cpp:851`, `:1091`, `:2019`; `sendUpdateRangeRank` (`:333`) |
| `sendUpdateRangeRank` | `void sendUpdateRangeRank(int initId, int endId)` | `CFileDB.cpp:2023`, `:2038` |
| `riseRankingElement` | `void riseRankingElement(int rankId)` | `tryInsertInRanking` (`:261`), `increaseRankingElementValue` (`:383`) |
| `increaseRankingElementValue` | `int increaseRankingElementValue(int rankId, int newValue, int classvalue)` | `CFileDB.cpp:2035` |
| `updateElement` | `void updateElement(int rankId, STRUCT_RANKING*)` | **no callers, no definition** (`CRanking.h:61`) |

Ambiguity noted: `getRankingIndexFromName` (`Code/DBSrv/CRanking.h:80`) is declared but has no definition in `CRanking.cpp`; either it is dead API surface or its implementation was lost during decompilation. It duplicates the intent of `GrindRanking::getElementIndex`, which is what all callers actually use.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| TMSrv -> DBSrv exp updates | Internal service (inbound) | Supply new experience/class/level for a character | DBSrv binary message over Winsock (`_MSG_UpdateExpRanking`) | Packed `STRUCT_RANKING` + 12-byte `_MSG` header | None: no validation of `Name` termination, `Class` range, `ClassMaster` range, or `Value` magnitude; `long long -> int` truncation at `CFileDB.cpp:2035` |
| DBSrv -> TMSrv notifications | Internal service (outbound) | Inform online players of rank changes | DBSrv binary message over Winsock (`_MSG_GrindRankingData`) | Packed `MSG_SendExpRanking`, 172 bytes | Socket presence check only (`CRanking.cpp:312`); `SendOneMessage` return value ignored (`CPSock.cpp:686-693`) |
| TMSrv -> client forwarding | Behavioural integration | Deliver rank panel data to the game client | Same message, forwarded by `ProcessDBMessage` | `MSG_SendExpRanking` | Name-match filter (`ProcessDBMessage.cpp:1293`); dropped silently when `Mode != USER_PLAY` or names differ |
| Account files `./account/**` | File system (inbound) | Source of truth for startup rebuild | Win32 FindFirstFile/FindNextFile + CRT `fopen_s`/`fread` | `STRUCT_ACCOUNTFILE` binary (~7500-7600 bytes) | Size gate with silent `TODO` branch (`CRanking.cpp:175-200`); unchecked `fread`; unchecked `fopen_s` return |
| `../../Common/Ranking.txt` | File system (outbound) | Publish the leaderboard for external consumers | CRT `fopen("wt")`/`fprintf` | Plain text, `"%d %s %s %s\n"` | Silent return if open fails (`CReadFiles.cpp:1049-1050`); no temp-file/rename, so truncation is not atomic |
| `Log` facility | Internal utility | Startup timing diagnostic | In-process call | Formatted string | None; the format itself is wrong (`resultClock/60` reported as "seconds", `CRanking.cpp:107`) |
| `pUser`/`CPSock` | Internal runtime | Resolve a session to a socket | Direct array access | `CUser.cSock` | Bound check admits index `MAX_SERVER` on a `pUser[MAX_SERVER]` array (`CRanking.cpp:283`, `:332`) |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Singleton (global instance) | `RankingSystem rankingSystem;` + `extern` declaration | `CRanking.cpp:29`, `CRanking.h:88` | One ranking for the whole DBSrv process; no injection point, no reset |
| Container / Repository (in-memory) | `GrindRanking` wrapping a fixed pointer array with name lookup | `CRanking.h:50-66`, `CRanking.cpp:32-90` | Encapsulates slot access, name search, and conn-id storage |
| Fixed-capacity table with sentinel values | `MAX_RANK_INDEX` slots; `OUT_OF_RANK = -1`; empty slots as zero records | `Basedef.h:61`, `CRanking.h:25-30` | Avoids dynamic resizing and null states; all 500 slots always valid |
| Insertion sort (adjacent-swap "bubble up") | `riseRankingElement` driven by the loops in `tryInsertInRanking` and `increaseRankingElementValue` | `CRanking.cpp:257-268`, `:342-356`, `:379-388` | Maintain a sorted invariant with O(k) work per mutation instead of re-sorting 500 entries |
| Two-key comparator (class, then value) with absolute precedence | Compound conditions at `Code/DBSrv/CRanking.cpp:259`, `:381`, `Code/DBSrv/CFileDB.cpp:2033` | same | Encode the game's class-tier precedence over raw experience |
| Value-object out-parameters (eviction snapshot) | `olderLastRank` / `olderLastConnId` pointers, guarded for NULL | `CRanking.cpp:238-242` | Preserve a copy of a record that is about to be destroyed so the owner can be notified |
| Publish/subscribe (push notification, coarse) | `sendUpdateRangeRank` fan-out by displacement range | `CRanking.cpp:321-337` | Update every affected session without a per-subscriber registry; the "subscription" is the conn id |
| Lazy rebuild / derived cache | Ranking recomputed from account files at every start; `Ranking.txt` is output-only | `CRanking.cpp:99-111` | Treat the ranking as a cache of the account data rather than a system of record |
| Periodic snapshot export | `SecCounter % 600` timer writing the full file | `Server.cpp:2041-2046` | Simple, stateless durability without dirty tracking |
| Structural coupling to the wire format | `STRUCT_RANKING` used identically in memory, in packets, and as a struct field | `Basedef.h:808-835`, `:2196-2233` | Zero-copy packet construction; ties memory layout to protocol layout (`#pragma pack 1`) |

Architectural decisions and their observable consequences:

1. **Rebuild-on-start instead of load-on-start.** The persisted ranking file is never read back; the ranking is re-derived from account files. This guarantees internal consistency with character data and eliminates "ranking vs. character" drift, at the cost of losing the historical ordering of inactive accounts and of coupling startup time to the number of account files.
2. **Single-threaded mutation domain.** `DBSrv` runs a Win32 message pump (`Server.cpp:617-621`) with a 1-second timer (`:613`) and `WSAAsyncSelect`-driven socket events; ranking state is touched only from that thread. No locks exist in the component, and none are needed as long as this holds. Any future threading of DBSrv would immediately invalidate the safety of the 500-slot arrays and the conn-id bindings.
3. **Zero-copy payload types.** `STRUCT_RANKING` is `#pragma pack(1)` and used directly in messages, so a send is a `memcpy` of a contiguous struct. The trade-off is ABI sensitivity: changing the struct silently changes the wire format of both `MSG_SendExpRanking` and `MSG_UpdateExpRanking` and the ranking semantics of every consumer.
4. **Policy split across the boundary.** The decision "may this update be applied" lives in the dispatcher (`CFileDB.cpp:2033`) while the decision "where does it go" lives in the service (`CRanking.cpp:379-388`). The two use different class encodings and different comparison operators, an architectural split that no single function can verify.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | `increaseRankingElementValue` / `CFileDB` gate | Gate compares raw `ClassMaster` with normalized stored `ClassMaster` (`CFileDB.cpp:2033` vs `CRanking.cpp:224-228`); for `MORTAL` it is always true, and `CRanking.cpp:377` overwrites the value unconditionally | Stored ranking experience can regress for mortal characters (exp corrections, replays, truncation) |
| High | `increaseRankingElementValue` | Loop condition compares the element's own class with the incoming parameter instead of the neighbour's class (`CRanking.cpp:381`) | When a character's class tier changes, the second disjunct is constant and the element can rise to slot 499 regardless of experience |
| High | `CFileDB.cpp:851`, `:1091` | `new STRUCT_RANKING(...)` allocated per login and passed to a read-only parameter; never freed | Unbounded memory growth proportional to the number of logins for the process lifetime |
| High | `GrindRanking` | 500 `new` allocations, no destructor, no copy control (`CRanking.h:50-66`) | Memory retained for process lifetime; a default copy of `GrindRanking` would double-free 500 owning pointers |
| High | `sendUpdateIndividualRank` / `sendUpdateRangeRank` | Bound checks admit `tmsrvId == MAX_SERVER` (`CRanking.cpp:283`, `:332`) while `pUser` is `pUser[MAX_SERVER]` (`Server.cpp:82`) | Out-of-bounds access on `pUser[10]` if a conn id ever equals `MAX_SERVER` |
| Medium | `tryInsertInRanking` / `increaseRankingElementValue` | `long long` value stored, `int` parameter on the update path, explicit `(int)` cast (`CFileDB.cpp:2035`) | Truncation of large experience values, possibly to negative, corrupting both value and ordering |
| Medium | Login path | Conn id is registered but never cleared on logout (`CFileDB.cpp:847`, `:1087`; no counterpart anywhere) | Notifications addressed to stale `(TMSrvId, PlayerId)` pairs; TMSrv name filter prevents wrong-client delivery but traffic and state are wrong |
| Medium | `readAccountsInDir` | `strcpy(tmp,dir); tmp[strlen(dir)-1]='\0'; sprintf(tmp,"%s%s",tmp,fileName)` overlaps source and destination of the same buffer (`CRanking.cpp:158-160`) | Undefined behaviour in path assembly; also unbounded if the recursive path exceeds `tmp[1024]` |
| Medium | `readAccountsInDir` | `FindNextFile` is called before processing, so the first entry returned by `FindFirstFile` is never examined (`CRanking.cpp:127-135`) | One account file (or subdirectory) per directory is silently skipped on every startup |
| Medium | `readAccountsInDir` | Directory test uses `==` instead of a bitmask (`dwFileAttributes == FILE_ATTRIBUTE_DIRECTORY`, `CRanking.cpp:142`) | Directories carrying any additional attribute are treated as files and parsed |
| Medium | `readAccountsInDir` | Recursion ignores the `dir` argument and hard-codes `"./account/%s/*"` (`CRanking.cpp:144`) while files use `dir` (`:158-160`) | The function only works when started from `"./account/*"`; any other root produces inconsistent traversal |
| Medium | Activity filter | Calendar-field comparison `wMonth < wMonth - 1` (`CRanking.cpp:155`) ignores year boundaries and is always false in January | Ranking loses entries of accounts idle for one month; January behaviour differs from the rest of the year |
| Medium | Persistence | `fopen("wt")` truncates in place with no temp-file swap (`CReadFiles.cpp:1047`) and the open failure is silent (`:1049-1050`) | A crash or full disk leaves a truncated leaderboard file for external consumers |
| Medium | Persistence | Label tables indexed without bounds checks (`CReadFiles.cpp:1060-1061`) using `ClassMaster`/`Class` read from memory and originally from a network packet | Out-of-bounds read and garbage labels if a corrupted or hostile peer sends an out-of-range class tier |
| Medium | `STRUCT_RANKING` | `strncpy(Name, name, sizeof(Name))` does not null-terminate a 32-byte name (`Basedef.h:822`); all later `strcmp`/`%s` calls run past the field | Out-of-bounds read; mis-matched identity comparisons |
| Medium | Admission / bootstrap | `tryInsertInRanking` never checks name uniqueness; the loader ignores the return value (`CRanking.cpp:192`) | Duplicate entries for the same character name; only the first is ever updated, the duplicate freezes |
| Low | `getElement` | Returns a mutable pointer with no ownership documentation (`CRanking.cpp:49-55`); `increaseRankingElementValue` mutates through it (`:375-377`) | Encapsulation leak; invariants (ordering) can be broken by any consumer |
| Low | `loadRanking` | Runs from static initialization of a global (`CRanking.cpp:29`, `:96`) | File I/O and logging happen before `main`, before configuration/CI ordering is established; startup failures are hard to diagnose |
| Low | `loadRanking` | `resultClock / 60` and `resultClock % 60` reported as "seconds" (`CRanking.cpp:107`) where `GetTickCount` returns milliseconds | Operator-visible diagnostic is wrong by a factor of ~16.7 |
| Low | `readAccountsInDir` | `entryCount++` increments for every examined character regardless of admission (`CRanking.cpp:194`) | "Read N ranking entries" overstates the number of ranked characters |
| Low | Header | `updateElement` declared, never defined; `getRankingIndexFromName` declared, never defined (`CRanking.h:61`, `:80`) | Dead API surface; a future caller fails at link time |
| Low | Header | `extern CUser pAdmin[MAX_ADMIN]` declared and unused in `CRanking.cpp:26` | Dead declaration duplicated from `Server.h:107` |
| Low | Fan-out | Full-range over-notification: an insertion rising from slot 0 to 499 sends up to 500 individual packets (`CRanking.cpp:321-337`) | Wasted bandwidth and CPU proportional to displacement distance; bounded and acceptable at 500 slots |
| Low | Threading model | No synchronization anywhere; correctness depends entirely on the single-threaded message pump | Latent hazard for any future multi-threading of DBSrv |
| Low | `readAccountsInDir` | `errno = 0` set but never read; `fopen_s` return ignored in favour of a NULL check; `fread` return unchecked (`CRanking.cpp:162-180`) | Partial reads degrade to silently-missing characters |

---

## 11. Test Coverage Analysis

**No tests exist for this component, or for any part of the project.**

Evidence collected across the whole repository (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`):

1. **No test files by name.** A glob for `**/*{test,Test,TEST,spec,Spec}*` over the entire project returned no files.
2. **No test framework.** A content search for `gtest`, `catch2`, `CppUnit`, and `assert(` across `Code/*.cpp`, `Code/*.h`, `Code/DBSrv/**`, and `Code/TMSrv/**` returned zero matches.
3. **No test project.** The solution `W2PP Code Project.sln` contains exactly three entries: `DBSrv` (`Code/DBSrv/DBSrv.vcxproj`), `TMSrv` (`Code/TMSrv/TMSrv.vcxproj`), and a "Solution Items" folder. There is no test/CI project.
4. **No test configuration.** The only build/config files in the tree are those two `.vcxproj` files and the `.sln`; there is no `CMakeLists.txt`, `Makefile`, or CI workflow YAML anywhere in the repository.
5. **No fixtures or harness.** No mock, stub, or fixture directory exists; the component is only exercised by running the two servers.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `RankingSystem` | 0 | 0 | 0% | None — all 8 operations (`loadRanking`, `readAccountsInDir`, `tryInsertInRanking`, `sendUpdateIndividualRank`, `sendUpdateRangeRank`, `riseRankingElement`, `increaseRankingElementValue`, `getRankingIndexFromName`) are untested |
| `GrindRanking` | 0 | 0 | 0% | None — constructor, `setElement`, `getElement`, `getElementIndex`, `setElementConnId`, `getElementConnId` untested; `updateElement` has neither a definition nor a test |
| `GrindRankingConnId` | 0 | 0 | 0% | None — both constructors and the default-value contract untested |
| `CReadFiles::WriteRanking` (ranking persistence) | 0 | 0 | 0% | None — format, label indexing, loop bounds, and the omitted slot 0 are unverified |

Consequence for confidence: the behaviours most in need of verification are precisely the ones that are neither tested nor testable without refactoring — the two-key comparator (BR-02), the class normalization vs. the caller gate (BR-03/BR-08), the `int` truncation (BR-07), the eviction snapshot lifetime (BR-05/BR-21), and the conn-id lifecycle (BR-19). All of them are edge-case-dense and currently verified only by production behaviour. There is also no way to regression-test the persistence format or the 499-row limit (BR-14), so an external consumer of `Ranking.txt` has no contract test either.

The absence of tests is compounded by the fact that the component's only non-trivial logic is reached through global state (`rankingSystem`, `pUser`), file-system side effects (`./account/**`, `../../Common/Ranking.txt`), and a live socket — none of which is behind an interface that could be substituted in a test.

---

## 12. Configuration Management

There is no external configuration for this component. All tunables are compile-time constants:

| Constant | Value | Location | Effect |
|----------|-------|----------|--------|
| `MAX_RANK_INDEX` | 500 | `Code/Basedef.h:61` | Table capacity; `FIRST` = 499 |
| `MAX_SERVER` | 10 | `Code/Basedef.h:48` | Bound check for `tmsrvId` (and size of `pUser`) |
| `MAX_USER` | 1000 | `Code/Basedef.h:56` | Bound check for `playerId` |
| `MOB_PER_ACCOUNT` | 4 | `Code/Basedef.h:71` | Characters examined per account file |
| `MORTAL` / `ARCH` / `CELESTIAL` / `CELESTIALCS` / `SCELESTIAL` | 2 / 1 / 3 / 4 / 5 | `Code/Basedef.h:178-182` | Raw class-tier encoding; two of them are remapped by BR-03 |
| `RANKING_PATH` | `"../../Common/Ranking.txt"` | `Code/DBSrv/CReadFiles.cpp:58` | Output file, relative to the DBSrv working directory |
| Account root | `"./account/*"` | `Code/DBSrv/CRanking.cpp:102`, `:144` | Input root, relative to the DBSrv working directory |
| Persistence period | 600 seconds | `Code/DBSrv/Server.cpp:2041` | Hard-coded `% 600`, not a named constant |
| Activity window | current year, month >= current-1 | `Code/DBSrv/CRanking.cpp:155` | Hard-coded calendar comparison |
| Level ceiling | 1000 | `Code/DBSrv/CRanking.cpp:187`, `Code/DBSrv/CReadFiles.cpp:1057` | Magic number duplicated in two files |
| Label tables | `classmaster[6]`, `classe[4]` | `Code/DBSrv/CReadFiles.cpp:1052-1053` | Presentation labels; sizes must match the class-tier domain |

All file paths are relative, so behaviour depends on the process working directory — the ranking is read from `./account/**` and written to `../../Common/Ranking.txt`, which assumes DBSrv is launched from its own output directory (e.g. `Code/DBSrv/Release`). No environment variables, feature flags, or config files are consulted.

---

## 13. Assumptions, Ambiguities and Evidence Gaps

| Item | Statement | Confidence / Evidence |
|------|-----------|-----------------------|
| Intent of the omitted slot 0 in persistence | No comment explains why the export loop stops at index 1; treated here as an observed behaviour, not an intended rule | Observed fact (`CReadFiles.cpp:1055`); intent unknown |
| `getRankingIndexFromName` | Declared in the header, no definition in the implementation; assumed dead surface lost in decompilation | High — content search for the symbol finds only the declaration |
| `updateElement` | Declared, no definition and no callers; assumed dead declaration | High — content search finds only `CRanking.h:61` |
| Client-name length validation | Whether 32+ byte character names can be created is decided outside this component; the risk in BR-09 is conditional on that | Medium — not verifiable within the component boundary |
| `Class` range of loaded accounts | The component trusts `STRUCT_ACCOUNTFILE` contents; the `[0,3]` check exists only at character creation (`CFileDB.cpp:873`) | High — no validation in the read path |
| Threading | Assumed single-threaded because DBSrv uses a Win32 message pump (`Server.cpp:613-621`) with `WSAAsyncSelect`; if any DBSrv code creates a worker thread, the ranking arrays are unguarded | High for the files inspected; the full DBSrv source was not exhaustively searched for thread creation beyond a targeted search that returned no `CreateThread`/`_beginthread` matches |
| `Ranking.txt` consumers | No code in the repository reads the file; assumed consumed by external panel/website tooling | High — only two references to the path exist (`CReadFiles.cpp:58`, `:1047`) |
| Two className "C" entries in the label table | `{"SEM CLASSMASTER","M","A","C","C","SC"}` has "C" twice (indices 3 and 4); the intended distinction (e.g. "CS" vs "C") cannot be recovered from code | Medium — code-level fact, intent ambiguous |
| Historical intent of the `>=`/`<=` asymmetry | The two bubble loops use different operators; whether this is a transcription artifact cannot be determined from the source or from git history (both commits touching this file are license-only: `30170b5`, `e98bcd8`) | High on the fact, unknown on the intent |

---

## 14. Analysis Method and Files Examined

Read in full: `Code/DBSrv/CRanking.cpp` (393 lines), `Code/DBSrv/CRanking.h` (88 lines).
Read in the relevant ranges: `Code/Basedef.h` (48-71, 160-204, 440-479, 795-864, 920-937, 2185-2244), `Code/DBSrv/CFileDB.cpp` (820-874, 1060-1114, 1960-2079), `Code/DBSrv/CReadFiles.cpp` (1020-1066), `Code/DBSrv/CReadFiles.h` (30-56), `Code/DBSrv/Server.cpp` (605-625, 2013-2064), `Code/CPSock.cpp` (686-693), `Code/CPSock.h` (53-80), `Code/TMSrv/MobKilled.cpp` (320-364), `Code/TMSrv/ProcessDBMessage.cpp` (1275-1304), `Code/TMSrv/_MSG_ReqRanking.cpp` (1-77).
Consulted for context: `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` (component inventory and role), `docs/reports/MANIFEST.md` (report naming convention), `Code/DBSrv/DBSrv.vcxproj` (build inclusion), `W2PP Code Project.sln` (project inventory), `git log`/`git show` for `Code/DBSrv/CRanking.cpp` and `.h`.

Excluded from analysis per the ignore list: `.agents`, `.codegraph`, `.opencode`, `openspec`. Build artifacts (`Code/DBSrv/Debug/CRanking.obj`, `Code/DBSrv/Release/CRanking.obj`, etc.) were not analyzed as source.

No project files were modified during this analysis.
