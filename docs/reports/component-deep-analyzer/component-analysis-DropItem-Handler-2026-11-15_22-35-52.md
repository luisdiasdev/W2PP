# Component Deep Analysis Report - DropItem Handler

Component: `DropItem Handler` (`Exec_MSG_DropItem`)
Primary location: `Code/TMSrv/_MSG_DropItem.cpp` (146 lines, 3,970 bytes / ~4 KB)
Project: W2PP - decompiled C/C++ (Win32, MSVC v143) MMORPG game server emulator for W2
Analysis scope: entire project root (`E:\wyd\test-setup\w2pp`), excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
Analysis date: 2026-11-15
Report type: read-only component analysis (no project file was modified)

---

## 1. Executive Summary

### 1.1 Purpose

`Exec_MSG_DropItem` is the TMSrv command handler for the client-to-game message `_MSG_DropItem`
(`Code/Basedef.h:1638`, opcode `114 | FLAG_CLIENT2GAME` = `0x0272` = 626). It implements one business
transaction: *move one item object out of a player-owned container (`MOB.Equip[]`, `MOB.Carry[]` or
`pUser[].Cargo[]`) and register it as a world ground object at a grid coordinate the client proposes*.

The component is a single free function with no class, no header of its own, and no state beyond the
process-wide globals it touches. It declares itself only through `Code/TMSrv/ProcessClientMessage.h:103`
and is wired into the dispatcher at `Code/TMSrv/ProcessClientMessage.cpp:198-200`. It is the exact
counterpart of `Exec_MSG_GetItem` (`Code/TMSrv/_MSG_GetItem.cpp:20`): drop creates and registers a world
object through `CreateItem` (`Code/TMSrv/Server.cpp:7254`), pickup de-registers the same object.

The handler is 146 lines of straight-line `if`/`return` validation followed by a 28-line commit block
(`_MSG_DropItem.cpp:112-139`). It owns no data structure; it orchestrates four collaborators:
`GetEmptyItemGrid` (`Code/TMSrv/GetFunc.cpp:1876`), `GetItemPointer` (`Code/Basedef.cpp:2340`),
`CreateItem` (`Code/TMSrv/Server.cpp:7254`) and the socket/logging helpers.

### 1.2 Role in the system

- Layer: TMSrv command-handler layer (`Code/TMSrv/_MSG_*.cpp`), one of the components catalogued at
  `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:141`.
- Architectural classification (per that report): *Command Handler (inventory)*, afferent 1, efferent 14,
  "drop placement into the item grid".
- Runtime call chain:
  `WSA_READ (Server.cpp:3919) -> CPSock::ReadMessage (CPSock.cpp:353) -> ProcessClientMessage
  (ProcessClientMessage.cpp:38) -> Exec_MSG_DropItem -> [grid resolution -> world item creation -> broadcast]`.
- Authoritative ownership: it is one of only five live creators of world items through `CreateItem`
  (others: `Server.cpp:7173` boot items, `Server.cpp:6306`, `MobKilled.cpp:2193` mob loot,
  `ProcessSecMinTimer.cpp:2273` event prize) and the only creator initiated by a player command.
- It is the *only* path by which a player voluntarily destroys/releases inventory items in the compiled
  configuration: the alternative player-death drop path in `MobKilled.cpp:2305-2420` is compiled out
  because `PKDrop` is commented out at `Code/Basedef.h:29`.
- The component does not persist anything itself. Persistence is asynchronous through `SaveUser`
  (`Server.cpp:7056`, one user per timer tick at `ProcessSecMinTimer.cpp:189`, plus save-on-logout at
  `Server.cpp:7117`), so the drop commit is memory-only.

### 1.3 Key findings

1. **Strictly ordered validate-then-commit, but 14 of 15 failure exits are silent.** Only three rejection
   paths send text to the client (`_NN_CantWhenAutoTrade`, `_NN_Cant_Drop_Here`,
   `_NN_Guild_Medal_Cant_Be_Droped`); the remaining eleven produce only a server log or nothing at all
   (`_MSG_DropItem.cpp:24-107`). A legitimate player whose drop is refused by the size/shape of the
   container, by an invalid item index, or by the feature flag receives no feedback.
2. **The free-cell search runs before the item is validated.** Grid resolution and the packet mutation
   (`:54-66`) happen before the source container, source position, item index and blacklist checks
   (`:68-110`). A rejected drop has already consumed a 3x3 grid scan and rewritten `m->GridX/m->GridY`.
3. **No proximity check on the drop coordinate.** The handler validates only `GridX < 4096` and
   `GridY < 4096` (`:44-49`); it never compares the target cell against `pMob[conn].TargetX/TargetY`.
   By contrast the pickup side enforces a 3-tile radius (`_MSG_GetItem.cpp:74-82`). A modified client can
   therefore place an item anywhere on the 4096x4096 map, including an area it cannot reach.
4. **Item policy is a hard-coded, divergent blacklist.** Eight index tests over `{508, 509, 522, 446, 747,
   3993, 3994} u [526,537]` (`:109-110`). The same conceptual policy is written differently in
   `_MSG_Trade.cpp:147-177`, `_MSG_SendAutoTrade.cpp:80-82`, `_MSG_Sell.cpp:92-93`,
   `_MSG_TradingItem.cpp:165-179` and `MobKilled.cpp:2321-2325`, with different index sets. The
   engine's declarative `EF_NOTRADE` flag (`Code/ItemEffect.h:153`), which both trade paths honour, is
   *not* consulted here.
5. **`SrcItem` and `BackupItem` are the same pointer.** Lines 99-100 call `GetItemPointer` twice with
   identical arguments; no copy is taken and `BackupItem` is only null-tested. The apparent rollback
   mechanism is a no-op, so the commit block has no undo path.
6. **The commit order is: create world object -> write audit log -> zero the source -> confirm.** Between
   `CreateItem` (`:112`) and `memset(SrcItem, ...)` (`:125`) there is no failure exit, but the client is
   only told afterwards (`:127-139`) and only through `MSG_CNFDropItem`; no inventory re-sync packet
   (`SendItem`, `SendFunc.cpp:1055`) is emitted. A lost confirmation leaves the client showing an item the
   server no longer owns.
7. **Ground lifetime is an implicit ~90-second decay.** `CreateItem` writes `Delay = 90`, `Decay = 0`
   (`Server.cpp:7282-7283`); `ProcessDecayItem` (`Server.cpp:8353`) decrements `Delay` once per
   `SecCounter % 2` tick (`ProcessSecMinTimer.cpp:1101-1102`) with `SecCounter` advanced on a 500 ms timer
   (`Server.cpp:3631`), i.e. roughly 90 s of world lifetime for every dropped item. `Decay` is never set
   to `-1` anywhere in the code base, so the `Decay != -1` guard (`Server.cpp:8374`) is dead: no dropped
   item is permanent (except archetype 1727 and `Mode == 2` treasure objects, both skipped explicitly).
8. **Decay clears the item-grid cell with transposed indices.** `Server.cpp:8381` writes
   `pItemGrid[itemPosX][itemPosY] = 0` while the array is declared `pItemGrid[MAX_GRIDY][MAX_GRIDX]`
   (`Server.cpp:364`) and every other writer uses `[y][x]` (e.g. `_MSG_GetItem.cpp:248`). The decayed
   item's real cell therefore keeps a stale non-zero index while an unrelated cell is cleared. This is a
   grid-integrity defect created by the drop path's data and inherited by the pickup path.
9. **Currency is dropped through the same item path with no dedicated rule.** Nothing in the handler
   inspects `EF_VOLATILE`; a money object (`EF_VOLATILE == 2`, `Code/ItemEffect.h:77`) that is not
   blacklisted is droppable as ordinary cargo. Its value is carried in `EF_HWORDCOIN`/`EF_LWORDCOIN` and is
   only read by the pickup branch (`_MSG_GetItem.cpp:180-202`), which credits `MOB.Coin` with a
   2,000,000,000 ceiling. Drop never validates or escrows the amount.
10. **The rotation field is passed through unvalidated, and `ItemID` is ignored.** `m->Rotate` (an `int`)
    is stored verbatim into `pItem[].Rotate` (`Server.cpp:7279`) and truncated into the `unsigned char`
    of `MSG_CreateItem` (`Basedef.h:1602`) but echoed in full in `MSG_CNFDropItem` (`Basedef.h:1856`).
    `MSG_DropItem.ItemID` (`Basedef.h:1647`) is never read by the handler.
11. **Protocol length validation is disabled project-wide.** `_MSG_DropItem` is listed in
    `BASE_CheckPacket` at `Basedef.cpp:6518`, but the entire function body is commented out
    (`Basedef.cpp:6476-6594`) and it returns `FALSE`. `CPSock::ReadMessage` only bounds the frame to
    `[sizeof(HEADER), 8192]` (`CPSock.cpp:397`), so a short `_MSG_DropItem` frame is cast to the full
    32-byte struct and its missing fields are read from adjacent buffer bytes.
12. **Zero automated test coverage.** No test project (`*.vcxproj` are only `TMSrv` and `DBSrv`), no test
    framework, no `assert`, no fixtures, no CI workflow (Section 11).

---

## 2. Data Flow Analysis

### 2.1 Macro flow (network frame to world state)

```
 1. Client sends frame Type = 114 | FLAG_CLIENT2GAME = 0x0272, Size in the 12-byte HEADER
 2. CPSock::ReadMessage (CPSock.cpp:353) frames by Size, rejects Size outside [12, 8192]
    (CPSock.cpp:397), decrypts and verifies the per-byte checksum (CPSock.cpp:424-470)
 3. Server.cpp:3975 WSA_READ loop -> ProcessClientMessage(User, Msg, FALSE)      [Server.cpp:4001]
 4. ProcessClientMessage: ID range gate (:42), ServerDown gate (:53),
    LastReceiveTime stamp (:57), SKIPCHECKTICK gate (:63)
 5. switch(std->Type) -> case _MSG_DropItem -> Exec_MSG_DropItem(conn, pMsg)    [cpp:198-200]
 6. Cast frame to MSG_DropItem*                                                 [_MSG_DropItem.cpp:22]
 7. Session gate: HP <= 0 or Mode != USER_PLAY -> AddCrackError(1,14)
    + SendHpMode + return                                                      [24-29]
 8. Trade gate: Trade.OpponentID != 0 -> RemoveTrade(opponent) + RemoveTrade(self)
    + return                                                                   [31-36]
 9. Personal-shop gate: TradeMode != 0 -> _NN_CantWhenAutoTrade + return        [38-42]
10. Coordinate range gate: GridX >= 4096 or GridY >= 4096 -> Log + return       [44-49]
11. Feature-flag gate: isDropItem == 0 -> silent return                         [51-52]
12. Local copy of requested cell: gridx/gridy = m->GridX/m->GridY               [54-55]
13. Grid resolution: GetEmptyItemGrid(&gridx,&gridy)                           [57]
    -> writes resolved cell back into the packet                                 [59-60]
14. Resolution failure (no free cell in 3x3): _NN_Cant_Drop_Here + return       [62-66]
15. Source-type gate: SourType == ITEM_PLACE_EQUIP (0) -> Log + return          [68-72]
16. Source-position gate (CARRY 0..MaxCarry-1 / CARGO 0..127 / anything else)   [74-97]
17. Resolve source item: SrcItem = GetItemPointer(MOB, Cargo, SourType, SourPos) [99]
    plus a second identical call assigned to BackupItem (never used later)       [100]
18. Null gate: SrcItem == NULL || BackupItem == NULL -> silent return           [103-104]
19. Archetype gate: sIndex <= 0 or sIndex >= MAX_ITEMLIST (6500) -> silent      [106-107]
20. Policy gate: blacklist {508,509,522,446,747,3993,3994} u [526,537]
    -> _NN_Guild_Medal_Cant_Be_Droped + return                                  [109-110,141-145]
21. Commit: CreateItem(GridX, GridY, SrcItem, Rotate, 1)                        [112]
    -> CreateItem re-resolves the cell, allocates a free pItem slot,
       copies STRUCT_ITEM, sets Mode=1/State=STATE_OPEN/Delay=90/Decay=0,
       GridCharge = EF_GROUND ability, Height = pHeightGrid[y][x],
       writes pItemGrid[y][x] = slot, broadcasts MSG_CreateItem 33x33          [Server.cpp:7254-7317]
22. Creation failure: raw literal "Can't create object(item)" + return          [114-118]
23. Audit: BASE_GetItemCode -> ItemLog("dropitem, <name/id/effects>")           [120-123]
24. Commit point: memset(SrcItem, 0, sizeof(STRUCT_ITEM))                       [125]
25. Confirmation: MSG_CNFDropItem {SourType, SourPos, Rotate, GridX, GridY}
    sent with SendOneMessage (immediate flush)                                  [127-139]
26. Asynchronous consequences: world decay timer (Server.cpp:8353) and the
    pickup handler Exec_MSG_GetItem (_MSG_GetItem.cpp:20) end the object's life
```

### 2.2 Exit map (all 15 exits, in evaluation order)

| # | Location | Condition | Server reaction | Client reaction | World state |
|---|----------|-----------|-----------------|-----------------|-------------|
| 1 | `:24-29` | `MOB.CurrentScore.Hp <= 0` or `Mode != USER_PLAY` | `AddCrackError(conn,1,14)` + `SendHpMode` | HP/mode refresh only | unchanged |
| 2 | `:31-36` | `Trade.OpponentID != 0` | `RemoveTrade()` both sides | trade window close signal (900) | inventory unchanged, trade cancelled |
| 3 | `:38-42` | `TradeMode != 0` | `_NN_CantWhenAutoTrade` | panel message | unchanged |
| 4 | `:44-49` | `GridX >= 4096` or `GridY >= 4096` | `Log("err,wrong drop pos x y")` | none | unchanged |
| 5 | `:51-52` | `isDropItem == 0` | none | none | unchanged |
| 6 | `:62-66` | `GetEmptyItemGrid` returned FALSE | `_NN_Cant_Drop_Here` | panel message | unchanged (packet cell already rewritten, `:59-60`) |
| 7 | `:68-72` | `SourType == ITEM_PLACE_EQUIP` | `Log("err,dropitem - sourtype")` | none | unchanged |
| 8 | `:78-82` | `SourType == CARRY` and `SourPos < 0 or >= MaxCarry` | `Log("err,dropitem - carry equip")` | none | unchanged |
| 9 | `:86-90` | `SourType` neither CARRY nor CARGO | `Log("err,dropitem - sourtype")` | none | unchanged |
| 10 | `:91-95` | `SourType == CARGO` and `SourPos < 0 or >= 128` | `Log("err,dropitem - sourpos cargo")` | none | unchanged |
| 11 | `:103-104` | `GetItemPointer` returned NULL | none | none | unchanged |
| 12 | `:106-107` | `sIndex <= 0` or `sIndex >= 6500` | none | none | unchanged |
| 13 | `:114-118` | `CreateItem` returned FALSE/0 | none (raw literal message) | "Can't create object(item)" | unchanged, source intact |
| 14 | `:141-145` | blacklisted archetype | `_NN_Guild_Medal_Cant_Be_Droped` | panel message | unchanged |
| 15 | `:112-139` | all guards passed | audit log + source cleared | `MSG_CNFDropItem` | new world object + empty source slot |

Note the ordering consequence: exit 14 (policy) is evaluated *after* exit 6 (grid resolution), so a
blacklisted item dropped onto a cell with no free neighbour reports "can't drop here" instead of the
policy message.

### 2.3 State written by a successful drop

| Global | Field | Before | After | Line |
|--------|-------|--------|-------|------|
| `pItem[slot]` (`CItem`, `Server.cpp:279`, `CItem.h:24-45`) | `Mode` | 0 (free) | 1 | `Server.cpp:7272` |
| | `PosX`, `PosY` | stale | resolved cell | `7274-7275` |
| | `ITEM` | stale | copy of source `STRUCT_ITEM` | `7277` |
| | `Rotate` | stale | client-supplied `int` | `7279` |
| | `State` | stale | `STATE_OPEN` (1) | `7280` |
| | `Delay` | stale | 90 | `7282` |
| | `Decay` | stale | 0 | `7283` |
| | `GridCharge` | stale | `BASE_GetItemAbility(item, EF_GROUND)` | `7285` |
| | `Height` | stale | `pHeightGrid[y][x]` | `7289` |
| `pItemGrid[y][x]` | cell | 0 | `slot` | `7287` |
| `pMob[conn].MOB.Equip/Carry[pos]` or `pUser[conn].Cargo[pos]` | whole struct | item | zeroed | `_MSG_DropItem.cpp:125` |
| `pUser[conn].cSock` send buffer | — | — | `MSG_CNFDropItem` flushed | `:139` |

The player's `STRUCT_MOB` (which contains `Equip[]`/`Carry[]`, `Basedef.h:458-459`) is only persisted to
the database at the next rotation save or logout; `pUser[conn].Cargo[]` travels in the same save
(`SaveUser`, `Server.cpp:7065`).

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | Validation (session) | A drop is only processed for a character with `Hp > 0` and `Mode == USER_PLAY`; violations count as a "crack" error and force an HP/mode refresh | `_MSG_DropItem.cpp:24-29` |
| BR-02 | Business logic (concurrency) | While a player-to-player trade session exists (`Trade.OpponentID != 0`), a drop aborts the trade for both sides and performs no drop | `_MSG_DropItem.cpp:31-36` |
| BR-03 | Business logic (mode) | While a personal shop / auto-trade session is open (`TradeMode != 0`) dropping is refused with `_NN_CantWhenAutoTrade` | `_MSG_DropItem.cpp:38-42` |
| BR-04 | Validation (bounds) | Drop coordinates must satisfy `GridX < MAX_GRIDX (4096)` and `GridY < MAX_GRIDY (4096)`; violations are logged and dropped silently | `_MSG_DropItem.cpp:44-49` |
| BR-05 | Configuration | Dropping is globally enabled only when `isDropItem != 0` (gameconfig.txt "DropItem", GM command `dropitem`) | `_MSG_DropItem.cpp:51-52`; `Server.cpp:376,1109`; `imple.cpp:1766-1773` |
| BR-06 | Business logic (placement) | The requested cell is used only if `pItemGrid == 0` and `pHeightGrid != 127`; otherwise the 8 surrounding cells are scanned in `y-1..y+1`, `x-1..x+1` order and the first free cell is substituted | `_MSG_DropItem.cpp:57-66`; `GetFunc.cpp:1876-1899` |
| BR-07 | Business logic (eligibility) | Worn equipment (`SourType == ITEM_PLACE_EQUIP == 0`) can never be dropped | `_MSG_DropItem.cpp:68-72`; `Basedef.h:86` |
| BR-08 | Validation (source) | `CARRY` sources must satisfy `0 <= SourPos < pMob[conn].MaxCarry`; `CARGO` sources `0 <= SourPos < MAX_CARGO (128)`; any other non-zero `SourType` is rejected | `_MSG_DropItem.cpp:74-97` |
| BR-09 | Validation (item) | The resolved source must be non-null and carry an archetype in `(0, MAX_ITEMLIST=6500)` | `_MSG_DropItem.cpp:103-107` |
| BR-10 | Business logic (policy) | Guild-bound, medal and crown archetypes `{508, 509, 522, 446, 747, 3993, 3994} u [526,537]` can never be dropped | `_MSG_DropItem.cpp:109-110,141-145` |
| BR-11 | Business logic (creation) | A droppable item becomes a world object through `CreateItem`, which re-validates the archetype, finds a free grid cell, allocates a free `pItem` slot (1..4999), copies the payload, sets `State=STATE_OPEN`, `Delay=90`, `Decay=0`, computes `GridCharge = EF_GROUND`, registers `pItemGrid` and multicasts `MSG_CreateItem` to the 33x33 view | `_MSG_DropItem.cpp:112`; `Server.cpp:7254-7317` |
| BR-12 | Error handling | If `CreateItem` fails the source item is preserved and the player gets the raw literal `"Can't create object(item)"` | `_MSG_DropItem.cpp:114-118` |
| BR-13 | Audit | Every successful drop writes `dropitem, <name/id/effects>` to the daily item log before the source is cleared | `_MSG_DropItem.cpp:120-123`; `Basedef.cpp:1479-1495`; `Server.cpp:9264-9284` |
| BR-14 | Business logic (commit) | The source slot is zeroed only after the world object exists and the audit record is written | `_MSG_DropItem.cpp:125` |
| BR-15 | Protocol | The dropper receives `MSG_CNFDropItem` with the resolved `GridX/GridY` and the original `SourType/SourPos/Rotate`; no inventory re-sync is sent | `_MSG_DropItem.cpp:127-139`; `Basedef.h:1850-1859` |
| BR-16 | Business logic (lifetime) | A dropped object is unconditionally transient: `Delay = 90` is consumed by `ProcessDecayItem` every `SecCounter % 2` tick (500 ms timer => ~90 s), then the object and its grid cell are released | `Server.cpp:7282-7283,8353-8396`; `ProcessSecMinTimer.cpp:1101-1102` |
| BR-17 | Business logic (currency) | Currency objects (`EF_VOLATILE == 2`) follow the ordinary drop path; the value is not inspected on drop and is credited to `MOB.Coin` on pickup with a 2,000,000,000 ceiling | `_MSG_DropItem.cpp` (no branch); `_MSG_GetItem.cpp:180-202`; `ItemEffect.h:77` |
| BR-18 | Protocol | Client-supplied `Rotate` is stored unvalidated; client-supplied `ItemID` is ignored entirely | `_MSG_DropItem.cpp:112,133-137`; `Basedef.h:1639-1648` |
| BR-19 | Idempotency | A replayed drop packet cannot duplicate the item because the source archetype becomes 0 and the handler silently returns | `_MSG_DropItem.cpp:106-107` |
| BR-20 | Placement authority (gap) | No rule constrains the drop cell to the player's vicinity, map region, or reachable terrain other than `pHeightGrid == 127` | `_MSG_DropItem.cpp:44-57` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 - Session eligibility gate

**Overview:**
A drop request is only meaningful for a live, fully logged-in character. The handler therefore opens
with a compound session guard that rejects any packet arriving from a connection whose character is dead
(`pMob[conn].MOB.CurrentScore.Hp <= 0`) or whose user record is not in the in-game state
(`pUser[conn].Mode != USER_PLAY`).

**Detailed description:**

`USER_PLAY` is the in-game mode constant `22` (`Code/TMSrv/CUser.h:36`); every other mode (`USER_EMPTY`,
`USER_SELCHAR`, `USER_CHARWAIT`, `USER_WAITDB`, `USER_SAVING4QUIT`) means the character is not currently
authoritative for world actions, and the inventory pointers the handler would dereference are not
guaranteed to be populated. The HP test is the second half of the same idea: a dead character is still
`USER_PLAY` until the death handling changes the mode, so `Hp <= 0` is the only reliable "not able to
act" test at packet level. Both halves are evaluated in one `if` with short-circuit `||`, and the
handler does not distinguish between them in the log.

The reaction is deliberately different from every other rejection: instead of a message, the server calls
`AddCrackError(conn, 1, 14)` and `SendHpMode(conn)`. `AddCrackError` (`Server.cpp:684-707`) is the
server's cheat/violation accumulator: it logs `cra point: 1  type: 14` (type 14 is not in the
suppression list `{3, 8, 15}`, so it *is* logged with account and IP), adds the weight `1` to
`pUser[conn].NumError`, and when that sum reaches `2,000,000,000` sends `_NN_Bad_Network_Packets` and
calls `CharLogOut`. `SendHpMode` (`SendFunc.cpp:1622-1645`) rebuilds `MSG_SetHpMode` so a client that
believes it is alive is corrected. The pairing is a deliberate anti-desync measure: the client is told
its real HP and mode, and the server records that this connection sent an action it should not have been
able to send.

The rule is not a security boundary by itself. It does not stop a player who is alive from dropping, and
it does not verify that the client's view of HP agrees with the server before the action. Its practical
effect is to protect the handler from dereferencing `pMob[conn].MOB` state that might be mid-login and to
feed the violation counter. Because the counter only trips at two billion, a single violation has no
consequence beyond a log line; the rule is therefore diagnostic, not punitive, for any realistic packet
rate on this opcode.

**Rule workflow:**

```
if (pMob[conn].MOB.CurrentScore.Hp <= 0 || pUser[conn].Mode != USER_PLAY)
{
    AddCrackError(conn, 1, 14);   // log "cra point: 1  type: 14", NumError += 1
    SendHpMode(conn);             // client receives MSG_SetHpMode with real Hp/Mode
    return;                       // no grid scan, no world change
}
```

---

### Business Rule: BR-02 - An active trade aborts the drop and cancels the trade

**Overview:**
The presence of a trade session (`pUser[conn].Trade.OpponentID != 0`) makes the drop request
non-executable. Instead of refusing only the drop, the handler tears down the trade on both sides and
returns, leaving the inventory untouched.

**Detailed description:**

`pUser[conn].Trade` is a `MSG_Trade` (`Basedef.h:2046-2057`) holding up to `MAX_TRADE = 15`
(`Basedef.h:79`) offered items, the offered money and `OpponentID`. `OpponentID` is an `unsigned short`
and is zero when no session is open, which is why it can be tested as a boolean. Because a trade is a
two-sided lock on each player's inventory, the handler cannot safely remove an item from a container that
the peer's client is displaying as "offered": that would desynchronise the peer's trade window and could
leave the trade confirmation comparing a stale snapshot.

The response chosen is the harshest available: `RemoveTrade(pUser[conn].Trade.OpponentID)` followed by
`RemoveTrade(conn)` (`Server.cpp:7319-7352`). `RemoveTrade` zeroes the whole `Trade` structure, resets
every `InvenPos[i]` to `-1`, clears `AutoTrade` and its `CarryPos[]`, emits `SendClientSignal(conn, conn,
900)` to close the trade UI, and - if `TradeMode` was set - clears it and re-broadcasts the player's mob
to the surrounding grid. Calling it for the opponent *before* the caller matters: `RemoveTrade` reads
`pUser[conn].Trade.OpponentID` internally only through the peer's own record, and resetting the caller
first would orphan the peer's window. The order in the source is the safe one.

Three properties of this rule are notable. First, it is silent: no message explains why the trade
vanished, and no `Log` line records the event, so a player who presses "drop" during a trade sees their
trade window disappear with no explanation. Second, it is *not* conditional on the drop being otherwise
valid - a drop request for a blacklisted item or a non-existent item still destroys the trade, because
the trade check precedes every item check. Third, the handler returns immediately after cancelling, so
the player must repeat the drop after the trade closes; there is no retry or deferred execution.

**Rule workflow:**

```
if (pUser[conn].Trade.OpponentID)
{
    RemoveTrade(pUser[conn].Trade.OpponentID);  // peer: zero Trade, InvenPos=-1, signal 900,
                                                //       TradeMode=0 + mob rebroadcast if shop
    RemoveTrade(conn);                          // self: same
    return;                                     // drop is NOT performed
}
```

---

### Business Rule: BR-03 - Personal shop / auto-trade lockout

**Overview:**
A player who is running a personal shop (`TradeMode != 0`) cannot drop items. The refusal is explicit and
the only lockout in the component that uses a message table entry rather than a raw literal or silence.

**Detailed description:**

`TradeMode` is set to `1` by `Exec_MSG_SendAutoTrade` (`_MSG_SendAutoTrade.cpp:105`) after the shop
payload has been validated and mirrored into `pUser[conn].AutoTrade`, and it is reset by `RemoveTrade`
(`Server.cpp:7339-7351`). While it is non-zero, the player's avatar is represented to other clients as a
shop object (`MSG_CreateMobTrade`), and the auto-trade payload copies up to `MAX_AUTOTRADE = 12`
(`Basedef.h:80`) item descriptors plus prices out of the player's cargo. Dropping an item that is also
listed in the shop would invalidate the `memcmp` identity check that `_MSG_SendAutoTrade.cpp:90-94` and
the purchase path rely on, so the lockout protects a cross-handler invariant rather than this handler's
own state.

The check is placed after the trade check and before the coordinate check, so a shop-running player who
sends garbage coordinates receives the auto-trade message rather than the coordinate log. The message is
`g_pMessageStringTable[_NN_CantWhenAutoTrade]` (`Language.h:190`, index 170), the same text used by
`_MSG_Attack.cpp:29`, `_MSG_Sell.cpp:40`, `_MSG_GetItem.cpp:40`, `_MSG_TradingItem.cpp:40`,
`_MSG_Quest.cpp:44` and `_MSG_SendAutoTrade.cpp:41`. That uniformity is the one place in this component
where behaviour matches the rest of the inventory family exactly.

Unlike BR-02, this rule does not destroy any state: the shop stays open and the item stays in place. The
handler returns before the grid scan, so the packet's coordinates are not even rewritten. The rule also
covers the act of *opening* a shop only implicitly: nothing prevents a player from dropping an item while
an auto-trade *request* is in flight, because `TradeMode` is set synchronously at the end of the
validation in `_MSG_SendAutoTrade` and the message loop is single-threaded.

**Rule workflow:**

```
if (pUser[conn].TradeMode)
{
    SendClientMessage(conn, g_pMessageStringTable[_NN_CantWhenAutoTrade]);
    return;                                  // shop remains open, item remains in inventory
}
```

---

### Business Rule: BR-04 - Drop coordinate range validation

**Overview:**
The client proposes the destination cell in two `unsigned short` fields. The handler accepts them only if
both are strictly below the grid limits `MAX_GRIDX = MAX_GRIDY = 4096` (`Basedef.h:100-101`); otherwise it
writes an error line to the server log and returns without informing the player.

**Detailed description:**

The two fields are `unsigned short GridX, GridY` (`Basedef.h:1646`), so the value domain is `0..65535` and
a lower-bound test is unnecessary - an unsigned value can never be negative, and the repository's habit of
writing `x < 0` checks on unsigned fields elsewhere (`GetItemPointer`, `Basedef.cpp:2346`) is redundant
by construction. The single comparison per axis therefore fully covers the domain: `4096..65535` is
rejected, `0..4095` is accepted. This is the *only* integrity test applied to the destination before it
is used to index the global `pItemGrid[MAX_GRIDY][MAX_GRIDX]` (`Server.cpp:364`) and
`pHeightGrid[MAX_GRIDY][MAX_GRIDX]` (`Server.cpp:365`), so the guard's correctness is what prevents an
out-of-bounds write in `CreateItem` (`Server.cpp:7287`) and in `GetEmptyItemGrid`
(`GetFunc.cpp:1878`).

The rejection path is diagnostic only. `sprintf(temp, "err,wrong drop pos %d %d", m->GridX, m->GridY)`
writes into the global 4096-byte `temp` buffer (`Server.cpp:277`), then `Log(temp, account, ip)` writes to
the daily text log and the server window (`Server.cpp:6701-6727`). The client receives nothing: the drop
silently does not happen. Because a value `>= 4096` cannot be produced by an unmodified client that is
inside the map, this exit is effectively an exploit/telemetry path, which explains the absence of a
player-facing message. The log line contains no account-independent identifier other than the two
numbers, so correlating it with a specific attempt requires the surrounding lines that `Log` writes with
account and IP.

The rule has two gaps worth recording. It does not reject the coordinate `(0,0)` or any other map border
cell, even though the engine treats certain coordinates as non-walkable in `GetEmptyItemGrid` only through
the height-grid sentinel `127`; and it does not relate the coordinate to the acting player at all (see
BR-20). In other words the rule answers "is this index inside the arrays?" and not "is this a legitimate
place for this player to put an item?".

**Rule workflow:**

```
if (m->GridX >= MAX_GRIDX || m->GridY >= MAX_GRIDY)     // 4096
{
    sprintf(temp, "err,wrong drop pos %d %d", m->GridX, m->GridY);
    Log(temp, pUser[conn].AccountName, pUser[conn].IP);
    return;                                            // no client feedback
}
// accepted domain: GridX in [0,4095], GridY in [0,4095]
```

---

### Business Rule: BR-05 - Global drop feature flag (`isDropItem`)

**Overview:**
Dropping is a switchable server feature. `isDropItem` (`Server.cpp:376`, declared `Server.h:349`) must be
non-zero; when it is `0` the handler returns immediately and silently, before any grid work.

**Detailed description:**

The flag is loaded from the server configuration file `gameconfig.txt` by `ReadConfig`
(`Server.cpp:761-1115`). The relevant line is the "Etc Settings" line, validated by an exact
`strcmp` of its labels including `DropItem` (`Server.cpp:1087`), parsed with a fixed eight-field
`sscanf` (`Server.cpp:1085`) and assigned at `Server.cpp:1109` from `value[4]`. The same value is written
back when the server regenerates the file (`Server.cpp:1313-1314`), so the flag round-trips. Its default
is `0` (`Server.cpp:376`), which means a server whose configuration file is missing or unparsable starts
with dropping disabled: `ReadConfig` shows a modal error and returns without touching the flag when the
file is absent (`Server.cpp:766-775`). The flag can also be changed at runtime through the GM command
`dropitem` (`imple.cpp:1766-1773`), which assigns `ival1` and redraws the configuration window.

Because the check sits after the session/trade/coordinate guards but before `GetEmptyItemGrid`, disabling
drops still permits the trade-cancellation side effect of BR-02 and still writes the "wrong drop pos" log
for bad coordinates. That ordering is a small inconsistency: an operator who disables dropping for an
event would probably expect a drop packet to be a complete no-op, but a player in a trade will still have
the trade destroyed. The flag also has no per-map, per-item or per-player granularity; it is a whole-server
switch, and it is the only one of the handler's rules that is configurable at all - every other policy
(blacklist, decay time, coordinate authority) is compiled in.

The flag's silent failure mode is a usability trap. When `isDropItem == 0` the player's client sends the
drop, the server ignores it, and the client receives neither a confirmation nor a refusal. Since the
client only removes the item from its local inventory on `MSG_CNFDropItem` (BR-15), the player sees the
item stay in the inventory and will normally retry, producing an unbounded stream of silently ignored
packets. Nothing rate-limits that stream (BR-19 discusses idempotency but not throttling).

**Rule workflow:**

```
ReadConfig (Server.cpp:761)
  -> gameconfig.txt line "Etc Settings: partydif ... DropItem <n> ..."
  -> sscanf 8 fields, label strcmp must match           (Server.cpp:1085-1094)
  -> isDropItem = value[4]                              (Server.cpp:1109)

GM: "/dropitem <n>" -> isDropItem = ival1; DrawConfig(TRUE)   (imple.cpp:1766-1773)

Exec_MSG_DropItem:
if (isDropItem == 0)
    return;                                             // silent, no client message
```

---

### Business Rule: BR-06 - Ground cell resolution (self plus 8-neighbour substitution)

**Overview:**
The requested cell is not used blindly. `GetEmptyItemGrid` accepts it only when the cell is unoccupied and
not flagged as blocked terrain; otherwise it substitutes the first free cell among the eight neighbours,
and the handler writes the substituted coordinates back into both the packet and the client
confirmation.

**Detailed description:**

The resolution function (`GetFunc.cpp:1876-1899`) is a nine-cell search with a specific order. It first
tests the requested cell: `pItemGrid[*gridy][*gridx] == 0 && pHeightGrid[*gridy][*gridx] != 127`. If that
holds it returns `TRUE` without touching the output parameters. Otherwise it iterates `y` from `*gridy-1`
to `*gridy+1` and, inside, `x` from `*gridx-1` to `*gridx+1`, skipping coordinates outside
`[0, MAX_GRIDX) x [0, MAX_GRIDY)`, and returns the first cell with an empty item grid and a height
different from `127`. The scan therefore prefers the row above the requested cell (and, within it, the
cell to the left) over the cell directly below, which is an arbitrary but deterministic tie-break that
players can observe as drops "jumping" up-left.

`pItemGrid` is `unsigned short [4096][4096]` (`Server.cpp:364`) holding `pItem` slot indices and is the
authoritative occupancy map used by both drop and pickup (`_MSG_GetItem.cpp:164-172`). `pHeightGrid` is
`char [4096][4096]` (`Server.cpp:365`) loaded verbatim from `./heightmap.dat` in `Reboot`
(`Server.cpp:7146-7156`) and post-processed by `BASE_ApplyAttribute`. The value `127` is the engine-wide
"impassable / no placement" sentinel: the mob-placement helpers test exactly the same value
(`GetFunc.cpp:1913,1929,1949,1969,1989,2015,2033`). Because the grid is flat and the item is a single
cell, no footprint, rotation-aware shape or stacking merge is considered; whatever cell is returned is the
cell the object occupies.

Two consequences follow. First, a drop aimed at a wall or at an occupied cell is not refused: it is
relocated up to one cell away, and the client is told the new position only through the confirmation
message, never through a "moved" indication. Second, the handler's mutation of the packet
(`m->GridX = gridx; m->GridY = gridy;` at `:59-60`) happens *before* the resolved value is checked for
success (`:62`), which is harmless only because `GetEmptyItemGrid` leaves its out-parameters untouched
when it fails. Both `CreateItem` (`Server.cpp:7259`) and `CreateTreasureBox` (`Server.cpp:9397`)
independently re-run the same resolution, so the handler's call is redundant work whose only purpose is to
produce a position for the confirmation packet and to be able to fail early with
`_NN_Cant_Drop_Here`.

**Rule workflow:**

```
gridx = m->GridX; gridy = m->GridY;                 // :54-55
titem = GetEmptyItemGrid(&gridx,&gridy);            // :57
m->GridX = gridx; m->GridY = gridy;                 // :59-60

GetEmptyItemGrid:                                   // GetFunc.cpp:1876
  if (pItemGrid[y][x] == 0 && pHeightGrid[y][x] != 127) return TRUE;
  for (yy = y-1 .. y+1)
    for (xx = x-1 .. x+1)
      if (xx,yy) inside grid and pItemGrid[yy][xx] == 0 and pHeightGrid[yy][xx] != 127
      { *gridx = xx; *gridy = yy; return TRUE; }
  return FALSE;

if (titem == 0) { SendClientMessage(conn, _NN_Cant_Drop_Here); return; }
```

---

### Business Rule: BR-07 - Worn equipment can never be dropped

**Overview:**
`SourType == ITEM_PLACE_EQUIP` (`0`) is rejected outright, so items in the 16 equipment slots
(`MAX_EQUIP = 16`, `Basedef.h:75`) can never be released to the ground through this handler.

**Detailed description:**

The container selector is an `int SourType` (`Basedef.h:1643`) whose documented values are
`ITEM_PLACE_EQUIP 0`, `ITEM_PLACE_CARRY 1`, `ITEM_PLACE_CARGO 2` (`Basedef.h:86-88`). Because `EQUIP` is
also the value a zero-initialised packet carries, the check `if (m->SourType == ITEM_PLACE_EQUIP)` is
simultaneously an "equipment" rule and a "default value" rule: a frame whose `SourType` field was never
set (or was truncated away, see finding 11) is treated as an attempt to drop worn gear and logged as
`err,dropitem - sourtype`. This is the correct fail-closed choice, but it also means the log line cannot
distinguish "tried to drop a helmet" from "sent a malformed packet".

The design reason for the rule is that equipped items contribute live combat stats through
`BASE_GetMobAbility` (`Basedef.cpp:2374`) and are re-derived by `GetCurrentScore`; removing one silently
would require a full stat recalculation that this handler does not perform. The handler deliberately
limits itself to containers whose removal is stat-neutral: `Carry[]` (inventory) and
`pUser[].Cargo[]` (warehouse storage). Every other place in the code base that needs to remove equipment
goes through a dedicated path that also recomputes stats and re-sends the score
(for example `ProcessSecMinTimer.cpp:552-568` and `MobKilled.cpp:2463-2465`).

The check is a *type* rejection, not a position rejection: because it precedes the position checks
(BR-08), an equip-typed packet with an out-of-range `SourPos` still logs the generic
`err,dropitem - sourtype` line rather than a position error. The rule is absolute - there is no
"drop on death" or "drop when shackled" exemption - and it is not configurable. Note the deliberate
asymmetry with the death drop code in `MobKilled.cpp:2380-2404`, which *does* drop equipment, but that
path is compiled out (`PKDrop` undefined, `Basedef.h:29`).

**Rule workflow:**

```
if (m->SourType == ITEM_PLACE_EQUIP)          // 0
{
    Log("err,dropitem - sourtype", account, ip);
    return;                                    // silent to the client
}
```

---

### Business Rule: BR-08 - Source container and slot validation

**Overview:**
For the two permitted containers the slot number must be inside the container's *live* bound:
`0 <= SourPos < pMob[conn].MaxCarry` for `CARRY`, and `0 <= SourPos < MAX_CARGO (128)` for `CARGO`.
Anything that is neither `CARRY` nor `CARGO` is rejected.

**Detailed description:**

`MaxCarry` is not the array size. The array is `Carry[MAX_CARRY]` with `MAX_CARRY = 64`
(`Basedef.h:76`, `Basedef.h:458-459`), but the live limit is computed at login as `30`, plus `15` if
`Carry[60]` holds archetype `3467`, plus another `15` if `Carry[61]` holds `3467`
(`ProcessDBMessage.cpp:751-757`), i.e. 30/45/60. This is the expansion-bag mechanic: slots 60 and 61 hold
the bags themselves and are outside the usable range, so the handler's `SourPos >= MaxCarry` test does two
jobs at once - it keeps the client out of the unused tail of the array (60..63) *and* makes the expansion
items themselves impossible to drop (a `SourPos` of 60 or 61 is always `>= MaxCarry`, since `MaxCarry`
tops out at 60). The same recomputation is mirrored in the timer (`ProcessSecMinTimer.cpp:571-597`) when a
bag expires, which is why the value can change during a session and why the check must read the live field
rather than a constant.

The `CARGO` branch uses `MAX_CARGO = 128` (`Basedef.h:77`) against `pUser[conn].Cargo[MAX_CARGO]`
(`CUser.h:50`). There is no `MaxCargo` analogue, so all 128 slots are addressable. This is the warehouse
container, normally manipulated through the deposit/withdraw NPC handlers, and nothing in this handler
verifies that the player is actually at a warehouse: a drop with `SourType == CARGO` is a valid way to
pull a stored item directly onto the ground from anywhere on the map. Whether an unmodified client can
produce that packet outside the warehouse UI is a client-behaviour question; the server-side rule does not
constrain it.

The remaining branch is a catch-all: a non-zero `SourType` that is neither `1` nor `2` is logged as
`err,dropitem - sourtype`. Negative values (a signed `int` field) fall into this branch, so a crafted
`SourType = -1` cannot reach `GetItemPointer`, which would otherwise only test `== 1`/`== 2` and return
`NULL`. Two logs are produced in this area (`err,dropitem - carry equip` for a bad carry slot,
`err,dropitem - sourpos cargo` for a bad cargo slot); both are silent to the client.

**Rule workflow:**

```
if (m->SourType)                                   // non-zero
{
    if (m->SourType == ITEM_PLACE_CARRY)           // 1
    {
        if (m->SourPos < 0 || m->SourPos >= pMob[conn].MaxCarry)   // 30/45/60
            { Log("err,dropitem - carry equip"); return; }
    }
    else
    {
        if (m->SourType != ITEM_PLACE_CARGO)       // not 2
            { Log("err,dropitem - sourtype"); return; }
        if (m->SourPos < 0 || m->SourPos >= MAX_CARGO)             // 128
            { Log("err,dropitem - sourpos cargo"); return; }
    }
}
// SourType == 0 already returned at BR-07, so only 1 and 2 reach GetItemPointer
```

---

### Business Rule: BR-09 - Source item existence and archetype validity

**Overview:**
The resolved source pointer must be non-null and the archetype it holds must be inside
`(0, MAX_ITEMLIST)`. Both failures are completely silent.

**Detailed description:**

`GetItemPointer` (`Basedef.cpp:2340-2372`) maps `(type, pos)` to a `STRUCT_ITEM*`: `EQUIP` to
`mob->Equip[pos]` with `MAX_EQUIP` bound, `CARRY` to `mob->Carry[pos]` with `MAX_CARRY` bound, `CARGO` to
`cargo[pos]` with `MAX_CARGO` bound, anything else to `NULL`. It then re-validates the *archetype* and
returns `NULL` when `sour->sIndex < 0 || sour->sIndex >= MAX_ITEMLIST`. The handler calls it twice with
identical arguments (lines 99 and 100), names the second result `BackupItem`, null-tests both, and then
re-tests `sIndex <= 0 || sIndex >= MAX_ITEMLIST` itself at line 106. The `sIndex < 0` half of the
`GetItemPointer` test is unreachable because `sIndex` is a `short` and the condition is `short < 0` -
which is reachable for negative shorts, so the comment applies to the handler's own `<= 0` test, which
correctly excludes both `0` (empty slot) and negatives.

The practical meaning of the rule is "there is really an item there". An empty carry slot has
`sIndex == 0`, so a client that drops from an empty slot is rejected at `:106` without a message, and no
grid scan result is committed. A slot holding a garbage archetype (`>= 6500`) is likewise rejected; this
can only arise from corrupted or unvalidated item data, because the shop/quest/combine paths validate
against `MAX_ITEMLIST` before writing. Because the two `GetItemPointer` calls cannot disagree in a
single-threaded server, `BackupItem == NULL` is redundant: if `SrcItem` is non-null then `BackupItem` is
non-null. The null test therefore guards the same condition twice, and the "backup" name suggests an
intent (take a value copy so the item can be restored if creation fails) that was never implemented.

No log is written on either failure, which matters for forensics: a client sending a stream of drops from
empty slots produces no server-side trace at all, so this path cannot be distinguished from a
network-less client. Compare `_MSG_GetItem.cpp:57-61`, which does log `GetItemFail idx/mode` on the mirror
-image condition.

**Rule workflow:**

```
STRUCT_ITEM *SrcItem    = GetItemPointer(&pMob[conn].MOB, pUser[conn].Cargo, m->SourType, m->SourPos);
STRUCT_ITEM *BackupItem = GetItemPointer(&pMob[conn].MOB, pUser[conn].Cargo, m->SourType, m->SourPos);

if (SrcItem == NULL || BackupItem == NULL) return;      // silent
if (SrcItem->sIndex <= 0 || SrcItem->sIndex >= MAX_ITEMLIST) return;   // 6500, silent

GetItemPointer: type==0 -> Equip[pos] (pos<16)
                type==1 -> Carry[pos] (pos<64)
                type==2 -> Cargo[pos] (pos<128)
                else    -> NULL
                sIndex < 0 || sIndex >= 6500 -> NULL
```

---

### Business Rule: BR-10 - Non-droppable item policy (guild, medal and crown family)

**Overview:**
A hard-coded set of archetypes cannot be dropped: `508`, `509`, `522`, `446`, `747`, `3993`, `3994` and
the whole range `526..537`. The refusal uses the message `_NN_Guild_Medal_Cant_Be_Droped`.

**Detailed description:**

The predicate is one long negated conjunction (`:109-110`) that must be *false* for the drop to proceed.
Translated into the affirmative form, the blocked set is
`{508, 509, 522, 446, 747, 3993, 3994} u [526,537]`, i.e. fifteen distinct indices. Cross-references in
the code base identify the family: `508` and `509` are guild plate/medal items sold with a guild stamp and
a `GuildLevel` requirement (`_MSG_Buy.cpp:165-206`); `526..537` are the sub-guild variants, with
`BASE_GetSubGuild` mapping `526-537` to sub-guilds and `508` being rewritten to `532..534` for
sub-guilds 1-3 (`Basedef.cpp:4237-4245`, `_MSG_Buy.cpp:198-204`); `522` is treated exactly like a medal in
the whisper/summon gate (`_MSG_MessageWhisper.cpp:856` reads it from `Equip[12]`, the medal slot); `446` is
grouped with the guild items by the trade rule (`_MSG_Trade.cpp:160`); and `747`, `3993`, `3994` are the
crown family - `Server.cpp:672` and `Server.cpp:589` carry the inline comment `// 747 = Crown on itemlist`
and `Server.cpp:528-578` rewrites `3993 <-> 3994` according to clan 7/8. The identification of individual
names/archetypes is inferred from these cross-references because the item-list data file
(`g_pItemList`, `Basedef.cpp:753`) is not part of this repository; confidence is therefore high for the
grouping and medium for the exact in-game names.

The policy exists because guild-bound items carry their owner in effects `56/57` (`EF_HWORDGUILD`/
`EF_LWORDGUILD`) and crowns encode clan privileges. Dropping one would create an ownerless object that any
player could pick up, which is precisely the transfer the guild trade rule forbids unless both parties are
guild masters of the owning guild (`_MSG_Trade.cpp:160-177`). The message text
(`_NN_Guild_Medal_Cant_Be_Droped`, `Language.h:93`, index 73) is therefore accurate but narrow: it speaks
only of guild medals although the rule also covers crowns and the whole sub-guild band. The check is
evaluated *after* the free-cell search, so the "can't drop here" message can mask the policy message on a
crowded cell.

The rule is a duplicate of policy that lives in at least five other places, with materially different
index sets: `_MSG_Trade.cpp:147` blocks `{747, 3993, 3994}` as a first tier and `{508, 522, 446, 526..537}`
as a guild tier; `_MSG_SendAutoTrade.cpp:80-82` blocks `{508, 3993, 747, 509, 522, 446, 526..531}` (it
misses `532..537` and `3994`); `_MSG_Sell.cpp:92-93` blocks only `{3193, 3194, 747}`; the PK-death drop in
`MobKilled.cpp:2321-2325` blocks `{508, 509, 522, 526..531, 446}` and would drop crowns; and
`_MSG_TradingItem.cpp:165-179` blocks `{508, 522, 526..537}` only for equipment-slot destinations. The
engine's declarative `EF_NOTRADE` (`ItemEffect.h:153`), honoured by `_MSG_Trade.cpp:179` and
`_MSG_SendAutoTrade.cpp:84`, is not consulted by this handler at all, so an item flagged no-trade by data
can still be dropped unless its archetype happens to be in the literal list. The concrete divergence that
matters for this component is that `3193`/`3194` (clan mantles, per `_MSG_Sell.cpp`) are droppable here.

**Rule workflow:**

```
sIndex = SrcItem->sIndex;

if (sIndex != 508 && sIndex != 509 && sIndex != 522 &&
    (sIndex < 526 || sIndex > 537) && sIndex != 446 && sIndex != 747 &&
    sIndex != 3993 && sIndex != 3994)
{
    ... create world object ...                 // allow
}
else
{
    SendClientMessage(conn, g_pMessageStringTable[_NN_Guild_Medal_Cant_Be_Droped]);
    return;                                     // refuse, source untouched
}
```

---

### Business Rule: BR-11 - World object creation and broadcast

**Overview:**
A permitted item is converted into a ground object by `CreateItem(x, y, item, rotate, 1)`
(`Server.cpp:7254-7317`). The function re-validates the archetype, re-resolves the cell, allocates the
lowest free `pItem` slot, copies the payload, initialises the object's timers and state, registers the
grid cell and multicasts `MSG_CreateItem` to the surrounding 33x33 view.

**Detailed description:**

`CreateItem` starts with the same archetype test the handler already performed
(`if (item->sIndex <= 0 || item->sIndex >= MAX_ITEMLIST) return FALSE;`), which is redundant defence but
harmless, and then calls `GetEmptyItemGrid(&x, &y)` again. That second resolution is where the handler's
`MSG_CNFDropItem` coordinates could in principle diverge from the object's real position: the handler has
already confirmed a free cell, but `CreateItem` re-runs the nine-cell search and ignores its return value,
and its own occupancy test `if (pItemGrid[y][x]) return FALSE;` (`:7264`) then fails if the scan did not
find anything. In a single-threaded server no other writer can take the cell between the two calls, so the
second search is pure overhead; it becomes observable only if the handler's cell was valid but became
occupied by an interleaved timer event - which cannot happen here because both run on the same window
message loop.

The allocation step uses `GetEmptyItem` (`Server.cpp:6754-6763`), a linear scan from `1` to `MAX_ITEM-1`
returning the first `pItem[i].Mode == 0`. `pItem` is a `CItem[MAX_ITEM]` with `MAX_ITEM = 5000`
(`Server.cpp:279`, `CItem.h:24-45`), so at most 4999 ground objects can exist simultaneously; slot 0 is
never allocated. The object is filled with `memcpy(&pItem[empty].ITEM, item, sizeof(STRUCT_ITEM))`
(`:7277`), which copies `sIndex` plus the three 16-bit effect words (`Basedef.h:398-412`). Then
`Mode = 1`, `PosX/PosY = resolved cell`, `Rotate = rotate`, `State = STATE_OPEN (1)`
(`Basedef.h:1835`), `Delay = 90`, `Decay = 0`, and `GridCharge = BASE_GetItemAbility(item, EF_GROUND)`
(`ItemEffect.h:73`) - the "grid charge" value the client uses for ground effect rendering. `pItemGrid[y][x]`
is set to the slot (`:7287`) so occupancy and object are consistent, and `Height = pHeightGrid[y][x]`
(`:7289`) carries the terrain height for rendering.

The broadcast is `MSG_CreateItem` with `ID = ESCENE_FIELD (30000)` (`Basedef.h:170`),
`ItemID = slot + 10000`, the item payload, the cell, `Rotate`, `State`, and a `Height` that is only
overridden to `-204` when `Create == 2` (treasure boxes). `GridMulticast(x, y, msg, 0)`
(`SendFunc.cpp:843`) walks the 33x33 window (`VIEWGRIDX/Y = 33`, `HALFGRID* = 16`, `Basedef.h:95-98`)
clamped to the map, and sends to every mob slot found in `pMobGrid` except id 0. The `Create` flag passed
as `1` by this handler is otherwise unused by `CreateItem` (the `sm.Create = Create;` assignment is
commented out at `:7307`), so the only observable effect of the parameter is the `== 2` height override.

**Rule workflow:**

```
int drop = CreateItem(m->GridX, m->GridY, SrcItem, m->Rotate, 1);

CreateItem(x, y, item, rotate, Create):
  if (item->sIndex <= 0 || item->sIndex >= 6500) return FALSE;
  GetEmptyItemGrid(&x,&y);
  if (pItemGrid[y][x]) return FALSE;
  empty = GetEmptyItem();                      // first pItem[1..4999].Mode == 0
  if (empty == 0) return FALSE;                // 4999 objects already live
  pItem[empty].Mode = 1; PosX = x; PosY = y;
  memcpy(&pItem[empty].ITEM, item, 14 bytes);
  Rotate = rotate; State = STATE_OPEN; Delay = 90; Decay = 0;
  GridCharge = BASE_GetItemAbility(item, EF_GROUND);
  pItemGrid[y][x] = empty;
  Height = pHeightGrid[y][x];
  MSG_CreateItem { ItemID = empty+10000, Item, GridX, GridY, Rotate, State }
  GridMulticast(x, y, &sm, 0);                 // 33x33 window
  return TRUE;                                 // 1, not the slot index
```

---

### Business Rule: BR-12 - Creation failure preserves the source item

**Overview:**
If `CreateItem` returns `FALSE` (world object could not be created) the handler sends the raw literal
`"Can't create object(item)"` and returns without touching the source slot.

**Detailed description:**

`CreateItem` has three failure modes: invalid archetype (already excluded by BR-09/BR-10), no free grid
cell after the internal re-resolve, and no free `pItem` slot. Only the last two are reachable from this
handler, and the second is practically unreachable for the reason given in BR-11. The interesting case is
exhaustion of the 4999-slot object array: `GetEmptyItem` returns 0, `CreateItem` returns `FALSE`, and the
handler's `if (drop <= 0 || drop >= MAX_ITEM)` catches it. That condition is written as if `drop` were an
item index, which it is not - `CreateItem` returns `TRUE`/`FALSE` (`int`, 1/0) on every path. The
`drop >= MAX_ITEM` half is therefore dead, and the variable name `drop` is misleading; the effective test
is `drop == 0`.

The message is a string literal rather than an entry in `g_pMessageStringTable`, so it is not localised,
unlike every other player-facing message in the handler. It is sent through `SendClientMessage`
(`SendFunc.cpp:27`), which copies at most `MESSAGE_LENGTH` bytes into `MSG_MessagePanel` and appends the
message to the socket buffer without flushing. The client therefore receives the panel at the next flush;
the drop is not retried and the source remains.

Conservation is correct in this path: nothing has been logged, nothing has been zeroed, and the client has
received no confirmation, so the item stays in the inventory on both sides and the player can retry. This
is the only failure exit inside the commit block, and it is the reason the commit block does not need a
rollback - `CreateItem` either produces a complete world object or changes nothing that the handler cares
about.

**Rule workflow:**

```
drop = CreateItem(...);
if (drop <= 0 || drop >= MAX_ITEM)      // effective test: drop == 0
{
    SendClientMessage(conn, "Can't create object(item)");   // raw literal
    return;                                                  // source intact, no log
}
```

---

### Business Rule: BR-13 - Mandatory audit trail before destruction

**Overview:**
Immediately after successful creation and before the source is cleared, the handler serialises the item
into text and writes `dropitem, <serialised>` to the daily item log, together with account name and IP.

**Detailed description:**

`BASE_GetItemCode` (`Basedef.cpp:1479-1495`) renders an item as
`" <Name> : <sIndex>.<c0>.<v0>.<c1>.<v1>.<c2>.<v2>"` from `g_pItemList[sIndex].Name` and the three effect
pairs. The caller supplies a 2048-byte stack buffer (`char tmplog[2048]`, `:120`) while the name field is
at most 64 bytes (`ITEMNAME_LENGTH`, `Basedef.h:143`), so the `sprintf` cannot overflow for any valid
archetype; `BASE_GetItemCode` returns an empty string for an out-of-range index rather than writing
anything. The formatted result is then re-formatted into the global 4096-byte `temp` buffer with the
prefix `dropitem, ` (`:122`) and passed to `ItemLog`.

`ItemLog` (`Server.cpp:9264-9284`) prefixes the timestamp and, when `ip != 0`, the dotted IP, then appends
`account` and the message to `fItemLogFile`. The file is opened per day as
`./item_log/TM_<dd>_<mm>_<yyyy>_<hh>_<mm>_<ss>.txt` by the rotation helper at `Server.cpp:9250-9262`. It
does not flush per line, so a hard kill can lose the tail, and it does not include the source container or
slot, the destination coordinates, or the world slot id. Reconstructing "which item left which slot and
where it became a ground object" from the log alone is therefore impossible; the log proves only that the
account dropped an item with a given archetype and effect vector.

The rule's placement - after creation, before clearing - is the correct one for an audit trail: the
record is written exactly once for a drop that actually happened, and a failure of the log write cannot
undo the drop (the `fprintf` return value is ignored). It also means the log is written even if the
confirmation packet subsequently fails to send, which is desirable. What the rule does not provide is a
corresponding entry for the *pickup* side in all cases: `_MSG_GetItem` logs `getitem, ...` only for the
inventory branch (`_MSG_GetItem.cpp:228-231`), not for currency pickups (`:182-202`), so a dropped coin
stack leaves a `dropitem` line with no matching `getitem` line.

**Rule workflow:**

```
char tmplog[2048];
BASE_GetItemCode(SrcItem, tmplog);        // " Name : idx.c0.v0.c1.v1.c2.v2"
sprintf(temp, "dropitem, %s", tmplog);    // temp[4096]
ItemLog(temp, pUser[conn].AccountName, pUser[conn].IP);
   -> "[dd.mm.yyyy.hh.mm.ss IP: a.b.c.d] <account> dropitem, Name : ..."  appended to
      ./item_log/TM_<date-time>.txt
```

---

### Business Rule: BR-14 - Source slot clearing is the commit point

**Overview:**
After the world object exists and the audit line is written, the whole `STRUCT_ITEM` at the source address
is zeroed with `memset`. This is the handler's only destructive write and therefore its commit point.

**Detailed description:**

The write is `memset(SrcItem, 0, sizeof(STRUCT_ITEM))` (`:125`), which clears `sIndex` and all three
effect words in one operation. It targets the pointer returned by `GetItemPointer`, so it clears the exact
element of `pMob[conn].MOB.Equip[]`, `pMob[conn].MOB.Carry[]` or `pUser[conn].Cargo` - never a copy. There
is no compensating `SendItem` afterwards, so the client's inventory is corrected only by the confirmation
message's side effect (BR-15).

Because the world object was created by copying the payload *before* the clear, ordering is essential and
correct: create, log, clear, confirm. Any reordering would either destroy the item without creating the
ground object (clear first) or log an empty item (log after clear). The block is straight-line and has no
failure exits between `CreateItem` and `memset`, so there is no window in which the item exists in neither
place: from the server's point of view the transfer is atomic within one handler invocation, and the
Windows message loop guarantees no other handler runs in the middle.

The absence of a backup is worth noting against the `BackupItem` declaration: a genuine two-phase commit
would copy the item into a local `STRUCT_ITEM`, zero the source, and restore the copy if a later step
failed. Here, the only later step is the confirmation send, whose result is discarded
(`pUser[conn].cSock.SendOneMessage(...)` at `:139` returns a `BOOL` that is not checked). If the send
fails because the socket is gone, the drop has still committed: the ground object exists and the inventory
slot is empty. That is a defensible choice (the item must not vanish), but it is not what the unused
"backup" name suggests, and it means the comment-free code gives a future maintainer no signal about the
intended transaction boundary.

**Rule workflow:**

```
// preconditions: pItem[slot] fully initialised, pItemGrid[y][x] = slot, ItemLog written
memset(SrcItem, 0, sizeof(STRUCT_ITEM));      // 14 bytes: sIndex + 3 effect words
// postcondition: inventory slot is empty on the server; client is corrected by MSG_CNFDropItem
```

---

### Business Rule: BR-15 - Client confirmation and state synchronisation

**Overview:**
The dropper receives a `MSG_CNFDropItem` (`Basedef.h:1850-1859`) echoing `SourType`, `SourPos`, `Rotate`
and the resolved `GridX/GridY`, sent with `SendOneMessage` so it is flushed immediately. No inventory
re-sync packet is sent.

**Detailed description:**

The confirmation is constructed with `Type = _MSG_CNFDropItem (117 | FLAG_GAME2CLIENT = 0x0175)`,
`Size = sizeof(MSG_CNFDropItem)`, and the five fields copied from the request. `GridX/GridY` are the
*resolved* cell (BR-06), not the requested one, which is how the client learns that the drop was
relocated. `SendOneMessage` (`CPSock.cpp:686-693`) is `AddMessage` followed by `SendMessageA`, i.e. the
packet is pushed to the socket immediately rather than waiting for the next periodic flush; that is
unusual in this code base, where most sends accumulate and are flushed by the timer or by a later
`SendMessageA`. The return value is ignored.

The protocol assumes the client removes the item from the named slot on receipt. That is a thin contract:
there is no sequence number, no item identity (`ItemID` exists in the request but is not echoed), and no
follow-up `SendItem` (`SendFunc.cpp:1055`) that would let the client rebuild the slot from server truth.
Consequently, if the confirmation is lost (socket error, client crash between processing packets, or a
send-buffer overflow that `SendMessageA` swallows at `CPSock.cpp:634-645`), the client keeps showing an
item the server has already destroyed. The player then experiences "ghost items": equipping, selling or
trading them is rejected by the server's own authoritative checks, and the ghost disappears after relog
because `SaveUser` writes the cleared slot.

The same weakness exists in reverse for the *rejection* paths: because eleven of the fifteen exits send
nothing, a client that optimistically removes the item before confirmation would be wrong, and a client
that waits for confirmation is correct. Nothing in the server enforces which behaviour the client uses,
and no idempotent re-sync exists to repair a mismatch other than logout. `MSG_CNFDropItem` is therefore
the only synchronisation mechanism for this transaction, and it is fire-and-forget.

**Rule workflow:**

```
MSG_CNFDropItem sm_ditem;
memset(&sm_ditem, 0, sizeof(sm_ditem));
sm_ditem.Type     = _MSG_CNFDropItem;               // 0x0175
sm_ditem.Size     = sizeof(MSG_CNFDropItem);
sm_ditem.SourType = m->SourType;
sm_ditem.SourPos  = m->SourPos;
sm_ditem.Rotate   = m->Rotate;                      // int, echoed in full
sm_ditem.GridX    = m->GridX;                       // resolved cell
sm_ditem.GridY    = m->GridY;
pUser[conn].cSock.SendOneMessage((char*)&sm_ditem, sizeof(sm_ditem));   // flush ignored
```

---

### Business Rule: BR-16 - Ground-object lifetime and automatic decay

**Overview:**
Every object created by this handler is transient. `CreateItem` stamps `Delay = 90` and `Decay = 0`; the
server's decay sweep decrements `Delay` and, once it reaches zero, removes the object, clears its grid
cell and multicasts `MSG_DecayItem`. The effective lifetime is about 90 seconds.

**Detailed description:**

`ProcessDecayItem` (`Server.cpp:8353-8396`) is invoked from `ProcessSecTimer` under
`if ((SecCounter % 2) == 0)` (`ProcessSecMinTimer.cpp:1101-1102`). `SecCounter` is incremented once per
`ProcessSecTimer` invocation (`ProcessSecMinTimer.cpp:123`), and `TIMER_SEC` is installed with a 500 ms
period (`Server.cpp:3631`), so one `SecCounter` unit is approximately 0.5 s and the decay sweep runs about
once per second. For each active object the sweep first skips archetype `1727` (a special object that must
never decay, also special-cased by the pickup handler at `_MSG_GetItem.cpp:84`), then skips
`Mode == 0` (free) and `Mode == 2` (treasure/box objects created by `CreateTreasureBox`,
`Server.cpp:9392`). For the rest it decrements `Delay` and `continue`s while `Delay >= 1`, so the object
survives 90 sweep invocations - roughly 90 seconds - before the removal branch runs. Because `Decay` is set
to `0` by both creation paths and is compared against `-1` but never assigned `-1` anywhere in the code
base, that guard is always true and no dropped object is permanent.

The removal branch clears the item payload with `BASE_ClearItem` (`Basedef.cpp:4281-4291`), sets
`Mode = 0`, builds `MSG_DecayItem` with `ItemID = slot + 10000` and multicasts it to the decoration grid
window. The grid release, however, is written as `pItemGrid[itemPosX][itemPosY] = 0`
(`Server.cpp:8381`) even though `pItemGrid` is declared `[MAX_GRIDY][MAX_GRIDX]` (`Server.cpp:364`) and
every other user of the array indexes it `[y][x]` (drop: `Server.cpp:7287`; pickup:
`_MSG_GetItem.cpp:164-172,248`; the neighbouring helpers in `GetFunc.cpp` all use `[yy][xx]`). The decay
therefore clears a transposed cell: the expired object's real cell keeps its slot index (a permanent ghost
occupancy that blocks future drops there), and some unrelated cell is cleared even though the object
registered there is still `Mode == 1`. That is a genuine defect in the *data* the drop handler produces,
even though the defective line lives in the timer.

Two further properties matter. First, the sweep's loop arithmetic is broken but benign in effect:
`for (int i = 0; i < ItemCount; i++) { ItemCount++; if (ItemCount >= 5000) ItemCount = g_dwInitItem + 1; ... }`
mutates its own bound and relies on the reset to terminate, which means each call scans essentially the
whole `pItem` array once and leaves `ItemCount` near `g_dwInitItem`. Second, the 90-second budget starts
at creation, not at first pickup attempt: there is no owner, no pickup lock and no refresh-on-inspect, so
a drop into an unreachable cell (BR-20) simply expires.

**Rule workflow:**

```
CreateItem -> pItem[s].Delay = 90; pItem[s].Decay = 0;      (Server.cpp:7282-7283)

ProcessSecTimer (every 500 ms) -> SecCounter++              (ProcessSecMinTimer.cpp:123)
  if (SecCounter % 2 == 0) ProcessDecayItem();              (:1101-1102)

ProcessDecayItem (Server.cpp:8353):
  scan pItem[]:
    if (ITEM.sIndex == 1727) continue;                      // never decays
    if (Mode == 0 || Mode == 2) continue;                   // free or treasure box
    if (Delay >= 1) { Delay--; continue; }                  // ~90 decrements
    if (Decay != -1) {                                      // always true (Decay is never -1)
        BASE_ClearItem(ITEM);
        pItemGrid[PosX][PosY] = 0;                          // DEFECT: transposed [x][y]
        Mode = 0;
        MSG_DecayItem { ItemID = slot + 10000 } multicast
    }
```

---

### Business Rule: BR-17 - Currency (money) drops and pickup coupling

**Overview:**
There is no money-specific rule in the drop handler. A currency object - an item archetype whose
`EF_VOLATILE` ability equals `2` - is dropped like any other item, provided its archetype is not in the
BR-10 blacklist. Its value travels inside the item's effects and is only interpreted when somebody picks
it up.

**Detailed description:**

The repository models two kinds of wealth: `pMob[conn].MOB.Coin` (coin carried by the character; part of
`STRUCT_MOB`, saved with the mob) and `pUser[conn].Coin` (coin held in the account/warehouse side, saved
separately at `Server.cpp:7067` and moved by `_MSG_Deposit.cpp:33-40`). Ground money is a third
representation: a `STRUCT_ITEM` with `EF_VOLATILE == 2` (`ItemEffect.h:77`), whose amount is the 16-bit
pair `(EF_HWORDCOIN << 8) + EF_LWORDCOIN` (effects 36/37, `ItemEffect.h:75-76`). The drop handler never
reads `EF_VOLATILE` or either coin effect; it copies the whole `STRUCT_ITEM` into `pItem[].ITEM`
(`Server.cpp:7277`) and broadcasts it. The amount is thus carried byte-for-byte, with no escrow, no
validation and no denomination check.

The pickup side is where the money rule lives: `_MSG_GetItem.cpp:180-202` reads
`Vol = BASE_GetItemAbility(ditem, EF_VOLATILE)`; if `Vol == 2` it reconstructs `coin1`, computes
`tcoin = coin1 + pMob[conn].MOB.Coin`, refuses the pickup (leaving the object on the ground) when
`tcoin >= 2000000000`, otherwise adds the amount, clears the item and multicasts the removal. Two
observable consequences follow from the split. First, a transaction that is conceptually "transfer money"
is implemented as "drop an item, hope nobody else picks it up": pickup is strictly first-come-first-served
(no owner is stored in `CItem`, `CItem.h:24-45`, and the handler sets none). Second, dropping money is the
only way to move carried coin to the ground, and the drop is not audited as a currency movement: the
`dropitem` line (BR-13) records the archetype and effects, which does encode the amount, but the pickup
path writes no `getitem` line for currency, so the ledger is one-sided.

A duplication-relevant subtlety: because the amount lives in the item payload and the load path accepts any
effect values the server already holds, the drop handler's only protection against a forged amount is that
the payload comes from server memory, not from the client. The request contains no item payload
(`MSG_DropItem`, `Basedef.h:1639-1648`), so a modified client cannot inject a coin value directly through
this opcode; it can only drop a coin object it already legitimately owns. The ceiling is enforced once, at
pickup, which means a stack worth more than the recipient's headroom can be permanently stuck on the
ground until it decays (BR-16).

**Rule workflow:**

```
DROP (this component)                PICKUP (_MSG_GetItem.cpp:180-202)
  no EF_VOLATILE branch                Vol = BASE_GetItemAbility(ITEM, EF_VOLATILE)
  copy payload verbatim                if (Vol == 2):
  object on ground, Delay = 90             coin = (HWORD << 8) + LWORD
  no owner, no escrow                      if (MOB.Coin + coin >= 2,000,000,000) -> message 273, stay
                                           MOB.Coin += coin; BASE_ClearItem; remove object
                                       else: normal inventory insertion (logs "getitem, ...")
```

---

### Business Rule: BR-18 - Rotation passthrough and unused item identity

**Overview:**
The client controls the visual rotation of the dropped object and the server stores it without
validation. The client's `ItemID` field is ignored entirely, because the server derives the item from
`SourType`/`SourPos`.

**Detailed description:**

`m->Rotate` is an `int` (`Basedef.h:1645`). It is passed unchanged to `CreateItem`, stored unchanged in
`pItem[empty].Rotate` (an `int`, `CItem.h:32`), truncated to `unsigned char` when the object is broadcast
(`sm.Rotate`, `Basedef.h:1602`), and echoed unchanged in the confirmation (`MSG_CNFDropItem.Rotate` is
`int`). Any 32-bit value is therefore accepted: there is no `Rotate & 3`, no bounds check, no rejection of
negative values. Downstream consumers that read `pItem[].Rotate` see the full `int`; consumers that read
the packet see `value & 0xFF`. The practical impact is limited to rendering (a client may spin or
mis-orient the object) and to any future code that trusts `pItem[].Rotate` as a small enumeration; it is a
validation gap, not an exploit path in the current code.

`MSG_DropItem.ItemID` is a second, independent identity channel supplied by the client, and it is never
read. This is arguably the safer design: the server does not let the client name the item, it lets the
client name a *slot*, and resolves the payload from its own authoritative array. It does mean, however,
that the server performs no cross-check that the client's view of the slot matches the server's - there is
no `memcmp` against a client-supplied descriptor as `_MSG_SendAutoTrade.cpp:90-94` does for shop listings,
and no item-identity echo in the confirmation. The consequence is not item spoofing (a client cannot make
the server drop an item that is not in the named slot) but weaker diagnosability: a desynchronised client
that names slot 7 while believing it holds a different item will silently drop whatever the server has in
slot 7, and the server has no evidence that a mismatch occurred.

The binary layout of the request is 12 header bytes plus `int SourType`, `int SourPos`, `int Rotate`,
`unsigned short GridX`, `unsigned short GridY`, `unsigned short ItemID`. With the project's default
structure packing (no `/Zp` override in `TMSrv.vcxproj`) the struct occupies 32 bytes including tail
padding. That size is what `BASE_CheckPacket` *would* enforce at `Basedef.cpp:6518`, but that function is
disabled (finding 11), so the value is descriptive rather than enforced.

**Rule workflow:**

```
request fields:  SourType, SourPos, Rotate, GridX, GridY, ItemID
used:            SourType (BR-07/08), SourPos (BR-08), GridX/GridY (BR-04/06), Rotate (stored/broadcast)
ignored:         ItemID
stored:          pItem[s].Rotate = m->Rotate            (full int, unvalidated)
broadcast:       MSG_CreateItem.Rotate = (unsigned char)m->Rotate
confirmed:       MSG_CNFDropItem.Rotate = m->Rotate     (full int)
```

---

### Business Rule: BR-19 - Idempotency and replay behaviour

**Overview:**
A dropped item cannot be duplicated by replaying the same drop packet, because the commit clears the
source archetype and every subsequent request for that slot is rejected by BR-09. There is, however, no
rate limit and no per-request sequence control.

**Detailed description:**

The idempotency is a side effect of clearing the slot rather than an explicit design. After a successful
drop, `SrcItem->sIndex == 0`; a replayed frame goes through BR-01 to BR-08 unchanged, is relocated by
BR-06 (possibly to a different cell than the first time), and dies at `:106-107` because
`sIndex <= 0` - silently. No second world object is created, no second log line is written, and the player
is not told that the request was a duplicate. If the replayed slot has meanwhile been filled by a
different item (for example the player picked something up), the replay drops *that* item instead, because
the handler identifies items by slot, not by identity; the client's intent is unknowable to the server.

Because replays are cheap, they cannot be used to fill the world: each request either fails at BR-09 or
consumes a different real item. They can, however, be used to abuse CPU and log volume in the rejection
paths that write a log line (BR-04 writes one per bad-coordinate packet, BR-07 one per equip-typed packet,
BR-08 two variants). There is no per-opcode throttle anywhere in `ProcessClientMessage`
(`ProcessClientMessage.cpp:38-66` performs only ID, shutdown, timestamp and tick gates), and unlike
`_MSG_Attack`, which has cooldown state, this handler keeps no per-connection timing data.

The absence of a request sequence number also means the handler cannot distinguish "the client re-sent
because the confirmation was lost" from "the player pressed drop twice". Both are handled identically: the
second is either a rejection (slot empty) or a legitimate second drop (another item in the same slot),
which is exactly what a player expects but makes the protocol non-idempotent in the strict sense. The
confirmation (`MSG_CNFDropItem`, BR-15) carries no sequence or item id either, so the client cannot
correlate a confirmation with a specific request; it must rely on packet order on a single TCP stream.

**Rule workflow:**

```
request 1 -> guards pass -> CreateItem -> ItemLog -> memset(slot) -> CNF(slot)
request 2 (replay, same slot):
   guards pass
   grid resolved (possibly different cell: packet mutated again)
   GetItemPointer -> pointer to now-empty slot
   if (sIndex <= 0) return;                  // silent, no state change, no log
request 2' (slot refilled with another item): drops that other item
```

---

### Business Rule: BR-20 - Placement authority is confined to map bounds only

**Overview:**
The handler applies no rule that ties the destination cell to the acting player. Any cell inside
`[0,4095] x [0,4095]` that is unoccupied and not height-`127` is an acceptable drop target, regardless of
where the player stands, what map region it belongs to, or whether the player could reach it.

**Detailed description:**

The complete set of destination tests is: two upper-bound comparisons (`:44`), the occupancy and
height-sentinel test inside `GetEmptyItemGrid` (`GetFunc.cpp:1878,1888`), and the occupancy re-test inside
`CreateItem` (`Server.cpp:7264`). The player's position is never consulted: `pMob[conn].TargetX`,
`TargetY`, `MOB.SPX`, `MOB.SPY` do not appear in the component. Contrast the pickup counterpart, which
requires the item to lie within three cells of `pMob[conn].TargetX/TargetY`
(`_MSG_GetItem.cpp:74-82`) and logs `GetItemFail posx:... posy:... tx:... ty:...` when it does not. The
asymmetry is not accidental in the sense of a typo - the drop check is simply absent, and the client is
trusted to report its own position.

The consequences are concrete. A modified client can drop an item beside a boss, inside a guild castle
during a war, in a starting village, or in a cell that ordinary walking cannot reach (a height-`127` cell
is excluded, but cells enclosed by such cells are not). The item will be visible on the surrounding
clients' 33x33 window (`GridMulticast`, `SendFunc.cpp:843`) even though the dropper is nowhere near, and
anybody who can reach that cell can pick it up. Because the handler also accepts `SourType == CARGO`
(BR-08), the same request is a way to pull an item out of warehouse storage from anywhere and place it in
the world - a remote-withdrawal capability that no other rule in the component restricts.

From a business standpoint the missing rule is best described as *missing authorization on the destination*:
the handler authorises the actor (BR-01), the container (BR-07/08), the item (BR-09/10) and the array
bounds (BR-04), but not the location. Whether this is exploitable in practice depends entirely on the
client, which is not part of this repository; from the server's point of view a drop is legitimate
wherever it lands. The only mitigating factor is BR-16: an item placed in an unreachable cell expires in
about 90 seconds, so the capability is useful for denial-of-item and for smuggling into restricted areas,
not for permanent storage.

**Rule workflow:**

```
validate:  0 <= GridX < 4096
           0 <= GridY < 4096
           pItemGrid[y][x] == 0
           pHeightGrid[y][x] != 127
missing:   |GridX - pMob[conn].TargetX| <= N
           |GridY - pMob[conn].TargetY| <= N
           region/map ownership or war-state checks
```

### 3.3 Rule interaction and precedence

The order of the twenty rules is itself a specification. The table below records what a request that
violates *several* rules reports, because the first matching exit wins.

| Combination | First test hit | Observed outcome |
|-------------|----------------|------------------|
| dead + in trade | BR-01 | crack error, HP resend, trade **not** cancelled |
| in trade + personal shop | BR-02 | trade torn down, shop remains open (`TradeMode` cleared by `RemoveTrade`) |
| personal shop + bad coordinates | BR-03 | auto-trade message, coordinates not logged |
| bad coordinates + flag off | BR-04 | coordinate log (flag not reached) |
| flag off + equip source | BR-05 | silent return |
| no free cell + blacklisted item | BR-06 | "can't drop here" (policy message suppressed) |
| equip source + bad position | BR-07 | `err,dropitem - sourtype`, no position log |
| bad carry position + flag off | BR-05 | silent (flag precedes) |
| empty slot + no free cell | BR-06 | "can't drop here" (item check not reached) |
| empty slot + free cell | BR-09 | silent |
| blacklisted + world full | BR-10 | policy message (blacklist precedes creation) |
| world full + valid item | BR-12 | raw literal failure message |

Two structural observations follow. First, only BR-06 (message 72), BR-10 (message 73) and BR-03
(message 170) are player-visible refusals; the ordering means a crowded cell can always mask a policy
refusal. Second, the commit rules (BR-11 to BR-15) are ordered correctly for atomicity on a single thread:
no failure exit exists between object creation and slot clearing.

---

## 4. Component Structure

```
w2pp/
├── Code/
│   ├── Basedef.h                     # protocol structs (_MSG_DropItem:1638, MSG_DropItem:1639,
│   │                                 #   MSG_CNFDropItem:1850), limits (MAX_GRID*:100-101,
│   │                                 #   MAX_ITEM:103, MAX_ITEMLIST:139, MAX_CARRY:76, MAX_CARGO:77,
│   │                                 #   MAX_EQUIP:75), ITEM_PLACE_*:86-88, STATE_OPEN:1835
│   ├── Basedef.cpp                   # GetItemPointer:2340, BASE_GetItemCode:1479,
│   │                                 #   BASE_ClearItem:4281, BASE_CheckPacket:6475 (disabled)
│   ├── ItemEffect.h                  # EF_GROUND:73, EF_HWORDCOIN:75, EF_LWORDCOIN:76,
│   │                                 #   EF_VOLATILE:77, EF_NOTRADE:153
│   ├── CPSock.cpp / CPSock.h         # frame validation:353-411, SendOneMessage:686,
│   │                                 #   SendMessageA:617, MAX_MESSAGE_SIZE:38
│   └── TMSrv/
│       ├── _MSG_DropItem.cpp         # THE COMPONENT (146 lines, 1 function)
│       ├── ProcessClientMessage.cpp  # dispatch: case _MSG_DropItem:198-200
│       ├── ProcessClientMessage.h    # declaration:103
│       ├── ProcessSecMinTimer.cpp    # SecCounter:123, periodic save:189, HP regen:601,
│       │                             #   decay trigger:1101-1102
│       ├── Server.cpp                # CreateItem:7254, GetEmptyItem:6754, RemoveTrade:7319,
│       │                             #   ProcessDecayItem:8353, AddCrackError:684, SendHpMode
│       │                             #   call sites, SaveUser:7056, Log:6701, ItemLog:9264,
│       │                             #   globals pMobGrid:363, pItemGrid:364, pHeightGrid:365,
│       │                             #   isDropItem:376, temp:277, pItem:279
│       ├── Server.h                  # external declarations (isDropItem:349, CreateItem:143, ...)
│       ├── GetFunc.cpp               # GetEmptyItemGrid:1876
│       ├── SendFunc.cpp              # SendClientMessage:27, GridMulticast:843, SendHpMode:1622
│       ├── CItem.h / CItem.cpp       # ground-object class (Mode, PosX/Y, GridCharge, State,
│       │                             #   Rotate, Delay, Decay, Height, Money, ...)
│       ├── CUser.h                   # USER_PLAY:36, TradeMode:46, Cargo[128]:50, NumError:75
│       ├── _MSG_GetItem.cpp          # mirror handler (pickup, currency credit, grid release)
│       ├── MobKilled.cpp             # loot / PK drop producer (PKDrop compiled out)
│       ├── impl.cpp                  # GM command "dropitem":1766-1773
│       └── TMSrv.vcxproj             # component is compiled into TMSrv only
```

### 4.1 Symbol inventory of the component file

| Symbol | Kind | Lines | Responsibility |
|--------|------|-------|----------------|
| `Exec_MSG_DropItem(int conn, char *pMsg)` | free function (external linkage) | 20-146 | whole transaction |
| `m` | local `MSG_DropItem*` | 22 | view of the received frame (mutated at 59-60) |
| `gridx`, `gridy` | local `int` | 54-55 | resolved destination cell |
| `titem` | local `int` | 57 | result of `GetEmptyItemGrid` (BOOL semantics) |
| `SrcItem` | local `STRUCT_ITEM*` | 99 | authoritative source item |
| `BackupItem` | local `STRUCT_ITEM*` | 100 | duplicate of `SrcItem`, only null-tested |
| `drop` | local `int` | 112 | `CreateItem` result (TRUE/FALSE, not an index) |
| `tmplog` | local `char[2048]` | 120 | serialised item for the audit log |
| `sm_ditem` | local `MSG_CNFDropItem` | 127 | confirmation frame |

The file has no static data, no `#define`, no class and no internal helper. All state lives in the global
arrays and in `pUser`/`pMob`. The only include is `ProcessClientMessage.h` (`:18`), which transitively
pulls `Server.h`, `GetFunc.h` and `SendFunc.h`; the component therefore compiles against the whole server
header surface rather than a narrow interface.

---

## 5. Dependency Analysis

### 5.1 Internal dependency graph (call/global level)

```
Exec_MSG_DropItem
├── pMob[conn] ................................ STRUCT_MOB/CMob state (HP, MaxCarry, Equip[], Carry[])
├── pUser[conn] ............................... CUser state (Mode, Trade, TradeMode, Cargo[], cSock)
├── AddCrackError ............................. Server.cpp:684   (NumError accounting, CharLogOut)
├── SendHpMode ................................ SendFunc.cpp:1622 (MSG_SetHpMode)
├── RemoveTrade ............................... Server.cpp:7319  (trade teardown + signal 900)
├── SendClientMessage ......................... SendFunc.cpp:27   (MSG_MessagePanel)
├── g_pMessageStringTable ..................... Language.h:92-93,190 (table indices 72/73/170)
├── Log ....................................... Server.cpp:6701  (text log + server window)
├── isDropItem ................................ Server.cpp:376   (config flag)
├── GetEmptyItemGrid .......................... GetFunc.cpp:1876 (3x3 occupancy/height search)
│   └── pItemGrid, pHeightGrid
├── GetItemPointer ............................ Basedef.cpp:2340 (type+pos -> STRUCT_ITEM*)
├── CreateItem ................................ Server.cpp:7254
│   ├── GetEmptyItemGrid (again)
│   ├── GetEmptyItem .......................... Server.cpp:6754 (pItem slot allocator)
│   ├── BASE_GetItemAbility (EF_GROUND) ....... Basedef.cpp:1647 (ability lookup)
│   ├── pItem[] / pItemGrid[] / pHeightGrid[]
│   └── GridMulticast ......................... SendFunc.cpp:843 (MSG_CreateItem, 33x33)
├── BASE_GetItemCode .......................... Basedef.cpp:1479 (item -> text)
├── ItemLog ................................... Server.cpp:9264 (daily item log)
└── pUser[conn].cSock.SendOneMessage .......... CPSock.cpp:686 (immediate flush of MSG_CNFDropItem)
```

Reverse (afferent) edges into the component:

```
ProcessClientMessage.cpp:198  case _MSG_DropItem -> Exec_MSG_DropItem     (only caller)
CPSock.cpp:353                frames the bytes that reach the dispatcher
Server.cpp:4001               WSA_READ loop that invokes the dispatcher
```

### 5.2 External dependencies

| Dependency | Type | Used for | Coupling |
|------------|------|----------|----------|
| Windows Sockets / Winsock | OS API (indirect) | `SendOneMessage` -> `send` | indirect through `CPSock` |
| Win32 GDI window | OS API (indirect) | `Log` -> `SetWindowText` | indirect, side effect on every rejection |
| C runtime | library | `memset`, `sprintf`, `sizeof` | trivial |
| `./item_log/TM_<date>.txt` | local file | audit trail | through `ItemLog` |
| `gameconfig.txt` | local config | `isDropItem` feature flag | read at boot / GM command |
| `./heightmap.dat` | binary data | `pHeightGrid` sentinel `127` | indirect, boot-time |
| `g_pItemList` (item list data) | in-memory table | archetype names for logging, blacklist meaning | indirect |
| DBSrv (`_MSG_DBSaveMob`) | internal service | persistence of the cleared slot | asynchronous, not in this path |

The component has **no direct external network dependency**: `CreateItem`/`GridMulticast` write into the
in-process send buffers of nearby `CPSock` objects. There is no database call, no timer registration and
no heap allocation in the handler; every write targets a pre-allocated global.

### 5.3 Data dependencies (read/write matrix)

| Data | Read | Written | Via |
|------|------|---------|-----|
| `pMob[conn].MOB.CurrentScore.Hp` | yes | no | direct |
| `pUser[conn].Mode` | yes | no | direct |
| `pUser[conn].Trade.OpponentID` | yes | yes (indirect) | `RemoveTrade` |
| `pUser[conn].TradeMode` | yes | yes (indirect) | `RemoveTrade` |
| `pMob[conn].MaxCarry` | yes | no | direct |
| `pMob[conn].MOB.Equip/Carry[slot]` | yes | yes (`memset`) | `GetItemPointer` + direct |
| `pUser[conn].Cargo[slot]` | yes | yes (`memset`) | `GetItemPointer` + direct |
| `m->GridX/GridY` (packet buffer) | yes | yes (resolved cell) | direct |
| `pItemGrid[y][x]` | yes | yes (indirect) | `GetEmptyItemGrid`, `CreateItem` |
| `pHeightGrid[y][x]` | yes | no | `GetEmptyItemGrid`, `CreateItem` |
| `pItem[slot]` | no | yes (indirect) | `CreateItem` |
| `pUser[conn].NumError` | no | yes (indirect) | `AddCrackError` |
| `fLogFile`, `fItemLogFile` | no | yes (indirect) | `Log`, `ItemLog` |
| `pUser[*].cSock` send buffer | no | yes (indirect) | `GridMulticast` for 33x33 peers |

---

## 6. Afferent and Efferent Coupling

Coupling is measured per named symbol (C++ free function / class / struct), since the component is a
single free function in a procedural code base. Afferent (Ca) counts distinct call sites that depend on the
symbol; efferent (Ce) counts distinct external symbols the symbol calls or reads. Values were obtained by
searching the whole `Code/` tree and by the CodeGraph index (caller/callee edges), excluding the component
itself where noted.

| Component | Afferent Coupling (Ca) | Efferent Coupling (Ce) | Critical | Notes |
|-----------|------------------------|------------------------|----------|-------|
| `Exec_MSG_DropItem` | 1 (`ProcessClientMessage.cpp:199`) | 14 | High | single entry, fan-out to the whole inventory/creation surface |
| `CreateItem` | 9 call sites in 5 files (`_MSG_DropItem:112`, `imple:272`, `MobKilled:2193,2327,2384`, `ProcessSecMinTimer:2273`, `Server:6306,7173,8588`) | 7 (`GetEmptyItemGrid`, `GetEmptyItem`, `BASE_GetItemAbility`, `GridMulticast`, `pItem`, `pItemGrid`, `pHeightGrid`) | High | shared with loot, events, boot and GM commands |
| `GetEmptyItemGrid` | 3 call sites (`_MSG_DropItem:57`, `CreateItem:7259`, `CreateTreasureBox:9397`) | 2 (`pItemGrid`, `pHeightGrid`) | High | placement policy for items |
| `GetItemPointer` | 25 call sites in 8 files (`_MSG_DropItem:99,100`; `_MSG_UseItem` x17; `_MSG_TradingItem` x4; `_MSG_Sell:84`; `_MSG_PutoutSeal:59`; `CFileDB:1806`) | 0 | Medium | pure resolver, used by every inventory handler |
| `GetEmptyItem` | 2 (`CreateItem:7267`, `CreateTreasureBox:9403`) | 1 (`pItem`) | High | 4999-object capacity gate |
| `RemoveTrade` | 80+ call sites across trade/shop/attack/quest handlers | 4 (`pUser`, `pMobGrid`, `SendClientSignal`, `GetCreateMob`/`GridMulticast`) | Medium | shared session teardown |
| `AddCrackError` | 40+ call sites | 3 (`pUser.NumError`, `SendClientMessage`, `CharLogOut`) | Low | telemetry |
| `MSG_DropItem` (struct) | 1 producer (client), 1 consumer (this handler) | 0 | Low | packet contract |
| `MSG_CNFDropItem` (struct) | 3 producers (`_MSG_DropItem:127`, `MobKilled:2345`, `MobKilled:2404`) | 0 | Low | confirmation contract |
| `CItem` (ground object) | 12+ users (`CreateItem`, `CreateTreasureBox`, `GetCreateItem`, `ProcessDecayItem`, `_MSG_GetItem`, `_MSG_Action`, `imple`, ...) | 1 (`STRUCT_ITEM`) | High | central world-object type |

Interpretation:

- **Low afferent, high efferent** is the signature of a *transaction script*: nothing depends on this
  handler (it is a leaf in the call graph), but it depends on fourteen collaborators. A change to
  `CreateItem`'s signature or to `CItem`'s layout forces a review of this file, while a change to this file
  cannot break anything else except the client contract.
- **`CreateItem` is the true centre of gravity.** Its 9 call sites sit in five subsystems (player command,
  GM command, loot, timer event, boot); it is therefore the component's most dangerous dependency and the
  reason the drop path cannot be changed in isolation.
- **`GetItemPointer` is the second hotspot** with 25 call sites; its `MAX_CARRY`/`MAX_CARGO` bounds are the
  de-facto container contract for eight handlers.
- **Cohesion inside the component is functional but not sequential**: all lines serve "make a drop happen",
  yet the file mixes four concerns (session authorisation, packet sanitisation, placement policy, world
  mutation). That is reflected in the exit table of Section 2.2, which has fifteen exits for one
  transaction.

---

## 7. Endpoints

The component exposes no HTTP/REST, GraphQL or gRPC surface. Its endpoint is one message type on the
game server's proprietary binary protocol (TCP, per-byte keyed checksum on top of a 12-byte `HEADER`,
`CPSock.h:42-50`).

| Endpoint (opcode) | Method / Verb | Direction | Declared size | Description |
|-------------------|---------------|-----------|---------------|-------------|
| `_MSG_DropItem` = `114 \| FLAG_CLIENT2GAME` = `0x0272` (626) | client request (fire-and-forget) | client -> game server | `sizeof(MSG_DropItem)` = 32 bytes with default packing (derived); not enforced, see finding 11 | Drop one item from a container to a map cell |
| `_MSG_CNFDropItem` = `117 \| FLAG_GAME2CLIENT` = `0x0175` (373) | server confirmation | game server -> client | `sizeof(MSG_CNFDropItem)` = 28 bytes (derived) | Confirms the drop and reports the resolved cell |
| `_MSG_CreateItem` = `110 \| FLAG_CLIENT2GAME` = `0x026E` (622) | server broadcast (emitted by `CreateItem`) | game server -> 33x33 peers | `sizeof(MSG_CreateItem)` | Announces the new ground object to nearby clients |
| `_MSG_DecayItem` = `111 \| FLAG_GAME2CLIENT` = `0x016F` (367) | server broadcast (emitted by the decay sweep) | game server -> 33x33 peers | `sizeof(MSG_DecayItem)` | Removes the object after BR-16 |

### 7.1 Request field contract (`MSG_DropItem`, `Basedef.h:1639-1648`)

| Field | Type | Offset (derived) | Validated | Meaning |
|-------|------|------------------|-----------|---------|
| `Size` | `short` | 0 | only `[12,8192]` in `CPSock` | frame length |
| `KeyWord` | `char` | 2 | checksum machinery | decryption key index |
| `CheckSum` | `char` | 3 | verified in `CPSock` | frame checksum |
| `Type` | `short` | 4 | dispatcher switch | `0x0272` |
| `ID` | `short` | 6 | `[0, MAX_USER)` at `ProcessClientMessage.cpp:42` | connection id |
| `ClientTick` | `unsigned int` | 8 | `!= SKIPCHECKTICK` at `ProcessClientMessage.cpp:63` | anti-replay tick |
| `SourType` | `int` | 12 | BR-07/BR-08 | `0` equip (forbidden), `1` carry, `2` cargo |
| `SourPos` | `int` | 16 | BR-08 | slot in the selected container |
| `Rotate` | `int` | 20 | not validated (BR-18) | visual rotation, truncated to `uchar` on broadcast |
| `GridX` | `unsigned short` | 24 | BR-04, then rewritten by BR-06 | destination cell X |
| `GridY` | `unsigned short` | 26 | BR-04, then rewritten by BR-06 | destination cell Y |
| `ItemID` | `unsigned short` | 28 | never read (BR-18) | unused client-side identity |

### 7.2 Response field contract (`MSG_CNFDropItem`, `Basedef.h:1850-1859`)

| Field | Type | Source | Meaning |
|-------|------|--------|---------|
| `Type` | `short` | constant `0x0175` | message id |
| `Size` | `short` | `sizeof` | frame length |
| `SourType` | `int` | request echo | container that was emptied |
| `SourPos` | `int` | request echo | slot that was emptied |
| `Rotate` | `int` | request echo | rotation actually stored |
| `GridX` | `unsigned short` | **resolved** cell | where the object really is |
| `GridY` | `unsigned short` | **resolved** cell | where the object really is |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Game client (W2) | External client | Issues `_MSG_DropItem`, receives `MSG_CNFDropItem`/`MSG_CreateItem` | TCP, custom binary framing + per-byte keyed checksum | `MSG_DropItem` (32 B), `MSG_CNFDropItem` (28 B) | Silent for 11 of 15 rejections; no retry, no sequence, no resend |
| World grid (`pItemGrid`, `pHeightGrid`) | In-process global state | Placement of the object; occupancy and terrain sentinel | array access | `unsigned short [4096][4096]`, `char [4096][4096]` | `FALSE` from `GetEmptyItemGrid` -> message 72 |
| Ground object array (`pItem`, `CItem`) | In-process global state | Ownership of the world object | array access | `CItem[5000]` (payload + timers + render fields) | `FALSE` from `CreateItem` -> raw literal message |
| Broadcast bus (`GridMulticast` + `CPSock`) | In-process fan-out | `MSG_CreateItem` to 33x33 viewers | in-memory send buffers, then TCP | `MSG_CreateItem` | `SendMessageA` failure resets buffer, caller ignores (CPSock.cpp:634-656) |
| Trade subsystem (`RemoveTrade`, `Trade`, `TradeMode`) | Internal component | Cancels a conflicting trade before dropping | direct calls | `MSG_Trade` | Trade is destroyed unconditionally, no log |
| Autotrade / personal shop (`TradeMode`, `AutoTrade`) | Internal component | Lockout while a shop is open | flag read | `MSG_SendAutoTrade` state | message `_NN_CantWhenAutoTrade` |
| Decay timer (`ProcessDecayItem`) | Internal scheduler | Ends the ground object's life | direct call from `ProcessSecTimer` | `CItem.Delay/Decay`, `MSG_DecayItem` | none; grid release is defective (transposed indices) |
| Audit log (`ItemLog`) | Local file | Permanent record of the drop | `fprintf` to `./item_log/TM_<date>.txt` | text line | return value ignored |
| Server text log (`Log`) | Local file + Win32 window | Policy/violation telemetry | `fprintf` + `SetWindowText` | text line | return value ignored |
| Configuration (`gameconfig.txt`) | Local file | Enables/disables dropping | line-oriented text, strict label `strcmp` | `Etc Settings: ... DropItem <n> ...` | missing file -> modal error, flag stays 0 |
| GM command channel (`ProcessImple`) | Internal admin interface | Runtime toggle of `isDropItem` | `"/dropitem <n>"` parsed by `sscanf` | text command | no validation of the new value beyond `unsigned int` parse |
| Persistence (`SaveUser` -> DBSrv) | Internal service | Persists the emptied slot | `_MSG_DBSaveMob` over the DBSrv socket | `MSG_DBSaveMob` with `STRUCT_MOB` + `Cargo[128]` | asynchronous; no acknowledgement in this path |

---

## 9. Design Patterns & Architecture

| Pattern / Decision | Implementation | Location | Purpose |
|--------------------|----------------|----------|---------|
| Transaction Script | one function performs the whole drop transaction with explicit ordering | `_MSG_DropItem.cpp:20-146` | matches the handler-per-message architecture of the server |
| Guard Clauses / early return | 14 pre-commit exits | `:24-110` | keeps the commit block flat and avoids nesting |
| Fail-closed default | `SourType == 0` (equip) rejected; unknown type rejected; `NULL` rejected | `:68-97,103` | prevents default-initialised packets from reaching creation |
| Server-authoritative item resolution | client names a slot, server resolves the payload from `pMob`/`pUser` | `:99-100` | blocks item spoofing through the packet (contrast `_MSG_SendAutoTrade`'s `memcmp`) |
| Two-phase intent (abandoned) | `BackupItem` declared, never copied or restored | `:100,103` | a rollback design that was not completed |
| Mediator for world objects | `CreateItem` encapsulates slot allocation, initialisation, grid registration and broadcast | `Server.cpp:7254` | single point of truth for ground-object creation (9 call sites) |
| Spatial index / occupancy map | dense `pItemGrid[y][x]` + `pHeightGrid[y][x]` | `Server.cpp:364-365` | O(1) occupancy and terrain tests; 9-cell local search for placement |
| Publish/Subscribe (area multicast) | `GridMulticast(x, y, msg, skip)` over `pMobGrid` 33x33 | `SendFunc.cpp:843-952` | area-of-interest broadcast without a subscriber list |
| Hard-coded policy table | inline `sIndex` blacklist | `:109-110` | item non-transferability, duplicated across handlers |
| Feature flag (global) | `isDropItem` from config + GM command | `Server.cpp:376,1109`; `imple.cpp:1766` | operational kill switch |
| Fire-and-forget confirmation | `SendOneMessage` return ignored | `:139`; `CPSock.cpp:686` | immediate flush, no acknowledgement protocol |
| Lazy persistence | state committed in memory, persisted by rotation save | `ProcessSecMinTimer.cpp:189`; `Server.cpp:7056` | avoids a DB round-trip per action |
| Layering | handler -> engine helpers -> globals; no service/repository abstraction | whole component | typical of the decompiled single-process design |

Architectural decisions worth recording:

1. **Ground objects are global, not per-map-instance.** One `pItem[5000]` and one pair of 4096x4096 grids
   serve the whole server. The component therefore has no world/instance parameter and no sharding
   concern; capacity is a single global limit of 4999 objects (`GetEmptyItem`).
2. **Placement policy lives in a shared helper, not in the handler.** The handler cannot choose a cell
   other than "requested or adjacent", and `CreateItem` re-runs the same policy, so the handler's own
   coordinate work is advisory.
3. **The component is deliberately stateless.** No cooldown, no per-player drop counter, no request
   identity. Any abuse mitigation must come from the dispatcher (which has none) or the OS/log layer.
4. **Policy is expressed in code, not data.** The only data-driven behaviour is `EF_GROUND` for the
   render charge and `EF_VOLATILE` on the pickup side. Non-droppability is a literal list, and the
   declarative `EF_NOTRADE` flag is bypassed.

---

## 10. Technical Debt & Risks

### 10.1 Risk register

| Risk | Level | Component Area | Issue | Impact |
|------|-------|----------------|-------|--------|
| R-01 | High | Placement authority | No proximity/region check on `GridX/GridY` (`:44-49`); pickup enforces 3 tiles (`_MSG_GetItem.cpp:74-82`) | Modified client can place items map-wide, including unreachable cells and restricted areas; supports smuggling and item denial |
| R-02 | High | Grid integrity (data produced by this path) | `ProcessDecayItem` clears `pItemGrid[PosX][PosY]` (transposed) at `Server.cpp:8381` | Stale occupancy blocks future drops; unrelated live item loses its grid registration; possible ghost/orphan objects and pickup/decay desync |
| R-03 | High | Item policy duplication | Blacklist duplicated with different sets in `_MSG_Trade`, `_MSG_SendAutoTrade`, `_MSG_Sell`, `_MSG_TradingItem`, `MobKilled` | Policy drift: `3193/3194` droppable here, `532..537`/`3994` droppable in the (disabled) PK path, `EF_NOTRADE` ignored |
| R-04 | High | Transaction completeness | No `SendItem` re-sync after clearing; confirmation is fire-and-forget (`:125-139`) | Client can retain ghost items when the confirmation is lost; no repair path but relog |
| R-05 | Medium | Protocol validation | `BASE_CheckPacket` body commented out (`Basedef.cpp:6476-6594`); only `[12,8192]` enforced | Short frames cast to 32 bytes; fields may be read from adjacent buffer contents |
| R-06 | Medium | Duplication/rollback | `SrcItem` and `BackupItem` are the same pointer; no copy, no restore (`:99-103`) | No undo if a post-create step fails; misleading code invites a bad "fix" |
| R-07 | Medium | Silent failures | 11 of 15 exits produce no client message (`:24,31,44,51,68,74,86,91,103,106`) | Poor player experience; retry storms on a disabled flag; no way to distinguish refusal reasons client-side |
| R-08 | Medium | Cargo drop without NPC check | `SourType == CARGO` accepted anywhere (`:86-95`) | Warehouse items can be pulled to the ground remote from the warehouse NPC |
| R-09 | Medium | Container bound asymmetry | Uses live `MaxCarry` (30/45/60) while `GetItemPointer` uses `MAX_CARRY` (64) | Intentional (blocks expansion-bag slots 60/61) but undocumented; any future `MaxCarry > 64` would silently mismatch |
| R-10 | Medium | Currency handling | No currency branch on drop; value only validated at pickup with 2e9 ceiling (`_MSG_GetItem.cpp:192-197`) | Money can be stranded (over-cap recipient) and decays after ~90 s; no currency audit line |
| R-11 | Medium | Audit granularity | `dropitem` line has no source container/slot, no destination, no world slot (`:120-123`) | Cannot reconstruct item provenance or investigate dupes from the log alone |
| R-12 | Medium | Unvalidated `Rotate` | Arbitrary `int` stored in `pItem.Rotate` and truncated on broadcast (`:112`, `Server.cpp:7279`) | Rendering anomalies; any future code trusting `Rotate` as an enum inherits the gap |
| R-13 | Low | Dead condition | `drop >= MAX_ITEM` can never be true (`CreateItem` returns 1/0) (`:114`) | Misleading failure analysis; suggests the author expected an index return |
| R-14 | Low | Redundant work | `GetEmptyItemGrid` called by handler and again inside `CreateItem` (`:57`, `Server.cpp:7259`) | Extra 9-cell scan per drop; two sources of truth for the resolved cell |
| R-15 | Low | Unused field | `MSG_DropItem.ItemID` never read (`Basedef.h:1647`) | Protocol carries an unused identity channel; no cross-validation possible |
| R-16 | Low | Player-visible raw literal | `"Can't create object(item)"` not in `g_pMessageStringTable` (`:116`) | Untranslated message in a fully localised UI |
| R-17 | Low | Ordering anomaly | Grid resolution before item validation (`:54-66` vs `:68-110`); policy check after resolution | Wrong message precedence ("can't drop here" masks the medal message); packet mutated on rejected requests |
| R-18 | Low | Log buffering | `Log`/`ItemLog` do not flush per line (`Server.cpp:6722-6723,9282-9283`) | Tail loss on crash; audit gaps exactly when they matter most |
| R-19 | Low | Telemetry blind spots | Empty-slot and invalid-index drops leave no trace (`:103-107`) | Cannot distinguish malformed clients from silent normal behaviour |
| R-20 | Low | No throttling | Dispatcher performs no per-opcode rate limit (`ProcessClientMessage.cpp:38-66`) | Log/CPU amplification via rejection paths (BR-04/07/08) |

### 10.2 Detailed findings

**TD-01 - Duplicated `GetItemPointer` call creates a false rollback contract (R-06).**
Lines 99-100 invoke the resolver twice with byte-identical arguments. Both results are the same address
inside `pMob[conn].MOB` or `pUser[conn].Cargo`; no `STRUCT_ITEM` is copied. The null test at 103 therefore
tests one condition twice, and the name `BackupItem` documents an intention that the code does not
implement. Any later maintenance that "uses the backup to restore the item on failure" would restore the
same memory it just cleared, i.e. a no-op - the failure mode would be a silent loss, not a duplication.
The risk is therefore primarily maintainability, with a latent correctness trap.

**TD-02 - Duplication analysis of the drop path (explicit).**
Four independent duplication vectors were examined.

(a) *Packet replay.* Blocked by the source clear (BR-19). A second identical frame finds `sIndex == 0` and
returns at `:106` with no state change. No duplication.

(b) *Crash between commit and persistence.* The world object lives only in `pItem[]` (memory); the cleared
inventory slot is persisted later by `SaveUser`. A crash after `memset` and before the rotation save loses
the ground object and keeps the cleared inventory in memory but not on disk - the character file still has
the item, the ground object is gone. Net effect on restart: the item returns to the inventory (because the
DB was never updated) and no ground copy exists. No duplication, but a small item-creation window if the
ground object had been picked up before the crash (pickup is itself memory-only and equally unpersisted).
Because both sides of the transfer are memory-only until the next save, a crash can produce either loss or
a restored item, never two persisted copies.

(c) *Grid aliasing via the decay defect.* `ProcessDecayItem` clears a transposed cell
(`Server.cpp:8381`). If the transposed cell belongs to a *different live* object, that object's
registration is erased while its `pItem[].Mode` stays 1. A subsequent `CreateItem` can allocate that cell
for a new object (`pItemGrid[y][x]` is now 0), producing two live `pItem` entries associated with one cell.
The pickup handler (`_MSG_GetItem.cpp:164-172`) then refuses the orphan (`pItemGrid != itemID`), sends
`DecayItem` to the picker and - if the cell is empty - re-registers the orphan, orphaning the newer
object in turn. The cycle leaks `pItem` slots and produces client-visible objects that cannot be picked up.
This is the only realistic duplication/aliasing chain reachable from the drop path, and its root cause is
one transposed index pair in the timer.

(d) *Concurrent requests.* Impossible by construction: the socket loop (`Server.cpp:3973-4002`) and the
timer (`Server.cpp:3725-3726`) run on the same thread, and `ProcessClientMessage` is called serially per
connection. No lock or atomic is needed, and none exists.

**TD-03 - Grid/terrain indexing convention inconsistency (R-02).**
The engine's convention is `[y][x]` (declaration `Server.cpp:364-365`; drop `Server.cpp:7287`; pickup
`_MSG_GetItem.cpp:164-172,248`; helpers `GetFunc.cpp:1878-1895`). The decay sweep is the sole violator
(`Server.cpp:8381`), and the two locals it uses are named `itemPosX`/`itemPosY` but written in X-major
order. Because both values are clamped by the creation-time bounds check, the write stays inside the array
and therefore produces no crash - only silent corruption. This survives because no test exercises the decay
sweep.

**TD-04 - Policy expressed five times, differently (R-03).**
The non-transferable set is re-implemented at `_MSG_DropItem.cpp:109-110`, `_MSG_Trade.cpp:147-177`,
`_MSG_SendAutoTrade.cpp:80-82`, `_MSG_Sell.cpp:92-93`, `_MSG_TradingItem.cpp:165-179` and
`MobKilled.cpp:2321-2325,2380-2400`. Sets differ in membership and in range endpoints. `EF_NOTRADE`
(`ItemEffect.h:153`) exists and is honoured by two of those call sites, but not by this handler. The drop
handler is the most permissive of the item-release paths (it does not block `3193/3194`), so the
divergence is directly exploitable as "drop it, pick it up somewhere else" if a blocked-by-trade item is
in the permissive set of the other path.

**TD-05 - Missing destination authorisation (R-01).**
Covered in BR-20. The absence is structural, not a missing `if`: the component never reads the actor's
position, so no local patch can add the rule without inventing a radius policy. The pickup side's 3-tile
rule (`_MSG_GetItem.cpp:74-82`) is the only precedent in the code base for what the radius should be.

**TD-06 - Silent rejection surface (R-07).**
Eleven exits produce no client message. Three of them are pure telemetry (`:44`, `:68`, `:78`, `:86`,
`:91`), four are "nothing to do" (`:51`, `:103`, `:106`), and two are state conflicts (`:24`, `:31`). The
component's design assumes the client pre-validates (it only sends slots it believes hold items, only
drops while alive, never during a trade). A modified or desynchronised client experiences the refusal as a
no-op. There is no error code, no reason field and no signal message - in contrast with `MSG_DecayItem`,
which the pickup handler uses precisely to force the client to give up on an item.

**TD-07 - Disabled frame validation (R-05).**
`BASE_CheckPacket` is a table of `Type == X && Size != sizeof(X)` tests (`Basedef.cpp:6475-6594`) whose
entire body is inside a block comment that opens at line 6476 and closes at 6594, followed by
`return FALSE;`. Its call sites (`Server.cpp:3898,3983`, `CPSock.cpp:547`) are inside `#ifdef
_PACKET_DEBUG`, and that macro is defined for Debug builds only (`TMSrv.vcxproj:61`), so in Release the
function is not even called. The only remaining length gate is `CPSock::ReadMessage`'s
`Size > MAX_MESSAGE_SIZE || Size < sizeof(HEADER)` (`CPSock.cpp:397`). A `_MSG_DropItem` frame of 12 bytes
passes, is cast to a 32-byte struct at `:22`, and its `SourType/SourPos/Rotate/GridX/GridY` are read from
whatever bytes follow in the 128 KB receive buffer. The subsequent guards (BR-04/07/08/09) bound the
damage, but the component is validating memory it does not own rather than the fields the client declared.

**TD-08 - Commit without acknowledgement (R-04).**
`SendOneMessage`'s `BOOL` is discarded. `SendMessageA` itself can fail silently: it resets `nSendPosition`
and `nSentPosition` when the socket or buffer state is inconsistent (`CPSock.cpp:623-656`) and returns
`FALSE`, which the caller ignores. There is no retry queue for this message and no per-item authoritative
re-sync. The de-facto recovery mechanism is logout/login, which rewrites the client's inventory from
`SaveUser`.

**TD-09 - Redundant failure predicate (R-13).**
`int drop = CreateItem(...); if (drop <= 0 || drop >= MAX_ITEM)` reads as if `CreateItem` returns an item
index. It returns `TRUE` (1) or `FALSE` (0) on all paths (`Server.cpp:7257,7265,7270,7316`). The
`>= MAX_ITEM` half is unreachable and the `int` type hides the boolean contract. This is a documentation
defect with a real cost: it invites the reader to believe creation can succeed yet return an index.

**TD-10 - Code smells inside a 146-line file.**
Duplicate resolver call (99-100); duplicated policy blocks (109-110 vs 141-145, i.e. the blacklist appears
as a positive and a negative predicate); magic number `14` in `AddCrackError(conn, 1, 14)`; magic
archetypes with no symbolic names; `memset` of a structure that could be `BASE_ClearItem`
(`Basedef.cpp:4281`) for symmetry with the rest of the code base; `sprintf`/`memset` instead of safer
alternatives; local variable `titem` typed `int` for a `BOOL` result; `drop` misnamed; a `char[2048]`
buffer for a string that cannot exceed ~100 bytes; and the unused `MSG_DropItem.ItemID`.

**TD-11 - Build-configuration asymmetry.**
Debug builds define `_PACKET_DEBUG` (`TMSrv.vcxproj:61`), which enables packet logging in `CPSock`
(`CPSock.cpp:544-552`) and in the receive loop (`Server.cpp:3980-3988`). Release does not
(`TMSrv.vcxproj:79`). Behaviour that differs between builds in a validation-adjacent area is a risk: the
`BASE_CheckPacket` calls in the debug blocks will always return `FALSE` today, but if the table were
re-enabled, Debug builds would start rejecting frames that Release accepts.

---

## 11. Test Coverage Analysis

### 11.1 Evidence of test infrastructure (or its absence)

| Search | Command / location | Result |
|--------|--------------------|--------|
| Test source files by name | recursive scan for `*test*`, `*spec*`, `*fixture*`, `*mock*` (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`, `Debug`, `Release`) | 0 files |
| Test projects | recursive scan for `*.vcxproj` | only `Code/TMSrv/TMSrv.vcxproj` and `Code/DBSrv/DBSrv.vcxproj`; no test project |
| Test frameworks | search for `gtest`, `CppUnit`, `catch2`, `BOOST_TEST` | 0 matches |
| Assertions | search for `assert(` in `Code/**/*.cpp` and `Code/**/*.h` | 0 matches |
| Test harness / main | no `main` or test entry point beyond the Win32 `WinMain` server; no mocking layer | — |
| CI | no `.github`, no pipeline definition anywhere in the repository root | 0 files |
| Documentation of tests | `docs/` contains only analysis reports (`docs/reports/**`), no test plan, no acceptance criteria | — |
| Test hooks in the component | no `#ifdef TEST`, no dependency injection, no seam: the handler calls globals directly | `_MSG_DropItem.cpp` (whole file) |

Conclusion: **the DropItem Handler has no automated test coverage of any kind** - not unit, not
integration, not end-to-end, not property-based. There is no seam to test it in isolation: every
dependency is a global or a free function with global side effects, and the only input path is a raw
socket buffer.

### 11.2 Coverage assessment

| Area | Unit Tests | Integration Tests | Coverage | Test Quality |
|------|-----------|-------------------|----------|--------------|
| `Exec_MSG_DropItem` guard chain (BR-01..BR-10) | 0 | 0 | 0% | no executable specification; 15 exits unverified |
| `Exec_MSG_DropItem` commit block (BR-11..BR-15) | 0 | 0 | 0% | create/log/clear/confirm ordering unverified |
| `CreateItem` (`Server.cpp:7254`) | 0 | 0 | 0% | slot exhaustion, cell relocation, field initialisation unverified |
| `GetEmptyItemGrid` (`GetFunc.cpp:1876`) | 0 | 0 | 0% | 9-cell scan order and boundary clamping unverified |
| `GetItemPointer` (`Basedef.cpp:2340`) | 0 | 0 | 0% | type/pos matrix unverified |
| `ProcessDecayItem` (`Server.cpp:8353`) | 0 | 0 | 0% | decay timing and grid release unverified (defect R-02 lives here) |
| Money drop/pickup (BR-17) | 0 | 0 | 0% | 2e9 ceiling and stranded-coin case unverified |
| Protocol framing (`CPSock::ReadMessage`) | 0 | 0 | 0% | truncated-frame behaviour unverified |

### 11.3 Testability obstacles (why the gap persists)

1. **No composition root.** `pMob`, `pUser`, `pItem`, `pItemGrid`, `pHeightGrid`, `temp`,
   `g_pMessageStringTable` and `isDropItem` are process globals declared in `Server.h`/`Basedef.h`. A unit
   test would have to allocate the full 48 MB of grid state (`pItemGrid` 32 MB + `pHeightGrid` 16 MB) plus
   5000 `CItem` objects and 1000 `CUser` objects to exercise one branch.
2. **No observable outputs other than globals.** The handler returns `void` and reports through socket
   buffers (`pUser[conn].cSock`) and text files (`fItemLogFile`, `fLogFile`). Assertions would have to
   read those buffers or intercept file I/O.
3. **The handler is not isolated behind an interface.** It calls `CreateItem`, `GetItemPointer` and
   `GridMulticast` directly, so a test cannot substitute a fake world without linking the whole server.
4. **Windows-only build.** `TMSrv.vcxproj` targets `v143` with `_WINDOWS`, `winmm`/`ws2_32` usage and
   Win32 window calls in `Log`. Cross-platform CI would require an abstraction layer that does not exist.
5. **Behaviour is coupled to the tick loop.** The decay rule (BR-16) depends on `SecCounter`, which only
   advances inside `ProcessSecTimer` under a Win32 timer; there is no injectable clock.

### 11.4 Highest-value untested behaviours (for a future suite)

| Priority | Behaviour | Why it matters |
|----------|-----------|----------------|
| 1 | `ProcessDecayItem` grid release (`Server.cpp:8376-8393`) | the transposed indices are a live defect; a single test of "create at (x,y), decay, assert `pItemGrid[y][x] == 0`" would have caught it |
| 2 | Commit ordering and source conservation (`:112-139`) | verifies no item loss/duplication across create-failure and send-failure |
| 3 | Bound derivation: `MaxCarry` vs `MAX_CARRY` (`:78`) and `MAX_CARGO` (`:91`) | protects the expansion-bag slots and the warehouse range |
| 4 | `GetEmptyItemGrid` boundary clamping (`GetFunc.cpp:1881-1896`) | prevents out-of-bounds indexing at map edges |
| 5 | Blacklist membership consistency across all release paths | catches the policy drift of R-03 |
| 6 | Currency pickup ceiling (`_MSG_GetItem.cpp:192-197`) | verifies no overflow and no item loss on refusal |
| 7 | Truncated-frame handling (`CPSock.cpp:397` + handler cast) | documents the consequence of the disabled size table |

---

## 12. Ambiguities, Assumptions and Confidence

| Item | Statement | Confidence |
|------|-----------|------------|
| Item archetype semantics | `508/509/522/446/526..537` are guild plate/medal/sub-guild items and `747/3993/3994` are crowns, inferred from `_MSG_Buy.cpp:165-206`, `Server.cpp:528-578,589-610`, `_MSG_MessageWhisper.cpp:854-857`, `_MSG_Trade.cpp:147-177`, `_MSG_Sell.cpp:92-93` | Medium-High (grouping certain, names inferred; the item-list data file is not in the repository) |
| Decay timing | `Delay = 90` with one decrement per `SecCounter % 2` on a 500 ms timer, i.e. ~90 s | Medium-High (derived from `Server.cpp:3631` + `ProcessSecMinTimer.cpp:123,1101`); if `SecTimer` were re-armed at 1000 ms elsewhere the lifetime would double |
| Packet sizes | 32 bytes for `MSG_DropItem`, 28 for `MSG_CNFDropItem`, computed from field layout with default MSVC packing (no `/Zp` in the project) | Medium (no static_assert in the code base) |
| `pHeightGrid` sentinel | `127` means "not placeable" | High (consistent across `GetFunc.cpp:1878-2033`) |
| `MaxCarry` values | 30/45/60 depending on bag items 3467 in slots 60/61 | High (`ProcessDBMessage.cpp:751-757`) |
| `isDropItem` default | 0 (disabled) unless gameconfig.txt supplies a value | High (`Server.cpp:376,1109`) |
| Cargo drop as remote withdrawal | server accepts it; whether the shipped client can emit it away from the warehouse is unknown | Medium (server-side only analysed) |
| Duplication chain (c) | requires the decay transposition to hit another live object's cell, then a new `CreateItem` at that cell, then a pickup attempt | Medium (code path is real; probability depends on map/item density) |
| Historic intent of `BackupItem` | abandoned rollback design | Medium (name + unused variable; no comment) |

No business rule in this report was invented: every rule is anchored to a line of code or to a
configuration/data source cited in place. Where the meaning of a constant depends on data not present in
the repository (`g_pItemList`), that dependency is stated explicitly.

---

## 13. Reference Index (files and lines used)

| File | Lines | Used for |
|------|-------|----------|
| `Code/TMSrv/_MSG_DropItem.cpp` | 1-146 | the component, all business rules |
| `Code/TMSrv/ProcessClientMessage.cpp` | 38-66, 198-200 | dispatch, pre-dispatch gates |
| `Code/TMSrv/ProcessClientMessage.h` | 103 | declaration |
| `Code/Basedef.h` | 75-77, 86-88, 95-101, 103, 139, 170, 398-412, 458-459, 925-930, 1592-1616, 1638-1648, 1835-1836, 1850-1859, 2046-2057 | limits, structs, constants, packet layout |
| `Code/Basedef.cpp` | 1479-1495, 2340-2372, 4281-4291, 6475-6596 | item code, pointer resolver, clear, disabled packet table |
| `Code/TMSrv/Server.cpp` | 277, 279, 363-365, 376, 684-707, 761-1115, 1313-1314, 3631-3632, 3725-3726, 3973-4002, 6701-6727, 6754-6763, 7056-7083, 7127-7172, 7254-7317, 7319-7352, 8353-8396, 9264-9284, 9392-9449 | globals, config, helpers, creation, decay, logging |
| `Code/TMSrv/GetFunc.cpp` | 1876-1899, 1901-2040 | grid resolution |
| `Code/TMSrv/SendFunc.cpp` | 27-45, 843-952, 1622-1645 | client message, grid multicast, HP/mode |
| `Code/TMSrv/ProcessSecMinTimer.cpp` | 40, 123, 176-198, 601-619, 1101-1102 | tick, save rotation, decay trigger |
| `Code/TMSrv/CItem.h` / `CItem.cpp` | 24-45 / 22-30 | ground-object model and defaults |
| `Code/TMSrv/CUser.h` | 36, 46, 50, 59, 75 | user modes, trade, cargo, error counter |
| `Code/TMSrv/_MSG_GetItem.cpp` | 20-252 | pickup counterpart, currency, grid release |
| `Code/TMSrv/_MSG_SendAutoTrade.cpp` | 70-120 | personal shop lockout source |
| `Code/TMSrv/_MSG_Trade.cpp` | 140-180, 251-380 | guild-item transfer policy, money transfer |
| `Code/TMSrv/_MSG_Sell.cpp` | 84-93, 215-218 | divergent blacklist, coin ceiling |
| `Code/TMSrv/_MSG_TradingItem.cpp` | 76-80, 163-179 | container movement policy |
| `Code/TMSrv/MobKilled.cpp` | 2193, 2305-2420 | alternative drop producer (PK path disabled) |
| `Code/TMSrv/imple.cpp` | 46-82, 1766-1773 | GM command `dropitem` |
| `Code/TMSrv/ProcessDBMessage.cpp` | 751-757 | `MaxCarry` derivation |
| `Code/CPSock.cpp` / `CPSock.h` | 353-470 / 38-50 | framing, checksum, send |
| `Code/ItemEffect.h` | 73-77, 153 | effect codes (`EF_GROUND`, coin, `EF_VOLATILE`, `EF_NOTRADE`) |
| `Code/TMSrv/Language.h` | 92-93, 190 | message table indices used (72, 73, 170) |
| `Code/TMSrv/TMSrv.vcxproj` | 59-79 | build defines, no packing override |
| `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` | 141, 365 | component classification and consistency observations |

---

*End of report. No project file was created, modified or deleted by this analysis; the only file written is
this report.*
