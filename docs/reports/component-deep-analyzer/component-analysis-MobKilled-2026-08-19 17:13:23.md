# Component Deep Analysis Report

**Component:** MobKilled (Combat / Kill Resolution)
**Project:** W2PP (legacy C/C++ game server) - scope `legacy/`
**Report Date:** 2026-08-19 17:13:23
**Folders ignored:** `.git`, `.opencode`

---

## 1. Executive Summary

`MobKilled` is the single central combat-kill resolution routine of the W2PP
TMSrv game server. It is defined as a single global function `MobKilled(int target,
int conn, int PosX, int PosY)` in `legacy/Code/TMSrv/MobKilled.cpp` (2,469 lines,
declared in `legacy/Code/TMSrv/Server.h:142`). It is invoked every time an entity
(monster or player) reaches zero HP from any cause: direct player attacks, skill
attacks, periodic world/mob-AI simulation, and administrator commands. It is the
funnel through which all in-game deaths are processed, and it distributes
experience, drops (loot), gold, event items, quest progression, and PvP
penalties.

The component is a monolith: a single ~2.5k-line function that branches on the
killed entity's identity, level, class, clan, guild, `GenerateIndex` (spawn
template id), world coordinates, and a large set of global event/state flags
(`RvRState`, `GTorreState`, `CastleState`, `KefraLive`, `DOUBLEMODE`,
`NewbieEventServer`, etc.). It reaches deeply into shared game state via global
arrays (`pMob[]`, `pUser[]`, `pItem[]`, `pMobGrid[]`, `pHeightGrid[]`,
`mNPCGen`, `GuildInfo`, `Pista[]`, `CartaPos[]`, `WaterClear1[]`) and calls out
to roughly thirty distinct helper functions across `GetFunc`, `SendFunc`,
`Server.cpp`, `Basedef`, `CCastleZakum`, and `CWarTower`.

Key findings:

- **Two top-level branches** divide the logic into monster kills
  (target level > `MAX_LEVEL + 5`, i.e. not a player) and player deaths
  (PvP, target level <= `MAX_LEVEL`). Each has its own set of rules.
- **Experience is computed per party member** with a large, duplicated
  multi-branch formula (four near-identical copies separated only by zone
  coordinate checks) and many multiplicative modifiers. This duplication is a
  primary maintainability risk.
- **Loot and drops** are driven by per-carry-slot drop rates modulated by target
  level bands, `DropBonus`, special-stat modifiers, and test/local server flags.
- **Many world-event hooks** (Water temple, Secret Rooms / "Carta", Rune quests,
  Kefra boss, Kingdom War towers, Castle Zakum) are hardcoded into this single
  function via `GenerateIndex` range comparisons.
- **PvP branch** implements experience loss, PK-point bookkeeping, clan/war
  detection, arena/village protection, and conditional item drops (`#ifdef PKDrop`).
- **No automated tests exist anywhere in the repository** for this component
  (or any component); this is a significant risk given the complexity.
- A **parameter-name mismatch** exists between the declaration
  (`Server.h:142`, `(int conn, int target, ...)`) and definition
  (`MobKilled.cpp:41`, `(int target, int conn, ...)`); since both are `int`
  this compiles and functions, but documents an inconsistent naming contract.

---

## 2. Data Flow Analysis

Data flow through the component (inputs -> processing -> outputs):

```
1. Trigger event (entity HP <= 0):
   - _MSG_Attack.cpp:1743  (skill attack kills one or more targets)
   - Server.cpp:9203       (attack/damage resolution)
   - ProcessSecMinTimer.cpp:1940 (periodic / mob-AI damage)
   - imple.cpp:1661,1718   (admin commands "killkefra" / "npko")
   The caller passes: target (killed entity index), conn (killer index), PosX/PosY (kill position).

2. Entry validation (MobKilled.cpp:43-44):
   - Reject if conn/target out of [1, MAX_MOB-1], or target slot is USER_EMPTY.

3. Special "unkillable" guards (lines 50-109):
   - Entities with Level >= 1000 are revived (HP reset), not killed.
   - Nyerds fairy (Equip[13].sIndex==769) sanctification decrement.
   - Castle-state coordinate teleport.

4. Pet/Mount growth trigger (clan-4 summons, lines 111-169).

5. Kill confirmation packet assembly (MSG_CNFMobKill) + target HP := 0 (lines 173-183).

6. Leader resolution & summon-killer indirection (lines 185-205).

7. BRANCH A - Monster kill (target level > MAX_LEVEL+5) (lines 207-2055):
   a. Per-party-member EXP computation with zone/level/class modifiers.
   b. NPC death speech.
   c. Mob removal vs. persistence decision (clan 4/7/8 kept).
   d. Kefra boss handling.
   e. Castle gate opening.
   f. Water temple / Secret Room / Rune quest / Nightmare / RvR tower hooks.
   g. Coin (gold) drop.
   h. Event item drop.
   i. Per-slot loot table drop (with SetItemBonus / KeyDrop).
   j. Watching-town quest check.
   k. DeleteMob(target,1) + respawn hooks + CWarTower::MobKilled.

8. BRANCH B - Player death / PvP (target level <= MAX_LEVEL) (lines 2058-2467):
   a. EXP loss (deltaexp) computation by level band.
   b. PK point transfers (killer/killed), guilty, clan/war detection.
   c. Arena/village protection (no EXP loss).
   d. Same-clan and PvP item drops (#ifdef PKDrop).
   e. RvR death handling & respawn.
   f. Equip[13] HP-multiplier item sanctification loss.

9. Outputs / side effects:
   - Grid multicast of MSG_CNFMobKill / MSG_RemoveMob / MSG_CreateMob packets.
   - pMob[] / pUser[] global state mutation (HP, EXP, Coin, Carry, Equip, PK counters).
   - Item placement via PutItem / CreateItem.
   - DBSrv messages (MSG_UpdateExpRanking, MSG_DBItemDayLog, MSG_GuildInfo).
   - Log files (Log(), TesteDie.txt) and client messages.
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| # | Rule Type | Rule Description | Location |
|---|-----------|------------------|----------|
| R1 | Validation | Reject invalid kill (conn/target out of range, empty slot) | MobKilled.cpp:43-44 |
| R2 | Anti-Kill | Entities with Level >= 1000 cannot die; HP is restored | MobKilled.cpp:50-63 |
| R3 | Item Durability | Nyerds fairy (Equip[13]==769) loses sanctification on death and is cleared at 0 | MobKilled.cpp:65-104 |
| R4 | Event Teleport | In Castle state at (1046,1690), dead player teleports instead of dying | MobKilled.cpp:105-109 |
| R5 | Pet/Mount Growth | Clan-4 summon kills grant mount growth XP (mount index 2330-2390) | MobKilled.cpp:111-169 |
| R6 | Kill Confirmation | Broadcast MSG_CNFMobKill and set target HP to 0 | MobKilled.cpp:173-183 |
| R7 | Leader/Summon Resolution | Kill credit redirects to party leader / summoner | MobKilled.cpp:185-205 |
| R8 | EXP Distribution | Per-party-member EXP award with many level/class/zone/event modifiers | MobKilled.cpp:207-626 |
| R9 | NPC Death Speech | Random death message from generator DieAction | MobKilled.cpp:628-632 |
| R10 | Mob Persistence | Clan 4/7/8 targets (players) not deleted; others deleted | MobKilled.cpp:634-649 |
| R11 | Kefra Boss | Guild fame, KefraLive state, loot sweep, notices on Kefra kill | MobKilled.cpp:650-712 |
| R12 | Castle Gate | Opening castle gate item on gate-mob death during Castle state | MobKilled.cpp:714-755 |
| R13 | Water Temple | Room-clear progression + parchment rewards in Water M/N/A dungeons | MobKilled.cpp:762-978 |
| R14 | Special Mob Hooks | ORC_GUERREIRO recall, TORRE_NOATUM, REI_HARABARD/GLANTUAR kingdom clear | MobKilled.cpp:983-999 |
| R15 | Basic Mob Loot | Tutorial/basic mob loot tables (GenerateID 0-7, 3) | MobKilled.cpp:1001-1073 |
| R16 | Nightmare Respawn | Nightmare M/N/A zones regenerate mobs | MobKilled.cpp:1076-1084 |
| R17 | Secret Rooms (Carta) | Carta N/M/A room-clear teleports, timers, CartaSala progression | MobKilled.cpp:1086-1351 |
| R18 | Rune Quests | Rune quest LV0-LV6 boss kills award runes + progress state | MobKilled.cpp:1354-1694 |
| R19 | RvR Tower | Kingdom-war tower kill clears area and sets RvR bonus/state | MobKilled.cpp:1697-1724 |
| R20 | Coin Drop | Gold drop with level-based chance and 2-billion cap | MobKilled.cpp:1728-1757 |
| R21 | Event Item Drop | Server-event item drop with notice and index tracking | MobKilled.cpp:1760-1789 |
| R22 | Loot/Drop Table | Per-carry-slot drop rates modulated by level, bonus, special, flags | MobKilled.cpp:1791-1946 |
| R23 | Watching Town Quest | TerraMistica quest flag completion on specific mob kills | MobKilled.cpp:1948-1955 |
| R24 | Delete & Respawn | DeleteMob + rune/Kefra respawn hooks + CWarTower::MobKilled | MobKilled.cpp:1990-2031 |
| R25 | PvP EXP Loss | Player-death EXP loss deltaexp by level band | MobKilled.cpp:2061-2125 |
| R26 | PvP PK Points | PK-point transfer, clan/war detection, guilty handling | MobKilled.cpp:2111-2158, 2222-2356 |
| R27 | Arena/Village Protection | No EXP loss in arena/village zones; guild-zone item drop | MobKilled.cpp:2163-2195 |
| R28 | PvP Item Drop | Conditional carry/equip item drops on PK (#ifdef PKDrop) | MobKilled.cpp:2305-2418 |
| R29 | RvR Death Handling | RvR player death respawn + point counters | MobKilled.cpp:2260-2275 |
| R30 | HP-Multiplier Item | Equip[13] (10x/20x HP item) sanctification loss on player death | MobKilled.cpp:2446-2466 |

---

### Detailed breakdown of the business rules

### Business Rule: R1 - Kill Request Validation

**Overview:**
The function rejects any kill request that references an out-of-range entity index or an unoccupied mob slot. This is the first line of defense against out-of-bounds access to the global `pMob[]` array.

**Detailed description:**
`MobKilled` is called from multiple, independent code paths (attack handlers, timers, admin commands). Before any mutation of global state occurs, the routine verifies that both the killer connection (`conn`) and the killed target (`target`) fall within the valid entity range `[1, MAX_MOB-1]` (i.e., `1 <= x < 25000`), and that the target slot is not empty (`pMob[target].Mode != USER_EMPTY`). Because `conn` can legitimately be a monster (values `>= MAX_USER`, i.e. `>= 1000`) for pet/summon kills, the guard is intentionally permissive of high indices on the killer side. The check uses `conn <= 0` and `target <= 0`, meaning index 0 (reserved) and negative indices are rejected. This guards the subsequent heavy use of `pMob[]` and `pUser[]` indexing.

**Rule workflow:**
1. Caller invokes `MobKilled(target, conn, PosX, PosY)`.
2. Validate `conn` and `target` are `> 0` and `< MAX_MOB` (25000).
3. Validate `pMob[target].Mode != USER_EMPTY`.
4. On any failure, return immediately with no side effects.
5. Otherwise, proceed to the main kill-resolution logic.

---

### Business Rule: R2 - Immortal Entity Protection (Level >= 1000)

**Overview:**
Entities whose current score level is `>= 1000` cannot be killed. Instead, their HP is restored to maximum and their appearance is re-broadcast, effectively making them immortal.

**Detailed description:**
The threshold of 1000 (`pMob[target].MOB.CurrentScore.Level >= 1000`) is well above the player `MAX_LEVEL` of 399, and above the mob-level band used in the monster branch. This marks special entities (likely world/NPC/event entities, e.g., the "Nyerds" or specific boss-like non-player mobs) that must never die. When such an entity would be killed, the function resets its current HP to `MaxHp`, optionally calls `SetReqHp(target)` when the target is a user slot (`target < MAX_USER`), rebuilds and multicasts its `MSG_CreateMob` via `GridMulticast`, and refreshes the killer's score via `SendScore(conn)`. This makes the entity appear to take damage but never actually die, preserving world integrity.

**Rule workflow:**
1. Check `pMob[target].MOB.CurrentScore.Level >= 1000`.
2. Restore `Hp = MaxHp` (and `SetReqHp` if target is a user).
3. Rebuild `MSG_CreateMob` for target and `GridMulticast` to nearby clients.
4. `SendScore(conn)` to refresh the killer's HUD.
5. Return without deleting or distributing rewards.

---

### Business Rule: R3 - Nyerds Fairy Sanctification Depletion

**Overview:**
If the target's equipment slot 13 holds item index 769 (the "Nyerds" fairy) and no world event (RvR/GTorre/Castle) is active, the fairy item's sanctification is decremented by one; when sanctification reaches zero the item is cleared from the slot.

**Detailed description:**
Equipment slot 13 is used as a special auxiliary item slot (in this branch). Item 769 is a consumable/summon fairy whose durability is expressed as a "sanctification" value (`cEffect == 43` in one of the three effect slots). Each death of the entity decrements this value. The rule only applies when none of `RvRState`, `GTorreState`, or `CastleState` is active, meaning in normal (non-event) gameplay the fairy is consumed on death. After the decrement, the item state is pushed to the client via `SendItem` (for user targets) and `SendEmotion(target, 14, 2)`; the target's equip is re-broadcast (`SendEquip`), and, like R2, the entity is revived (HP restored, `GetCreateMob` + `GridMulticast`, `SendScore`). This effectively turns death into a "revive at the cost of one fairy charge" mechanic.

**Rule workflow:**
1. Verify `pMob[target].MOB.Equip[13].sIndex == 769` and no event state (`RvRState==0 && GTorreState==0 && CastleState==0`).
2. Read sanctification via `BASE_GetItemSanc`.
3. If `sanc > 0`: decrement; locate the effect slot with `cEffect == 43` and write the new value.
4. If `sanc == 0`: clear the item (`BASE_ClearItem`).
5. Push item update to client (user target) and broadcast equip.
6. Restore target HP and broadcast `MSG_CreateMob`; refresh killer score; return.

---

### Business Rule: R4 - Castle State Death Teleport

**Overview:**
When `CastleState > 1` and the dying user target is at the specific grid cell (1046, 1690), the player is teleported to (1066, 1717) instead of being killed.

**Detailed description:**
During castle-siege state (a defensive tower/altar scenario), players standing on a specific "throne"/gate coordinate must not die; instead they are repositioned. This is a hardcoded spatial rule tied to the castle event map. It only applies to user targets (`target > 0 && target < MAX_USER`). The `DoTeleport(target, 1066, 1717)` call moves the player, and the function returns immediately, bypassing all death processing, EXP loss, and item drops. This prevents a player from being repeatedly killed at a fixed castle anchor and instead relocates them to a safe rally point.

**Rule workflow:**
1. Check `CastleState > 1`.
2. Check target coordinates `TargetX == 1046 && TargetY == 1690`.
3. Check target is a user (`target > 0 && target < MAX_USER`).
4. `DoTeleport(target, 1066, 1717)` and return.

---

### Business Rule: R5 - Pet / Mount Growth on Kill

**Overview:**
When a clan-4 summon (a player's pet) is the killer, the pet's kills feed growth experience to its summoner's equipped mount (equip slot 14, item indexes 2330-2390).

**Detailed description:**
This rule links pet kills to mount leveling. The killer `conn` must be a non-user (`conn >= MAX_USER`), belong to clan 4 (pets), wield a face item index between 315 and 345, and the target must also be a non-user (`target > MAX_USER`) from a different clan. The summoner is resolved from `pMob[conn].Summoner`. If the summoner is a valid playing user with a mount in slot 14 whose index is in `[2330, 2390)`, the mount accrues growth. The required growth (`exp`) depends on the exact mount index (2330=+25, 2331=+35, ... 2335=+75, default +100). Growth only occurs while the mount's current XP is below both the killed mob's level and 100. On each qualifying kill `Crescimento` increments; when it reaches the threshold, the mount's level (`XP`) increments, `Crescimento` resets to 1, a "Mount Level" client message is sent, and `MountProcess` recomputes the mount.

**Rule workflow:**
1. Verify killer is a clan-4 summon with face 315-345 and target is a non-clan-4 mob.
2. Resolve `summoner`; validate it is a playing user.
3. Verify mount in slot 14 is in index range [2330, 2390).
4. Compute required growth from mount index.
5. If mount XP < killed-mob level and XP < 100: increment `Crescimento`.
6. If below threshold, persist `Crescimento`; else raise XP, reset counter, notify, `MountProcess`.

---

### Business Rule: R6 - Kill Confirmation Broadcast

**Overview:**
The core kill event is published to all nearby clients via a `MSG_CNFMobKill` packet, and the target's current HP is set to zero.

**Detailed description:**
Regardless of whether the target is a monster or a player, the function builds a `MSG_CNFMobKill` message (`sm`) with `Type=_MSG_CNFMobKill`, `ID=ESCENE_FIELD`, `KilledMob=target`, and `Killer=conn`. It zeroes the packet, sets the fields, and sets `pMob[target].MOB.CurrentScore.Hp = 0` as the authoritative death state. This packet is the client-side death notification that triggers death animations/UI. It is later multicast to the surrounding grid in both the monster and player branches via `GridMulticast(pMob[target].TargetX, pMob[target].TargetY, (MSG_STANDARD*)&sm, 0)`.

**Rule workflow:**
1. Zero-fill `MSG_CNFMobKill sm`.
2. Set `Type`, `Size`, `ID=ESCENE_FIELD`, `KilledMob=target`, `Killer=conn`.
3. Set `pMob[target].MOB.CurrentScore.Hp = 0`.
4. Later broadcast via `GridMulticast` in the appropriate branch.

---

### Business Rule: R7 - Party Leader and Summoner Credit Resolution

**Overview:**
The "effective killer" used for rewards is the party leader (or the killer if ungrouped); for a clan-4 summon kill, credit is redirected to the summoner, provided the summoner is still a valid playing user.

**Detailed description:**
Before EXP distribution, the function resolves who actually receives credit. It reads `Leader = pMob[conn].Leader`; if `Leader == 0` (no party), the killer `conn` is used. If the killer is a clan-4 summon (`conn >= MAX_USER && pMob[conn].MOB.Clan == 4`), the function attempts to resolve the summoner from `pMob[conn].Summoner`. If the summoner is invalid (`<= 0` or `>= MAX_USER`) or not in `USER_PLAY` mode, the kill is treated as a "lone pet kill": the confirmation packet is multicast and (for non-user targets) the mob is deleted, but no player receives EXP or loot. If the summoner is valid, `conn` is reassigned to the summoner for subsequent reward processing.

**Rule workflow:**
1. `Leader = pMob[conn].Leader`; if 0, `Leader = conn`.
2. If killer is clan-4 summon: resolve `Summoner`.
3. If summoner invalid/offline: multicast kill, delete mob (if non-user target), return.
4. Else set `conn = Summoner` and continue with leader-based rewards.

---

### Business Rule: R8 - Party Experience Distribution (Monster Kill)

**Overview:**
For monster kills, EXP is awarded to every eligible party member in range, using a heavily modified formula affected by class, level band, zone, bonuses, and many global event flags. This is the largest and most duplicated block in the component.

**Detailed description:**
This block iterates over party members (`MAX_PARTY + 1` iterations). For each eligible member (valid index, HP > 0, and within a zone/range condition), it computes `isExp = GetExpApply(...)` from the killed mob's EXP. `GetExpApply` applies class-based quest gates (e.g., ARCH level 355/370 quests) and a level-ratio multiplier. Then the formula computes `exp = (UNK_1 + myLevel) * isExp / (UNK_1 + myLevel)` (a near-identity but documents two mysterious constants, `UNK_1=30`, `UNK_2=0`). For non-MORTAL/non-ARCH (advanced) classes, `myLevel` is offset by `MAX_LEVEL + 1` and then `exp` is divided by a band factor (10, 20, 40, 80, 160, or 320) based on `myLevel` thresholds (120/150/170/180/190). It then applies `exp = 6 * exp / 10`, an `ExpBonus` percentage (0 < bonus < 500), a 10% `RvRBonus` when the clan matches, a +25% `NewbieEventServer` bonus for low-level non-celestial players, `DOUBLEMODE` doubling, a 50% cut when `KefraLive == 0`, and a final +/- 15% based on `NewbieEventServer`. The daily-log EXP (`DayLog.Exp`) is reset when the year-day changes and accumulated. A "Hold" (reserved EXP / level-cap reservoir) is consumed first; if all `exp` is absorbed by `Hold`, the member receives nothing. The final EXP is added to `pMob[party].MOB.Exp`, and a `MSG_UpdateExpRanking` packet is sent to DBSrv for the ranking system.

**Important zone/range variants:** There are four nearly identical copies of this formula, each gated by a different coordinate condition: grid zone `(9,1)` (the `tx/128==9 && ty/128==1` block), zone `(8,2)`, zone `(10,2)`, and a generic range check within `HALFGRIDX/HALFGRIDY` of the killer. Only the generic-range copy retains the `if (exp > eMob) exp = eMob;` cap that is commented out in the other three — an inconsistency that lets the zone-specific blocks award EXP above the mob's base EXP.

**Rule workflow:**
1. Loop over party slots; resolve each member from `Leader`/`PartyList`.
2. For each member passing zone/range + alive checks, compute `isExp` via `GetExpApply`.
3. Compute base `exp`; apply advanced-class band divisions.
4. Apply `ExpBonus`, `RvRBonus`, `NewbieEventServer`, `DOUBLEMODE`, `KefraLive`, and +/-15% modifiers.
5. Reset daily-log EXP on year-day change; accumulate `DayLog.Exp`.
6. Subtract from `Hold` reservoir; skip if fully absorbed.
7. Add remaining EXP to member; send `MSG_UpdateExpRanking` to DBSrv.

---

### Business Rule: R9 - NPC Death Speech

**Overview:**
On a monster kill, a random death line from the generator's `DieAction` table is broadcast as a chat message, provided the generator has such a line and the mob is not a summoned leader.

**Detailed description:**
The killed mob's `GenerateIndex` is used to index into the NPC generator list (`mNPCGen.pList[GenerateIndex]`). If `GenerateIndex >= 0` and the `DieAction` row for a random index (`DieSay = rand() % 4`) has a non-empty first character, and `pMob[target].Leader == 0` (the mob is not a party leader), the death line is sent via `SendChat(target, ...)`. This gives flavor text to NPCs when they die, drawn from data-driven generator configuration.

**Rule workflow:**
1. `DieSay = rand() % 4`.
2. If `GenerateIndex` valid and `DieAction[DieSay][0]` non-zero and not a leader: `SendChat`.

---

### Business Rule: R10 - Mob Persistence by Clan

**Overview:**
After a monster kill, the target is deleted from the world unless it belongs to clans 4, 7, or 8 (pet, and the two kingdom/war clans), which are treated as persistent/player-controlled entities.

**Detailed description:**
Following the kill-confirmation multicast, the function checks `pMob[conn].MOB.Clan`. If the killer's clan is non-zero and not 4, 7, or 8, the target mob is removed with `DeleteMob(target, 1)`. Otherwise (including when the killer is a user, clan 4, 7, or 8), the mob is *not* deleted, and the code proceeds into the extensive loot/event handling block that ultimately also calls `DeleteMob(target, 1)` at line 1990 after processing. Clans 7 and 8 correspond to the two kingdom-war factions (red/blue), and clan 4 to pets — all of which are expected to survive in some scenarios.

**Rule workflow:**
1. Multicast kill confirmation.
2. If killer clan in {1,2,3,5,6,...} (non-zero and not 4/7/8): `DeleteMob(target, 1)`.
3. Else: proceed to event/loot handling; eventually `DeleteMob(target, 1)` at line 1990 (unless a player branch already handled it).

---

### Business Rule: R11 - Kefra Boss Kill Handling

**Overview:**
Killing the Kefra world boss (GenerateIndex `KEFRA_BOSS`) awards guild fame, sets the global `KefraLive` state, sweeps loot from all players in the boss area, and broadcasts a server notice.

**Detailed description:**
When `pMob[target].GenerateIndex == KEFRA_BOSS` (396) and the killer belongs to a guild, the function records the guild (`KefraLive = usGuild`), resolves its name via `BASE_GetGuildName`, increments the guild's fame by 100 (`GuildInfo[usGuild].Fame += 100`), sends a `MSG_GuildInfo` to DBSrv, broadcasts an "End of Khepra" notice naming the guild, and refreshes config (`DrawConfig(TRUE)`). If the killer is not in a guild, `KefraLive = 1` and a generic "UoW" notice is broadcast. Then a loot sweep iterates over the rectangular area `x in [2335,2394]`, `y in [3896,3954]`, and for each player mob found (`tmob >= MAX_USER`), a random item from the boss's `Carry[]` table (`itemrand = rand() % 60`) is placed into the killer's inventory via `PutItem`. A log line `"etc,kefra killed"` is recorded.

**Rule workflow:**
1. If `GenerateIndex == KEFRA_BOSS`:
2. If killer has guild: set `KefraLive` to guild id, +100 fame, notify, send `MSG_GuildInfo` to DBSrv, `DrawConfig`.
3. Else: `KefraLive = 1`, generic notice, `DrawConfig`.
4. Sweep boss-area grid cells; for each player, place a random boss-carry item.
5. Log the kill.

---

### Business Rule: R12 - Castle Gate Opening

**Overview:**
Killing a castle gate mob (equip slot 0 item index 220) during Castle state opens the gate: the gate item at the mob's grid cell is updated and its delay is cleared.

**Detailed description:**
When `pMob[target].MOB.Equip[0].sIndex == 220` and `CastleState` is active, the function treats the death as a gate-breach event. It validates the gate's coordinates are within the grid; otherwise it logs an error and returns. It looks up the item occupying the gate cell in `pItemGrid[][]`. If a valid item exists (`ItemID` in range and active), it calls `UpdateItem(ItemID, 1, &heigth)`; on success it builds a `MSG_UpdateItem` (with `ItemID + 10000` encoding) and multicasts it, then resets `pItem[ItemID].Delay = 0` so the gate becomes passable. If no item is found, it logs `"err,no castle gate to open"`.

**Rule workflow:**
1. If gate mob (Equip[0]==220) and `CastleState`:
2. Validate gate coordinates in grid; else log and return.
3. Resolve gate item from `pItemGrid`.
4. `UpdateItem`; if changed, multicast `MSG_UpdateItem` and clear item delay.
5. Log error if no item.

---

### Business Rule: R13 - Water Temple Room-Clear Progression

**Overview:**
Killing the last mob in a Water temple (Água M/N/A) room awards the party a parchment item and signals the room-cleared timer; there are per-element variants with distinct item ids and timer caps.

**Detailed description:**
The three Water dungeons are identified by `GenerateIndex` ranges: `WATER_M_INITIAL..+7` and `+8..+11` (M), `WATER_N_INITIAL..+7`/`+8..+11` (N), and `WATER_A_INITIAL..+7`/`+8..+11` (A). For each, when the generator's `CurrentNumMob` reaches exactly 1 (the last mob in a room), the room is marked cleared. The room index is `Sala = GenerateID - WATER_*_INITIAL`. A "clear count" (`WaterClear1[element][Sala]`) is capped (15 for rooms 0-7, 5 for room 9). The party leader is resolved and receives a parchment item via `PutItem` (index `778+Sala` for M, `3174+Sala` for N, `3183+Sala` for A). A `_MSG_StartTime` timer signal (`WaterClear1 * 2`) is sent to the leader and each `USER_PLAY` party member, starting the room-cleared countdown. The room-9 (`+8..+11`) variants do not award parchment but do trigger the timer.

**Rule workflow:**
1. Determine Water element and room index from `GenerateIndex`.
2. If `CurrentNumMob == 1`: cap/update `WaterClear1` count.
3. Resolve party leader.
4. If element/room awards parchment, `PutItem` it to the leader.
5. Send `_MSG_StartTime` timer to leader and all playing party members.

---

### Business Rule: R14 - Special Mob Kill Hooks

**Overview:**
Specific `GenerateIndex` values trigger unique world effects: ORC_GUERREIRO recall, tower state resets, and kingdom-boss clear flags with area notices.

**Detailed description:**
This block dispatches on `GenerateID`:
- `ORC_GUERREIRO` (3): the killer is recalled via `DoRecall(conn)`.
- `TORRE_NOATUM1..TORRE_NOATUM3` (23-25): the corresponding war-tower state `LiveTower[...]` is reset to 0.
- `REI_HARABARD` (8): sets `Kingdom1Clear = 1` and broadcasts an area notice (`_NN_King1_Killed`) over the kingdom-1 region.
- `REI_GLANTUAR` (9): sets `Kingdom2Clear = 1` and broadcasts `_NN_King2_Killed` over the kingdom-2 region.
These hooks couple the generic kill routine directly to world-event state machines.

**Rule workflow:**
1. Switch on `GenerateID`.
2. ORC_GUERREIRO -> `DoRecall(conn)`.
3. TORRE_NOATUM range -> clear `LiveTower[GenerateID - TORRE_NOATUM1]`.
4. REI_HARABARD -> `Kingdom1Clear=1` + area notice.
5. REI_GLANTUAR -> `Kingdom2Clear=1` + area notice.

---

### Business Rule: R15 - Basic / Tutorial Mob Loot

**Overview:**
Low-level starter mobs (GenerateID 0-7 and 3) drop specific starter items on a fixed random chance when killed by a player on valid terrain.

**Detailed description:**
For `GenerateID == 0 || 1 || 2`: a `rand() % 14` roll; on 0 the item 419 drops, on 1 the item 420 drops; if set, `SetItemBonus` is applied and `PutItem(conn, &item)` places it. For `GenerateID == 5 || 6 || 7`: roll `rand() % 14`; on 0 an item `421 + (rand() % 7)` drops, on 1 item 419 drops; additionally the drop only occurs on terrain with `pHeightGrid[AlvoY][AlvoX] > -40 && < 36`. For `GenerateID == 3` (ORC_GUERREIRO): roll `rand() % 7`; items 1106, 1256, 1418, or 1568 drop with `SetItemBonus(&item, 75, 1, 0)` on valid terrain. This is a data-driven starter-loot system hardcoded by generator id.

**Rule workflow:**
1. For GenerateID 0/1/2: roll `rand()%14`; drop 419/420 on result 0/1; `SetItemBonus` + `PutItem`.
2. For GenerateID 5/6/7: roll `rand()%14`; drop 421-427 or 419; terrain gate; `PutItem`.
3. For GenerateID 3: roll `rand()%7`; drop one of 1106/1256/1418/1568; `SetItemBonus(...,75,1,0)`; terrain gate; `PutItem`.

---

### Business Rule: R16 - Nightmare Zone Respawn

**Overview:**
Killing a mob in the Nightmare M/N/A zones regenerates a new mob of the same generator index in place.

**Detailed description:**
Three ranges map to the three nightmare element zones: `NIGHTMARE_M_INITIAL..NIGHTMARE_M_END`, `NIGHTMARE_N_INITIAL..NIGHTMARE_N_END`, and `NIGHTMARE_A_INITIAL..NIGHTMARE_A_END`. When the killed mob's `GenerateIndex` falls in one of these ranges, `GenerateMob(GenerateIndex, 0, 0)` is called, immediately respawning a replacement. This creates a self-sustaining grinding zone where mobs respawn at the same spawn configuration without requiring an external timer.

**Rule workflow:**
1. Check `GenerateIndex` against the three nightmare ranges.
2. On match, `GenerateMob(GenerateIndex, 0, 0)` to respawn.

---

### Business Rule: R17 - Secret Room (Carta) Progression

**Overview:**
Killing the last mob in a Secret Room (Carta) area (N/M/A) triggers area teleport of survivors, starts a countdown timer, and increments the global `CartaSala` progression counter.

**Detailed description:**
Each Secret Room variant has 4 salas (rooms). For salas 1-3 (defined by paired `SECRET_ROOM_<E>_SALA<n>_MOB_1..2` ranges), when the combined `CurrentNumMob` of the two mob generators reaches 1, the room is cleared: `CartaTime` is set to 60, all survivors in the rectangular room area are teleported to the corresponding `CartaPos[room]` target via `ClearAreaTeleport(...)`, `CartaSala++` is incremented, and a `_MSG_StartTime` packet (`CartaTime * 2`) is multicast across map region `(6,28)` via `MapaMulticast`. For sala 4 (four mob generators), clearing sets `CartaTime = 3` and `CartaSala = 4` (the final room). The same structure repeats for the M and A element variants. This drives the timed "secret room" treasure-hunt instance.

**Rule workflow:**
1. Determine element (N/M/A) and sala from `GenerateID`.
2. Sum `CurrentNumMob` of the sala's mob generators.
3. If total == 1: set `CartaTime` (60 or 3), `ClearAreaTeleport` survivors, `CartaSala++` (or set to 4 for final).
4. Multicast `_MSG_StartTime` timer on map region (6,28).

---

### Business Rule: R18 - Rune Quest Progression

**Overview:**
Killing Rune Quest bosses and mobs (LV0-LV6) awards runes, "next pista" progression items, spawns follow-up mobs/bosses, and tracks per-party kill counts.

**Detailed description:**
This is a multi-level quest chain driven by `GenerateID`:
- **LV0 Lich (RUNEQUEST_LV0_LICH1/2):** on kill, `rand() % 100`; if `< 20`, all remaining lich mobs are deleted, the party leader and members each receive a random `PistaRune[0]` rune, and a `NextPista` item (index 5134, effect 43 value 1) is placed; else all liches are deleted and four new liches (two of each type) are regenerated.
- **LV1 Towers (RUNEQUEST_LV1_TORRE1-3):** reset `Pista[1].Party[i].MobCount = 0` for the killed tower; killing LV1 mobs increments the matching party's `MobCount` based on which tower is active.
- **LV2 Boss (RUNEQUEST_LV2_MOB_BOSS):** leader + members receive `PistaRune[2]`; `NextPista` (value 3) placed; completion logged.
- **LV3 Sulrang (RUNEQUEST_LV3_MOB_SULRANG...):** when the first party kills all sulrangs (`MobCount == 0`), a random LV3 boss (`RUNEQUEST_LV3_MOB_BOSS_INITIAL + rand()%7`) is spawned.
- **LV4 (RUNEQUEST_LV4_MOB...):** per-party `MobCount` decrementing; when a party reaches 0 (having killed the other two parties' quota), the leader + members teleport to (3351/3352, 1334) and the LV4 boss spawns; the LV4 boss awards `PistaRune[4]` and `NextPista` (value 5).
- **LV5 Boss (RUNEQUEST_LV5_MOB_BOSS):** sets `Pista[5].Party[0].MobCount = 1`, awards `PistaRune[5]`, `NextPista` (value 6).
- **LV6 (RUNEQUEST_LV6_MOB...):** decrements `Pista[6]` count and spawns the LV6 boss when a threshold is hit; the boss awards `PistaRune[6]` and logs completion.

**Rule workflow:**
1. Dispatch on `GenerateID` to the matching rune-quest level.
2. Award `PistaRune` runes + `NextPista` progression item to leader and members.
3. Update per-party `MobCount` state.
4. Delete/regenerate mobs or spawn bosses per the level's rules.
5. Log quest completion for the leader.

---

### Business Rule: R19 - Kingdom War (RvR) Tower Kill

**Overview:**
Killing a Kingdom-war tower (RVRTORRE_1/2) clears the war area, deletes remaining towers, sets the winning clan's `RvRBonus`, and resets `RvRState`.

**Detailed description:**
When `GenerateID == RVRTORRE_1 || RVRTORRE_2`, the function clears the entire war region (`ClearArea(1020,1916,1286,2178)`), deletes all surviving tower mobs, and — if the killer belongs to a clan — announces the winning kingdom via `_NN_KINGDOMWAR_DROP_` (red clan 8 / blue clan 7), sets `RvRBonus = pMob[conn].MOB.Clan`, and logs the winner. Finally `RvRState = 0` ends the war event. This couples the kill routine to the guild-kingdom-war endgame.

**Rule workflow:**
1. On RVRTORRE kill: `ClearArea` the war region.
2. Delete remaining towers.
3. If killer has a clan: announce winner, set `RvRBonus`.
4. Log winner, reset `RvRState = 0`.

---

### Business Rule: R20 - Coin (Gold) Drop

**Overview:**
Killed mobs drop gold based on their `Coin` value and a level-dependent chance; the award is capped at 2 billion total and 2000 per drop.

**Detailed description:**
The function computes a `UNKGOLD` chance multiplier from the mob's `BaseScore.Level` bands (level <10: 2, <20: 4, <30: 6, <50: 9, else 18) and rolls `UNKGOLD = rand() % (UNKGOLD+1)`. If the mob carries coins (`MobCoin`) and the roll is 0, the drop is computed as `MobCoin = 4 * (rand() % (((MobCoin+1)/4)+1) + (MobCoin+1)/4 + MobCoin)` and capped at 2000. If adding this to the killer's current coin would exceed `2000000000`, the killer is told `_NN_Cant_get_more_than_2G`; otherwise the coins are added and `SendEtc(conn)` refreshes the coin display.

**Rule workflow:**
1. `UNKGOLD` from level band; roll `rand() % (UNKGOLD+1)`.
2. If `MobCoin` and roll == 0: compute capped drop (<= 2000).
3. If coin sum > 2B: send over-cap message.
4. Else add to killer coin and `SendEtc`.

---

### Business Rule: R21 - Server Event Item Drop

**Overview:**
A configured server event drops a special item with a probability `evRate`; the item carries event-index effects and may broadcast a notice.

**Detailed description:**
When the event system is active (`evOn`), an item index and rate are configured (`evStartIndex`, `evEndIndex`, `evItem`, `evRate`, `evCurrentIndex`). If the current event index is within range and `rand() % evRate == 0`, an item is created with `sIndex = evItem`. If `evIndex` is set, the item gains event effects: effect 62/63 encode the event index (`evCurrentIndex / 256` and `evCurrentIndex`), and effect 59 is randomized; a notice `_SSD_S_get_S_D` names the item and index. Otherwise a generic `_SSD_S_get_S` message is used. If `evNotice` is set, the message is broadcast via `SendNotice`. `evCurrentIndex++` advances the event counter, `SetItemBonus` is applied with the killed mob's level, the item is placed, and `DrawConfig(1)` updates the client.

**Rule workflow:**
1. Check `evOn` and event-index in range; roll `rand() % evRate == 0`.
2. Build item with `sIndex=evItem`; add event effects if `evIndex`.
3. Compose and (if `evNotice`) broadcast notice.
4. `evCurrentIndex++`; `SetItemBonus`; `PutItem`; `DrawConfig(1)`.

---

### Business Rule: R22 - Per-Slot Loot / Drop Table

**Overview:**
Each of the mob's `Carry[]` slots is rolled against a drop rate modified by target level, killer `DropBonus`, special-stat reductions, and test/local server flags; qualifying items are awarded.

**Detailed description:**
For each of `MAX_CARRY` slots (i in 0..63), the base `droprate = g_pDropRate[i]` and a bonus `dropbonus = g_pDropBonus[i] + pMob[conn].DropBonus` are combined; if `dropbonus != 100` the rate is scaled as `dropbonus = 10000/(dropbonus+1); droprate = dropbonus * droprate / 100`. For slots 0-59 (i/8 in {0,1,2}), a target-level band factor applies (4-99% depending on `target_level < 10/20/30/40/60/else`). For slots >= 60, a different high-level band factor applies (90/60/50/43/38/50% for levels <170/200/230/255/320/else). `TESTSERVER` halves the rate; `LOCALSERVER` divides by 100. If `pMob[conn].MOB.Rsv & 4`, a special-stat reduction (`special2 = 100 - (Special[2]/10 + 10)`) scales the rate down. Slots 8/9/10 are forced to rate 4, slot 11 to rate 1. Rates are clamped to `[0, 32000]`; a `rand() % droprate` roll must equal 0 (or the slot is 11) to drop. The item is validated (`sIndex` in range and != 454), and if its `ReqLvl < 140` or the roll parity is odd, `SetItemBonus` is applied, `CCastleZakum::KeyDrop` decides placement, and the item is logged with the player's account/IP. A `_MSG_DBItemDayLog` packet is sent to DBSrv. On `LOCALSERVER`, the drop is announced via `SendSay`. Dropping slots 8/9/10 jumps the loop to slot 11 (a bounded special-drop sequence).

**Rule workflow:**
1. For each carry slot, compute `droprate` from tables + `DropBonus`.
2. Apply target-level band, special-stat, and server-flag modifiers.
3. Clamp rate; force rates for slots 8-11.
4. Roll `rand() % droprate`; drop when 0 (or slot 11).
5. Validate item; `SetItemBonus`; `KeyDrop`; `PutItem`; log; send day-log to DBSrv.

---

### Business Rule: R23 - Watching Town Quest Completion

**Overview:**
Killing certain mobs (equip slot 0 item 239 or 241) has a 1-in-20 chance to complete the "TerraMistica" watching-town quest for the killer.

**Detailed description:**
When the killer is a user and the killed mob's equip slot 0 holds item 239 or 241, and a `rand() % 20` roll succeeds, the killer's mortal quest flag `QuestInfo.Mortal.TerraMistica` is checked. If it is exactly 1 (quest accepted, not yet complete), the flag is advanced to 2 (complete) and the client is told `_NN_Watching_Town_Success`. This ties a specific mob-kill to single-quest-progression completion in the mortal class questline.

**Rule workflow:**
1. If killer is user and target Equip[0] in {239,241} and `!(rand()%20)`:
2. If `TerraMistica == 1`: notify success and set to 2.

---

### Business Rule: R24 - Mob Deletion, Respawn and War-Tower Hook

**Overview:**
After loot processing the target mob is deleted; specific rune/Kefra generators respawn, and the war-tower kill hook is invoked.

**Detailed description:**
The mob is removed with `DeleteMob(target, 1)` (line 1990). Depending on `GenerateID`, follow-up mobs are regenerated: LV1 (`RUNEQUEST_LV1_MOB_INITIAL..END`) and LV2 (`RUNEQUEST_LV2_MOB_INITIAL..END`) rune mobs respawn via `GenerateMob`; LV3 boss mobs regenerate a random LV3 boss and increment the appropriate party's `MobCount`; and Kefra mobs (`KEFRA_MOB_INITIAL..END`) regenerate when `KefraLive == 0`. Finally `CWarTower::MobKilled(target, conn, PosX, PosY)` is invoked to handle war-tower-specific consequences. On invalid terrain the mob is simply deleted without loot (the `else DeleteMob(target,1)` at line 2030).

**Rule workflow:**
1. `DeleteMob(target, 1)`.
2. If rune LV1/LV2/LV3 or Kefra generator: `GenerateMob` respawn + count updates.
3. `CWarTower::MobKilled(...)`.
4. On invalid terrain, delete without reward.

---

### Business Rule: R25 - PvP Experience Loss

**Overview:**
When a player (target level <= `MAX_LEVEL`) dies, they lose experience (`deltaexp`) computed from a level-band fraction of the difference between the next-level EXP tables.

**Detailed description:**
For player deaths, `deltaexp` is derived from the target's level `tlevel`: `curexp` and `nextexp` are read from `g_pNextLevel[tlevel]`/`g_pNextLevel_2[...]` depending on class (MORTAL/ARCH use the mortal table). `alphaexp = nextexp - curexp` and an initial `deltaexp = alphaexp / 20`. Progressive level bands reduce the divisor: `>=30: /22`, `>=40: /25`, `>=50: /30`, `>=60: /35`, `>=70: /40`, `>=80: /45`, `>=90: /50`, `>=100: /55`, `>=150: /70`, `>=200: /85`, `>=250: /100`. `deltaexp` is clamped to `[0, 150000]`. It is then scaled by the killed player's PK points: `deltaexp *= 3` if `10 < killed_pkpoint <= 25`, else `deltaexp *= 5`. For user killers, `deltaexp /= 6` (or `/3` on `TESTSERVER`).

**Rule workflow:**
1. Compute `curexp`/`nextexp` from class-appropriate level tables.
2. Set `deltaexp` from a level-band divisor (20..100).
3. Clamp `deltaexp` to [0, 150000].
4. Scale by killed PK points (x3 or x5).
5. For user killers divide by 6 (or 3 on test server).

---

### Business Rule: R26 - PvP PK Points, Clan and War Detection

**Overview:**
Player deaths adjust PK points of both killer and victim based on clan affiliation, war state, guilt, and equipment; same-clan kills and RvR deaths are handled specially.

**Detailed description:**
The function captures both players' PK points (`GetPKPoint`), kill counters (`GetCurKill`/`GetTotKill`), and guilt (`GetGuilty`). `SameClan` is set when clans are the two opposing war factions (7 vs 8). `AtWar` is true when guilds are at war (`g_pGuildWar` mutual), or during Castle (coordinates x==8,y==13), RvR (`RvRState != 0`), or GTorre (`GTorreState != 0`) events. For same-clan kills, the killer's kill counters increment and, if not at war/guilty/newbie, the killer loses PK points (`Lostpk = 3 * killed_pkpoint / -20`, clamped to [-9,0]) and is notified. In the non-same-clan branch, a different PK formula applies (`LostPk = 3 * killed_pkpoint / -25`, x3 if either side wears item 548/549 in equip slot 15). For any player death, if `killed_pkpoint < 75` and not at war and killer is a user, the victim's PK point increments by 1 (up to 75 cap) with a `_DD_PKPointPlus` message. `SetCurKill(target, 0)` resets the victim's current-kill streak on same-clan kills.

**Rule workflow:**
1. Read killer/victim PK, kills, guilt, clans, guilds.
2. Determine `SameClan` (7 vs 8) and `AtWar` (guild war / castle / RvR / GTorre).
3. Same-clan: increment kill counters; apply PK loss formula /-20.
4. Non-same-clan: apply PK loss formula /-25 (x3 with 548/549).
5. If victim PK < 75 and not at war: increment victim PK by 1.
6. Reset victim current-kill on same-clan kill.

---

### Business Rule: R27 - Arena / Village EXP-Loss Protection

**Overview:**
In arena and village zones the victim is told there is no EXP loss and the death penalty is skipped; in guild zones a specific item (431) is dropped instead.

**Detailed description:**
The target's grid zone is classified via `BASE_GetArena` and `BASE_GetVillage`, plus a `ZoneUnk` flag for the special coordinate (x==1, y==31). If the zone is not a "free PvP arena" (i.e., `arena != 5 || village != 5 || ZoneUnk`), the victim receives `_NN_In_Arena_No_Exp_Loss`, and if `arena == 5 && village != 5` a diagnostic line is appended to `TesteDie.txt`. If the zone is a guild zone (`arena in [0, MAX_GUILDZONE)`), a fixed item 431 is created at the death location via `CreateItem` (a guild-zone death drop). In the protected case, EXP loss and item-drop penalties are skipped.

**Rule workflow:**
1. Classify zone via `BASE_GetArena`/`BASE_GetVillage`/`ZoneUnk`.
2. If not free-PvP: notify "no exp loss"; optionally log to `TesteDie.txt`.
3. If guild zone: `CreateItem` item 431 at death location.
4. Skip EXP loss / drops in protected zones.

---

### Business Rule: R28 - PvP Item Drop on Death

**Overview:**
Under the `PKDrop` compile flag, a player killed with low PK points may lose carry and equip items, which are spawned on the ground at the death location.

**Detailed description:**
This block is gated by `#ifdef PKDrop`. If `killed_pkpoint <= 60`, the victim may lose carry items: `killed_loseitem = (75 - killed_pkpoint) / 10`; the loop iterates carry slots, skipping on `rand() % 5`, and each eligible item (valid index, not 446, not the protected 508/509/522/526-531 set) is spawned via `CreateItem` at the victim's coordinates, logged if `BASE_NeedLog`, zeroed from inventory, and removed client-side via `MSG_CNFDropItem`; the count is bounded by `killed_loseitem`. If `killed_pkpoint <= 35`, a second pass can drop a single equip item: `killed_loseitem = (killed_pkpoint + 10) / 10` (min 1), and a random equip slot (1-14, skipping 12) is selected; if it holds a valid droppable item, it is spawned, logged, removed from the slot, and confirmed to the client.

**Rule workflow:**
1. If `PKDrop` and `killed_pkpoint <= 60`: loop carry slots; roll `rand()%5`; spawn eligible items via `CreateItem`; log; clear; confirm drop; bound by `killed_loseitem`.
2. If `killed_pkpoint <= 35`: select a random equip slot (1-14, not 12); spawn/log/clear an eligible equip item; confirm drop.

---

### Business Rule: R29 - RvR Player Death Handling

**Overview:**
During Kingdom-war (RvR), a same-clan player death in the war region respawns the victim with 2 HP at the faction's rally point and increments the opposing faction's score counter.

**Detailed description:**
When a same-clan kill occurs within the RvR battlefield region (`TargetX in [1017,1290]`, `TargetY in [1911,2183]`), the victim is not fully killed: their HP is set to 2 and they are teleported to their faction's respawn rally — clan 7 (red) to `(1061 + rand()%2, 2129 + rand()%2)` and clan 8 (blue) to `(1237 + rand()%2, 1966 + rand()%2)`. Each such death increments the opposing faction's counter (`RvRRedPoint++` for clan 7 victims, `RvRBluePoint++` for clan 8 victims), tracking war score.

**Rule workflow:**
1. If same-clan kill in RvR region:
2. Clan 7 victim: HP=2, teleport to red rally, `RvRRedPoint++`.
3. Clan 8 victim: HP=2, teleport to blue rally, `RvRBluePoint++`.

---

### Business Rule: R30 - HP-Multiplier Item Sanctification Loss on Player Death

**Overview:**
A player killed while wearing the 10x/20x HP item (equip slot 13, item 753 or 1726) loses one sanctification charge, and the item is cleared when it reaches zero.

**Detailed description:**
On player death, the function checks `pMob[target].MOB.Equip[13].sIndex == 753 || 1726` (the "10X HP" and "20X HP" auxiliary items). It reads the item's sanctification via `BASE_GetItemSanc`; if greater than 0, it decrements and writes the new value into the effect slot with `cEffect == 43`; otherwise it clears the item with `BASE_ClearItem`. The updated equip slot is pushed to the victim's client via `SendItem(target, ITEM_PLACE_EQUIP, 13, ...)`. This makes the powerful HP-boost item a limited-use consumable that degrades with each death.

**Rule workflow:**
1. On player death, check Equip[13] for item 753 or 1726.
2. Read sanctification; if >0 decrement and write to the effect-43 slot.
3. If 0, `BASE_ClearItem`.
4. `SendItem` to update the victim's equip display.

---

## 4. Component Structure

```
legacy/Code/TMSrv/
├── MobKilled.cpp              # Sole implementation of the MobKilled() kill-resolution routine (2,469 lines)
├── Server.h                   # Declares MobKilled() (line 142); hosts global state (pMob, pUser, event flags)
├── Server.cpp                 # Provides DeleteMob, DoTeleport, GenerateMob, PutItem, SetItemBonus, ClearArea*, DoRecall, SetReqHp, Log, DrawConfig, UpdateItem
├── GetFunc.cpp / GetFunc.h    # GetExpApply, GetCreateMob, GetCurKill/GetTotKill/GetPKPoint/GetGuilty/Set* PK and kill helpers
├── SendFunc.cpp / SendFunc.h  # GridMulticast, SendScore, SendItem, SendEquip, SendEtc, SendNotice, SendChat, SendClientMessage, SendClientSignalParm, MapaMulticast, SendSay, SendEmotion
├── CCastleZakum.cpp / .h      # CCastleZakum::MobKilled + KeyDrop integration
├── CWarTower.cpp / .h         # CWarTower::MobKilled integration
├── CNPCGene.cpp / .h          # mNPCGen generator tables (DieAction), GenerateMob, spawn regions
├── CUser.h / CUser.cpp        # USER_EMPTY/USER_PLAY modes, per-player session state (pUser[])
├── CMob.h / CMob.cpp          # MOB_EMPTY, per-entity in-world state (pMob[])
├── ProcessSecMinTimer.cpp     # Periodic killer of MobKilled (line 1940)
├── _MSG_Attack.cpp            # Skill-attack killer of MobKilled (line 1743)
├── imple.cpp                  # Admin commands "killkefra" / "npko" invoking MobKilled (lines 1661, 1718)
└── (shared) ../Basedef.h      # Structs (STRUCT_MOB, STRUCT_SCORE, STRUCT_ITEM), constants, MSG_CNFMobKill
```

The component is not a class; it is one free function plus its supporting
module-level globals. Its "structure" is the internal region-based flow described
in the Data Flow section, heavily organized with `#pragma region` blocks (a
legacy Visual Studio code-folding convention).

---

## 5. Dependency Analysis

### Internal Dependencies (functions called by `MobKilled`)

```
MobKilled
├─► GetFunc:  GetExpApply, GetCreateMob, GetCurKill, GetTotKill, GetPKPoint, GetGuilty, SetCurKill, SetTotKill, SetPKPoint
├─► SendFunc: GridMulticast, SendScore, SendItem, SendEquip, SendEtc, SendNotice, SendNoticeArea, SendChat,
│             SendClientMessage, SendClientSignalParm, MapaMulticast, SendSay, SendEmotion
├─► Server.cpp: SetReqHp, DeleteMob, DoTeleport, DoRecall, GenerateMob, MountProcess, PutItem, CreateItem,
│               UpdateItem, ClearArea, ClearAreaTeleport, DrawConfig, SetItemBonus, Log
├─► Basedef:  BASE_GetItemSanc, BASE_ClearItem, BASE_GetArena, BASE_GetVillage, BASE_GetGuildName, BASE_NeedLog, BASE_GetItemCode
├─► CCastleZakum::MobKilled, CCastleZakum::KeyDrop
├─► CWarTower::MobKilled
└─► DBServerSocket.SendOneMessage   (MSG_UpdateExpRanking, MSG_DBItemDayLog, MSG_GuildInfo)
```

### External Dependencies

| Dependency | Type | Purpose | Location |
|------------|------|---------|----------|
| DBSrv (DBServerSocket) | Inter-process (TCP) | Ranking updates, item day-log, guild info persistence | MobKilled.cpp:341, 622, 676, 1920 |
| Filesystem (Log) | Local file | Audit logs (drops, PK, kills) | MobKilled.cpp:711, 1907, 2439, 2443 |
| Filesystem (TesteDie.txt) | Local file | Arena death diagnostic logging | MobKilled.cpp:2178-2182 |
| `pUser[].cSock` (client socket) | Inter-process (TCP) | Drop-item confirmation to victim | MobKilled.cpp:2356, 2410 |
| Winsock / network layer | Platform | Transport for above | (via CPSock) |

Note: `MobKilled` depends on a very large set of **global variables** declared in
`Server.h` (`pMob[]`, `pUser[]`, `pItem[]`, `pMobGrid[][]`, `pHeightGrid[][]`,
`mNPCGen`, `GuildInfo[]`, `Pista[][]`, `CartaPos[][]`, `CartaTime`, `CartaSala`,
`WaterClear1[][]`, `KefraLive`, `RvRState`, `RvRBonus`, `GTorreState`,
`CastleState`, `LiveTower[]`, `Kingdom1Clear`, `Kingdom2Clear`, `DOUBLEMODE`,
`NewbieEventServer`, `TESTSERVER`, `LOCALSERVER`, `DEADPOINT`, `FREEEXP`,
`MAX_LEVEL`, `MAX_PARTY`, etc.). This implicit global-state coupling is the
dominant dependency pattern.

---

## 6. Afferent and Efferent Coupling

Coupling is assessed at the function level. Afferent coupling = distinct
callers/call-sites that invoke `MobKilled`. Efferent coupling = distinct
helpers/entities `MobKilled` depends on.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|----------|
| MobKilled (function) | 5 call-sites across 4 files | ~30 distinct helper functions/entities + large global-state set | High |
| _MSG_Attack.cpp | 1 (to MobKilled) | High (combat + skills) | Medium |
| Server.cpp (attack) | 1 (to MobKilled) | Very high | High |
| ProcessSecMinTimer.cpp | 1 (to MobKilled) | High | Medium |
| imple.cpp (admin kill) | 2 (killkefra, npko) | Medium | Low |

Afferent call sites:
- `legacy/Code/TMSrv/Server.cpp:9203`
- `legacy/Code/TMSrv/ProcessSecMinTimer.cpp:1940`
- `legacy/Code/TMSrv/_MSG_Attack.cpp:1743`
- `legacy/Code/TMSrv/imple.cpp:1661` (command `killkefra`)
- `legacy/Code/TMSrv/imple.cpp:1718` (command `npko`)

The high efferent coupling (~30 distinct functions plus dozens of global
variables) makes `MobKilled` a classic "god function" / change amplifier: any
change to a reward, drop, quest, or PvP rule has a blast radius that touches
nearly every gameplay subsystem.

---

## 7. Endpoints

`MobKilled` does not expose any network endpoint. It is an internal server-side
function invoked by the message/event layer, and it is not registered in the
client-packet dispatch table (`ProcessClientMessage`). It consumes internal
triggers (attack/timer/admin) and produces game packets and DBSrv messages, but
none of those are endpoints *of* this component. Consequently, this section is
omitted.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| DBSrv (DBServerSocket) | Inter-process | Persist ranking (MSG_UpdateExpRanking), item day-log (MSG_DBItemDayLog), guild info (MSG_GuildInfo) | Custom binary socket | Packed C structs (MSG_*) | Fire-and-forget `SendOneMessage`; no acknowledgement/retry |
| Victim client socket | Inter-process | Confirm drop items removed (MSG_CNFDropItem) | Custom binary socket | Packed C structs | Direct `SendOneMessage`; no error path |
| Filesystem (Log) | Local file | Audit drops/PK/kills | Text file | Formatted strings | `Log()` helper; write failures not handled in this routine |
| Filesystem (TesteDie.txt) | Local file | Arena death diagnostics | Text file | `fprintf` lines | `fopen`/`fclose` without error checks |
| CCastleZakum / CWarTower | In-process (static) | Castle-quest and war-tower kill side-effects | C++ static method calls | In-memory | Guarded by internal state checks |
| Global event state | In-process | RvR/GTorre/Castle/Kefra/Newbie/DoubleMode flags | Shared globals | Primitive ints | Read without synchronization (single-threaded model) |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| God Function / Procedural Monolith | Single `MobKilled()` handling all kill types | MobKilled.cpp:41 | Centralized kill resolution (an anti-pattern in terms of cohesion) |
| Command/Trigger hooking | `GenerateIndex` range-dispatch to world-event logic | MobKilled.cpp:650-2027 | Couples kill routine to every event/quest subsystem |
| Data-driven configuration | `mNPCGen.pList[].DieAction`, `g_pDropRate`, `g_pDropBonus`, `PistaRune`, `CartaPos`, `WaterClear1` | MobKilled.cpp | Externalizes drop/quest parameters into data tables |
| Static utility integration | `CCastleZakum::MobKilled`, `CWarTower::MobKilled` | MobKilled.cpp:980, 2027 | Delegates event-specific kill logic to dedicated classes |
| Global shared-state | `pMob[]`, `pUser[]`, grids, flags via `Server.h` | throughout | Single-threaded cooperative access to world state |
| Feature flags (compile-time) | `#ifdef PKDrop`, `TESTSERVER`, `LOCALSERVER`, `DOUBLEMODE` | MobKilled.cpp:1854, 2305 | Compile/run-time toggles for testing and features |

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | EXP distribution (R8) | Four near-identical EXP formulas; zone-specific copies omit the `exp > eMob` cap present in the generic copy | Inconsistent EXP awards by zone; maintenance drift; bug-prone duplication |
| High | Whole component | No automated tests anywhere in repo | Regression risk on a critical, ~2.5k-line kill routine |
| High | Loot/RNG | `rand()` used without seeding context and with many `%` operations | Predictable/non-uniform drops; no RNG isolation for auditability |
| High | Global state | Reliance on dozens of mutable `extern` globals with no synchronization | Coupling, fragile ordering, difficult to reason about concurrency |
| Medium | Data integrity | `pMob[conn].MOB.Exp += exp` and coin/EXP mutation are not transactional; crash mid-function loses rewards | Lost EXP/drops on crash; no rollback |
| Medium | Signature contract | Declaration `Server.h:142` names params `(conn, target)` but definition `MobKilled.cpp:41` names `(target, conn)` | Misleading documentation; maintenance/reading errors (both `int`, so it compiles) |
| Medium | Drop logging | `Log()` write failures and `TesteDie.txt` `fopen` errors unchecked | Silent data loss in audit trail |
| Medium | Event coupling | Hardcoded map coordinates (Kefra area, RvR region, castle gates) throughout | Fragile when maps/coordinates change |
| Low | Naming | Opaque identifiers `UNK_1=30`, `UNK_2=0`, `UNKGOLD`, `UNKGOLD`, `UNK_*` | Reduced readability; the code itself contains a TODO to rename `UNK_s` |
| Low | Dead code | Large commented-out block (HP-leech on kill) at lines 1958-1988; commented `CEncampment::MobKilled` at line 981 | Clutter; possible confusion about intended behavior |

---

## 11. Test Coverage Analysis

A repository-wide search for test files (`*test*`, `*spec*`, `*unittest*`,
`test_*`) across `legacy/` and the whole workspace returned **no test files**.
The Visual Studio solution contains only the `TMSrv` and `DBSrv` game-server
projects; there is no test project, no test harness, and no unit-test framework
present. The only executable code in `MobKilled.cpp` is the production routine.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| MobKilled | 0 | 0 | 0% | No tests exist |

**Risk note:** Given the size (2,469 lines) and complexity (30+ business rules,
heavy RNG, global state, event coupling) of `MobKilled`, the complete absence of
automated tests is the single largest quality risk for this component. Any
change to EXP, drops, quests, or PvP behavior can only be validated manually.
The only "verification" present is the runtime `Log()` audit trail and the
`TESTSERVER`/`LOCALSERVER` compile-time branches that scale drop/EXP rates for
test environments.

---

## Limitations

- Coupling metrics are function-level estimates derived from call-site and
  symbol references, not an exhaustive static tool measurement.
- Business rules are extracted from code reading and are documented with the
  literal identifiers used in the source; a few constants (`UNK_1`, `UNK_2`,
  `UNKGOLD`) have unknown semantics and are flagged as such in the code.
- Test coverage is reported as absent because no test files exist; if tests are
  maintained outside the analyzed `legacy/` scope, they were not found by the
  repository-wide search.
- The analysis is read-only; no project files were modified.
