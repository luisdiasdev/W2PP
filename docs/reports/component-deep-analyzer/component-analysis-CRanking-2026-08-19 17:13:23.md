# Component Deep Analysis Report

**Component:** CRanking
**Subsystem:** Player ranking (Experience Grind Ranking) subsystem
**Codebase:** Legacy W2PP C/C++ server (Visual C++ / Win32, legacy/ folder)
**Analyzed:** 2026-08-19
**Scope boundary:** `legacy/Code/DBSrv/CRanking.h`, `legacy/Code/DBSrv/CRanking.cpp` plus their direct producers/consumers in DBSrv and TMSrv and the shared message/data definitions in `legacy/Code/Basedef.h`.
**Folders ignored:** `.git`, `.opencode`

---

## 1. Executive Summary

The **CRanking** component implements the *experience ("grind") ranking* subsystem for the legacy W2PP MMORPG server. It maintains an in-memory, fixed-capacity leaderboard of the top 500 player characters ranked primarily by accumulated experience points (EXP), with a secondary class-master tier comparison that lets higher-tier characters (Arch Masters) outrank lower-tier characters (Mortal Masters) even when EXP is equal or slightly lower.

The component is hosted inside the **DBSrv** (database server) process and is composed of two collaborating classes declared in `CRanking.h` and implemented in `CRanking.cpp`:

- `GrindRanking` — a raw container managing two parallel fixed arrays: the ranking entries (`STRUCT_RANKING* m_GrindRank[500]`) and the connection metadata mapping each ranked slot to an online player (`GrindRankingConnId m_GrindRankConnId[500]`).
- `RankingSystem` — the orchestration layer exposing the ranking algorithms: load-from-disk, insert, value-increase, bubble-up re-ranking, and notification of affected players.

A single process-global instance, `rankingSystem` (`CRanking.cpp:29`), is created at startup; its constructor triggers the initial full load of the ranking from the on-disk account files.

**Key findings:**

1. **File-based, in-memory architecture.** The ranking has no database backend. It is built at server startup by scanning account files (`./account/*`), maintained in RAM, periodically serialized to `../../Common/Ranking.txt` (`CReadFiles.cpp:1043-1066`), and propagated to clients through the TMSrv servers via a custom binary socket protocol.

2. **Runtime maintenance driven by gameplay.** Every monster kill in which a party member earns EXP produces an `MSG_UpdateExpRanking` packet sent from TMSrv to DBSrv (`MobKilled.cpp:338-342, 432-435, 525-528, 619-622`). DBSrv applies the update and pushes refreshed rank snapshots back to the affected online players.

3. **Fixed 500-slot capacity with last-place eviction.** When a new player enters a full ranking, the current last-place (index 0) entry is evicted and the evicted player is notified they are now out of the ranking.

4. **Class-master tier dominates EXP.** Ranking position is determined by a compound comparison where a strictly higher class-master tier always wins, otherwise the higher EXP wins. Special MORTAL/ARCH normalization adjusts the comparison values before ranking.

5. **No automated test coverage exists.** The entire repository (including the legacy folder) contains no unit, integration, or regression test files for this component.

6. **Technical debt.** Two members declared in the header (`RankingSystem::getRankingIndexFromName`, `GrindRanking::updateElement`) are never implemented or referenced; a 64-bit-to-32-bit truncation risk exists in the value-increase path; and there are several silent (empty) error branches in the directory scan.

---

## 2. Data Flow Analysis

The ranking subsystem is distributed across two server processes (TMSrv = game server, DBSrv = database server) connected over a TCP socket carrying a custom length-prefixed binary message protocol. The data flow is circular: gameplay events mutate EXP on TMSrv, which reports to DBSrv, which re-ranks and notifies back.

**Startup / build flow (DBSrv, one-time):**

```
1. RankingSystem constructor (CRanking.cpp:94) calls loadRanking()
2. loadRanking() -> readAccountsInDir("./account/*") (CRanking.cpp:99-111)
3. Recursive directory scan of ./account/ (CRanking.cpp:118-215)
4. For each valid account file: read STRUCT_ACCOUNTFILE
5. For each non-empty character (MobName != ""), build STRUCT_RANKING and call tryInsertInRanking() (CRanking.cpp:192)
6. CReadFiles::WriteRanking() serializes the final ranking to ../../Common/Ranking.txt (CReadFiles.cpp:1043-1066)
```

**Runtime update flow (per monster kill):**

```
1. TMSrv: player/party member gains EXP on monster death (MobKilled.cpp)
2. TMSrv builds STRUCT_RANKING from player state (MobKilled.cpp:338-342, 432-435, 525-528, 619-622)
3. TMSrv sends MSG_UpdateExpRanking (type _MSG_UpdateExpRanking, FLAG_GAME2DB) to DBSrv via DBServerSocket
4. DBSrv: Server.cpp ProcessClientMessage() validates FLAG_GAME2DB + ID range (Server.cpp:1343)
5. DBSrv: CFileDB::ProcessMessage() dispatches to case _MSG_UpdateExpRanking (CFileDB.cpp:1990)
6. Handler resolves rankId by name via grindRanking.getElementIndex()
7. If not ranked (OUT_OF_RANK): tryInsertInRanking() -> possible insertion + eviction (CFileDB.cpp:2009)
8. If already ranked and value improved: increaseRankingElementValue() + bubble-up (CFileDB.cpp:2035)
9. Affected players notified via sendUpdateRangeRank()/sendUpdateIndividualRank()
10. DBSrv builds MSG_SendExpRanking (type _MSG_GrindRankingData, FLAG_DB2GAME) and sends to the relevant TMSrv socket (CRanking.cpp:312)
11. TMSrv: ProcessDBMessage() case _MSG_GrindRankingData forwards packet to the client if in USER_PLAY mode and name matches (ProcessDBMessage.cpp:1285-1297)
```

**Character login flow (DBSrv, per login):**

```
1. DBSrv processes character login (CFileDB.cpp:843-852 and 1083-1092)
2. rankId = grindRanking.getElementIndex(MobName)
3. If ranked: grindRanking.setElementConnId(rankId, GrindRankingConnId(conn, sm.ID)) records where to notify this player
4. sendUpdateIndividualRank(rankId, ...) pushes the player's current rank snapshot to the client
```

**Periodic persistence flow (DBSrv, every 600 seconds):**

```
1. Server.cpp ProcessSecTimer(): if (SecCounter % 600 == 0) (Server.cpp:2041-2046)
2. CReadFiles::WriteRanking() writes the top-500 ranking to ../../Common/Ranking.txt
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Business Logic | Ranking capacity is fixed at 500 entries (indices 0..499) | Basedef.h:61; CRanking.cpp:34 |
| Business Logic | Ranking is ordered ascending by index: index 0 = LAST, index 499 = FIRST (best) | CRanking.h:25-30 |
| Validation | A player is excluded from ranking if character Level >= 1000 | CRanking.cpp:187-188; CReadFiles.cpp:1057 |
| Validation | Empty character names are skipped during load | CRanking.cpp:184-185 |
| Validation | Only account files with size in [7500, 7600] or == sizeof(STRUCT_ACCOUNTFILE) are read | CRanking.cpp:175 |
| Business Logic | Only "active" accounts are loaded: last-write year == current year AND month >= current month - 1 | CRanking.cpp:155 |
| Business Logic | Class-master tier is normalized: MORTAL -> -1, ARCH -> +1 before comparison | CRanking.cpp:224-228, 367-371 |
| Business Logic | A new player enters ranking only if (higher value AND >= class tier) OR strictly higher class tier than current LAST | CRanking.cpp:232 |
| Business Logic | Inserting a new player evicts the current LAST player (index 0) | CRanking.cpp:238-252 |
| Business Logic | After insertion, the new player bubbles up while the element above has higher value (with class-tier dominance) | CRanking.cpp:257-268 |
| Business Logic | An already-ranked player rises when their new value exceeds the element above under the same compound rule | CRanking.cpp:379-388 |
| Business Logic | riseRankingElement swaps two adjacent slots, including their connection metadata | CRanking.cpp:342-356 |
| Business Logic | Only online/valid players are notified of ranking changes (TMSrvId in [0, MAX_SERVER], PlayerId in [1, MAX_USER]) | CRanking.cpp:283, 332 |
| Notification | A player who drops out of the ranking is notified with rank position OUT_OF_RANK and PlayerAbove = current LAST | CRanking.cpp:289-291 |
| Notification | Rank snapshot packet includes PlayerAbove/PlayerUnder neighbors depending on position | CRanking.cpp:293-305 |
| Validation | TMSrv forwards rank packets to a client only if user is in USER_PLAY mode and player name matches | ProcessDBMessage.cpp:1293 |
| Validation | DBSrv accepts messages only if they carry FLAG_GAME2DB and ID in [0, MAX_USER] | Server.cpp:1343 |
| Trigger | An MSG_UpdateExpRanking packet is sent on every EXP-gaining monster kill in grinding sectors | MobKilled.cpp:338-342, 432-435, 525-528, 619-622 |

## Detailed breakdown of the business rules

---

### Business Rule: Fixed ranking capacity (500 slots)

**Overview:**
The grind ranking is a fixed-size leaderboard holding exactly 500 player entries. It is not a growable structure; a player can only be present in the ranking while they occupy one of the 500 indices.

**Detailed description:**
The capacity is enforced by the compile-time constant `MAX_RANK_INDEX = 500` (`Basedef.h:61`), which sizes the two parallel member arrays of `GrindRanking`: `STRUCT_RANKING* m_GrindRank[MAX_RANK_INDEX]` and `GrindRankingConnId m_GrindRankConnId[MAX_RANK_INDEX]` (`CRanking.h:53-54`). The `GrindRanking` constructor eagerly allocates a `new STRUCT_RANKING()` for every slot (`CRanking.cpp:34-38`), so all 500 pointers are always valid and never null; emptiness is indicated by an empty `Name[0] == '\0'` rather than a null pointer. Every algorithm in the component iterates from `0` to `MAX_RANK_INDEX` and performs bounds checks against `LAST` (0) and `FIRST` (499). Because capacity is fixed, when the ranking is full a new entrant must displace an existing occupant — this is the "eviction" behavior described in a later rule. The fixed capacity also means the ranking is intentionally limited to the 500 most-qualified characters, regardless of total server population.

**Rule workflow:**
1. `GrindRanking` constructor pre-allocates 500 `STRUCT_RANKING` instances.
2. All algorithms iterate indices in `[0, MAX_RANK_INDEX)`.
3. Bounds are validated against `LAST`/`FIRST` before any element access.
4. Full ranking forces eviction of the index-0 occupant on insertion.

---

### Business Rule: Ascending-index ranking order (index 0 = LAST, index 499 = FIRST)

**Overview:**
The ranking array is ordered so that a numerically higher index represents a better rank. Index 0 is the worst (LAST), and index 499 is the best (FIRST). This inverted ordering is fundamental to every comparison and bubble-up loop in the component.

**Detailed description:**
The `RankPos` enum (`CRanking.h:25-30`) defines `FIRST = MAX_RANK_INDEX - 1` (499), `LAST = 0`, and `OUT_OF_RANK = -1`. Throughout the code, "rising" in the ranking means moving toward higher indices: `riseRankingElement(rankId)` swaps slots `rankId` and `rankId + 1` (`CRanking.cpp:342-356`), and the bubble-up loops in `tryInsertInRanking` and `increaseRankingElementValue` iterate upward (`for (int i = rankId + 1; i < MAX_RANK_INDEX; i++)`). Consequently, the "last player" (the one at risk of eviction) is always at index 0, and the "top 1 player" is always at index 499. This inverted index convention is a notable inversion relative to the natural reading order and is a source of potential confusion; it must be respected by any consumer. For example, `CReadFiles::WriteRanking` iterates `for (i = FIRST; i > LAST; i--)` to write best-first to the output file (`CReadFiles.cpp:1055`).

**Rule workflow:**
1. `RankPos` establishes FIRST = 499, LAST = 0.
2. Insertion always places a new entrant at index `LAST` (0).
3. Bubble-up moves entries toward higher indices until the sort predicate fails.
4. Persistence and packet-building iterate from `FIRST` down toward `LAST`.

---

### Business Rule: Level-1000 exclusion

**Overview:**
Characters whose current level reaches or exceeds 1000 are excluded from the ranking entirely — both during the startup load and during periodic persistence.

**Detailed description:**
During the initial directory scan, each character is tested with `if (accBuffer.Char[i].BaseScore.Level >= 1000) continue;` (`CRanking.cpp:187-188`), which prevents a level-1000+ character from being inserted. The same filter is applied at persistence time in `CReadFiles::WriteRanking`, which only writes entries where `getElement(i)->Level < 1000` (`CReadFiles.cpp:1057`). In this legacy engine `MAX_LEVEL = 399` (`Basedef.h:117`), so the 1000 threshold is far above the normal level ceiling and functions as a hard anti-abuse / administrative exclusion: it prevents characters that have reached an extreme, normally-unattainable level (typically administratively boosted or debug characters) from occupying leaderboard slots. The threshold is a hard-coded literal in both locations rather than a named constant, so it must be kept consistent in two places.

**Rule workflow:**
1. On load, a character with `BaseScore.Level >= 1000` is skipped and never inserted.
2. On persist, entries with `Level >= 1000` are omitted from `Ranking.txt`.
3. If such a character were already present, it would be silently dropped from the persisted output.

---

### Business Rule: Active-account filter during ranking load

**Overview:**
Only characters belonging to "active" accounts are loaded into the ranking at startup. An account is considered active if its account file was last modified within the current year and within the previous or current calendar month.

**Detailed description:**
In `readAccountsInDir`, the file's last-write time is converted to a `SYSTEMTIME` and compared against the current local time: `if (fWriteTime.wYear != sysTime.wYear || fWriteTime.wMonth < sysTime.wMonth - 1) continue;` (`CRanking.cpp:155-156`). This means an account file is only eligible if its last-write year matches the current year AND its last-write month is not older than one month behind the current month. Characters of accounts inactive for more than roughly two months are excluded from the ranking, which keeps the leaderboard populated only with recently-played characters and prevents stale, long-abandoned characters from permanently occupying the 500 slots. This is a deliberate "recency" filter, though it is implemented purely from file modification timestamps rather than any in-game activity flag.

**Rule workflow:**
1. Directory scan obtains each file's `ftLastWriteTime`.
2. Converted to `SYSTEMTIME` and compared to current local time.
3. Files outside the current year OR older than the previous month are skipped.
4. Only surviving files are opened, validated, and parsed.

---

### Business Rule: Account file size validation

**Overview:**
An account file is only parsed if its size is recognized: either between 7500 and 7600 bytes inclusive, or exactly equal to `sizeof(STRUCT_ACCOUNTFILE)`.

**Detailed description:**
Before reading, the loader computes the file size via `fseek`/`ftell` and requires `(fsize >= 7500 && fsize <= 7600) || fsize == sizeof(STRUCT_ACCOUNTFILE)` (`CRanking.cpp:175`). This dual check tolerates legacy/variant account file layouts (the 7500-7600 byte band) as well as the canonical `STRUCT_ACCOUNTFILE` size. Files that fail this check are silently skipped (an empty `// TODO: file don't have the right size` branch at `CRanking.cpp:199-200`), and files that cannot be opened are also silently skipped (`CRanking.cpp:204-207`). Because a `STRUCT_ACCOUNTFILE` is read with a single `fread` of `sizeof(STRUCT_ACCOUNTFILE)` regardless of actual file size, the size gate protects against reading a truncated or malformed file into a too-small buffer.

**Rule workflow:**
1. Compute file size.
2. Accept only sizes in [7500, 7600] or == sizeof(STRUCT_ACCOUNTFILE).
3. Rejected or un-openable files are skipped (no error logged).
4. Accepted files are read into a zeroed `STRUCT_ACCOUNTFILE` buffer.

---

### Business Rule: Class-master tier normalization (MORTAL/ARCH adjustment)

**Overview:**
Before any class-tier comparison, the class-master value is normalized: a `MORTAL` (value 2) is decremented to 1, and an `ARCH` (value 1) is incremented to 2, so that Arch Masters compare as a higher tier than Mortal Masters.

**Detailed description:**
This normalization is applied identically in two places: at the top of `tryInsertInRanking` (`CRanking.cpp:224-228`) and at the top of `increaseRankingElementValue` (`CRanking.cpp:367-371`):

```c
if (classvalue == MORTAL)
    classvalue--;
else if (classvalue == ARCH)
    classvalue++;
```

Given `MORTAL = 2` and `ARCH = 1` (`Basedef.h:178-179`), a Mortal Master's effective comparison tier becomes 1 and an Arch Master's becomes 2. The effect is that in the class-tier dominance comparisons, Arch Masters are treated as strictly higher tier than Mortal Masters. This enforces the game rule that the advanced (Arch) class-master path outranks the basic (Mortal) path for leaderboard placement purposes. The normalization is defensive and only triggers on the two specific sentinel values; any other class-master value passes through unmodified.

**Rule workflow:**
1. Receive the raw `classvalue` (class-master tier).
2. If `MORTAL` (2), reduce effective tier to 1.
3. If `ARCH` (1), raise effective tier to 2.
4. Use the normalized tier in all subsequent rank comparisons.

---

### Business Rule: Ranking entry eligibility (insertion predicate)

**Overview:**
A player not currently ranked may enter the ranking only if they beat the current LAST-place entry under a compound predicate: `(value > LAST.value AND classmaster >= LAST.classmaster) OR (classmaster > LAST.classmaster)`.

**Detailed description:**
`tryInsertInRanking` gates entry on `if (value > grindRanking.getElement(LAST)->Value && classvalue >= grindRanking.getElement(LAST)->ClassMaster || classvalue > grindRanking.getElement(LAST)->ClassMaster)` (`CRanking.cpp:232`). Interpreting operator precedence (`&&` binds tighter than `||`): a candidate enters if either (a) they have strictly more EXP than the current last-place player AND a class-master tier greater than or equal to it, or (b) they have a strictly higher class-master tier than the last-place player (regardless of EXP). This encodes two complementary paths to entry: a straight EXP-based improvement at equal or higher tier, and a tier-based override that lets a higher-tier player enter even with less EXP. If the candidate fails this predicate, `tryInsertInRanking` returns the unchanged `OUT_OF_RANK`, and the caller treats the insertion as a no-op.

**Rule workflow:**
1. Determine current LAST (index 0) entry.
2. Compare candidate value/classvalue against LAST using the compound predicate.
3. On success, proceed to eviction + insertion.
4. On failure, return OUT_OF_RANK unchanged.

---

### Business Rule: Last-place eviction on insertion

**Overview:**
When a new player enters a full 500-slot ranking, the current LAST-place (index 0) player is evicted, their data is captured for notification, and the new player takes index 0.

**Detailed description:**
On successful insertion, the code captures the evicted player into the caller-provided output parameters `*olderLastRank = *grindRanking.getElement(LAST)` and `*olderLastConnId = grindRanking.getElementConnId(LAST)` (`CRanking.cpp:238-242`). The new player's connection metadata is then installed at index 0 (`grindRanking.setElementConnId(LAST, *connId)` or a default `GrindRankingConnId()` when no connection is supplied, `CRanking.cpp:244-247`), a fresh `STRUCT_RANKING` is heap-allocated for the new player (`new STRUCT_RANKING(name, value, classvalue, Level, cls)`, `CRanking.cpp:249`), and it replaces the slot (`grindRanking.setElement(LAST, newRank)`, `CRanking.cpp:251`). Because the slot previously held the evicted player's heap object, `setElement` deletes the old object by default (`deleteOlder = true`). The caller uses the captured `olderLastRank`/`olderLastConnId` to notify the evicted player (if online) that they are now `OUT_OF_RANK` (`CFileDB.cpp:2018-2019`).

**Rule workflow:**
1. Capture evicted LAST player's data and connection into outputs.
2. Set the LAST slot's connection to the new player (or default).
3. Allocate and install a new `STRUCT_RANKING` for the new player at index 0.
4. Return the captured evicted data to the caller for notification.

---

### Business Rule: Bubble-up re-ranking after insertion

**Overview:**
After being inserted at index 0, a new player repeatedly swaps with the element above while the sort predicate holds: the player wins the slot above when `(value > above.value AND classmaster >= above.classmaster) OR (classmaster > above.classmaster)`.

**Detailed description:**
Following insertion, `tryInsertInRanking` runs `rankId = LAST` and iterates `for (int i = rankId + 1; i < MAX_RANK_INDEX; i++)` (`CRanking.cpp:257-268`). At each step it evaluates the compound predicate comparing the current slot against the next-higher index; when the predicate is true, it calls `riseRankingElement(rankId)` (which swaps indices `rankId` and `rankId + 1`, including connection metadata) and advances `rankId = i`; when false, it `break`s. This yields an ascending-order stable-ish insertion where the new entrant climbs to its correct position. The loop boundary and the `break` guarantee the array remains ordered: the bubble stops at the first higher index that beats the entrant. The final `rankId` (the highest index reached) is returned so the caller knows the range of affected slots.

**Rule workflow:**
1. Start at rankId = 0 (LAST).
2. Compare against the next-higher index with the compound predicate.
3. If it wins, swap adjacent elements and move up one index.
4. If it loses, stop.
5. Return the final rank index reached.

---

### Business Rule: Rise on value increase for already-ranked players

**Overview:**
When an already-ranked player's EXP increases, their entry's value is updated and they may bubble up toward a better rank using the same compound predicate, but with a tier comparison inverted relative to the ascending element.

**Detailed description:**
`increaseRankingElementValue(rankId, newValue, classvalue)` first normalizes the class value (MORTAL/ARCH), then writes `thisRank->Value = newValue` (`CRanking.cpp:377`). It then iterates upward from `newRankId + 1`, testing `if (thisRank->Value > grindRanking.getElement(i)->Value && thisRank->ClassMaster <= classvalue || thisRank->ClassMaster > classvalue)` (`CRanking.cpp:381`). Note the tier condition here is `thisRank->ClassMaster <= classvalue` (the player's own stored tier is at most the incoming tier) rather than the `>=` used in the ascending-element comparison; this is because the comparison is between the player (rankId) and the element above it (i), so the predicate must determine whether the player deserves to rise above element i. When true, it swaps upward and continues; otherwise it breaks. The method returns the final (best) rank index reached, which the caller uses to notify the affected range. The value parameter is declared `int`, and the caller casts the 64-bit EXP to `int` (`CFileDB.cpp:2035`) — a potential truncation risk for very large EXP values (see Technical Debt).

**Rule workflow:**
1. Normalize the class value (MORTAL/ARCH).
2. Update the entry's stored Value to the new EXP.
3. Compare upward against the next-higher index with the player-rise predicate.
4. Swap upward while the predicate holds, else break.
5. Return the final rank index.

---

### Business Rule: Adjacent slot swap preserving connection metadata

**Overview:**
Rising in the ranking is implemented as a swap of two adjacent array slots, moving both the ranking entry and its associated connection metadata together so online-player routing stays consistent.

**Detailed description:**
`riseRankingElement(rankId)` guards on `rankId >= LAST && rankId < FIRST` (`CRanking.cpp:344`) and performs two coordinated swaps (`CRanking.cpp:347-354`). First the `STRUCT_RANKING*` pointers are swapped using `setElement(..., false)` (the `false` disables deletion of the displaced object, since the two objects are being exchanged rather than replaced). Second, the `GrindRankingConnId` entries are swapped. Because a ranking entry and its connection ID must always describe the same player, both swaps must be atomic in effect; the method performs them sequentially with no synchronization, which is safe only because the component is single-threaded within the DBSrv message loop. Swapping pointers (rather than copying struct contents) is efficient and avoids reallocating heap objects.

**Rule workflow:**
1. Validate rankId is within [LAST, FIRST-1].
2. Swap the two `STRUCT_RANKING*` pointers without deletion.
3. Swap the two `GrindRankingConnId` values.
4. The two players have exchanged positions and routing metadata.

---

### Business Rule: Connection-aware player notification with bounds validation

**Overview:**
Ranking changes are pushed to online players only, and only when the stored connection metadata points to a valid TMSrv socket and player ID.

**Detailed description:**
`sendUpdateRangeRank(initId, endId)` iterates from `initId - 1` to `endId + 1` (to include neighbors of the affected range) but clamps each index to `[LAST, FIRST]` (`CRanking.cpp:325-328`). For each valid slot it retrieves the connection ID and only calls `sendUpdateIndividualRank` if `TMSrvId` is in `[0, MAX_SERVER]` and `PlayerId` in `[1, MAX_USER]` (`CRanking.cpp:332-333`). Similarly, `sendUpdateIndividualRank` re-validates the same socket/player bounds before sending (`CRanking.cpp:283`) and checks that the target socket handle is non-null (`pUser[tmsrvId].cSock.Sock`, `CRanking.cpp:312`). The connection metadata is populated at character login (`CFileDB.cpp:847, 1087`), so only currently-logged-in players have valid connections; offline players simply receive no notification, and their in-memory ranking entry remains until evicted.

**Rule workflow:**
1. Compute the affected range (plus neighbor slots) from the old and new rank IDs.
2. Clamp iteration to valid indices.
3. Fetch each slot's connection metadata.
4. Notify only entries whose TMSrvId and PlayerId pass bounds checks and whose socket is open.

---

### Business Rule: Rank-snapshot packet construction and OUT_OF_RANK handling

**Overview:**
`sendUpdateIndividualRank` builds a `MSG_SendExpRanking` packet containing the target player's own rank data plus the ranking entries immediately above and below, with position-specific neighbor selection.

**Detailed description:**
The packet (`MSG_SendExpRanking`, `Basedef.h:2197-2215`) carries `RankPosition`, `PlayerAbove`, `PlayerRank`, and `PlayerUnder`. The neighbor selection is position-dependent (`CRanking.cpp:289-305`):
- `OUT_OF_RANK` (not ranked): `PlayerAbove` = current LAST entry, `PlayerUnder` left default; used to tell an evicted player who now holds the bottom slot.
- `FIRST` (top player): `PlayerUnder` = element at `rankId - 1` (the runner-up).
- `LAST` (bottom player): `PlayerAbove` = element at `rankId + 1`.
- Middle ranks: both `PlayerUnder` (rankId-1) and `PlayerAbove` (rankId+1) are populated.

The player's own rank is copied into `PlayerRank` only when a non-null `playerRank` is supplied (`CRanking.cpp:307-310`). The packet is then written directly to the TMSrv socket via `SendOneMessage` (`CRanking.cpp:312-313`). The TMSrv validates before forwarding to the client (see next rule).

**Rule workflow:**
1. Validate socket/player bounds.
2. Build `MSG_SendExpRanking` with the target player ID.
3. Set RankPosition from the resolved rankId.
4. Populate PlayerAbove/PlayerUnder based on position (OUT_OF_RANK/FIRST/LAST/middle).
5. Copy PlayerRank if provided.
6. Send to the owning TMSrv socket.

---

### Business Rule: Client-forwarding guard on TMSrv

**Overview:**
TMSrv forwards an incoming `MSG_SendExpRanking` (type `_MSG_GrindRankingData`) to the game client only if the target user is in `USER_PLAY` mode and the packet's player name matches the user's current character name.

**Detailed description:**
In `ProcessDBMessage` (`ProcessDBMessage.cpp:1285-1297`), the case `_MSG_GrindRankingData` casts the message to `MSG_SendExpRanking*` and forwards it via `pUser[conn].cSock.SendOneMessage` only when `pUser[conn].Mode == USER_PLAY && m->PlayerRank.Name[0] != '\0' && !strcmp(pMob[conn].MOB.MobName, m->PlayerRank.Name)`. The comment explains that in character-selection mode (0xC) the TMSrv does not yet have the player's data, so the `PlayerRank.Name` check is intentionally skipped only in that state; here the full check ensures the packet is delivered only to the player it actually describes. This prevents rank snapshots for other players from being delivered to the wrong client and suppresses notifications while the user is in a non-game mode.

**Rule workflow:**
1. TMSrv receives `_MSG_GrindRankingData`.
2. If the target user is in USER_PLAY and the name matches, forward to client.
3. Otherwise drop the packet.

---

### Business Rule: DBSrv message acceptance gate

**Overview:**
DBSrv only processes inbound messages that carry the `FLAG_GAME2DB` bit and have a destination player ID within `[0, MAX_USER]`.

**Detailed description:**
`ProcessClientMessage` (`Server.cpp:1339-1357`) rejects any message for which `!(std->Type & FLAG_GAME2DB)` or whose `std->ID` is outside `[0, MAX_USER]`, logging the offending packet and returning without dispatch. This is the transport-level gate through which every `MSG_UpdateExpRanking` must pass before reaching the ranking handler. The `_MSG_UpdateExpRanking` type is defined with `FLAG_GAME2DB` set (`Basedef.h:2217`), so it satisfies this gate, and the `ID` carries the TMSrv-side player index used later as the routing target.

**Rule workflow:**
1. DBSrv reads an inbound message.
2. If the type lacks FLAG_GAME2DB or ID is out of range, log and drop.
3. Otherwise dispatch to `CFileDB::ProcessMessage`, which handles `_MSG_UpdateExpRanking`.

---

### Business Rule: EXP-update trigger on monster kill (TMSrv producer)

**Overview:**
TMSrv emits an `MSG_UpdateExpRanking` packet to DBSrv each time a party member actually gains EXP from a monster kill in one of the grinding map sectors, carrying that player's current total EXP.

**Detailed description:**
`MobKilled.cpp` contains four near-identical producer blocks (at `338-342`, `432-435`, `525-528`, `619-622`), each gated by a distinct map-sector/geometry condition (e.g. `(tx / 128) == 8 && (ty / 128) == 2`, `== 10 && == 2`, and a general `HALFGRID` proximity case). Each block builds `STRUCT_RANKING(pMob[party].MOB.MobName, pMob[party].MOB.Exp, pMob[party].extra.ClassMaster, pMob[party].MOB.CurrentScore.Level, pMob[party].MOB.Class)` and sends `MSG_UpdateExpRanking(party, rankInfo)` to DBSrv. The `Value` field is the player's *cumulative* total EXP (`pMob[party].MOB.Exp`, a `long long`), not the incremental gain, which is consistent with DBSrv's value-replacement and re-ranking logic. These packets are the only runtime mutation inputs to the ranking, and they fire on every EXP-granting kill — meaning the ranking is updated continuously during active grinding.

**Rule workflow:**
1. A monster dies and a party member is eligible to gain EXP.
2. EXP is computed and added to the player's `MOB.Exp`.
3. A `STRUCT_RANKING` snapshot of current EXP is built.
4. An `MSG_UpdateExpRanking` is sent to DBSrv.

---

## 4. Component Structure

```
legacy/Code/
├── Basedef.h                       # Shared protocol/data definitions
│   ├── MAX_RANK_INDEX = 500        # (Basedef.h:61)
│   ├── struct STRUCT_RANKING       # (Basedef.h:809-835) ranking entry payload
│   ├── MSG_SendExpRanking          # (Basedef.h:2197-2215) DB->Game->Client rank snapshot
│   └── MSG_UpdateExpRanking        # (Basedef.h:2218-2233) Game->DB EXP update
├── DBSrv/                          # Database server (owns CRanking)
│   ├── CRanking.h                  # Class declarations (GrindRanking, RankingSystem, RankPos, GrindRankingConnId)
│   ├── CRanking.cpp                # Core ranking algorithms + global `rankingSystem`
│   ├── CFileDB.cpp                 # Message dispatch; _MSG_UpdateExpRanking handler; login rank init
│   ├── CReadFiles.cpp              # WriteRanking() periodic persistence to Ranking.txt
│   ├── CReadFiles.h                # Declares WriteRanking() (CReadFiles.h:53)
│   ├── CUser.h / CUser.cpp         # pUser[] connection objects (sockets to TMSrvs)
│   ├── Server.cpp                  # ProcessClientMessage gate; ProcessSecTimer periodic WriteRanking
│   └── Server.h                    # Log()/StartLog(); pUser/pAdmin externs
└── TMSrv/                          # Game server (producer/consumer)
    ├── MobKilled.cpp               # Produces MSG_UpdateExpRanking on EXP-gaining kills (4 sites)
    ├── ProcessDBMessage.cpp        # Forwards _MSG_GrindRankingData snapshots to clients
    └── Server.cpp                  # (DoRanking PvP challenge - related but separate subsystem)
```

**Class-level structure (CRanking component):**

| Type | Members | Role |
|------|---------|------|
| `enum RankPos` | `FIRST`, `LAST`, `OUT_OF_RANK` | Rank index sentinels |
| `struct GrindRankingConnId` | `TMSrvId`, `PlayerId` | Maps a rank slot to an online player location |
| `class GrindRanking` | `m_GrindRank[500]`, `m_GrindRankConnId[500]` | Container holding entries + routing metadata |
| `class RankingSystem` | `grindRanking` | Orchestrator: load, insert, rise, notify |
| `struct STRUCT_RANKING` | `Name[32]`, `Value` (long long), `ClassMaster`, `Level`, `Class` | The rankable payload (defined in Basedef.h) |

**Note on scope:** `_MSG_ReqRanking.cpp` and the `DoRanking()`/`RankingProgress`/`Ranking1`/`Ranking2` globals in TMSrv (`Server.cpp:414, 8052-8100`) form a *separate* PvP/guild challenge ("ranking challenge duel") subsystem. Although it shares the "Ranking" terminology, it does not use the `CRanking` class at all (it operates on its own globals and teleport logic). It is documented here only to disambiguate scope; it is not part of the CRanking component.

---

## 5. Dependency Analysis

### Internal dependencies (within the CRanking component and its host process)

```
RankingSystem ──> GrindRanking          (owns grindRanking member)
RankingSystem ──> STRUCT_RANKING        (Basedef.h)
RankingSystem ──> GrindRankingConnId    (routing metadata)
RankingSystem ──> CReadFiles::WriteRanking  (persistence)
RankingSystem ──> Log()                 (Server.h)
RankingSystem ──> pUser[] / CUser       (socket send; Server.cpp extern)
RankingSystem ──> MSG_SendExpRanking    (packet build)
CFileDB (DBSrv) ──> RankingSystem       (login init, _MSG_UpdateExpRanking handler)
CReadFiles (DBSrv) ──> RankingSystem     (WriteRanking reads grindRanking)
Server.cpp (DBSrv) ──> CRanking.h        (header include)
```

### Cross-process dependencies

```
TMSrv MobKilled.cpp ──(MSG_UpdateExpRanking, _MSG_UpdateExpRanking)──> DBSrv CFileDB
DBSrv CRanking.cpp  ──(MSG_SendExpRanking, _MSG_GrindRankingData)──> TMSrv ProcessDBMessage ──> client
```

### External dependencies

| Dependency | Type | Purpose | Notes |
|------------|------|---------|-------|
| Win32 File APIs | Platform SDK | Directory scan, file open/read, file time | `FindFirstFile`, `fopen_s`, `FileTimeToSystemTime`, `GetLocalTime` |
| Win32 `GetTickCount` | Platform SDK | Startup load timing/logging | `CRanking.cpp:101-107` |
| C standard library | Runtime | `strcmp`, `strncpy`, `sprintf`, `fprintf` | String/format helpers |
| Custom socket protocol | Internal | DBSrv <-> TMSrv message exchange | Length-prefixed binary messages; `SendOneMessage` |

There is **no** database, no ORM, no external web service, and no third-party library. All persistence is via flat files (`./account/*` and `../../Common/Ranking.txt`).

---

## 6. Afferent and Efferent Coupling

The component is C++ (object-oriented), so coupling is measured at the class/struct level. "Files" indicate distinct source files that reference the type.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|----------|
| RankingSystem | 3 (CFileDB.cpp, CReadFiles.cpp, Server.cpp) | 6 (GrindRanking, STRUCT_RANKING, GrindRankingConnId, CReadFiles, CUser/pUser, MSG_SendExpRanking) | High |
| GrindRanking | 3 (RankingSystem, CFileDB.cpp, CReadFiles.cpp) | 2 (STRUCT_RANKING, GrindRankingConnId) | High |
| GrindRankingConnId | 4 (GrindRanking, RankingSystem, CFileDB.cpp, CRanking.h users) | 0 | Low |
| STRUCT_RANKING (Basedef.h) | High (all ranking code + MobKilled.cpp + packet defs) | 0 | High |
| MSG_SendExpRanking | 2 (CRanking.cpp producer, ProcessDBMessage.cpp consumer) | 1 (STRUCT_RANKING) | Medium |
| MSG_UpdateExpRanking | 2 (MobKilled.cpp producer, CFileDB.cpp consumer) | 1 (STRUCT_RANKING) | Medium |

Notes:
- `RankingSystem` is the highest-fan-in orchestration class and is the entry point for all ranking mutations; it is a critical, heavily-referenced hub.
- `GrindRanking` is accessed *directly* by consumers (`rankingSystem.grindRanking.getElementIndex(...)`, `.setElementConnId(...)`, `.getElement(...)`) rather than only through `RankingSystem`, so its public surface leaks into CFileDB and CReadFiles.
- Both `RankingSystem` and `GrindRanking` expose their full array-access surface publicly (no encapsulation), raising afferent coupling on low-level operations.

---

## 7. Endpoints

The CRanking component exposes **no** REST/GraphQL/gRPC endpoints. It communicates exclusively through the legacy custom binary socket protocol between TMSrv and DBSrv. The relevant protocol messages (the component's de-facto interface) are:

| Message Type | Direction | Description |
|--------------|-----------|-------------|
| `_MSG_UpdateExpRanking` (MSG_UpdateExpRanking) | TMSrv -> DBSrv | Reports a player's updated total EXP for re-ranking; handled at CFileDB.cpp:1990 |
| `_MSG_GrindRankingData` (MSG_SendExpRanking) | DBSrv -> TMSrv -> Client | Rank snapshot (position + above/under neighbors); built at CRanking.cpp:279-316, forwarded at ProcessDBMessage.cpp:1285 |

Both types are declared in `Basedef.h` (`2196-2215` and `2217-2233`) with the `FLAG_NEW`, `FLAG_DB2GAME`, `FLAG_GAME2DB`, and `FLAG_GAME2CLIENT` routing bits.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| TMSrv -> DBSrv (EXP updates) | Internal service | Report EXP gains to re-rank players | Custom TCP binary message | `MSG_UpdateExpRanking` (packed struct, `FLAG_GAME2DB`) | DBSrv gate rejects non-GAME2DB/out-of-range IDs (Server.cpp:1343); no retry/backoff |
| DBSrv -> TMSrv (rank snapshots) | Internal service | Push rank changes to online players | Custom TCP binary message | `MSG_SendExpRanking` (packed struct, `FLAG_DB2GAME`) | Socket null-check (CRanking.cpp:312); TMSrv drops if not USER_PLAY/name mismatch |
| DBSrv -> Client (via TMSrv) | Internal service | Display ranking to players | Custom TCP binary message | `MSG_SendExpRanking` | Guarded in ProcessDBMessage.cpp:1293 |
| Account files (`./account/*`) | File system | Build ranking at startup | File I/O | Binary `STRUCT_ACCOUNTFILE` | Size gate; silent skip on open/size failure (no logging) |
| Ranking output (`../../Common/Ranking.txt`) | File system | Persist top-500 ranking | File I/O | Text (pos, name, classmaster, class) | Silent return if fopen fails (CReadFiles.cpp:1049-1050) |

There is no external third-party service integration; all integration is internal (process-to-process over the custom socket protocol) and local file I/O.

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Global Singleton | Process-global `rankingSystem` instance | CRanking.cpp:29 | Single shared ranking state across DBSrv |
| Container / Data-store pattern | `GrindRanking` with parallel fixed arrays | CRanking.h:50-66 | Centralized, indexable ranking storage |
| In-place bubble sort | `riseRankingElement` + upward loops | CRanking.cpp:257-268, 379-388 | Maintain ascending order on insert/update |
| Message-driven command dispatch | Switch on message type in CFileDB::ProcessMessage | CFileDB.cpp:240, 1990 | Route inbound protocol messages to handlers |
| Push-based notification | sendUpdateRangeRank/sendUpdateIndividualRank | CRanking.cpp:321-337, 279-316 | Propagate rank changes to affected online players |
| Recursive directory traversal | `readAccountsInDir` self-recursion | CRanking.cpp:118-215 | Walk account directory tree at load |
| Compile-time capacity | `MAX_RANK_INDEX` macro sizing arrays | Basedef.h:61 | Fixed-size leaderboard |

Architectural observations:
- The component follows a **two-tier class split**: a dumb container (`GrindRanking`) and a logic layer (`RankingSystem`), but the container's internals are fully public, blurring the boundary.
- The **file-based, in-memory, periodically-serialized** persistence model is a deliberate legacy design with no transactional guarantees.
- **Single-threaded assumption:** all mutations and the swap operations assume exclusive access within the DBSrv message loop; there is no locking or synchronization.
- The **rank order inversion** (index 0 = worst) is an intentional design that must be maintained consistently by all consumers.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | `tryInsertInRanking` / `increaseRankingElementValue` | Compound predicates rely on operator precedence (`&&` before `||`) with no parentheses | Correct only by precedence rules; fragile to maintain and easy to misread |
| High | `CFileDB.cpp:2035` + `increaseRankingElementValue(int newValue)` | 64-bit `long long` EXP is cast to 32-bit `int` | Potential value truncation for large EXP, producing incorrect re-ranking |
| Medium | `readAccountsInDir` | Silent empty error branches (invalid handle, wrong size, open failure) at CRanking.cpp:131, 199, 206 | Startup failures are invisible; partial/empty ranking built without logs |
| Medium | `loadRanking` timing log | `resultClock / 60` and `% 60` mislabels milliseconds | Logged "seconds" figure is misleading (off by ~factor 60) |
| Medium | `readAccountsInDir` | Recursive scan with `strcpy`/`sprintf` into fixed `char tmp[1024]` and unbounded depth | Potential stack/heap buffer concerns on deep/odd directory names |
| Medium | CRanking.h:61, 80 | `GrindRanking::updateElement` and `RankingSystem::getRankingIndexFromName` declared but never implemented/referenced | Dead declarations; API surface promises behavior that does not exist |
| Medium | `CFileDB.cpp:2033` | Update predicate `m->RankInfo.ClassMaster > thisRank->ClassMaster` allows rise with no value increase when tier improves | Class-tier alone can re-rank a player even without EXP gain (intended but notable) |
| Low | `CReadFiles.cpp:1052-1053` | Hard-coded class name lookup arrays indexed by raw class value | Inconsistent/out-of-range class values index out of bounds or print wrong labels |
| Low | `WriteRanking` | `fopen` failure returns silently (CReadFiles.cpp:1049-1050) | Persistence loss undetected |
| Low | Threshold literals | Level-1000 exclusion duplicated as literals in two files (CRanking.cpp:187, CReadFiles.cpp:1057) | Drift risk if changed in only one place |
| Low | Encapsulation | `GrindRanking` arrays and accessors fully public; consumers reach into internals | High coupling, low cohesion on low-level operations |
| Low | Concurrency | No synchronization on shared arrays; assumes single-threaded message loop | Unsafe if architecture ever multi-threads |

---

## 11. Test Coverage Analysis

An exhaustive search of the entire repository (`/home/luisdias/dev/github/luisdiasdev/w2pp`, including `legacy/`) for unit, integration, or regression test files (patterns: `*test*`, `*spec*`, `*unittest*`, `*gtest*`, `*.test.cpp`) returned **no results**. There are no test directories and no test projects in the solution.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| CRanking (RankingSystem + GrindRanking) | 0 | 0 | 0% | N/A — no automated tests exist |
| _MSG_UpdateExpRanking handler (CFileDB) | 0 | 0 | 0% | N/A |
| MSG_SendExpRanking forwarding (TMSrv ProcessDBMessage) | 0 | 0 | 0% | N/A |
| MobKilled EXP-update producers | 0 | 0 | 0% | N/A |

**Coverage risk:** The ranking subsystem's core algorithms (insertion predicate, eviction, bubble-up ordering, class-tier dominance, OUT_OF_RANK neighbor selection) are pure, deterministic, and highly testable, yet they carry zero automated coverage. The correctness of the compound comparison predicates and the inverted index ordering are high-risk areas that would particularly benefit from tests, but none exist. There is also no test harness configured in `DBSrv.vcxproj` or the solution file.

**Note:** All the business-rule and complexity analysis above was derived from direct static source inspection; no runtime execution or test execution was performed (per analysis constraints).

---

## 12. Report Metadata

- **Component name:** CRanking (player experience grind ranking subsystem)
- **Primary files:** `legacy/Code/DBSrv/CRanking.h`, `legacy/Code/DBSrv/CRanking.cpp`
- **Direct consumers:** `legacy/Code/DBSrv/CFileDB.cpp`, `legacy/Code/DBSrv/CReadFiles.cpp`, `legacy/Code/DBSrv/Server.cpp`, `legacy/Code/TMSrv/MobKilled.cpp`, `legacy/Code/TMSrv/ProcessDBMessage.cpp`
- **Shared definitions:** `legacy/Code/Basedef.h`
- **Host project:** `legacy/Code/DBSrv/DBSrv.vcxproj` (compiles `CRanking.cpp`; includes `CRanking.h`)
- **Persistence artifact:** `Common/Ranking.txt` (relative path `../../Common/Ranking.txt` from DBSrv)
- **Folders ignored during analysis:** `.git`, `.opencode`
