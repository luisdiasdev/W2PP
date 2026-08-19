# Component Deep Analysis Report

**Component:** CCastleZakum (Castle / Zakum Siege Event)
**Project:** W2PP - Legacy C/C++ codebase (`legacy/`)
**Analysis Date:** 2026-08-19 17:13:23
**Component Boundary:** `legacy/Code/TMSrv/CCastleZakum.{h,cpp}` and its integration points across `TMSrv`

---

## 1. Executive Summary

`CCastleZakum` is a static (all-member `static`) C++ class in the `TMSrv` (game server) module of the W2PP legacy project. It implements the **Castle Zakum dungeon/raid event** — a party-based timed quest in which a leader opens the main gate with a quest key, a party fights spawned monsters culminating in two boss kills, and the party receives item/experience/coin rewards. The component manages the full lifecycle of the event: gate interaction, monster spawning, per-party registration, timed progression, cleanup, and reward distribution.

The component is a pure game-logic service: it exposes **no network endpoints of its own**, is driven entirely by callbacks from other server modules (message handlers, timers, and the mob-kill pipeline), and maintains global state that is mutated across all of these call sites. It is fully static and data-oriented, operating on process-global arrays (`pMob`, `pItem`, `pUser`, `mNPCGen`, `CastleQuest`, etc.) rather than encapsulated instance state.

### Key Findings

- **Architectural role:** `CCastleZakum` is a coordinating subsystem that bridges item interaction (`_MSG_UpdateItem`), movement validation (`_MSG_Action`), monster death (`MobKilled`), and server timers (`ProcessSecMinTimer`) into a single "Castle Quest" game event.
- **Config-driven design:** All quest parameters (monster ranges, boss IDs, prizes, experience, coins, time limits, party-prize flag) are loaded at runtime from a plain-text settings file `../../Common/Settings/CastleQuest.txt` via a custom line parser (`ParseCastleString`). The file is **not present in the repository** — it is a runtime deployment artifact.
- **Global mutable state:** The event uses process-global variables (`CastleQuest`, `CastleQuestClear`, `CastleQuestTime`, `CastleQuestParty`, `CastleQuestLevel`, `CastleLeader`) with no synchronization, consistent with the single-threaded server loop design.
- **No test coverage:** No unit, integration, or specification test files exist anywhere in the legacy codebase for this component (or the project as a whole). This is documented as a significant risk.
- **Hardcoded geometry/coordinates:** The event's monster clearing zones and the "safe area" positions that block player movement are hardcoded as map coordinates within the `.cpp` file, reducing configuration flexibility.
- **Portuguese-language comments and messages:** The code and user-facing string-table lookups carry Portuguese comments/identifiers; localization is centralized in `Language.h` string-table IDs.

---

## 2. Data Flow Analysis

The following describes how data flows through the `CCastleZakum` component across its seven public entry points. Because the component is event-driven and asynchronous, data enters at multiple independent points.

```
1. [Server startup / reload] Server.cpp:3629 / imple.cpp:259
   → CCastleZakum::ReadCastleQuest()
   → fopen("../../Common/Settings/CastleQuest.txt")
   → ParseCastleString() populates global STRUCT_CASTLEQUEST CastleQuest[MAX_CASTLE_QUEST]

2. [Player uses a gate item] _MSG_UpdateItem.cpp:51
   → CCastleZakum::OpenCastleGate(conn, gateid, m)
   → reads gate key (EF_KEYID 10-14) from pItem[gateid].ITEM
   → scans player's carry (MAX_CARRY) for matching key (EF_KEYID + EF_QUEST band)
   → if gatekey==10: registers party, sets CastleQuestLevel/Time/Party/Leader, spawns mobs
   → consumes the key item, opens gate (STATE_OPEN) via UpdateItem()
   → GridMulticast to broadcast open state, writes audit Log

3. [Monster killed] MobKilled.cpp:980
   → CCastleZakum::MobKilled(target, conn, PosX, PosY)
   → if killed mob's GenerateIndex is a BOSS for a quest level:
       → broadcast notice, set CastleQuestClear=1
       → reward party leader & members: PutItem (Prize[]), Exp, Coin (≤2,000,000,000 cap)

4. [Monster drops a key] MobKilled.cpp:1904
   → CCastleZakum::KeyDrop(target, conn, PosX, PosY, Key)
   → if dropped item's EF_KEYID is 11-14: stamps EF_QUEST=CastleQuestLevel and consumes (returns FALSE, item not put on ground)

5. [Timer - seconds] ProcessSecMinTimer.cpp:1172
   → CCastleZakum::ProcessSecTimer()
   → every 2nd second: decrements CastleQuestTime; at 0 clears the area & kills remaining mobs

6. [Timer - minutes] ProcessSecMinTimer.cpp:2110
   → CCastleZakum::ProcessMinTimer()
   → two-stage clear: CastleQuestClear 1→2 (notice) then 2→0 (clear area/mobs)

7. [Player movement] _MSG_Action.cpp:238
   → CCastleZakum::CheckMove(conn, TargetX, TargetY)
   → if a non-registered player enters a restricted position block, teleports them back
```

The shared global state (`CastleQuest*`, `CastleQuestClear`, `CastleQuestTime`, etc.) is both the input and the output of most of these flows, meaning the component's "data" is its process-global event state that is mutated from many independent call sites and read back by others.

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Gate must carry an `EF_KEYID` between 10 and 14 to be considered a Zakum gate | CCastleZakum.cpp:79 |
| Validation | Player must carry an item with matching `EF_KEYID` and matching `EF_QUEST` band (except main gate) | CCastleZakum.cpp:86-103 |
| Validation | Inner gates (key 11-14) require `Quest == CastleQuestLevel` | CCastleZakum.cpp:190 |
| Validation | Main gate (key 10) requires quest index `0 <= Quest < MAX_CASTLE_QUEST` | CCastleZakum.cpp:108 |
| Validation | Only one active quest allowed at a time (`CastleQuestTime != -1` blocks new starts) | CCastleZakum.cpp:111 |
| Business Logic | Main gate consumes a quest-key item with `EF_QUEST` set to the chosen quest level | CCastleZakum.cpp:106-185 |
| Business Logic | Opening the main gate registers the party and its leader | CCastleZakum.cpp:158-184 |
| Business Logic | Opening the main gate spawns all configured mobs (twice each) within `[MOB_INITIAL, MOB_END]`, excluding bosses | CCastleZakum.cpp:147-154 |
| Business Logic | Opening the main gate starts the quest countdown timer | CCastleZakum.cpp:156 |
| Business Logic | Any inner gate opened while party registered consumes the key item and opens the gate | CCastleZakum.cpp:195-203 |
| Business Logic | Killing a boss sets the quest as cleared and rewards the party | CCastleZakum.cpp:210-297 |
| Business Logic | Coin reward capped at 2,000,000,000 (2G) total | CCastleZakum.cpp:246,283 |
| Business Logic | Key drops (keys 11-14) are tagged with the current quest level and returned to the player (not dropped on ground) | CCastleZakum.cpp:299-312 |
| Timing | Quest has a countdown (QuestTime); when it reaches 0 the event is reset and the area cleared | CCastleZakum.cpp:314-343 |
| Timing | Quest clear is two-stage over two minute-ticks before area cleanup | CCastleZakum.cpp:345-375 |
| Constraint | Non-registered players attempting to enter restricted positions are teleported back | CCastleZakum.cpp:377-394 |
| Configuration | Quest parameters loaded from `CastleQuest.txt`; `#` starts a new quest record | CCastleZakum.cpp:396-443 |
| Configuration | `PARTYPRIZE:ON` enables per-member item rewards | CCastleZakum.cpp:258,528-529 |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: Gate identification and key validation

**Overview:**
A physical "gate" in the Zakum dungeon is identified by an `EF_KEYID` item effect value between 10 and 14. Key value 10 designates the main castle gate (which starts the quest), while values 11 through 14 designate inner gates. The gate's key requirement is read from the static item data of the gate object (`pItem[gateid].ITEM`) via `BASE_GetItemAbility(..., EF_KEYID)` (CCastleZakum.cpp:77).

**Detailed description:**
`OpenCastleGate` is invoked from the `_MSG_UpdateItem` message handler whenever a client attempts to update/activate a gate item (legacy/Code/TMSrv/_MSG_UpdateItem.cpp:51). The function first inspects the gate's own `EF_KEYID`; if this value is outside the inclusive range 10-14, the gate is not considered a Zakum gate and the function returns `FALSE` (CCastleZakum.cpp:79-80), which causes the caller to fall through to the generic gate-opening logic elsewhere in `_MSG_UpdateItem.cpp`. Only gates with a key id in this specific band are handled by the Zakum subsystem.

The player must present a matching key. The handler scans the player's entire carry inventory (up to `MAX_CARRY`, which is 64) looking for an item whose `EF_KEYID` equals the gate's key id and whose `EF_QUEST` band matches the currently active quest level (CCastleZakum.cpp:86-95). The main gate (key 10) has a relaxation: the quest-band comparison is skipped (`Quest != CastleQuestLevel && gatekey != 10`), so the main gate only requires the key id itself. If no matching key is found and the gate is not a special item (sIndex != 773), a localized "no key" message is sent to the client and the function returns `TRUE` (blocking the generic handler) without opening the gate (CCastleZakum.cpp:97-103).

**Rule workflow:**
1. Read gate `EF_KEYID` via `BASE_GetItemAbility`.
2. If key id not in [10,14], return `FALSE` (fall through to generic gate logic).
3. Scan player carry for item with matching `EF_KEYID` and (unless main gate) matching `EF_QUEST`.
4. If no match, notify "no key" (unless gate is item 773) and block the generic handler (`TRUE`).
5. If matched, proceed to main-gate or inner-gate logic.

---

### Business Rule: Main gate quest initiation (single active quest)

**Overview:**
The main gate (key id 10) is the single entry point to start a Zakum quest. It validates the quest band selected by the player's key, enforces that no other quest is currently running, clears any leftover monsters from a previous run, spawns the configured mob set, records the party and leader, and starts the countdown timer.

**Detailed description:**
When a player presents a key with key id 10, the function first validates that the key's `EF_QUEST` value is a valid quest index (`Quest >= 0 && Quest < MAX_CASTLE_QUEST`, where `MAX_CASTLE_QUEST = 64`) (CCastleZakum.cpp:108). This `EF_QUEST` value selects which of the up-to-64 configured quest records in `CastleQuest[]` is being attempted.

The component enforces a hard invariant of **one active quest at a time**: if `CastleQuestTime != -1` (meaning a quest is currently running), the handler reports the ongoing quest to the player — showing the stored leader name and the current party member count — and returns `TRUE` without starting a new one (CCastleZakum.cpp:111-127). This prevents overlapping events. The party member count is computed by iterating `CastleQuestParty[0..MAX_PARTY)` counting non-zero, valid (< `MAX_USER`) entries.

Before starting, the component cleans the entire Zakum play field (map rectangle x:2180-2296, y:1160-1269), deleting every mob that is not a user and resetting each such mob's generator (`mNPCGen.pList[generate].MinuteGenerate = -1` and `DeleteMob(tmob, 3)`) (CCastleZakum.cpp:130-144). This removes "leftover" mobs from any prior quest run.

It then spawns the mobs for the selected quest: for every generator index `x` from `CastleQuest[Quest].MOB_INITIAL` through `MOB_END`, it enables the generator (`MinuteGenerate = 1`) and calls `GenerateMob(x, 0, 0)` twice, unless that index is one of the two boss indices (`BOSS[0]`/`BOSS[1]`), which are excluded from the initial spawn (CCastleZakum.cpp:147-154). Bosses are thus spawned by a separate mechanism (normal generator respawn), not eagerly at quest start.

Finally the component sets the global quest state: `CastleQuestLevel = Quest`, `CastleQuestTime = CastleQuest[Quest].QuestTime - 1` (CCastleZakum.cpp:155-156), resolves the party leader (using `pMob[conn].Leader`, defaulting to `conn` if the leader id is 0) and copies the leader name into `CastleLeader` (CCastleZakum.cpp:158-163). A `_MSG_StartTime` signal with the quest's total time is broadcast to the leader and to every online party member in `USER_PLAY` mode (CCastleZakum.cpp:165-173). The party roster is snapshotted into the global `CastleQuestParty[]` array (slot `MAX_PARTY` holds the leader) (CCastleZakum.cpp:175-184).

**Rule workflow:**
1. Validate `Quest` is within `[0, MAX_CASTLE_QUEST)`.
2. If a quest is already running (`CastleQuestTime != -1`), report leader/count and abort.
3. Delete all leftover mobs in the play field and reset their generators.
4. For each generator in `[MOB_INITIAL, MOB_END]` (excluding bosses), enable and spawn twice.
5. Set `CastleQuestLevel`, `CastleQuestTime = QuestTime - 1`.
6. Resolve and record leader name; send start-time signal to leader and party.
7. Snapshot party list into `CastleQuestParty[]`.

---

### Business Rule: Inner gate passage

**Overview:**
Inner gates (key ids 11-14) represent the interior gates of the dungeon that separate quest stages. To pass an inner gate, the player must not only hold a key with the matching key id but must also be registered in the currently active quest (their key's `EF_QUEST` must equal `CastleQuestLevel`).

**Detailed description:**
The inner-gate branch (`gatekey >= 11 && gatekey <= 14`) performs an additional strict check: `if (Quest != CastleQuestLevel) return TRUE;` (CCastleZakum.cpp:188-192). This means a player holding an inner-gate key that was stamped with a different (or no) quest level is denied passage. Because `KeyDrop` stamps dropped keys 11-14 with the current `CastleQuestLevel` (see Key Drop rule below), a player can only pass inner gates with keys that originated during the currently running quest. This couples gate progression tightly to the active quest level and prevents carrying keys across runs.

If the check passes, the code falls through to the shared key-consumption path: the matched carry slot is zeroed and the client is updated (`SendItem` with `ITEM_PLACE_CARRY`), the gate is opened via `UpdateItem(gateid, STATE_OPEN, ...)`, and the open state is multicast to nearby players (`GridMulticast`); an audit log entry records the gate id and coordinates (CCastleZakum.cpp:195-206). The key item is consumed in the process — opening a gate uses up the key.

**Rule workflow:**
1. If `gatekey` in [11,14], require `Quest == CastleQuestLevel`.
2. If mismatch, deny (return `TRUE`) without opening or consuming.
3. On success, consume the carry key, open the gate (`STATE_OPEN`), and multicast the state.

---

### Business Rule: Boss kill and reward distribution

**Overview:**
When a player kills a monster whose generator index matches one of the two boss indices (`BOSS[0]` or `BOSS[1]`) of any configured quest, the quest is marked cleared (`CastleQuestClear = 1`), a server-wide notice is broadcast to the play area, and rewards (items, experience, coins) are distributed to the party leader and, if `PartyPrize` is enabled, to every online party member.

**Detailed description:**
`MobKilled` is called from the central monster-kill pipeline (`MobKilled.cpp:980`) after any monster death. It iterates all configured quests (`for k in 0..MAX_CASTLE_QUEST`) and checks whether the killed mob's `GenerateIndex` equals `CastleQuest[k].BOSS[0]` or `BOSS[1]` (CCastleZakum.cpp:214-217). On a match, a localized notice (`_SN_CastleQuest_Killed`) naming the killer is broadcast over the play area rectangle (2176,1160)-(2300,1276) and `CastleQuestClear = 1` (CCastleZakum.cpp:218-220).

The party leader is resolved (killer's `Leader`, defaulting to the killer if `<= 0`) (CCastleZakum.cpp:224-227). The leader then receives: every non-empty entry of `CastleQuest[k].Prize[]` via `PutItem` (CCastleZakum.cpp:229-233); the experience prize indexed by the leader's class-master slot (`ExpPrize[pMob[partyleader].extra.ClassMaster]`), which is added to `MOB.Exp` with a localized "experience gained" message (CCastleZakum.cpp:234-240); and the coin prize (`CoinPrize`) added to the leader's coins, subject to the 2,000,000,000 coin cap (see Coin cap rule) (CCastleZakum.cpp:242-256).

If `CastleQuest[k].PartyPrize` is non-zero, the same three rewards are repeated for every online party member (`pUser[partymember].Mode == USER_PLAY`) in the leader's party list (CCastleZakum.cpp:258-294). Item prizes are granted via `PutItem`; experience is added to each member's `MOB.Exp`; coin is added subject to the same cap. This makes the "party prize" a multiplier that grants full rewards to every member rather than only the leader.

**Rule workflow:**
1. On mob death, check `GenerateIndex` against each quest's `BOSS[0]/BOSS[1]`.
2. On match: broadcast boss-killed notice; set `CastleQuestClear = 1`.
3. Resolve leader (fallback to killer).
4. Grant leader item/exp/coin prizes (coin capped).
5. If `PartyPrize` set, grant the same to each online party member.

---

### Business Rule: Coin prize cap (2,000,000,000)

**Overview:**
Coin rewards from a boss kill are only granted if the resulting total coin balance does not exceed 2,000,000,000 (2G). If adding the prize would exceed the cap, the coin is not granted and a "cannot get more than 2G" message is sent instead.

**Detailed description:**
Both the leader and party-member coin grant paths compute `unsigned int Coin = pMob[..].MOB.Coin + CastleQuest[k].CoinPrize` (CCastleZakum.cpp:244 and 281). The grant only proceeds if `Coin <= 2000000000`; in that case a localized "received X gold" message is shown, `MOB.Coin` is set to the new total, and `SendEtc` refreshes the client's stats (CCastleZakum.cpp:246-252 and 283-289). Otherwise the coin is not awarded and a `_NN_Cant_get_more_than_2G` message is displayed (CCastleZakum.cpp:255 and 292). This guards against an integer-overflow/balance-corruption scenario where a player could otherwise accumulate more than the game's 2-billion coin ceiling. The prize is effectively lost in this overflow case — there is no partial award or alternate compensation.

**Rule workflow:**
1. Compute prospective new coin total = current + prize.
2. If total <= 2,000,000,000, award coin and refresh client.
3. Otherwise, refuse the award and show "cannot get more than 2G".

---

### Business Rule: Key drop tagging (EF_QUEST stamping)

**Overview:**
When a monster drops an item that is a Zakum inner-gate key (its `EF_KEYID` is between 11 and 14), the component stamps that item's `EF_QUEST` effect with the current `CastleQuestLevel` and returns it directly to the killer's inventory instead of allowing it to fall to the ground. `KeyDrop` returns `FALSE` in this case to signal that the generic drop handler must not also `PutItem` the item.

**Detailed description:**
`KeyDrop` is called from the drop pipeline (`MobKilled.cpp:1904`) before the generic item-drop handling. It reads the dropped item's `EF_KEYID` via `BASE_GetItemAbility` (CCastleZakum.cpp:301). If the key id is in the range 11-14, the item is treated as a dungeon key: its first effect slot is set to `EF_QUEST` with value `CastleQuestLevel` (CCastleZakum.cpp:305-306), and the item is placed directly into the killer's inventory via `PutItem(conn, Key)` (CCastleZakum.cpp:308). The function returns `FALSE` (CCastleZakum.cpp:309), which causes the caller's `if (CCastleZakum::KeyDrop(...) == TRUE) PutItem(conn, item);` branch to skip the normal ground-drop placement, preventing duplication. This tagging mechanism is what later gates inner-gate passage: the dropped key now carries the quest level so it only works during the same run.

**Rule workflow:**
1. Read dropped item's `EF_KEYID`.
2. If in [11,14]: set `EF_QUEST = CastleQuestLevel`, `PutItem` into killer inventory.
3. Return `FALSE` to suppress the generic ground-drop.

---

### Business Rule: Quest time limit and timed reset

**Overview:**
The active quest runs under a countdown (`CastleQuestTime`, initialized to `QuestTime - 1`). Every 2 seconds (via the seconds timer), the countdown decrements. When it reaches exactly 0, the event is reset: the play area is cleared and all remaining mobs are deleted, effectively ending the quest without rewards.

**Detailed description:**
`ProcessSecTimer` is invoked from the server's seconds timer (`ProcessSecMinTimer.cpp:1172`). It runs its logic only on even-second ticks (`SecCounter % 2 == 0`) (CCastleZakum.cpp:316). When `CastleQuestTime == 0`, the timer sets `CastleQuestTime = -1` (marking the quest as no longer active) and clears the whole play field rectangle (2180,1160)-(2296,1269) by calling `ClearArea` and deleting all non-user mobs while resetting their generators (CCastleZakum.cpp:318-339). When `CastleQuestTime > 0`, it decrements the counter by 1 (CCastleZakum.cpp:340-341). Because the block runs only on even ticks, the effective countdown rate is one decrement per 2 seconds of wall-clock time. If the party fails to kill both bosses before the timer hits 0, the monsters are wiped and the quest silently resets with no reward.

**Rule workflow:**
1. On every 2nd second-tick: if `CastleQuestTime == 0`, reset to -1 and clear area/mobs.
2. If `CastleQuestTime > 0`, decrement.

---

### Business Rule: Two-stage quest clear / area reset

**Overview:**
After the boss is killed (`CastleQuestClear = 1`), the cleanup is deferred across two minute-ticks to give players a grace period. The first minute-tick announces the reset notice; the second minute-tick performs the actual area/mob cleanup and clears the clear flag.

**Detailed description:**
`ProcessMinTimer` is invoked from the server's minute timer (`ProcessSecMinTimer.cpp:2110`). It acts on the `CastleQuestClear` state machine: if the flag is 1, it sets it to 2 and broadcasts a `_NN_CastleQuest_Initialize` notice over the play area (2176,1160)-(2300,1276) (CCastleZakum.cpp:347-351); if the flag is 2, it sets it back to 0 and performs the physical cleanup — `ClearArea` plus deletion of all remaining non-user mobs with generator reset over the (2180,1160)-(2296,1269) rectangle (CCastleZakum.cpp:352-374). This staggered approach separates the user-facing "quest will now reset" notification from the destructive cleanup, avoiding an abrupt removal of players' surroundings and giving a one-minute pause between the announcement and the actual mob wipe.

**Rule workflow:**
1. If `CastleQuestClear == 1`: set to 2, broadcast reset notice.
2. If `CastleQuestClear == 2`: set to 0, clear area and delete remaining mobs.

---

### Business Rule: Restricted-zone movement enforcement

**Overview:**
Players who are not registered as part of the active quest's party are prevented from entering the inner quest zones. The component maintains a set of hardcoded rectangular "position blocks" (`CastleQuestPosition[MAX_CASTLE_POS][4]`); a non-registered player whose movement target falls within any of these rectangles is teleported back to their previous position.

**Detailed description:**
`CheckMove` is called from the movement handler (`_MSG_Action.cpp:238`) whenever a player issues a movement action. The function first determines whether the player is a registered quest participant by scanning `CastleQuestParty[]` for a matching connection id (CCastleZakum.cpp:381-385). If the player is NOT found (`i == MAX_PARTY + 1`), it iterates the 9 hardcoded position blocks — the main block plus four left-side and four right-side stage rectangles (CCastleZakum.cpp:46-63) — and checks whether the movement target `(targetx, targety)` falls inside any of them (CCastleZakum.cpp:388). On a hit, the player is teleported back to their prior target coordinates (`DoTeleport(conn, pMob[conn].TargetX, pMob[conn].TargetY)`) (CCastleZakum.cpp:390-391). This keeps unregistered players (and outsiders) from entering the inner quest areas while allowing registered party members to move freely, since the check short-circuits for registered members.

**Rule workflow:**
1. Scan `CastleQuestParty[]`; if the player is registered, return (no restriction).
2. If not registered, check movement target against the 9 hardcoded blocks.
3. If inside any block, teleport the player back to their previous target.

---

### Business Rule: Configuration file loading and parsing

**Overview:**
All quest content is loaded from the text file `../../Common/Settings/CastleQuest.txt` (path stored in `CASTLE_QUEST_PATH`). The file is parsed line-by-line; `#` begins a new quest record, and key-value lines set fields on the current record. Missing file at startup triggers a modal error box. The same loader is also exposed through the GM `reloadfile` command for hot reload.

**Detailed description:**
`ReadCastleQuest` opens the file in text mode; if it cannot be opened it shows a `MessageBoxA` with a Portuguese "CastleQuest.txt was not found" message and returns (CCastleZakum.cpp:398-405). It then reads lines of up to 1024 bytes. A line starting with `#` increments the record index `Num` (bounded by `MAX_CASTLE_QUEST = 64`) and zero-initializes that `CastleQuest[Num]` record; a line starting with `/` (ASCII 47, comment) is skipped; otherwise the line is passed to `ParseCastleString` (CCastleZakum.cpp:413-441). Lines that fail to parse (`pars == 0`) are skipped (CCastleZakum.cpp:436-439).

`ParseCastleString` tokenizes the line and matches the first token (uppercased) against the known field keys: `MOB_INITIAL:`, `MOB_END:`, `BOSS1:`, `BOSS2:`, `COINPRIZE:`, `EXPPRIZE_ARCH:`, `EXPPRIZE_MORTAL:`, `EXPPRIZE_CELESTIAL:`, `EXPPRIZE_SUBCELESTIAL:`, `PARTYPRIZE:` (value `"ON"` → 1), `QUESTTIME:`, and `PRIZE_N:` (for N in 0..MAX_CARRY) (CCastleZakum.cpp:498-552). The `PRIZE_` tokens populate an item's index and up to three effect pairs (effect id + value). Note the class-master experience mapping: `EXPPRIZE_ARCH:` → `ExpPrize[1]`, `EXPPRIZE_MORTAL:` → `ExpPrize[2]`, `EXPPRIZE_CELESTIAL:` → `ExpPrize[3]` and `[4]`, `EXPPRIZE_SUBCELESTIAL:` → `ExpPrize[2]` (overwriting the MORTAL slot — a data-driven quirk). Fields are matched against the class-master index used at reward time.

**Rule workflow:**
1. Open `CastleQuest.txt`; if missing, show error box and abort.
2. Parse lines: `#` → new record; `/` → comment; `\r` (ASCII 13) → skip.
3. `ParseCastleString` matches uppercased field keys and sets the current record's fields.
4. The file is re-readable at runtime via the `reloadfile` GM command.

---

## 4. Component Structure

```
TMSrv/
├── CCastleZakum.h            # Class declaration (7 static methods + static path constant)
├── CCastleZakum.cpp          # Implementation (555 lines) + global event state
└── ... (other TMSrv modules that integrate with it)
```

### Global state declared in CCastleZakum.cpp

| Symbol | Type | Purpose |
|--------|------|---------|
| `CastleQuest[MAX_CASTLE_QUEST]` | `STRUCT_CASTLEQUEST[64]` | Loaded quest configuration (monsters, bosses, prizes, timers) |
| `CASTLE_QUEST_PATH` | `const char*` | Path to `CastleQuest.txt` |
| `CastleQuestPosition[MAX_CASTLE_POS][4]` | `int[9][4]` | Hardcoded restricted movement rectangles |
| `CastleQuestClear` | `int` | 0/1/2 clear-state machine |
| `CastleQuestTime` | `int` | Countdown (active if != -1) |
| `CastleQuestParty[MAX_PARTY + 1]` | `int[13]` | Registered party roster (last slot = leader) |
| `CastleQuestLevel` | `int` | Currently active quest level index |
| `CastleLeader[NAME_LENGTH]` | `char[16]` | Leader name of active quest |

### Class methods

| Method | Role |
|--------|------|
| `OpenCastleGate` | Validate and open a gate; start quest on main gate |
| `MobKilled` | Detect boss kills and distribute rewards |
| `KeyDrop` | Tag and reroute inner-gate key drops |
| `ProcessSecTimer` | Decrement quest countdown; timed reset |
| `ProcessMinTimer` | Two-stage clear/area reset |
| `CheckMove` | Enforce restricted zones for non-registered players |
| `ReadCastleQuest` / `ParseCastleString` | Load and parse configuration file |

---

## 5. Dependency Analysis

### Internal Dependencies (within TMSrv)

```
CCastleZakum
├── ProcessClientMessage / _MSG_UpdateItem  → invokes OpenCastleGate (indirect: caller)
├── _MSG_Action                              → invokes CheckMove (indirect: caller)
├── MobKilled.cpp                            → invokes MobKilled, KeyDrop (indirect: caller)
├── ProcessSecMinTimer.cpp                   → invokes ProcessSecTimer, ProcessMinTimer (indirect: caller)
├── Server.cpp / imple.cpp                   → invokes ReadCastleQuest (indirect: caller)
├── SendFunc.cpp / SendFunc.h                → SendClientMessage, SendNoticeArea, SendClientSignalParm,
│                                               SendItem, SendEtc, GridMulticast
├── Server.cpp / Server.h                    → PutItem, GenerateMob, DeleteMob, UpdateItem, DoTeleport,
│                                               ClearArea, mNPCGen, pMobGrid, pMob, pItem, pUser, Log
├── GetFunc.cpp / GetFunc.h                  → BASE_GetItemAbility (via Basedef), SetItemBonus (in caller)
├── Language.h                               → g_pMessageStringTable IDs
└── Basedef.h / Basedef.cpp                  → STRUCT_CASTLEQUEST, constants, BASE_GetItemAbility
```

**Dependency chain (compile-time):** `CCastleZakum.cpp` includes `Basedef.h`, `CPSock.h`, `ItemEffect.h`, `Language.h`, `CItem.h`, `Server.h`, `GetFunc.h`, `SendFunc.h`, `ProcessClientMessage.h`, `ProcessDBMessage.h`, `CReadFiles.h`, `CCastleZakum.h`.

**Call relationship:** `CCastleZakum` is purely **efferent into** the server's global helper functions; no TMSrv module depends *on* `CCastleZakum`'s public methods except via the callers listed above. It reads and writes shared process-global game state.

### External Dependencies

| Dependency | Type | Notes |
|------------|------|-------|
| `CastleQuest.txt` (runtime) | Configuration file | Loaded at startup and on `reloadfile`; not present in repo |
| `Language.h` string table | Localization | User-facing messages resolved by ID at runtime |
| Windows API / WinSock | Platform | `Windows.h`, `MessageBoxA`, socket layer (`CPSock`) |
| `ItemEffect.h` | Item-effect encoding | `EF_KEYID` (39), `EF_QUEST` (58) and others |

---

## 6. Afferent and Efferent Coupling

Coupling is assessed at the class-member level for this C++ component (the programming paradigm is object-oriented; the class is fully static).

| Component / Member | Afferent Coupling | Efferent Coupling | Critical |
|--------------------|-------------------|-------------------|----------|
| `OpenCastleGate` | 1 (called from `_MSG_UpdateItem.cpp:51`) | High: `pItem`, `pMob`, `BASE_GetItemAbility`, `SendClientMessage`, `UpdateItem`, `GridMulticast`, `GenerateMob`, `DeleteMob`, `SendClientSignalParm`, `PutItem`, `SendItem`, `Log`, `mNPCGen`, `pMobGrid` | High |
| `MobKilled` | 1 (called from `MobKilled.cpp:980`) | Medium: `pMob`, `SendNoticeArea`, `PutItem`, `SendClientMessage`, `SendEtc` | High |
| `KeyDrop` | 1 (called from `MobKilled.cpp:1904`) | Low: `BASE_GetItemAbility`, `PutItem` | Medium |
| `ProcessSecTimer` | 1 (called from `ProcessSecMinTimer.cpp:1172`) | Medium: `ClearArea`, `DeleteMob`, `mNPCGen`, `pMobGrid` | Medium |
| `ProcessMinTimer` | 1 (called from `ProcessSecMinTimer.cpp:2110`) | Medium: `ClearArea`, `DeleteMob`, `mNPCGen`, `pMobGrid`, `SendNoticeArea` | Medium |
| `CheckMove` | 1 (called from `_MSG_Action.cpp:238`) | Low: `DoTeleport`, `pMob` | Medium |
| `ReadCastleQuest` / `ParseCastleString` | 2 (called from `Server.cpp:3629` and `imple.cpp:259`) | Low: `fopen`/`fgets`/`sscanf`, `MessageBoxA` | Low |

**Interpretation:** The class as a whole has very low afferent coupling (each public method is invoked from exactly one call site, `ReadCastleQuest` from two), but each method fans out into a wide set of shared global helpers and process-global data structures. This yields a "hub-like" efferent coupling: the component is a thin orchestrator over the server's global game state. The heaviest coupling is in `OpenCastleGate`, which both reads and writes many global structures and calls many side-effecting helpers. The shared mutable globals (`CastleQuest`, `CastleQuestTime`, `CastleQuestParty`, `CastleQuestLevel`, `CastleQuestClear`, `CastleLeader`) create implicit coupling between every method of the class.

---

## 7. Endpoints

The `CCastleZakum` component **does not expose any network endpoints** (REST, GraphQL, gRPC, etc.). It is a server-internal game-logic subsystem invoked through C++ callbacks from message handlers, timers, and the mob-kill pipeline. It communicates with clients only indirectly by sending existing protocol signals/messages (e.g., `_MSG_StartTime`, notice messages, item updates) through the server's `SendFunc` helpers.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| `_MSG_UpdateItem` (message handler) | Internal invocation | Gate open / quest start | In-process call | `MSG_UpdateItem` struct | Returns `FALSE` to fall through; returns `TRUE` to block; "no key" message |
| `MobKilled` pipeline | Internal invocation | Boss-kill rewards + key drops | In-process call | `target`, `conn`, `PosX`, `PosY`, `STRUCT_ITEM*` | Guards on index/level; coin cap message |
| `_MSG_Action` (movement handler) | Internal invocation | Restricted-zone enforcement | In-process call | target coords | Teleport-back |
| Server timers (`ProcessSecMinTimer`) | Internal invocation | Quest countdown + cleanup | In-process call | — | State-machine guarded |
| `CastleQuest.txt` | Configuration file | Quest content definition | File I/O (`fopen`/`fgets`/`sscanf`) | Plain text key-value | Missing file → `MessageBoxA` abort |
| `Server` globals (`pMob`, `pItem`, `pUser`, `mNPCGen`, `pMobGrid`) | Shared state | Game state read/write | In-process arrays | Structs | Range guards where present |
| `Language.h` string table | Localization | User-facing messages | In-process array | String IDs | Resolved via `g_pMessageStringTable` |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Static utility / service class | All members `static`; no instances | CCastleZakum.h:25 | Group related game logic without instance state |
| Global state / data-oriented design | Process-global `CastleQuest`, `CastleQuestTime`, etc. | CCastleZakum.cpp:42-70 | Share event state across call sites |
| Configuration-driven content | Text-file loader + line parser | `ReadCastleQuest`/`ParseCastleString` | Decouple quest content from code |
| State machine (clear flag) | `CastleQuestClear` 0 → 1 → 2 → 0 | `ProcessMinTimer` | Staggered post-clear cleanup |
| Command/reload pattern | GM `reloadfile` re-invokes `ReadCastleQuest` | imple.cpp:259 | Hot-reload configuration |
| Observer/callback | Component methods called by message/timer handlers | Various call sites | Event-driven integration |
| Localization table | `g_pMessageStringTable` lookups | Throughout | Centralize user-facing strings |
| FSM on quest lifetime | `CastleQuestTime != -1` signals active quest | `OpenCastleGate`, `ProcessSecTimer` | Enforce single active quest |

**Architectural decisions:** The component intentionally centralizes all "Castle Zakum" event logic into one class rather than scattering it. It relies entirely on the server's global game state and helper functions, making it highly coupled to the surrounding server architecture but keeping the event's own logic self-contained. Content is externalized to a text file to allow tuning without recompilation. The class is fully static, meaning there is no per-instance encapsulation; all state is global, which fits the single-threaded, monolithic server design.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | Whole component | No test coverage anywhere in the codebase | Regressions in event logic undetectable; the component is complex and reward/money-sensitive |
| High | `ParseCastleString` | `EXPPRIZE_SUBCELESTIAL:` overwrites `ExpPrize[2]` (same slot as `EXPPRIZE_MORTAL:`); CELESTIAL sets both [3] and [4] | Class-master experience mapping may award unintended values; data-order dependent |
| Medium | Global state | No synchronization; relies on single-threaded loop invariant | Any reentrancy/threading change could corrupt event state |
| Medium | `OpenCastleGate` | Large monolithic method with mixed responsibilities (validation, spawning, party registration, signaling) | Hard to reason about and test; high cyclomatic load |
| Medium | `ProcessSecTimer`/`ProcessMinTimer` | Coordinate rectangles (play field and notice area) hardcoded and duplicated across functions | Map changes require code edits; inconsistent ranges (2180-2296 vs 2176-2300) |
| Medium | Configuration | `CastleQuest.txt` absent from repository; parsing quirks (`\r` ASCII 13 skip, `/` comment) | Deployment requires the file; behavior differs if file malformed |
| Medium | `MobKilled` | Reward distribution logic duplicated between leader and party-member blocks | Drift risk between the two paths |
| Low | `ParseCastleString` | `sscanf` on fixed buffers with `%s` (no width limits) into `char[128]`/`char[256]` | Potential buffer over-read/overflow on malformed long tokens |
| Low | `ReadCastleQuest` | `strlen(str2) > 80` guard applied after `sscanf` into `str2[128]` | Guard order limits but does not fully prevent oversize input |
| Low | `OpenCastleGate` | Uses `strncpy` for `CastleLeader` without guaranteed null termination | Potential non-terminated string in edge cases |
| Info | Localization/comments | Portuguese comments and message identifiers | Maintainability for non-Portuguese developers |

---

## 11. Test Coverage Analysis

**A systematic search of the entire repository (excluding `.git` and `.opencode`) found NO test files** — no unit tests, integration tests, specifications, fixtures, or mocks — for `CCastleZakum` or for the legacy C/C++ codebase in general.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `OpenCastleGate` | 0 | 0 | 0% | No test artifacts exist |
| `MobKilled` (rewards) | 0 | 0 | 0% | No test artifacts exist |
| `KeyDrop` | 0 | 0 | 0% | No test artifacts exist |
| `ProcessSecTimer` / `ProcessMinTimer` | 0 | 0 | 0% | No test artifacts exist |
| `CheckMove` | 0 | 0 | 0% | No test artifacts exist |
| `ReadCastleQuest` / `ParseCastleString` | 0 | 0 | 0% | No test artifacts exist |

**Search evidence:**
- Glob/find for `*test*`/`*spec*` across the repo returned only matches under `.opencode/node_modules` (third-party libraries such as `zod`, `effect`, `isexe`) — none related to the legacy server code.
- The `legacy/` tree contains no test directories or test source files.
- No test project/reference exists in `W2PP Code Project.sln`.

**Consequence:** The complex, reward- and money-sensitive logic of this component (coin caps, party-prize distribution, class-master experience mapping, key tagging, timed resets) is entirely unprotected by automated regression tests. The hardcoded geometry and state-machine logic would particularly benefit from test coverage, but none is present. This absence should be treated as a High-risk gap.

---

## 12. Report Metadata

- **Component analyzed:** `CCastleZakum`
- **Boundary files:** `legacy/Code/TMSrv/CCastleZakum.h`, `legacy/Code/TMSrv/CCastleZakum.cpp`
- **Integration call sites:** `_MSG_UpdateItem.cpp`, `_MSG_Action.cpp`, `MobKilled.cpp`, `ProcessSecMinTimer.cpp`, `Server.cpp`, `imple.cpp`
- **Supporting definitions:** `legacy/Code/Basedef.h` (STRUCT_CASTLEQUEST, constants), `legacy/Code/ItemEffect.h` (EF_KEYID, EF_QUEST), `legacy/Code/TMSrv/Language.h` (message IDs), `legacy/Code/TMSrv/SendFunc.*`, `Server.*`
- **Folders excluded:** `.git`, `.opencode`
- **Key constants:** `MAX_CASTLE_QUEST = 64`, `MAX_CASTLE_POS = 9`, `MAX_PARTY = 12`, `MAX_USER = 1000`, `MAX_CARRY = 64`, `NAME_LENGTH = 16`, `STATE_OPEN = 1`, `EF_KEYID = 39`, `EF_QUEST = 58`
