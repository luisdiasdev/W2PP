# Component Deep Analysis Report: ProcessSecMinTimer

## 1. Executive Summary

**Component:** ProcessSecMinTimer
**File:** `legacy/Code/TMSrv/ProcessSecMinTimer.cpp` (2314 lines)
**Project:** W2PP legacy C/C++ server codebase (Visual C++ / Win32)

`ProcessSecMinTimer` is the **heartbeat / periodic world-update engine** of the TMSrv (Theater/Map Server) game server. It contains two top-level entry functions, `ProcessSecTimer()` (`legacy/Code/TMSrv/ProcessSecMinTimer.cpp:40`) and `ProcessMinTimer()` (`legacy/Code/TMSrv/ProcessSecMinTimer.cpp:2019`), that are invoked by the Windows message loop through `WM_TIMER` handlers in `legacy/Code/TMSrv/Server.cpp:3723-3729`:

- `TIMER_SEC` fires every **500 ms** and calls `ProcessSecTimer()` (Server.cpp:3631, 3726).
- `TIMER_MIN` fires every **12 s** and calls `ProcessMinTimer()` (Server.cpp:3632, 3728).

Despite the historical names, neither timer actually runs on a real seconds or minutes cadence; they are the high-frequency (0.5 s) and low-frequency (12 s) world-update loops. The component is the central orchestrator that drives nearly all non-player-initiated server behavior.

**Key findings:**

1. **Monolithic god-function orchestration.** `ProcessSecTimer()` (1978 lines) is a single extremely long function containing dozens of independently scheduled subsystems: server shutdown sequencing, user auto-save, socket flushing, billing-server reconnection, per-user stat regeneration, mob AI (idle/peace/combat), premium-item expiry, quest-area clearing, dungeon/nightmare/runetrack event scheduling, weather, ranking, castle war, battle royale, and merchant-mob restocking. `ProcessMinTimer()` (296 lines) handles log rotation, newbie-server rotation, castle-server assignment, guild-impostor NPC persistence, item-lock state transitions, NPC respawn scheduling, and weather.

2. **Massive global-state coupling.** The component operates almost exclusively through global arrays (`pUser[MAX_USER]`, `pMob[MAX_MOB]`, `pMobGrid`, `pItemGrid`, `Pista[7]`, `mNPCGen`, `pMobMerc[MAX_MOB_MERC]`) and global scalar state (`SecCounter`, `MinCounter`, `ServerDown`, `BILLING`, `BillCounter`, `SaveCount`, `CartaTime`, `BrState`, `KefraLive`, `PartyPesa[3]`, etc.), all declared as `extern` in `legacy/Code/TMSrv/Server.h`. It has no encapsulated state of its own; it is a thin driver over the entire global server model.

3. **Hard-coded world/event logic.** The component embeds many concrete map coordinates, item indices, mob generator indices, and schedule rules directly in code (e.g., runetrack portal teleport coordinates at lines 251-465, nightmare clear minutes at 660-687, premium-item index ranges at 539-599). These represent business rules that are not data-driven and are tightly coupled to specific game content.

4. **No automated test coverage.** No unit, integration, or regression tests exist for this component or for the legacy codebase as a whole (see Section 11). The only test artifacts in the repository live under the ignored `.opencode/node_modules` directory and are unrelated to the game code.

5. **Debug/diagnostic file writes.** The combat processor writes diagnostic dump files (`AttackDieTeste.txt`, `Gethalf.txt`) directly from production code paths (`ProcessSecMinTimer.cpp:1652`, `:1674`), and `ProcessMinTimer()` writes guild-impostor NPC files under `./npc/` (lines 2082-2103).

---

## 2. Data Flow Analysis

The component is event-driven by Windows timers; it does not receive client network packets directly. Data flows from global game state, through the timer functions, into the various processing subsystems, and out to clients via socket send queues and to the database server via `DBServerSocket`.

```
1. Windows timer WM_TIMER fires (TIMER_SEC @ 500ms / TIMER_MIN @ 12s)
        └─> Server.cpp:3726/3728 dispatches to ProcessSecTimer() / ProcessMinTimer()
2. ProcessSecTimer():
   a. Server shutdown path (ServerDown countdown) → SendBilling2() + CloseUser()
   b. SecCounter++ ; CurrentTime = timeGetTime()
   c. Billing reconnection (BillCounter → ConnectBillServer → SendBilling2)
   d. Round-robin user auto-save (SaveCount) → SaveUser() → DBServerSocket (MSG_DBSaveMob)
   e. Socket flush loop → pUser[bb].cSock.SendMessageA()
   f. Per-user ProcessorSecTimer() (pMob[bb].ProcessorSecTimer)
   g. Event scheduling (localtime() checks): Kefra/nightmare/runetrack spawn + teleport
   h. Premium/dated-item expiry scan → BASE_ClearItem + SendItem
   i. HP/MP regeneration (ApplyHp/ApplyMp) → SendScore/SendSetHpMp
   j. Distributed mob AI loops (MOB_IDLE/MOB_PEACE via StandingByProcessor, MOB_COMBAT via BattleProcessor)
        └─> GetAttack/GridMulticast → client sockets
        └─> MobKilled() → reward/exp flow
        └─> DeleteMob() → world state cleanup
3. ProcessMinTimer():
   a. Log rotation (StartLog/StartChatLog/StartItemLog)
   b. Newbie/castle server rotation; FailAccount reset
   c. Guild-impostor NPC save → ./npc/ files (raw STRUCT_MOB write)
   d. Init-item state machine (STATE_OPEN → STATE_LOCKED) → GridMulticast MSG_CreateItem
   e. NPC generator spawn scheduling (MinuteGenerate) → GenerateMob / CreateItem (dungeon drops)
   f. Weather system → SendWeather()
   g. GuildProcess() (CWarTower tower war)
```

Data origin points are the global mutable arrays (`pUser`, `pMob`, `Pista`, `mNPCGen`, `pMobMerc`, `pItemGrid`) and the system clock (`localtime`/`timeGetTime`). Data sinks are: client TCP sockets (via `SendClientMessage`, `GridMulticast`, `SendItem`, `SendScore`), the DB server socket (`SaveUser` → `DBServerSocket.SendOneMessage`), the billing server socket (`BillServerSocket.SendBillMessage` via `SendBilling2`), and the filesystem (log files, `./npc/` files, debug dumps).

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Scheduling | High-frequency world tick (500 ms) | `legacy/Code/TMSrv/Server.cpp:3631,3726` |
| Scheduling | Low-frequency world tick (12 s) | `legacy/Code/TMSrv/Server.cpp:3632,3728` |
| Shutdown | Server shutdown sequence with user save/disconnect | `ProcessSecMinTimer.cpp:42-119` |
| Shutdown | Shutdown countdown notices every 20 ticks (10 s) | `ProcessSecMinTimer.cpp:105-109` |
| Billing | Billing server reconnection with countdown | `ProcessSecMinTimer.cpp:130-167` |
| Persistence | Round-robin user auto-save every 8 ticks (4 s) | `ProcessSecMinTimer.cpp:169-199` |
| Billing | Child/free-exp user logout during restricted hours | `ProcessSecMinTimer.cpp:178-186` |
| Network | Per-user send-buffer flush with disconnect on failure | `ProcessSecMinTimer.cpp:201-221` |
| World | Per-user `ProcessorSecTimer()` invocation | `ProcessSecMinTimer.cpp:223-224` |
| World | Celestial Lv180 recall from nightmare zone | `ProcessSecMinTimer.cpp:226-231` |
| Event | Balrog runetrack portal navigation | `ProcessSecMinTimer.cpp:233-495` |
| Broadcast | Secret-room mob-count / RvR score broadcast every 12 ticks | `ProcessSecMinTimer.cpp:497-518` |
| Quest | Quest-area clearing + QuestFlag reset every 1200 ticks | `ProcessSecMinTimer.cpp:520-538` |
| Items | Premium/dated equipped item expiry every 120 ticks | `ProcessSecMinTimer.cpp:539-599` |
| Combat | HP/MP regeneration every 2 ticks (1 s) | `ProcessSecMinTimer.cpp:601-619` |
| Combat | Passive ReqHp/ReqMp regen every 20 ticks (10 s) | `ProcessSecMinTimer.cpp:621-640` |
| Event | Kefra boss spawn Tuesday 12:00 | `ProcessSecMinTimer.cpp:649-657` |
| Event | Nightmare map clear schedules | `ProcessSecMinTimer.cpp:660-687` |
| Event | Rune-track entry/exit + prize distribution | `ProcessSecMinTimer.cpp:690-1036` |
| Event | Nightmare start broadcasts + mob generation | `ProcessSecMinTimer.cpp:1039-1098` |
| Items | Ground item decay every 2 ticks | `ProcessSecMinTimer.cpp:1101-1102` |
| World | Ranking processing + water-scroll cleanup every 4 ticks | `ProcessSecMinTimer.cpp:1104-1130` |
| Quest | Secret-room (Carta) time management | `ProcessSecMinTimer.cpp:1132-1170` |
| World | Castle Zakum timer hook | `ProcessSecMinTimer.cpp:1172` |
| World | Idle disconnect + RegenMob + ProcessAffect (distributed) | `ProcessSecMinTimer.cpp:1174-1210` |
| World | Quest-area recall guards | `ProcessSecMinTimer.cpp:1191-1209` |
| Event | Battle Royale / kingdom / leader env effects | `ProcessSecMinTimer.cpp:1212-1337` |
| Economy | Merchant-mob stock renewal + respawn | `ProcessSecMinTimer.cpp:1339-1349` |
| AI | Non-user mob idle/peace AI (distributed every 6 ticks) | `ProcessSecMinTimer.cpp:1353-1552` |
| AI | Non-user mob combat AI (distributed every 4 ticks) | `ProcessSecMinTimer.cpp:1554-2016` |
| Logging | Daily log rotation | `ProcessSecMinTimer.cpp:2027-2034` |
| World | Newbie-event-server daily rotation | `ProcessSecMinTimer.cpp:2036-2057` |
| World | Castle-server weekly assignment | `ProcessSecMinTimer.cpp:2060-2071` |
| Persistence | Guild-impostor NPC save to `./npc/` files | `ProcessSecMinTimer.cpp:2074-2104` |
| World | Kingdom clear-area reset | `ProcessSecMinTimer.cpp:2112-2133` |
| Security | Login fail-account reset every 10 min | `ProcessSecMinTimer.cpp:2136-2137` |
| Items | Initial item open→lock state transitions | `ProcessSecMinTimer.cpp:2141-2209` |
| World | NPC generator minute-based respawn scheduling | `ProcessSecMinTimer.cpp:2211-2280` |
| World | Dynamic dungeon event item drops | `ProcessSecMinTimer.cpp:2253-2275` |
| World | Weather system (random or forced) | `ProcessSecMinTimer.cpp:2284-2311` |
| War | Guild tower-war process hook | `ProcessSecMinTimer.cpp:2313` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: Server shutdown sequence

**Overview:**
The server shutdown is a coordinated, countdown-driven procedure that safely disconnects and persists every connected user before terminating the process. It is driven by the global `ServerDown` counter and processed within `ProcessSecTimer()`.

**Detailed description:**
The shutdown logic occupies the first block of `ProcessSecTimer()` (lines 42-119) and is gated on the global `ServerDown` variable. When `ServerDown == 120` (interpreted as "two minutes until the server closes" per the comment on line 42), the function initiates a teardown that first disconnects from the billing server. It allocates a zeroed `_AUTH_GAME Unk` structure (196 bytes), calls `SendBilling2(&Unk, 4)` to signal billing teardown, and sets `BILLING = 0` to disable further billing operations. It then enters a `while (TRUE)` loop that locates the first non-empty user slot starting at `UserCount`. If `UserCount >= MAX_USER`, all users have been processed and the loop exits the application: it logs "server down complete", deletes the font handle `hFont`, and posts a quit message via `PostQuitMessage(NULL)` to terminate the Win32 message loop, ending the server. If a slot holds a user in `USER_EMPTY` mode, the index is advanced; otherwise the loop breaks to process that user. For each real user found, the function null-terminates the account name, logs a `"sys,save %d - %d - %s"` line, invokes `CloseUser(UserCount)` (which closes the socket and resets the slot to `USER_EMPTY`, see `legacy/Code/TMSrv/CUser.cpp:81`), advances `UserCount`, and returns so that the next timer tick continues with the following user. This ensures that users are saved/disconnected one per tick rather than all at once, avoiding a shutdown spike.

**Rule workflow:**
1. `ServerDown == 120` triggers teardown mode.
2. Billing server notified (`SendBilling2(..., 4)`), `BILLING = 0`.
3. Loop finds next non-empty user slot from `UserCount`.
4. If no users remain (`UserCount >= MAX_USER`), log, delete font, `PostQuitMessage` → exit.
5. Otherwise log the save event, `CloseUser()` the slot, increment `UserCount`, return.

---

### Business Rule: Shutdown countdown notices and timer re-arm

**Overview:**
While the server is being shut down (but not yet in the terminal teardown state), the component broadcasts periodic countdown notices to all players and eventually re-arms the timer at a faster cadence for the final teardown phase.

**Detailed description:**
Lines 96-119 handle the `ServerDown > -1000` branch. If `ServerDown <= 0` the counter is simply incremented and control jumps to the main processing label `lbl_PST1` (line 121), meaning the countdown has finished and normal tick processing continues. Otherwise, when `ServerDown % 20 == 1` (i.e., once every 20 ticks, roughly every 10 seconds at 500 ms per tick), the function computes a message id `messageId = ServerDown / 20` and broadcasts `g_pMessageStringTable[messageId + 17]` to all players via `SendNotice`, giving a staged countdown announcement (for example 120/20 = 6 announcements during the two-minute window). The counter is then incremented. When `ServerDown == 120`, the timer is re-armed via `SetTimer(hWndMain, TIMER_SEC, 200, 0)` — reducing the interval from 500 ms to 200 ms for the final teardown phase — and the function returns early, deferring teardown to subsequent faster ticks.

**Rule workflow:**
1. If `ServerDown <= 0`, increment and continue normal processing.
2. Every 20th tick, broadcast countdown notice derived from `ServerDown/20`.
3. Increment `ServerDown`.
4. At `ServerDown == 120`, re-arm `TIMER_SEC` at 200 ms and return.

---

### Business Rule: Billing server reconnection

**Overview:**
The component supervises connectivity to the external billing server. If the billing socket is down while billing is enabled, it runs a countdown and attempts a reconnection, disabling billing if reconnection fails.

**Detailed description:**
Lines 130-167 are gated on `BILLING != 0 && BillCounter > 0 && BillServerSocket.Sock == 0`. When these hold, `BillCounter` is decremented each tick. Once `BillCounter <= 0`, the function captures the current time, logs `"err,start reconnect BILL"`, and invokes `BillServerSocket.ConnectBillServer(BillServerAddress, BillServerPort, *pip, WSA_READBILL)` (see `legacy/Code/CPSock.cpp:257`). If the connection attempt returns `0` (failure), it logs `"err,Reconnect BILL Server(x2) fail."` and sets `BILLING = 0`, permanently disabling billing for the session. If the reconnect succeeds, it sends a fresh billing handshake via `SendBilling2(&Unk, 4)`. If billing is disabled or the socket is already connected, the `else` branch resets `BillCounter = 0`. `BillCounter` is set to `360` on a billing disconnect in the `WSA_READBILL` handler (`legacy/Code/TMSrv/Server.cpp:3738`), so this reconnection attempt occurs after 360 high-frequency ticks (~3 minutes).

**Rule workflow:**
1. Check `BILLING != 0 && BillCounter > 0 && Sock == 0`.
2. Decrement `BillCounter` each tick.
3. On `BillCounter <= 0`, attempt `ConnectBillServer` (with 3 bind retries inside).
4. On failure: log, `BILLING = 0`.
5. On success: send billing handshake (`SendBilling2(..., 4)`).
6. Otherwise reset `BillCounter = 0`.

---

### Business Rule: Round-robin user auto-save (persistence)

**Overview:**
To avoid persisting all users simultaneously, the server auto-saves users in a round-robin fashion, one user per save window, and selectively enforces a "child pay" free-exp policy that logs out qualifying users during restricted hours.

**Detailed description:**
Lines 169-199 run every `SecCounter % 8 == 0` (every 8 ticks, i.e., every 4 seconds). It iterates up to `MAX_USER` slots to find the next slot to save, using the global `SaveCount` as a rotating cursor (wrapped to 1 when it reaches `MAX_USER`). For each candidate slot where `pUser[SaveCount].Mode == USER_PLAY` and the mob is not empty (`pMob[SaveCount].Mode != MOB_EMPTY`), it first evaluates a special billing policy: if `BILLING == 2`, the user flag `Unk_2728 == 1`, the character level is at least `FREEEXP` (default 35, `Server.cpp:43`), and the server hour `g_Hour` is `<= 12 || >= 19` (free-exp restricted evening/night window), the server sends a "child pay" message (`_NN_Child_Pay`) and forces a logout via `CharLogOut(SaveCount)` (which saves the user, deletes the mob, and transitions the slot to `USER_SELCHAR`, see `Server.cpp:7085`). Otherwise the user is saved normally via `SaveUser(SaveCount, 0)` — which packages the mob, cargo, coin, slot, donate, short skills, affects, and extra into a `MSG_DBSaveMob` and sends it to the DB server over `DBServerSocket` (`Server.cpp:7056-7083`). After saving/logging out one user, the cursor is advanced and the loop breaks, so exactly one user is processed per window.

**Rule workflow:**
1. Every 8 ticks, locate next `SaveCount` slot in play.
2. If child-pay policy applies (BILLING==2, flag, level≥FREEEXP, restricted hour): message + `CharLogOut`.
3. Else `SaveUser(SaveCount, 0)` → `MSG_DBSaveMob` → DB server.
4. Advance cursor; process one user per window.

---

### Business Rule: Per-user socket flush and disconnect on send failure

**Overview:**
Every high-frequency tick, the component flushes each user's pending outgoing socket buffer and disconnects the user if the send fails, guarding against wedged connections.

**Detailed description:**
Lines 201-221 iterate over all user slots from `1` to `MAX_USER`. If a slot is active (`pUser[bb].Mode` non-zero) and has pending buffered data (`cSock.nSendPosition` non-zero), it calls `pUser[bb].cSock.SendMessageA()`. If the send returns `FALSE`, the component logs a diagnostic line `"err,send fail close %d/%d %d/%d"` (slot, socket, send position, sent position), null-terminates the account name, logs the event with the account and IP, and disconnects the user via `CloseUser(bb)`. This is the server's mechanism for detecting and purging clients whose sockets can no longer accept data, preventing unbounded buffer growth and leaking resources. This runs unconditionally every tick (it is not gated by `SecCounter % N`), so it is the most frequent operation in the loop.

**Rule workflow:**
1. For each active user slot: if `nSendPosition != 0`, call `SendMessageA()`.
2. On `FALSE`: log, then `CloseUser(bb)`.

---

### Business Rule: Celestial level-180 nightmare-zone recall

**Overview:**
A character of the Celestial master class (or its variants) that reaches level 180 or higher while standing inside the "pesadelo" (nightmare) zone is forcibly recalled to a safe location.

**Detailed description:**
Lines 226-231 (with the Portuguese comment "Celestial Lv181 dentro da zona do pesadelo") compute the 128-tile-grid cell of the character's `TargetX`/`TargetY`. If the cell matches one of three specific nightmare-zone cells `(9,1)`, `(8,2)`, or `(10,2)`, and the character's `extra.ClassMaster` is `CELESTIAL`, `CELESTIALCS`, or `SCELESTIAL` with `CurrentScore.Level >= 180`, the component invokes `DoRecall(bb)`. This removes an overpowered Celestial from a low-level nightmare farming area, an anti-exploitation rule. It executes for every user slot every high-frequency tick (within the per-user loop), so the recall triggers as soon as the condition is met.

**Rule workflow:**
1. Compute cell `(TargetX/128, TargetY/128)`.
2. If in nightmare cell set AND class is Celestial variant AND level ≥ 180 → `DoRecall(bb)`.

---

### Business Rule: Balrog runetrack portal navigation

**Overview:**
During the level-5 runetrack (Pista Balrog) dungeon, the component continuously monitors party leaders standing on specific portal tiles and teleports them (and their party members) between dungeon rooms according to randomized portal routing, consuming a required key item and spawning the level-5 boss when a party reaches the final room.

**Detailed description:**
Lines 233-495 define an extensive portal-routing state machine for the Balrog runetrack (rooms 1-4 with multiple portals). The block iterates `Pista[5].Party[x]` (the level-5 track's three parties). A candidate party must have `pUser[bb].Mode == USER_PLAY`, the mob alive (`Hp > 0`), a valid leader (`LeaderID != 0` and equal to the current slot `bb`), a matching `LeaderName`, and the leader must not itself be a follower (`Leader == -1` or `Leader == 0`). The tile is computed via `xv = TargetX & 0xFFFC` / `yv = TargetY & 0xFFFC` to align to the portal grid. Depending on which `PistaBalrogPortalPos[sala][portal]` the leader occupies, a pseudo-random `_rd = rand()%3` chooses a destination from `PistaBalrogPos`. Reaching the final room (Sala 4) portals with `_rd == 2` triggers `GenerateMob(RUNEQUEST_LV5_MOB_BOSS, 0, 0)` if the room's `MobCount == 0`, spawning the level-5 boss. Before teleporting, the code scans the leader's inventory (`Carry`) for a required key item `sIndex == 4032`; if found it clears the key (`BASE_ClearItem`) and notifies the client (`SendItem`). If the key is not found (`inv == pMob[bb].MaxCarry`), the leader is skipped. The leader is teleported with `DoTeleport(Pista[5].Party[x].LeaderID, tx, ty)`, and all connected party members (`PartyList`) are teleported to the same destination. This is a hard-coded, content-specific dungeon progression rule.

**Rule workflow:**
1. For each of 3 parties in `Pista[5]`, validate leader/party conditions.
2. Compute aligned portal tile.
3. Match portal to room/portal coordinates; pick random destination.
4. On final-room `_rd==2`: spawn LV5 boss if room empty.
5. Require and consume key item 4032.
6. Teleport leader + all party members to destination.

---

### Business Rule: Secret-room and RvR mob-count broadcast

**Overview:**
Every 12 high-frequency ticks (6 s), the component computes the current mob population of the four secret-room (Carta) chambers and the rune-track "coelho" room, and broadcasts these counts (plus RvR red/blue scores) to clients within the relevant map areas.

**Detailed description:**
Lines 497-518 run when `SecCounter % 12 == 0`. It sums `CurrentNumMob` across the `mNPCGen.pList[]` generator entries for each secret room (Sala1-4) across the three difficulty tiers (N, M, A — normal, mystic, arcane) using the `SECRET_ROOM_*` generator index constants defined in `legacy/Code/Basedef.h:294-340`. It then calls `SendSignalParmArea` with hard-coded map rectangles and `_MSG_MobLeft` to push each room's remaining-mob count to clients viewing that area. It also broadcasts the rune-track +6 "Coelho" room count via `Pista[6].Party[0].MobCount` and the RvR red/blue point totals (`RvRRedPoint + 1024`, `RvRBluePoint + 1024`) via `SendShortSignalParm2Area` with `_MSG_MobCount`. This keeps player-facing dungeon/mode HUD elements synchronized with server-side mob population.

**Rule workflow:**
1. Every 12 ticks, sum per-room generator counts (Sala1-4, tiers N/M/A).
2. Broadcast `_MSG_MobLeft` per room to its area.
3. Broadcast rune +6 count and RvR scores.

---

### Business Rule: Quest-area clearing and quest-flag reset

**Overview:**
Every 1200 high-frequency ticks (10 minutes), the component clears designated quest/hazard areas and resets the per-mob quest flag so that quest areas can reset and re-trigger.

**Detailed description:**
Lines 520-538 run when `SecCounter % 1200 == 0`. It calls `ClearAreaQuest` for a set of hard-coded map rectangles corresponding to named quest/content zones: the "Coveiro" (line 522), "Capa Verde" (523), two skill-reset areas (524, 526), "Carbuncle" (525), "Kaizen" (527), "Hidras" (528), "Elfos" (529), and "Quest Gargula" (530), plus a `ClearArea` for the "Lanhouse" area (531). It then iterates all user slots and, for each in-play user with a non-zero `pMob[x].QuestFlag`, resets `QuestFlag = 0`. This periodically re-arms quest zones that rely on the flag (which is also used as a guard in the recall logic at lines 1191-1209).

**Rule workflow:**
1. Every 1200 ticks, clear named quest areas.
2. Reset `QuestFlag` to 0 for all in-play users.

---

### Business Rule: Premium and dated-item expiry

**Overview:**
Every 120 high-frequency ticks (1 minute), the component scans each in-play user's equipped and special inventory slots and removes/refreshes items whose expiry date has passed, applying item-specific side effects such as inventory-capacity reduction.

**Detailed description:**
Lines 539-599 run when `SecCounter % 120 == 0` and iterate all users in `USER_PLAY`. Four equipment/inventory rules are applied:

1. **Equip[14] (mount/vehicle slot), premium range 3980-3989:** if `BASE_CheckItemDate` reports expiry, the item is cleared (`BASE_ClearItem`) and the client is updated (`SendItem`), logging `"etc,premium item end"`.
2. **Equip[12] range 4150-4188:** same expiry-clear behavior for a second premium band.
3. **Equip[13] (fairy slot) range 3900-3913:** calls `BASE_CheckFairyDate`, refreshes the item via `SendItem`, and recomputes the character score via `pMob[user].GetCurrentScore(user)`.
4. **Carry[60]/[61] slot-expansion item 3467:** when the dated item expires, it is cleared and the user's `MaxCarry` capacity is recomputed: reset to 30, then +15 for each of Carry[60]/[61] still holding index 3467 (lines 576-582, 590-596).

This is a periodic enforcement of time-limited premium content.

**Rule workflow:**
1. Every 120 ticks, for each in-play user:
2. Check Equip[14] (3980-3989) → clear if expired.
3. Check Equip[12] (4150-4188) → clear if expired.
4. Check Equip[13] (3900-3913) → fairy date refresh + rescore.
5. Check Carry[60]/[61] (3467) → clear if expired + recompute MaxCarry.

---

### Business Rule: HP/MP regeneration and passive pool regen

**Overview:**
The component periodically regenerates hit points and mana for alive in-play characters via two complementary mechanisms: an active regen every 2 ticks and an additive passive pool regen every 20 ticks.

**Detailed description:**
Lines 601-619 run when `SecCounter % 2 == 0` (every 1 s). For each in-play user with `Hp > 0`, it resets the `Unk_2688` flag to 0 and calls `ApplyHp(i)` and `ApplyMp(i)`; if the HP application returned non-zero it pushes a full score update (`SendScore`), otherwise if MP changed it pushes a lighter HP/MP update (`SendSetHpMp`). Lines 621-640 run when `SecCounter % 20 == 0` (every 10 s) and, for the same alive-in-play population, increment the user's `ReqHp` and `ReqMp` pools by `Level + 30` before re-applying, so that the passive regeneration pool grows over time. This split cadence drives the game's natural recovery economy.

**Rule workflow:**
1. Every 2 ticks: `ApplyHp`/`ApplyMp`; `SendScore` or `SendSetHpMp` on change.
2. Every 20 ticks: add `Level + 30` to `ReqHp`/`ReqMp`, then re-apply.

---

### Business Rule: Kefra boss and nightmare-zone scheduling

**Overview:**
Using the local system clock, the component schedules the weekly Kefra boss spawn (Tuesdays at 12:00) and periodically clears/repopulates the three nightmare maps on a repeating minute-based timetable.

**Detailed description:**
Lines 642-687 run when `SecCounter % 2 == 0` and evaluate the wall-clock time. If it is Tuesday (`tm_wday == 2`), hour 12, minute 0, and seconds in [0,2], and `KefraLive` is still true, it clears `KefraLive` and generates the Kefra boss (`KEFRA_BOSS`) plus its add-mobs (`KEFRA_MOB_INITIAL..KEFRA_MOB_END`). Separately, three nightmare-clear schedules delete and clear maps: at minutes 9/29/49 the "Pesa A" (arcane) map is cleared (`DeleteMobMapa(9,1)` + `ClearMapa(9,1)` + `PartyPesa[2]=0`, lines 660-668); at minutes 4/24/44 the "Pesa M" (mystic) map (`DeleteMobMapa(8,2)`/`ClearMapa(8,2)`/`PartyPesa[1]=0`, lines 669-677); and at minutes 19/39/59 the "Pesa N" (normal) map (`DeleteMobMapa(10,2)`/`ClearMapa(10,2)`/`PartyPesa[0]=0`, lines 679-687). Each clear is logged. This implements a periodic world-reset timetable for instanced nightmare content.

**Rule workflow:**
1. Tuesday 12:00:00-02 → spawn Kefra boss + adds, clear `KefraLive`.
2. Minutes 9/29/49:00 → clear nightmare Arcane map + `PartyPesa[2]=0`.
3. Minutes 4/24/44:00 → clear nightmare Mystic map + `PartyPesa[1]=0`.
4. Minutes 19/39/59:00 → clear nightmare Normal map + `PartyPesa[0]=0`.

---

### Business Rule: Rune-track entry, exit, and prize distribution

**Overview:**
The rune-track dungeon opens and closes on a repeating timetable: at scheduled minutes parties waiting at the entrance are teleported into their assigned rooms and content is spawned, and at exit minutes the track is cleared, survivors are ejected, and winning parties receive rune and prize items.

**Detailed description:**
**Entry (lines 690-755, minutes 20/40/00):** The code sets `Pista[4].Party[0..2].MobCount = 1` and `Pista[6].Party[0].MobCount = 100`. For each of the 7 tracks and 3 parties, if the party has a valid leader at the entrance cell `(25,13)`, the leader and co-located party members are teleported via `DoTeleport` to `PistaPos[Pista[s].Party[t].Sala][t]`. Content is spawned per track: track 0 spawns Lich mobs (`RUNEQUEST_LV0_LICH2/LICH1`), track 1 spawns three towers plus mobs (`RUNEQUEST_LV1_TORRE1/2/3` and `RUNEQUEST_LV1_MOB_INITIAL..END`), track 2 spawns the LV2 boss, track 4 sets a randomized `MobCount = 8 + rand()%8` and spawns `RUNEQUEST_LV4_MOB_INITIAL..END`.

**Exit (lines 758-1036, minutes 15/35/55):** All mobs in the rune-track rectangle (`TargetX 3310-3588`, `TargetY 1005-1663`) are deleted (`DeleteMob(c,3)`), and users in that rectangle are revived if dead (`Hp = 1`) and teleported out to `(3294, 1701/1686)`. Winning parties for tracks 1 (Sulrang) and 3 are determined by the highest `MobCount` among the three parties; the winning leader and each party member receive a randomized rune item (`PistaRune[track][rand()%5]`) via `PutItem`, and the leader additionally receives a `Prize` item (index 5134, effect 43 with value 2 for track 1 or 4 for track 3). Note several copy-paste inconsistencies exist (e.g., lines 882 and 980 use the wrong party index in the `PutItem` call for party 3 / party 2 cases). Finally, all `Pista` party state is reset (`LeaderID=0`, `LeaderName=" "`, `MobCount=0`, `Sala=0`).

**Rule workflow:**
1. Entry minutes 20/40/00: teleport entrance parties to rooms; spawn track-specific content.
2. Exit minutes 15/35/55: delete track mobs, revive+eject users.
3. Determine winning party by highest `MobCount`; reward rune items to leader + members, bonus prize to leader.
4. Reset all `Pista` party state.

---

### Business Rule: Nightmare start broadcast and mob generation

**Overview:**
When a nightmare map is about to begin its cycle and players are present in the trigger area, the component broadcasts a 900-second start timer and spawns the map's mob population.

**Detailed description:**
Lines 1039-1098 run when `SecCounter % 2 == 0` (with the parent block). Three checks each test whether users are present in a specific trigger rectangle (`GetUserInArea`) AND the wall-clock minute/second matches the map's start schedule:

1. **Arcane (Pesa A):** area `(1152,128)-(1280,256)` at minutes 14/34/54:00 → broadcast `_MSG_StartTime` with `Parm=900` to map `(9,1)` via `MapaMulticast`, spawn `NIGHTMARE_A_INITIAL..END` mobs, log `"etc,nigthmare_arcan started"`.
2. **Mystic (Pesa M):** area `(1024,256)-(1152,384)` at minutes 9/29/49:00 → same pattern to map `(8,2)`, spawn `NIGHTMARE_M_INITIAL..END`, log `"etc,nigthmare_mistycal started"`.
3. **Normal (Pesa N):** area `(1280,256)-(1408,384)` at minutes 24/44/04:00 → same to map `(10,2)`, spawn `NIGHTMARE_N_INITIAL..END`, log `"etc,nigthmare_normal started"`.

The 900-second (`Parm=900`) timer shown to clients corresponds to the ~15-minute nightmare cycle. Generation is gated on player presence to avoid spawning content in unoccupied maps.

**Rule workflow:**
1. If users present in trigger area AND scheduled minute/second match:
2. Broadcast `_MSG_StartTime`(900) to the map.
3. Spawn the map's nightmare mob range.
4. Log the start event.

---

### Business Rule: Ground item decay

**Overview:**
Every 2 high-frequency ticks, the component advances the decay of items lying on the ground so that dropped items expire over time.

**Detailed description:**
Line 1101-1102 invokes `ProcessDecayItem()` whenever `SecCounter % 2 == 0`. This delegates ground-item lifetime management to the global decay routine (declared in `legacy/Code/TMSrv/Server.h:158`), which is responsible for decrementing the timers of `pItem` instances and removing expired dropped items from `pItemGrid`. This prevents the world from accumulating dropped items indefinitely and bounds the memory/state footprint of ground items.

**Rule workflow:**
1. Every 2 ticks → `ProcessDecayItem()`.

---

### Business Rule: Ranking processing and water-scroll cleanup

**Overview:**
Every 4 high-frequency ticks, the component recomputes server rankings and decrements water-scroll ("scroll of return" transport) timers, teleporting users back to a hub when a scroll's timer expires.

**Detailed description:**
Lines 1104-1130 run when `SecCounter % 4 == 0`. It calls `ProcessRanking()` to refresh leaderboards. It then processes the `WaterClear1[3][10]` timer grid: values `> 1` are decremented; when a value reaches exactly `1` and its column index `j <= 7`, `ClearAreaTeleport` moves users near `WaterScrollPosition[i][j]` (±8 tiles) to the hub `(1965,1769)`; when `j > 7`, the same happens with a ±12-tile radius. This is a two-tier radius rule based on the scroll column. It effectively returns players to a base location after a water-scroll buff expires.

**Rule workflow:**
1. Every 4 ticks → `ProcessRanking()`.
2. Decrement `WaterClear1` timers.
3. On expiry (value 1): teleport area users to hub (radius 8 for columns ≤7, radius 12 otherwise).

---

### Business Rule: Secret-room (Carta) cycle management

**Overview:**
The secret-room dungeon (Carta) advances through four chambers on a fixed 60-tick cycle, teleporting/clearing each chamber in turn and finally clearing and resetting the entire dungeon.

**Detailed description:**
Lines 1132-1170 run when `SecCounter % 4 == 0` (within the same block as ranking). The global `CartaTime` is decremented when `> 1`. When it reaches exactly `1`, it resets to 60 and advances the dungeon: for `CartaSala == 1`, users in chamber 1 are teleported to `CartaPos[1]`; for `CartaSala == 2`, chamber 2 to `CartaPos[2]`; for `CartaSala == 3`, chamber 3 to `CartaPos[3]` (each via `ClearAreaTeleport` with the chamber's rectangle). For `CartaSala == 4`, the whole dungeon area is cleared (`ClearArea(776,3595,834,3648)`), `CartaTime` and `CartaSala` are reset to 0, and `"etc,clear secretroom"` is logged. Otherwise `CartaSala++`. A `MSG_STANDARDPARM` with `_MSG_StartTime` and `Parm = CartaTime*2` is broadcast to map `(6,28)` so clients see the chamber timer. This drives the four-stage secret-room progression.

**Rule workflow:**
1. Every 4 ticks: decrement `CartaTime` when > 1.
2. At `CartaTime == 1`: reset to 60.
3. Teleport/clear current chamber based on `CartaSala` (1/2/3); for sala 4, full clear + reset.
4. Advance `CartaSala`; broadcast timer to map (6,28).

---

### Business Rule: Idle-disconnect, regeneration, and quest-area recall guards

**Overview:**
The component enforces anti-idle and anti-exploit rules by periodically disconnecting idle users, regenerating in-play characters, and recalling under-level characters that trespass into quest-protected areas.

**Detailed description:**
Lines 1174-1210 process each user slot using modulo distribution to spread work across ticks: a slot is checked for idleness when `i % 32 == Sec32` (every 32 ticks per slot), and regenerated when `i % 16 == Sec16`. `CheckIdle(i)` (see `Server.cpp:4304`) disconnects a user whose `LastReceiveTime` is more than 720 ticks old (roughly 6 minutes) by calling `CloseUser`, resetting the timestamp if it is out of range. For in-play, alive mobs, `RegenMob(i)` (handles castle-status charging, PK points, mount feeding, guilt decay) and `ProcessAffect(i)` run, incrementing `UsersDie`. Additionally, five quest-area recall guards (lines 1191-1209) recall any character with level `< 1000` whose `QuestFlag` does not match the area and who stands inside a protected rectangle: Coveiro (2379-2426, 2076-2133), Jardin (2228-2257, 1700-1728), Kaizen (459-497, 3887-3916), Hidra (658-703, 3728-3762), and Elfos (1312-1348, 4027-4055). This keeps under-level players out of quest zones and keeps inactive connections from consuming resources.

**Rule workflow:**
1. For each slot: if `i%32==Sec32` → `CheckIdle(i)` (disconnect if idle >720 ticks).
2. If `i%16==Sec16` and alive+in-play → `RegenMob(i)` + `ProcessAffect(i)`.
3. For each quest area: if level<1000 and `QuestFlag` mismatched and inside rectangle → `DoRecall(i)`.

---

### Business Rule: Battle Royale, kingdom, and leader environmental effects

**Overview:**
The component emits periodic environmental damage/effects for the Battle Royale arena, the two kingdoms' territory, and the rune-track leader platforms to animate hazard zones.

**Detailed description:**
Lines 1212-1337 handle hazard rendering. When `BrState == 2` (Battle Royale active) and the slot modulo aligns (`i % 8 == Sec16`) and `BRItem > 0`, it sends damage (`SendDamage`) and environment effects (`SendEnvEffect`) over arena rectangles, with a phase change at `BrMod >= 9` using shifted rectangles (lines 1214-1230). Every 6 ticks (`SecCounter % 6 == 0`), `SendEnvEffectKingdom`/`SendDamageKingdom` emit effects over the blue-kingdom (1050-1070, 2108-2146) and red-kingdom (1204-1245, 1947-1988) hazard rectangles (lines 1234-1245). Every 5 ticks (`SecCounter % 5 == 0`), a large set of `SendEnvEffectLeader`/`SendDamageLeader` calls animate the rune-track leader platforms at coordinates in the ~3335-3452 x 1285-1401 range (lines 1247-1337). These are cosmetic/affect hazard emitters that keep hazard zones visually active and apply periodic damage to occupants.

**Rule workflow:**
1. Battle Royale active → arena damage/effects (phased by `BrMod`).
2. Every 6 ticks → kingdom hazard effects.
3. Every 5 ticks → leader-platform hazard effects.

---

### Business Rule: Merchant-mob stock renewal and respawn

**Overview:**
Merchant mobs (`pMobMerc`) have independent stock-renewal and respawn schedules, each driven by a per-mob tick interval.

**Detailed description:**
Lines 1339-1349 iterate all `MAX_MOB_MERC` merchant slots. A slot is skipped if any of `RenewTime`, `RebornTime`, or `GenerateIndex` is zero. When `SecCounter % RenewTime == 0`, the merchant's `Stock` is restored from `MaxStock` via `memcpy`. When `SecCounter % RebornTime == 0`, the merchant's mob is respawned via `GenerateMob(pMobMerc[k].GenerateIndex, 0, 0)`. This decouples merchant inventory restock cadence from merchant-body respawn cadence, allowing a vendor to reappear and replenish goods on independent schedules.

**Rule workflow:**
1. For each merchant slot (skip if unconfigured):
2. `SecCounter % RenewTime == 0` → restock `Stock` from `MaxStock`.
3. `SecCounter % RebornTime == 0` → respawn merchant mob.

---

### Business Rule: Non-user mob idle/peace AI (distributed)

**Overview:**
The component advances the AI of all non-user mobs in idle and peace states, spreading the work across ticks by slot offset to avoid processing all mobs every tick.

**Detailed description:**
Lines 1353-1552 distribute processing of mob indices `[MAX_USER, MAX_MOB)` in chunks of 6 per tick, using `Sec6 = SecCounter % 6` and `initial = Sec6 + MAX_USER`, iterating `index += 6`. For mobs in `MOB_IDLE` or `MOB_PEACE`, it first guards against dead mobs (`Hp <= 0` → log + `DeleteMob(index,1)`). It calls `ProcessAffect` and, for `MOB_PEACE`, invokes `StandingByProcessor()`. The returned bitmask encodes actions: `0x10000000` signals an attack → `SetBattle(index, Target)` (with party-wide engagement and enemy-list cleanup); other bits drive movement (`GetEmptyMobGrid`+`GetAction`+`GridMulticast`, bit 1), flee dialogue (bit 0x10), despawn (bit 0x100 → `DeleteMob(index,3)`), random wander (bit 0x1000), and teleport (bit 2). Content-specific rules are embedded: LV6 rune-track mobs entering a kill zone (`TargetX 3423-3442`, `TargetY 1492-1511`) increment `Pista[6].Party[0].MobCount` (if `< 100` and non-zero) then despawn (lines 1388-1398, 1536-1546), and LV2 mobs in a bounded area are deleted (lines 1490-1494). Segment-progress dialogue (`SendSay`) is emitted when the mob's `SegmentProgress` changes (lines 1482-1488, 1496-1504).

**Rule workflow:**
1. Distribute mob indices over 6-tick chunks.
2. For idle/peace mobs: delete if dead; `ProcessAffect`.
3. `StandingByProcessor` → act on returned bitmask (battle/move/flee/despawn/wander/teleport).
4. Apply rune-track kill-zone and segment-dialogue content rules.

---

### Business Rule: Non-user mob combat AI (distributed)

**Overview:**
The component drives combat for mobs in `MOB_COMBAT` state, distributing across a 4-tick cycle, including target selection, attack resolution, damage application with defensive modifiers, and death handling.

**Detailed description:**
Lines 1554-2016 distribute combat processing over mob indices using `Sec4 = SecCounter % 4` and stepping `index += 4`. For each combat mob, dead mobs are logged and deleted (lines 1567-1576). `ProcessAffect` runs when `index % 16 == Sec16`. LV6 rune-track mobs entering the kill zone increment `Pista[6]` count and despawn (lines 1581-1591). Target management removes allied targets from the enemy list (`RemoveEnemyList`) when leader or guild matches (lines 1595-1619). `BattleProcessor()` returns a bitmask: bit 0x20 → despawn; bit 0x1000 → perform an attack via `GetAttack`. The attack applies a range of modifiers: parry (`GetParryRate`, with skill 79/22 at 30%), boss-clan damage reduction (`Clan == 4` → 2/5), mount absorption (`Equip[14]` 2360-2390 → 3/4), mana-shield absorption (Affect type 18 using MP, lines 1843-1868), and special sanctification reductions for `Equip[13]` indices 786 (×2), 1936 (×10), 1937 (×1000) (lines 1871-1903). It broadcasts the attack via `GridMulticast`, updates user `ReqHp`/`SendSetHpMp`, and on death invokes `MobKilled(Target, index, 0, 0)`. Movement bits (0x100, 1, 0x10, 2) drive approach/teleport actions. Diagnostic dump files (`AttackDieTeste.txt`, `Gethalf.txt`) are written in specific branches (lines 1650-1705). A Kefra special-attack branch (skills 109/110/111, lines 1712-1716) jumps to a disabled (`if (0 == 1)`) area-attack label.

**Rule workflow:**
1. Distribute combat mobs over 4-tick chunks.
2. Delete dead mobs; apply rune-track kill-zone rule.
3. Purge allied targets from enemy list.
4. `BattleProcessor` → attack/move/despawn per bitmask.
5. Resolve attack with parry, mount, mana-shield, and sanctification modifiers.
6. Broadcast; update user HP pools; on death → `MobKilled`.

---

### Business Rule: Daily log rotation

**Overview:**
At each low-frequency tick, the component verifies the current calendar day and rolls over the server, chat, and item log files when the day changes.

**Detailed description:**
Lines 2027-2034 (top of `ProcessMinTimer`) read the local time once via `time()`/`localtime()`. If `tm_mday != LastLogDay`, `StartLog()` is invoked to rotate the main server log; if `tm_mday != LastChatLogDay`, `StartChatLog()` rotates the chat log; if `tm_mday != LastItemLogDay`, `StartItemLog()` rotates the item log. Each rotation function closes the previous day's file and opens a new one named with the date. This bounds log-file size and keeps per-day audit trails.

**Rule workflow:**
1. Read current day-of-month.
2. If changed: `StartLog()`, `StartChatLog()`, `StartItemLog()` as needed.

---

### Business Rule: Newbie-event-server and castle-server rotation

**Overview:**
The component rotates which server in the group hosts the newbie event and which server is the castle server, on a daily/weekly basis, also cleaning up trade state when the newbie channel moves.

**Detailed description:**
Lines 2036-2071 run every `MinCounter % 2 == 0` (every 2 low-frequency ticks). A `NewbieServerID` is computed as `(tm_mday - 1) % NumServerInGroup`. If `LOCALSERVER != 0` (this server is a dedicated newbie server) or `ServerIndex == NewbieServerID`, then `NewbieEventServer = 1`. Else, if `NewbieEventServer` was 1 but the local server no longer matches the newbie ID, it iterates all users and calls `RemoveTrade(i)` for any user in trade mode (`TradeMode == 1`) before clearing `NewbieEventServer = 0` — aborting active trades because the newbie channel moved. Separately (always, since `if (1 != 0)` is effectively always true, marked as a likely debug flag at line 2060), the castle server is assigned: `wNum = BASE_GetWeekNumber()`, `wMod = wNum % 7`, `wDay = wNum / 7`; if `wMod == 0` and `(wDay % 2) == (ServerIndex % 2)`, `CastleServer = 1`, else `CastleServer = 0`. This yields a weekly alternation of the castle-hosting server.

**Rule workflow:**
1. Every 2 min ticks: compute newbie server by day.
2. Set `NewbieEventServer`; on channel move, abort trades and clear flag.
3. Compute castle server by week parity; set `CastleServer`.

---

### Business Rule: Guild-impostor NPC persistence

**Overview:**
The component persists guild-impostor NPCs (tax-collector NPCs) to files on disk so their state survives server restarts.

**Detailed description:**
Lines 2074-2104 iterate all `MAX_GUILDZONE` guild zones. For each zone with a non-zero `GuildImpostoID[i]` and an alive mob (`Hp > 0`), the function builds a path `./npc/<MobName>` where spaces in the mob name are replaced by underscores (lines 2082-2092). It opens the file with `_open(temp, _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE)`; on failure it logs `"-system","fail - save npc imposto file"` and returns early. Otherwise it writes the raw `STRUCT_MOB` for the impostor via `_write` and closes with `_close`. This is a crude binary-persistence mechanism for guild tax NPCs.

**Rule workflow:**
1. For each guild zone with an alive impostor NPC:
2. Build `./npc/<name>` path (spaces → underscores).
3. Open/create binary file; on failure log and return.
4. Write raw `STRUCT_MOB`; close.

---

### Business Rule: Login fail-account reset

**Overview:**
The component periodically clears the failed-login accounting table so that transient login failures do not permanently lock accounts.

**Detailed description:**
Lines 2136-2137 run when `MinCounter % 10 == 0` (every 10 low-frequency ticks, i.e., every 2 minutes at 12 s each) and zero the `FailAccount` array via `memset`. `FailAccount` tracks login-failure counts used elsewhere in the login flow to throttle repeated failed attempts; this rule periodically resets that state, providing a lockout-window rather than a permanent lock.

**Rule workflow:**
1. Every 10 min ticks → `memset(FailAccount, 0, sizeof(FailAccount))`.

---

### Business Rule: Initial item open-to-lock state transitions

**Overview:**
The component advances pre-placed world items from an "open" state to a "locked" state after a one-tick delay, broadcasting the new state to clients.

**Detailed description:**
Lines 2141-2209 iterate initial world items from index 17 to `g_dwInitItem`. For each item with a valid index (`0 < sIndex < MAX_ITEMLIST`), it reads the item's ability via `BASE_GetItemAbility(&item, 39)`. If the ability is non-zero and the item is in `STATE_OPEN` with `iKey < 15`, then: if `Delay == 0`, it sets `Delay = 1` and continues (deferring one tick); otherwise it calls `UpdateItem(ipg, 3, ...)`, sets the item's state to `STATE_LOCKED`, resets `Delay = 0`, and broadcasts a `MSG_CreateItem` via `GridMulticast` so clients render the locked state. (A commented-out alternative implementation is preserved in the block.) This implements a one-tick delay before locking pre-placed open items.

**Rule workflow:**
1. For each initial item with key ability 39 and `iKey < 15`:
2. If `STATE_OPEN`: set `Delay=1` first tick; on next tick, `UpdateItem`, set `STATE_LOCKED`, reset `Delay`, broadcast `MSG_CreateItem`.

---

### Business Rule: NPC generator minute-based respawn scheduling

**Overview:**
The component spawns NPC/mob generator groups on a minute-based schedule, with randomized respawn intervals that widen for rarer groups, and injects dungeon-event item drops for the rarest tier.

**Detailed description:**
Lines 2211-2280 iterate `mNPCGen.NumList` generators, skipping the fixed indices `{0,1,2,5,6,7}` and those with `MinuteGenerate <= 0`. For each, the modulo `mod = i % MinuteGenerate` is computed; when `MinCounter % MinuteGenerate == mod`, the group is spawned via `GenerateMob(i, 0, 0)`. The respawn interval is then re-randomized by tier: `500-1000` → `rand()%500+500`; `1000-2000` → `rand()%1000+1000`; `2000-3800` → `rand()%1800+2000`; `>=3800` → `rand()%1000+3800`. For the rarest tier (`>=3800`), if the `DUNGEONEVENT` flag is set, it spawns dungeon-item drops: a random position from `DungeonPos[rand()%30]`, a random count `RndL = rand()%5+5`, and for each, a `STRUCT_ITEM` with `sIndex = DungeonItem[rand()%10]`, bonuses via `SetItemBonus`, placed with `CreateItem(dpX, dpY, &PrizeItem, rand()%4, 1)`. This implements tiered, randomized respawn cadence plus a bonus-loot event.

**Rule workflow:**
1. For each eligible generator, compute modulo and spawn on match.
2. Re-randomize `MinuteGenerate` by tier.
3. For rarest tier + `DUNGEONEVENT`: spawn dungeon prize items.

---

### Business Rule: Weather system

**Overview:**
The component maintains the server's weather state, either evolving randomly on a probability schedule or forced to a configured value, and broadcasts changes to clients.

**Detailed description:**
Lines 2284-2311 compute `rndWeather = rand()%1200`. If `ForceWeather == -1` (no override), weather evolves probabilistically: clear (`CurrentWeather=0`) when `rndWeather` in `[0,260)`; rain/snow type 1 when in `[30,50)`; type 2 when in `[55,60)` — each only if the current weather differs, then `SendWeather()`. If `ForceWeather != -1`, the weather is forced: whenever `ForceWeather != CurrentWeather`, `CurrentWeather = ForceWeather` and `SendWeather()` broadcasts the change. This lets operators pin weather or let it fluctuate randomly.

**Rule workflow:**
1. Compute random weather value.
2. If no override: transition weather per probability windows; `SendWeather()` on change.
3. If override set: apply forced weather; `SendWeather()` on change.

---

### Business Rule: Guild tower-war process hook

**Overview:**
At the end of every low-frequency tick, the component invokes the guild tower-war state machine, which manages the channel tower-war event.

**Detailed description:**
Line 2313 calls `GuildProcess()`, the free-function wrapper for `CWarTower::GuildProcess(tm*)` (see `legacy/Code/TMSrv/CWarTower.cpp:42`). That machine broadcasts a 5-minute begin notice, spawns the `GTORRE` tower mob, and at minute 59 awards `Fame += 100` to the defending guild before resetting `GTorreState`. Because it is invoked every `ProcessMinTimer` (12 s) with the current time, the tower-war transitions are evaluated continuously against the wall clock. This is the final step of the low-frequency cycle.

**Rule workflow:**
1. End of each min tick → `GuildProcess()` → evaluate tower-war state machine against current time.

---

## 4. Component Structure

The component is a single translation unit with no internal class hierarchy; it exposes two free functions declared in `legacy/Code/TMSrv/Server.h:105-106` and compiled as part of the TMSrv project (`legacy/Code/TMSrv/TMSrv.vcxproj:104`, filters at `TMSrv.vcxproj.filters:273`).

```
legacy/Code/TMSrv/
├── ProcessSecMinTimer.cpp        # Component source (2314 lines)
│   ├── ProcessSecTimer()          # High-frequency world update (0.5 s cadence)
│   │   ├── Server shutdown sequence            (lines 42-119)
│   │   ├── Macblock refresh                     (127-128)
│   │   ├── Billing reconnection                 (130-167)
│   │   ├── Round-robin user auto-save           (169-199)
│   │   ├── Per-user socket flush                (201-221)
│   │   ├── Per-user ProcessorSecTimer()         (223-224)
│   │   ├── Celestial nightmare recall           (226-231)
│   │   ├── Balrog runetrack portal nav          (233-495)
│   │   ├── Secret-room / RvR broadcasts         (497-518)
│   │   ├── Quest-area clear + flag reset        (520-538)
│   │   ├── Premium/dated item expiry            (539-599)
│   │   ├── HP/MP regeneration                   (601-640)
│   │   ├── Kefra/nightmare/runetrack events     (642-1099)
│   │   ├── Ground item decay                    (1101-1102)
│   │   ├── Ranking + water-scroll cleanup       (1104-1130)
│   │   ├── Secret-room (Carta) cycle            (1132-1170)
│   │   ├── CCastleZakum::ProcessSecTimer()      (1172)
│   │   ├── Idle/regen/recall guards             (1174-1210)
│   │   ├── BR/kingdom/leader env effects        (1212-1337)
│   │   ├── Merchant-mob restock/respawn         (1339-1349)
│   │   ├── Non-user mob idle/peace AI           (1353-1552)
│   │   └── Non-user mob combat AI               (1554-2016)
│   └── ProcessMinTimer()          # Low-frequency world update (12 s cadence)
│       ├── Log rotation                         (2027-2034)
│       ├── Newbie/castle server rotation        (2036-2071)
│       ├── Guild-impostor NPC save              (2074-2104)
│       ├── CCastleZakum::ProcessMinTimer()      (2110)
│       ├── Kingdom clear-area reset             (2112-2133)
│       ├── FailAccount reset                    (2136-2137)
│       ├── Init-item open→lock transitions      (2141-2209)
│       ├── NPC generator respawn scheduling     (2211-2280)
│       ├── Weather system                       (2284-2311)
│       └── GuildProcess() (tower war)           (2313)
└── Server.h                     # Declarations (lines 105-106) + extern globals
```

The component is a **procedural driver** over shared global state; it has no member data, no constructors, and no internal decomposition beyond its two functions and their inline `#pragma region` blocks.

---

## 5. Dependency Analysis

### Internal Dependencies (functions called from this component)

The component calls into the TMSrv module's shared helpers, which are declared in `Server.h`, `GetFunc.h`, `SendFunc.h`, `ProcessClientMessage.h`, `ProcessDBMessage.h`, `CItem.h`, and `CReadFiles.h`, plus `CCastleZakum` and `CWarTower` classes and `CPSock`/`CUser` methods. Representative chains:

```
ProcessSecTimer()
├── CReadFiles::ReadMacblock()              # macblock data refresh
├── BillServerSocket.ConnectBillServer()    # billing reconnection (CPSock.cpp:257)
├── SendBilling2()                          # billing handshake (Server.cpp:1382)
├── CloseUser()                             # socket teardown (CUser.cpp:81)
├── SaveUser() → DBServerSocket.SendOneMessage  # persistence (Server.cpp:7056)
├── CharLogOut() → SaveUser() + DeleteMob() # forced logout (Server.cpp:7085)
├── pMob[].ProcessorSecTimer()              # per-user tick
├── DoRecall() / DoTeleport()               # movement (Server.cpp)
├── GenerateMob() / DeleteMob()             # world population (Server.cpp)
├── ApplyHp() / ApplyMp() / RegenMob()      # stats (Server.cpp)
├── ProcessAffect() / SetAffect() / SetTick()  # status effects (Server.cpp)
├── CheckIdle()                             # anti-idle (Server.cpp:4304)
├── ProcessRanking() / ProcessDecayItem()   # global subsystems (Server.cpp)
├── StandingByProcessor() / BattleProcessor() / GetRandomPos()  # CMob methods
├── GetAttack() / GetAttackArea() / GetAction()  # combat resolution
├── GetParryRate() / GetInHalf() / SetBattle()   # combat rules
├── MobKilled()                             # death/reward (MobKilled.cpp)
├── LinkMountHp() / ProcessAdultMount()     # mount handling (Server.cpp)
├── PutItem() / CreateItem() / SetItemBonus()    # item creation
├── SendItem() / SendScore() / SendSetHpMp() / SendNotice() / SendWeather()
├── GridMulticast() / MapaMulticast() / SendSignalParmArea() / SendShortSignalParm2Area()
├── SendEnvEffectKingdom() / SendDamageKingdom() / SendEnvEffectLeader() / SendDamageLeader()
├── ClearArea() / ClearAreaQuest() / ClearAreaTeleport() / DeleteMobMapa() / ClearMapa()
├── CCastleZakum::ProcessSecTimer() / ProcessMinTimer()
└── GuildProcess()                          # CWarTower (CWarTower.cpp:42)

ProcessMinTimer()
├── StartLog() / StartChatLog() / StartItemLog()
├── RemoveTrade()
├── BASE_GetWeekNumber() / BASE_GetItemAbility()
├── UpdateItem() / CreateItem() / SetItemBonus()
├── GenerateMob()
├── SendWeather()
├── CCastleZakum::ProcessMinTimer()
└── GuildProcess()
```

### External Dependencies

| Dependency | Type | Purpose | Notes |
|-----------|------|---------|-------|
| Win32 API | Platform | Timers, sockets, file I/O, message loop | `SetTimer`, `timeGetTime`, `PostQuitMessage`, `_open`/`_write`/`_close` |
| Winsock (WSAAsyncSelect) | Platform | Client + billing + DB sockets | Async network via window messages |
| Billing server | External service | Authentication/billing handshake + reconnect | Via `BillServerSocket.ConnectBillServer`/`SendBilling2` |
| DB server (DBSrv) | Internal service | Character persistence (`MSG_DBSaveMob`) | Via `DBServerSocket.SendOneMessage` |
| Filesystem | External | Logs, `./npc/` guild impostor save, debug dumps | `./Log/*.txt`, `./npc/<name>`, `AttackDieTeste.txt`, `Gethalf.txt` |
| C runtime / stdlib | Library | `rand()`, `time()`, `localtime()`, `sprintf`, `memcpy` | Deterministic-ish PRNG usage |

---

## 6. Afferent and Efferent Coupling

Because the codebase is procedural C++ over global state, the coupling "components" are the top-level functions and the shared global structures they touch. Coupling counts are approximate, derived from call/global references in the translation unit.

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|-------------------|
| ProcessSecTimer() | 1 (Server.cpp:3726) | ~55 distinct callees + ~40 global arrays/scalars | High |
| ProcessMinTimer() | 1 (Server.cpp:3728) | ~20 distinct callees + ~20 globals | High |
| pUser[] / pMob[] global arrays | High (touched throughout both) | n/a (data store) | High |
| Pista[7] / PistaPos / PistaRune | Medium (event scheduling) | n/a (data store) | High |
| SecCounter / MinCounter / ServerDown | High (tick scheduling) | n/a (state) | Medium |

**Assessment:** Both functions have maximal afferent coupling (single timer-driven call site each) but extreme efferent coupling — they reach into nearly every subsystem of the server. `ProcessSecTimer` in particular is a critical hub: it touches user persistence, networking, all mob AI, all event scheduling, item management, and status effects. Any change to a shared subsystem (e.g., `pMob` layout, `GetAttack`, `GenerateMob`) has a direct blast radius through this file. The component's cohesion is low in the sense that it aggregates many unrelated responsibilities into one function; its coupling to global mutable state is maximal.

---

## 7. Endpoints

This component does **not** expose any network endpoints (REST, GraphQL, gRPC, or client message handlers). It is a server-side internal driver invoked by Win32 timers and has no request/response surface. The `_MSG_*` messages it *sends* (`_MSG_MobLeft`, `_MSG_MobCount`, `_MSG_StartTime`, `_MSG_CreateItem`, `_MSG_UpdateItem`) are outbound client notifications, not inbound endpoints. This section is therefore intentionally omitted per guidelines.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Billing server | External Service | Billing/auth handshake + reconnection | Winsock TCP (async WSAAsyncSelect, `WSA_READBILL`) | `_AUTH_GAME` struct (196 B) via `SendBilling2` | Countdown retry; `BILLING=0` on reconnect failure (`:130-167`) |
| DB server (DBSrv) | Internal Service | Character persistence on save/logout | Winsock TCP (`DBServerSocket.SendOneMessage`) | `MSG_DBSaveMob` struct | Fire-and-forget send; no ack handling in this component |
| Client sockets | Internal | Outbound notifications (score, item, mob, area messages) | Winsock TCP (send queue) | `MSG_*` structs | Send failure → `CloseUser` (`:201-221`) |
| Filesystem (logs) | External | Server/chat/item logs | File I/O | Text (`Log`/`StartLog`/etc.) | fopen fail returns; logged |
| Filesystem (`./npc/`) | External | Guild-impostor NPC persistence | Raw binary file I/O (`_open`/`_write`/`_close`) | Raw `STRUCT_MOB` | On open fail → log + return (`:2097-2101`) |
| Filesystem (debug dumps) | External | Combat diagnostics | File I/O (append) | Text (`AttackDieTeste.txt`, `Gethalf.txt`) | fopen null-checked in `Gethalf.txt` path only |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Scheduler / Timer tick | Two cadences via `SetTimer` + `WM_TIMER` dispatch | Server.cpp:3631-3632, 3723-3729 | Drive periodic world updates |
| Round-robin work distribution | `SecCounter % N` modulo gating per subsystem | throughout (`% 8`, `% 12`, `% 20`, `% 120`, `% 1200`) | Spread cost across ticks |
| Slot-offset distribution | `Sec4`/`Sec6`/`Sec16`/`Sec32` index phases | `:1106`, `:1174`, `:1353`, `:1554` | Distribute per-entity work over N ticks |
| State machine (bitmask) | `StandingByProcessor()`/`BattleProcessor()` bit flags | `:1406`, `:1621` | Drive mob behavior |
| Anti-exploit guards | Recall rules keyed to class/level/area | `:226-231`, `:1191-1209` | Enforce content boundaries |
| Time-based scheduling | `localtime()` minute/hour/weekday checks | `:642-1099`, `:2027-2071` | World-event timetables |
| Randomized respawn | `rand()%N` re-seeded intervals | `:2224-2276` | Add variance to spawn cadence |
| Centralized logging | `Log()`/`StartLog()` wrappers | throughout | Auditable server state |
| Data-persistence adapter | `SaveUser()` → `MSG_DBSaveMob` → DB socket | Server.cpp:7056-7083 | Decouple save format from callers |

**Architectural observations:** The component follows a **centralized tick-orchestrator** architecture rather than an event-driven component boundary. There is no dependency injection or configuration-driven behavior for the world/event rules — nearly all rules are hard-coded literals (map rectangles, item indices, generator indices, schedule minutes). The architecture relies on a single global `pMob[]`/`pUser[]` array space shared across the whole server, which maximizes coupling. The `CCastleZakum` and `CWarTower` integrations show the only class-encapsulated subsystems, invoked statically from this component.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | ProcessSecTimer() | Single 1978-line god function aggregating dozens of unrelated subsystems | Extreme complexity, poor readability, high regression risk |
| High | Global mutable state | Operates entirely on shared `pUser`/`pMob`/`Pista` arrays; no encapsulation | Race/ordering sensitivity, hard-to-test logic |
| High | Event rules | Map coordinates, item indices, and schedules hard-coded as literals | Brittle to content changes; cannot be reconfigured at runtime |
| High | Rune-track rewards | Copy-paste index bugs (`PutItem(Pista[1].Party[0]...)` at `:882`; `PutItem(Pista[3].Party[3]...)` at `:980`) award the wrong party | Incorrect prize distribution |
| High | Rune-track rewards | `PutItem(Pista[1].Party[2].LeaderID)` in party-3 branch inconsistent with leader used for members | Leader/member reward mismatch |
| Medium | Debug dumps | Production code writes `AttackDieTeste.txt` / `Gethalf.txt` | Unbounded debug file growth, I/O on hot combat path |
| Medium | Kefra attack | `if (0 == 1)` dead branch with `goto KefraAttackLabel` | Dead/deactivated code, confusing control flow |
| Medium | Macblock refresh | `if(SecCounter % 120)` reads macblock on nearly every tick (only skips multiples of 120) | Likely inverted intent; possible redundant I/O |
| Medium | Disabled hook | `CEncampment::ProcessSecTimer()` commented out (`:1351`, `:2045`) | Dead/inactive subsystem |
| Medium | `if (1 != 0)` flag | Always-true debug flag around castle-server assignment (`:2060`) | Unclear intent, always-executing block |
| Medium | Persistence | Guild-impostor NPCs written as raw binary files with no atomic write/validation | Corrupt state on partial write/crash |
| Medium | Timer semantics | Names `SecTimer`/`MinTimer` imply seconds/minutes but run at 0.5 s / 12 s | Misleading; cadence derived from constants, not the names |
| Low | Non-determinism | `rand()` without documented seeding for many gameplay decisions | Non-reproducible behavior |
| Low | Error handling | File open failures inconsistently handled (some logged, some silent) | Partial observability |

---

## 11. Test Coverage Analysis

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| ProcessSecTimer() | 0 | 0 | 0% | No tests exist |
| ProcessMinTimer() | 0 | 0 | 0% | No tests exist |
| Whole legacy codebase | 0 | 0 | 0% | No test framework present |

**Test file locations:** A project-wide search for test artifacts (`*test*`, `*spec*`, `gtest`, `catch2`, `CppUnit`, `TEST(`) across `legacy/` returned **no test files** and no test framework. The only matches in the repository live under the ignored `.opencode/node_modules` directory (Node.js dependency tests, unrelated to this C++ codebase).

**Assessment:** The component — and the legacy server as a whole — has **zero automated test coverage**. The logic is almost entirely untested, which compounds the risk of the hard-coded business rules and the numerous content-specific branches (Balrog portals, nightmare schedules, rune-track rewards, premium-item expiry, mob AI bitmasks). There are no unit tests for the timer functions, no integration tests for the scheduling logic, and no fixtures or mocks for the external billing/DB integrations. The diagnostic dump files (`AttackDieTeste.txt`, `Gethalf.txt`) suggest ad-hoc manual verification rather than automated testing. This is a significant risk given the component's centrality to world simulation.

---

## 12. Save Location

Report saved to: `/home/luisdias/dev/github/luisdiasdev/w2pp/docs/reports/component-deep-analyzer/component-analysis-ProcessSecMinTimer-2026-08-19 17:13:23.md`

Component analyzed: **ProcessSecMinTimer**
