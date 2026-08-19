# Component Deep Analysis Report

## Component: CItem

**Scope analyzed:** W2PP legacy C/C++ codebase at `legacy/Code` (TMSrv game server)
**Folders excluded:** `.git`, `.opencode`
**Report date:** 2026-08-19

---

## 1. Executive Summary

`CItem` is the domain-entity component that models an **in-world dropped item** in the W2PP game server (TMSrv). It is a plain C++ data class that describes the runtime state of a physical item object occupying a cell in the world grid, as opposed to items held in a player's inventory (which are plain `STRUCT_ITEM` values inside `pMob[].Carry` / `Equip`).

The component is defined in exactly two files:

- `legacy/Code/TMSrv/CItem.h` — the class declaration and the global array `extern CItem pItem[MAX_ITEM];`
- `legacy/Code/TMSrv/CItem.cpp` — the constructor/destructor that initializes the object to an empty state.

The class itself is intentionally **anemic** (a passive data holder with no behavior). All behavior is implemented externally by functions that read/write the global `pItem[5000]` array through free functions distributed across the TMSrv module. The component's true role is therefore best understood as the **"world item" subsystem**: an in-memory object pool plus a grid index (`pItemGrid`) that tracks which item occupies each world cell, together with the lifecycle logic that creates, places, renders, picks up, updates, and decays world items.

Key findings:

- **Global, mutable, single-instance pool.** `pItem` is a module-level global array of 5000 `CItem` records (`MAX_ITEM = 5000`), indexed by a slot number. There is no encapsulation, no ownership abstraction, and no concurrency control; all subsystems touch the array directly.
- **Slot addressing protocol.** The wire (client) identifier for a world item is the internal array index **plus 10000** (`sm.ItemID = empty + 10000`); inbound messages subtract 10000 (`itemID = m->ItemID - 10000`). Index 0 is reserved/invalid.
- **The `Mode` field is the de-facto state machine**: `0` = empty/free slot, `1` = normal dropped/placed item, `2` = event/treasure item that is exempt from decay.
- **The grid index is the spatial backbone.** `pItemGrid[MAX_GRIDY][MAX_GRIDX]` (4096x4096) stores the item slot occupying each world cell, so only one item may occupy a cell at a time; a 3x3 neighborhood search finds a free placement.
- **Multiple subsystems consume the component.** At least 12 source files reference `pItem`/`CItem`, spanning client-message handlers, the per-second timer, mob-kill loot, death penalties, rendering/broadcast, and castle-gate logic.
- **Dead/unused fields.** `Money`, `Open`, `ItemQuest` (set only in the constructor), and `Unk[20]` are not read anywhere in the analyzed code; they are reserved or vestigial.
- **No automated tests exist** anywhere in the project (verified across the entire repository).

---

## 2. Data Flow Analysis

The following traces the lifecycle of an in-world item through the `CItem`/`pItem` subsystem.

### 2.1 Item creation (world population)

```
1. Spawn source (three paths):
   a. Server startup: for each g_pInitItem[i], build a STRUCT_ITEM (sIndex) and call
      CreateItem(posX, posY, &Item, rotate, 1)   [Server.cpp:7166-7173]
   b. Player drop / mob loot / death penalty: call CreateItem(...) from
      _MSG_DropItem.cpp:112, MobKilled.cpp:2193/2327/2384, ProcessSecMinTimer.cpp:2273
   c. Event/treasure: CreateTreasureBox(...)  [Server.cpp:9392]
2. CreateItem validates sIndex, resolves a free grid cell (GetEmptyItemGrid),
   obtains a free slot (GetEmptyItem), fills pItem[empty], and registers it in
   pItemGrid[y][x] = empty.                  [Server.cpp:7254-7317]
3. CreateItem builds an MSG_CreateItem and broadcasts it to the surrounding grid
   via GridMulticast so connected clients render the new item.
```

### 2.2 Item rendering (on connect / on move)

```
1. GridMulticast / connection-spawn scans pItemGrid[y][x] within view.
2. SendCreateItem(conn, item) -> GetCreateItem(idx, &sm) builds MSG_CreateItem
   from pItem[idx] (PosX/PosY/ITEM/Rotate/State/Height).  [SendFunc.cpp:320, GetFunc.cpp:1195]
3. Special rendering transforms for sIndex 3145 (guild-zone statue) and 5700 (hidden).
4. On de-render / move-out-of-view, SendRemoveItem sends MSG_DecayItem. [SendFunc.cpp:510]
```

### 2.3 Item pickup (GetItem — client request)

```
1. Exec_MSG_GetItem receives MSG_GetItem (ItemID, DestType, DestPos, GridX/Y).
2. Validates player state, trade, destination, item slot, distance (<=3 cells),
   grid ownership, item index, special-item constraints.
3. Distinguishes coin stacks (Volatile == 2) from tangible items.
4. For tangible items: copies pItem[itemID].ITEM into a carry slot, logs the
   pickup, sends MSG_CNFGetItem, clears grid cell + Mode, broadcasts MSG_DecayItem.
   [_MSG_GetItem.cpp:20-252]
```

### 2.4 Item drop (client request)

```
1. Exec_MSG_DropItem receives MSG_DropItem (SourType, SourPos, Rotate, GridX/Y).
2. Validates player state, grid bounds, isDropItem flag, source slot, item index,
   and drop-prohibited item list.
3. Calls CreateItem(...) to place the item, zeroes the source carry/cargo slot,
   and replies MSG_CNFDropItem.             [_MSG_DropItem.cpp:20-146]
```

### 2.5 Item decay (per-second timer)

```
1. ProcessSecMinTimer.cpp:1102 calls ProcessDecayItem() once per server tick.
2. ProcessDecayItem walks pItem[], skips sIndex 1727, Mode 0/2, and decrements Delay.
3. When Delay hits 0 and Decay != -1, it clears ITEM, clears the grid cell, sets
   Mode=0, and broadcasts MSG_DecayItem.    [Server.cpp:8353-8396]
```

### 2.6 Interactive world objects (gates/doors/keys)

```
1. Exec_MSG_UpdateItem validates State (0..5), ItemID range, resolves gateid.
2. Applies castle-gate logic (CCastleZakum) and key (EF_KEYID) verification.
3. Calls UpdateItem(gateid, state, &height) which mutates pItem[Gate].State/Height,
   handles ground-mask collision and keyed-gate mob spawn. [Server.cpp:7472-7556]
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | World item slot must be in range `1..MAX_ITEM-1` (index 0 invalid) | _MSG_GetItem.cpp:52-55 |
| Addressing | Client item ID = internal index + 10000 (decode subtracts 10000) | _MSG_GetItem.cpp:50, Server.cpp:7298 |
| State Machine | `Mode`: 0 = empty, 1 = normal item, 2 = event/treasure (decay-exempt) | Server.cpp:7272/9408, Server.cpp:8365 |
| Grid Constraint | Only one item per grid cell via `pItemGrid`; 3x3 neighborhood search for placement | GetFunc.cpp:1876-1899 |
| Capacity | Fixed pool `MAX_ITEM = 5000`; first free slot (linear scan) via GetEmptyItem | Server.cpp:6754-6763 |
| Validation | Player must be alive and in `USER_PLAY` mode to get/drop | _MSG_GetItem.cpp:24, _MSG_DropItem.cpp:24 |
| Validation | Cannot get/drop while trading (removes trade) or in autotrade mode | _MSG_GetItem.cpp:31-42, _MSG_DropItem.cpp:31-42 |
| Business Logic | GetItem pickup distance limited to 3 cells (Chebyshev) from player target | _MSG_GetItem.cpp:74-82 |
| Business Logic | Item 1727 requires player Level >= 1000 to pick up; never decays | _MSG_GetItem.cpp:84-85, Server.cpp:8362 |
| Business Logic | Item 470 (PilulaOrc quest pill) is a one-time consumable granting +9 SkillBonus | _MSG_GetItem.cpp:94-127 |
| Business Logic | Items sIndex 490-499 trigger a server-wide notice on pickup | _MSG_GetItem.cpp:129-143 |
| Business Logic | Coin stacks (Volatile == 2) are added directly to player Coin (cap 2,000,000,000) | _MSG_GetItem.cpp:180-202 |
| Validation | Tangible pickup requires a free carry slot; occupied slot causes slot shift | _MSG_GetItem.cpp:206-225 |
| Validation | Drop source type: EQUIP rejected; CARRY/CARGO position bounds validated | _MSG_DropItem.cpp:68-97 |
| Validation | Drop requires `isDropItem` server flag enabled | _MSG_DropItem.cpp:51-52 |
| Business Logic | Guild medals and special items (508,509,522,526-537,446,747,3993,3994) cannot be dropped | _MSG_DropItem.cpp:109-145 |
| Validation | Item sIndex must be in range `1..MAX_ITEMLIST-1` (6500) | _MSG_GetItem.cpp:91, _MSG_DropItem.cpp:106 |
| Decay | Items decay when Delay reaches 0 unless Decay == -1 (permanent) | Server.cpp:8368-8394 |
| State Logic | Interactive gates use State OPEN/CLOSED/LOCKED and require matching key (EF_KEYID) | _MSG_UpdateItem.cpp, Server.cpp:7472 |
| State Logic | Keyed gates (EF_KEYID 1..14) are auto-locked back to STATE_LOCKED after use | ProcessSecMinTimer.cpp:2157-2206 |
| Rendering | Item 3145 is rendered with dynamic guild-zone ownership data; item 5700 is hidden | GetFunc.cpp:1215-1230 |
| Attack | World item sIndex 746 is an attackable world object | _MSG_Attack.cpp:172 |

### Detailed breakdown of the business rules

---

### Business Rule: World item slot addressing and bounds

**Overview:**
World items are stored in a fixed global array `pItem[5000]`. The internal identity of an item is its array index. This index is never sent raw over the network; instead the wire ID is the index plus a 10000 offset. Any inbound or outbound reference must be mapped through this offset, and every reference is range-checked against `MAX_ITEM`.

**Detailed description:**
The `pItem` array is declared as `extern CItem pItem[MAX_ITEM]` where `MAX_ITEM` is `5000` (Basedef.h:103). Slot 0 is treated as invalid/absent throughout the system: `GetEmptyItem()` iterates from index 1 upward (`for (int i = 1; i < MAX_ITEM; i++)`) and returns 0 only when no free slot exists, which callers interpret as failure. When an item is created, its wire-facing `ItemID` is computed as `empty + 10000` (Server.cpp:7298), and the same transform is applied in `GetCreateItem` (`sm->ItemID = idx + 10000`, GetFunc.cpp:1205) and `SendRemoveItem` (`sm_deci.ItemID = 10000 + itemid`, SendFunc.cpp:518). On the inbound side, `Exec_MSG_GetItem` recomputes the internal index via `int itemID = m->ItemID - 10000` (_MSG_GetItem.cpp:50) and then validates `itemID <= 0 || itemID >= MAX_ITEM`. The same bounds guard appears in `Exec_MSG_UpdateItem` (`m->ItemID < 10000 || m->ItemID >= 10000 + MAX_ITEM`, _MSG_UpdateItem.cpp:36) and the subsequent `gateid` re-check (_MSG_UpdateItem.cpp:45). This consistent offset-and-check pattern is the single most important correctness invariant of the component, since an out-of-range index would produce an out-of-bounds array access into the 5000-entry pool.

**Rule workflow:**
```
1. Client sends ItemID (index + 10000).
2. Handler subtracts 10000 to recover internal index.
3. Handler validates index against [1, MAX_ITEM).
4. pItem[index] is read/written only after validation passes.
5. Outbound messages re-add 10000 before sending.
```

---

### Business Rule: Item `Mode` state machine

**Overview:**
`CItem::Mode` encodes the lifecycle state of each pool slot and doubles as the free-slot indicator. Three values are meaningful: `0` (empty/free), `1` (normal placed/dropped item), and `2` (event/treasure item that must not decay).

**Detailed description:**
The constructor sets `Mode = 0` (CItem.cpp:24), representing an empty slot. `GetEmptyItem()` returns the first slot whose `Mode == 0` (Server.cpp:6758), and `CreateItem` sets `Mode = 1` when placing a normal item (Server.cpp:7272), whereas `CreateTreasureBox` sets `Mode = 2` for event/treasure boxes (Server.cpp:9408). The `Mode` value is used as the presence test throughout the code: pickup validates `pItem[itemID].Mode == 0` to reject a phantom item (_MSG_GetItem.cpp:55), rendering gates on `pItem[titem].Mode` (SendFunc.cpp:611/739/754), and the connection-spawn scan uses it to decide whether to send or clear the cell (SendFunc.cpp:611-614). Crucially, `ProcessDecayItem` skips slots where `Mode == 0 || Mode == 2` (Server.cpp:8365), meaning event/treasure items (Mode 2) never decay away, while normal dropped items (Mode 1) are subject to the decay timer. When an item is removed (pickup, decay, or trade), the handler sets `Mode = 0` to free the slot.

**Rule workflow:**
```
1. Slot allocated: Mode = 0.
2. CreateItem -> Mode = 1 (normal) OR CreateTreasureBox -> Mode = 2 (event).
3. Presence checks test Mode != 0.
4. Decay timer processes Mode == 1 only; Mode 0/2 are skipped.
5. Removal sets Mode = 0, freeing the slot for reuse.
```

---

### Business Rule: One-item-per-cell grid occupancy

**Overview:**
The world is partitioned into a 4096x4096 cell grid. A parallel index array `pItemGrid[y][x]` records which item slot occupies each cell, enforcing the constraint that only one item may sit in a cell. Placement searches a 3x3 neighborhood when the target cell is occupied or blocked.

**Detailed description:**
`pItemGrid` is declared as `unsigned short pItemGrid[MAX_GRIDY][MAX_GRIDX]` (Server.cpp:364). `CreateItem` refuses placement if `pItemGrid[y][x]` is already non-zero (Server.cpp:7264) and, upon successful placement, records `pItemGrid[y][x] = empty` (Server.cpp:7287). `GetEmptyItemGrid` (GetFunc.cpp:1876) first accepts the requested cell if it is empty AND the height grid cell is walkable (`pHeightGrid != 127`); otherwise it scans a 3x3 neighborhood (y-1..y+1, x-1..x+1) for a free, walkable cell, updating the caller's coordinates. On pickup, the handler verifies grid ownership: it requires `pItemGrid[itemY][itemX] == itemID` (_MSG_GetItem.cpp:164) and that the client-supplied grid matches the server's record (`itemX == m->GridX && itemY == m->GridY`, _MSG_GetItem.cpp:174), rejecting and clearing otherwise. Removal clears the cell with `pItemGrid[y][x] = 0` (_MSG_GetItem.cpp:248, Server.cpp:8381). This grid is also consulted by rendering (SendFunc.cpp:609-615) and by `UpdateItem`'s ground-mask collision logic, which relocates mobs standing on cells the item's ground footprint covers (Server.cpp:7510-7553).

**Rule workflow:**
```
1. Placement requests a target cell.
2. If occupied or blocked (height 127), search 3x3 neighborhood.
3. Claim the free cell: pItemGrid[y][x] = slot.
4. Pickup verifies cell ownership matches slot and client grid.
5. Removal releases the cell: pItemGrid[y][x] = 0.
```

---

### Business Rule: Item pickup eligibility and validation

**Overview:**
Picking up an item (`Exec_MSG_GetItem`) is gated by a sequence of eligibility checks: the player must be alive and playing, must not be trading, must target a carry destination, and the item must exist within pickup range and be legitimate.

**Detailed description:**
The handler first rejects the request if the player is dead or not in `USER_PLAY` mode, logging a crack error and re-sending HP/mode state (_MSG_GetItem.cpp:24-29). If the player is currently trading (`Trade.OpponentID` set), both sides' trades are removed (_MSG_GetItem.cpp:31-36); if autotrade mode is active, a "cannot when auto trade" message is sent (_MSG_GetItem.cpp:38-42). The destination type must be `ITEM_PLACE_CARRY` (1); any other destination is logged and rejected (_MSG_GetItem.cpp:44-48). The item index is bounds-checked, and if the slot is empty (`Mode == 0`), a `MSG_DecayItem` is sent back to desynchronize the client (_MSG_GetItem.cpp:52-72). A proximity rule limits pickup to a 3x3-cell Chebyshev distance from the player's target coordinates (`TargetX/Y` within ±3 of `PosX/Y`, _MSG_GetItem.cpp:74-82). Finally the item's `sIndex` must be a valid item-list index (`1..MAX_ITEMLIST-1`, i.e. 1..6499, _MSG_GetItem.cpp:91). These checks collectively prevent exploiting item pickup during invalid states or at impossible ranges.

**Rule workflow:**
```
1. Verify alive + USER_PLAY.
2. Reject/clear if trading or autotrade.
3. Require DestType == ITEM_PLACE_CARRY.
4. Bounds-check item index; reject empty slots.
5. Enforce 3-cell pickup range.
6. Validate sIndex range.
```

---

### Business Rule: Special pickup items (1727, 470, 490-499)

**Overview:**
A small set of item indices carries bespoke pickup behavior that overrides the normal "move to carry" logic: a level-gated item (1727), a one-time quest consumable (470), and server-announced items (490-499).

**Detailed description:**
Item 1727 is gated by player level: `if (itemID == 1727 && pMob[conn].MOB.CurrentScore.Level < 1000) return;` (_MSG_GetItem.cpp:84-85) silently prevents low-level pickup; it is also the one item that is explicitly exempt from decay in `ProcessDecayItem` (`if (pItem[ItemCount].ITEM.sIndex == 1727) continue;`, Server.cpp:8362), marking it as a persistent world object. Item 470 (the Orc pill quest item) is a one-time consumable: if the player already has the quest flag `Mortal.PilulaOrc`, a "you've done it already" message is shown; otherwise the item is consumed (`BASE_ClearItem`), the grid cell cleared, `Mode = 0`, the quest flag is set to 1, and the player receives `+9 SkillBonus` with an emotion and stat refresh (_MSG_GetItem.cpp:94-127). Items with sIndex in the range 490-499 are broadcast to the whole server via `SendNotice` ("[player] got [item]") while still being picked up normally (_MSG_GetItem.cpp:129-143). These special cases demonstrate how the otherwise generic item flow is specialized by item index.

**Rule workflow:**
```
1. 1727: require Level >= 1000; never decays.
2. 470: if quest flag set -> reject; else consume, set flag, +9 SkillBonus, refresh.
3. 490-499: broadcast server notice, then normal pickup.
```

---

### Business Rule: Coin stack pickup and currency cap

**Overview:**
Items flagged `EF_VOLATILE == 2` represent coin stacks carried in the world. Picking one up adds its value to the player's `Coin` balance instead of placing an item in the carry inventory, subject to a hard currency ceiling.

**Detailed description:**
In `Exec_MSG_GetItem`, after grid ownership is confirmed, the handler reads the volatile attribute via `BASE_GetItemAbility(ditem, EF_VOLATILE)` (_MSG_GetItem.cpp:180). When it equals 2, the coin amount is reconstructed from the item's high/low word coin attributes: `coin1 = (EF_HWORDCOIN << 8) + EF_LWORDCOIN` (_MSG_GetItem.cpp:184-189). The handler then computes the prospective total `tcoin = coin1 + pMob[conn].MOB.Coin` and, if `tcoin >= 2000000000` (2,000,000,000), sends a message and aborts the pickup without granting coins (_MSG_GetItem.cpp:192-196) — an explicit overflow guard. Otherwise the coin value is added to `MOB.Coin`, a `MSG_DecayItem` is sent to remove the stack from the world, and `BASE_ClearItem` frees the item (_MSG_GetItem.cpp:197-202). Coin pickup bypasses the carry-slot machinery entirely, so the destination position is not consulted for coin stacks.

**Rule workflow:**
```
1. Read EF_VOLATILE == 2.
2. Reconstruct coin value from HWORD/LWORD coin attributes.
3. Guard against total exceeding 2,000,000,000.
4. Add coins, remove stack from world, clear item.
```

---

### Business Rule: Tangible pickup carry-slot resolution

**Overview:**
For tangible (non-coin) items, pickup places the item into a specific carry inventory slot supplied by the client, but only if that slot is currently empty; otherwise the request is re-routed to the last valid slot.

**Detailed description:**
After the coin branch, the handler validates the destination slot: `if (m->DestPos < 0 || m->DestPos >= MAX_CARRY)` it logs a trading failure and returns (_MSG_GetItem.cpp:206-210). It then inspects the target carry slot `bItem = &pMob[conn].MOB.Carry[m->DestPos]` and computes `can = (bItem->sIndex == 0) ? 1 : 0` — the slot is only writable if it is empty (_MSG_GetItem.cpp:212-215). If the slot is occupied, and the destination is a valid carry position, the handler decrements `DestPos` and re-sends that slot's item to the client (`SendItem(conn, ITEM_PLACE_CARRY, m->DestPos, ...)`), then returns without granting the item (_MSG_GetItem.cpp:217-225). When the slot is free, the world item is copied via `memcpy(bItem, ditem, sizeof(STRUCT_ITEM))`, an `ItemLog` audit record is written with the item code, and a `MSG_CNFGetItem` confirmation plus `MSG_DecayItem` are sent (_MSG_GetItem.cpp:226-251). The grid cell is cleared and `Mode` set to 0.

**Rule workflow:**
```
1. Validate DestPos in [0, MAX_CARRY).
2. Accept only if target slot empty (sIndex == 0).
3. If occupied -> decrement slot, resend, abort.
4. If free -> copy item, log pickup, confirm, clear world cell + slot.
```

---

### Business Rule: Item drop eligibility and prohibited items

**Overview:**
Dropping an item (`Exec_MSG_DropItem`) is gated by player state, a server feature flag, grid availability, and source validation, and is fully prohibited for guild-medal and other special items.

**Detailed description:**
The handler rejects dead/non-`USER_PLAY` players and clears trades (_MSG_DropItem.cpp:24-42). It bounds-checks the target grid cell and, critically, requires the global `isDropItem` server-config flag to be nonzero (`if (isDropItem == 0) return;`, _MSG_DropItem.cpp:44-52), a runtime switch controlling whether world drops are permitted at all. It then resolves a free grid cell via `GetEmptyItemGrid`, rejecting with "cannot drop here" if none is available (_MSG_DropItem.cpp:57-66). The source type is validated: `ITEM_PLACE_EQUIP` (0) is outright rejected, carry positions are bounded by `MaxCarry`, and cargo positions by `MAX_CARGO` (_MSG_DropItem.cpp:68-97). The source item's `sIndex` must be valid, and it must not be in the prohibited set — guild medals and special items 508, 509, 522, 526-537, 446, 747, 3993, 3994 (_MSG_DropItem.cpp:106-110). If prohibited, the "guild medal cannot be dropped" message is sent and the operation is aborted; otherwise `CreateItem` places the item, the source slot is zeroed, and a `MSG_CNFDropItem` confirms placement (_MSG_DropItem.cpp:112-145).

**Rule workflow:**
```
1. Verify alive + USER_PLAY; clear trades.
2. Bounds-check grid; require isDropItem flag.
3. Find a free grid cell.
4. Validate SourType and source position bounds.
5. Validate sIndex; reject prohibited items.
6. CreateItem + zero source slot + confirm.
```

---

### Business Rule: Item creation and decay lifecycle

**Overview:**
World items are created through `CreateItem` (and the specialized `CreateTreasureBox`), which initialize a slot's physical state, and are later removed either by pickup or by the periodic decay timer that removes abandoned items.

**Detailed description:**
`CreateItem` (Server.cpp:7254) validates `sIndex`, resolves a grid cell, finds an empty slot, sets `Mode = 1`, records position, copies the `STRUCT_ITEM`, sets `Rotate`, `State = STATE_OPEN`, `Delay = 90`, `Decay = 0`, and computes `GridCharge` from the item's `EF_GROUND` attribute (the size of its ground footprint used by `UpdateItem`). Height is taken from the height grid. The item is registered in `pItemGrid` and broadcast to the local grid (_Server.cpp:7272-7316). `ProcessDecayItem` (Server.cpp:8353) is invoked from the per-second timer (ProcessSecMinTimer.cpp:1102). It walks the pool, skipping sIndex 1727 (persistent), and skipping `Mode 0/2` (free or event slots). It decrements `Delay` each tick until zero; when `Delay` is 0 and `Decay != -1`, the item is cleared (`BASE_ClearItem`), its grid cell zeroed, `Mode = 0`, and a `MSG_DecayItem` is multicast to despawn it (_Server.cpp:8368-8394). The `Decay == -1` branch is designed to mark permanent items but no assignment to `-1` was found in the code. A notable defect exists in the loop: `ItemCount++` is executed both in the loop header and inside the body (Server.cpp:8356-8357), so the loop actually steps by 2 and can skip slots; the wrap-around cap at 5000 (`g_dwInitItem + 1`) is also applied inconsistently.

**Rule workflow:**
```
1. CreateItem fills a slot (Mode=1, Delay=90, Decay=0, grid registered, broadcast).
2. Per-second timer calls ProcessDecayItem.
3. Skip sIndex 1727, Mode 0/2.
4. Decrement Delay; on 0 and Decay != -1, clear item + cell + Mode=0 + broadcast.
```

---

### Business Rule: Interactive gates, keys, and state transitions

**Overview:**
Certain world items (gates, doors, treasure boxes) are interactive objects whose open/close/locked state is manipulated through `Exec_MSG_UpdateItem` and `UpdateItem`, and which can require a matching key to open.

**Detailed description:**
`Exec_MSG_UpdateItem` validates that the submitted state is in `0..5` and that the item ID is a valid world item, then delegates to castle-gate logic via `CCastleZakum::OpenCastleGate` (_MSG_UpdateItem.cpp:30-52). It reads the item's key requirement via `BASE_GetItemAbility(&pItem[gateid].ITEM, EF_KEYID)` (_MSG_UpdateItem.cpp:57). If the current or requested state is the locked/open keyed state, the handler scans the player's carry for an item with a matching `EF_KEYID` and consumes it (zeroes the slot, re-sends it); if no matching key exists, it sends a "no key" message and aborts (_MSG_UpdateItem.cpp:59-90). `UpdateItem` (Server.cpp:7472) delegates to `BASE_UpdateItem` to compute the new ground-mask footprint; on success it updates `State`/`Height`, resets `Delay`, validates `GridCharge` is in `0..5`, and — for keyed gates (`keyid == 15`) transitioning open — spawns a `GATE` NPC (Server.cpp:7499-7500). It also relocates any mob standing on the footprint cells the item now covers (Server.cpp:7510-7553). Independently, the per-second timer auto-locks keyed gates: for an item with `EF_KEYID` in `1..14` currently `STATE_OPEN`, after a one-tick `Delay` it calls `UpdateItem(ipg, 3 /*STATE_LOCKED*/, ...)` and re-broadcasts the locked state (ProcessSecMinTimer.cpp:2157-2206).

**Rule workflow:**
```
1. Client sends UpdateItem (ItemID, State).
2. Validate state range + item bounds; try castle-gate handler.
3. Determine EF_KEYID requirement.
4. If keyed transition -> require matching carry key, consume it.
5. UpdateItem computes new footprint, sets State/Height, handles mob collision.
6. Timer auto-locks keyed gates (1..14) back to STATE_LOCKED after use.
```

---

## 4. Component Structure

The `CItem` component boundary is the class definition plus the global pool; its behavioral surface is spread across the TMSrv module.

```
legacy/Code/
├── Basedef.h                          # STRUCT_ITEM, MAX_ITEM(5000), MAX_ITEMLIST(6500),
│                                      #   item-state constants, item protocol messages,
│                                      #   ITEM_PLACE_* constants, item effect (EF_*) constants
├── ItemEffect.h                       # SHARED: item effect attribute IDs (EF_*)
└── TMSrv/
    ├── CItem.h                        # CItem class + extern CItem pItem[MAX_ITEM]   [the component]
    ├── CItem.cpp                      # CItem()/~CItem() constructor (anemic)       [the component]
    ├── Server.cpp                     # CreateItem, UpdateItem, GetEmptyItem,
    │                                  #   ProcessDecayItem, CreateTreasureBox, pItemGrid,
    │                                  #   server-start init-item placement
    ├── Server.h                       # extern declarations (CreateItem/UpdateItem/GetEmptyItem)
    ├── GetFunc.cpp                    # GetCreateItem (render), GetEmptyItemGrid (grid search)
    ├── SendFunc.cpp                   # SendCreateItem, SendRemoveItem, GridMulticast, grid scanning
    ├── ProcessClientMessage.cpp       # dispatches _MSG_GetItem/DropItem/UpdateItem/DeleteItem
    ├── _MSG_GetItem.cpp               # Exec_MSG_GetItem (pickup logic)
    ├── _MSG_DropItem.cpp              # Exec_MSG_DropItem (drop logic)
    ├── _MSG_UpdateItem.cpp            # Exec_MSG_UpdateItem (gate/key logic)
    ├── _MSG_DeleteItem.cpp            # Exec_MSG_DeleteItem (inventory delete, not world item)
    ├── _MSG_Attack.cpp                # attackable world item sIndex 746
    ├── MobKilled.cpp                  # loot drops + player-death item loss via CreateItem
    ├── ProcessSecMinTimer.cpp         # per-second item decay trigger + gate auto-lock + event drops
    ├── imple.cpp                      # gate/treasure state transitions via UpdateItem
    ├── CCastleZakum.cpp               # castle-gate integration
    └── CWarTower.cpp                  # includes CItem.h (build dependency)
```

Note: `CItem.cpp`/`.h` are intentionally minimal. The class is a data container; all domain behavior lives in the free functions listed above, which operate on the global `pItem` array and the parallel `pItemGrid` index.

---

## 5. Dependency Analysis

### Internal Dependencies (within TMSrv)

```
ProcessClientMessage.cpp --dispatches--> _MSG_GetItem.cpp / _MSG_DropItem.cpp
                                          / _MSG_UpdateItem.cpp / _MSG_DeleteItem.cpp
  |-- _MSG_GetItem.cpp    --uses--> pItem, pItemGrid, BASE_* helpers, SendItem, GridMulticast
  |-- _MSG_DropItem.cpp   --uses--> pItem, pItemGrid, GetEmptyItemGrid, CreateItem, GetItemPointer
  |-- _MSG_UpdateItem.cpp --uses--> pItem, UpdateItem, CCastleZakum, BASE_GetItemAbility
Server.cpp   --defines--> CreateItem, UpdateItem, GetEmptyItem, ProcessDecayItem, CreateTreasureBox, pItemGrid
SendFunc.cpp --uses--> pItem, GetCreateItem, GridMulticast
GetFunc.cpp  --uses--> pItem (GetCreateItem, GetEmptyItemGrid)
MobKilled.cpp --uses--> CreateItem, pItem, SendItem
ProcessSecMinTimer.cpp --uses--> ProcessDecayItem, UpdateItem, CreateItem, pItem
imple.cpp    --uses--> pItem, UpdateItem, GridMulticast
CCastleZakum.cpp --uses--> pItem, UpdateItem
```

### Shared helper dependency (Code/Basedef.cpp / Basedef.h)

```
BASE_GetItemAbility(STRUCT_ITEM*, EF_*)  -- attribute lookup (EF_VOLATILE, EF_KEYID, EF_GROUND,
                                            EF_HWORDCOIN, EF_LWORDCOIN, EF_QUEST)
BASE_UpdateItem(...)                     -- ground-mask footprint computation for gates
BASE_ClearItem(STRUCT_ITEM*)             -- zero an item record
BASE_GetItemCode(...)                    -- item audit-log code
BASE_NeedLog(...)                        -- audit-log predicate
BASE_GetVillage(x,y)                     -- zone lookup for item 3145 rendering
```

### External Dependencies

- **No external libraries** are used by this component. It depends only on the standard C runtime (`string.h` for `memset`/`memcpy`) and the in-project `Basedef.h` / `ItemEffect.h` shared headers.
- **Data files (runtime):** item definitions (`g_pItemList`), initial world items (`g_pInitItem`), and the height grid (`pHeightGrid`) are loaded at server startup by `CReadFiles` and consumed by `CreateItem`/rendering. These are data, not code, dependencies.
- **Client protocol:** the component depends on the wire message structs (`MSG_GetItem`, `MSG_DropItem`, `MSG_UpdateItem`, `MSG_CreateItem`, `MSG_DecayItem`, `MSG_CNFGetItem`, `MSG_CNFDropItem`) defined in `Basedef.h`.

---

## 6. Afferent and Efferent Coupling

The component in this C/C++ codebase is the global `pItem` pool and the `CItem` type. Afferent coupling (incoming) is the set of source files that read/write `pItem`/`CItem`; efferent coupling (outgoing) is the set of external types/functions the component depends on.

| Component / Area | Afferent Coupling | Efferent Coupling | Critical |
|------------------|-------------------|-------------------|----------|
| `pItem` global pool / `CItem` type | 12 files | 3 (`STRUCT_ITEM`, `BASE_*` helpers, message structs) | High |
| `CreateItem` | 5 call sites (drop, loot, death, event, init) | 5 (`GetEmptyItemGrid`, `GetEmptyItem`, `BASE_GetItemAbility`, `GridMulticast`, `pItemGrid`) | High |
| `UpdateItem` | 6 call sites (update-msg, timer, gates, mob-kill, imple) | 4 (`BASE_UpdateItem`, `BASE_GetItemAbility`, `pItemGrid`, `GridMulticast`/`CreateMob`) | Medium |
| `ProcessDecayItem` | 1 (per-second timer) | 4 (`BASE_ClearItem`, `GridMulticast`, `pItemGrid`, `pItem`) | Medium |
| `GetEmptyItemGrid` | 2 (`CreateItem`, `_MSG_DropItem`) | 2 (`pItemGrid`, `pHeightGrid`) | Medium |
| `GetEmptyItem` | 3 (`CreateItem`, `CreateTreasureBox`, castle) | 1 (`pItem`) | Medium |

**Interpretation:** The component is high-fan-in (12 afferent files) because `pItem` is a module-level global shared implicitly across the whole game loop. This is a classic sign of a "shared mutable global" architectural seam — every handler and timer mutates the same pool without ownership boundaries, making the coupling high and the blast radius large. Efferent coupling is low, confined to the shared `Basedef.h` types and `BASE_*` helper functions, which is appropriate for a data entity.

---

## 7. Endpoints

The component does not expose a traditional REST/GraphQL/gRPC API. In this client/server game architecture, the "endpoints" are the binary network protocol messages that trigger or carry the world-item logic. Flags: `FLAG_CLIENT2GAME` = client->server, `FLAG_GAME2CLIENT` = server->client.

### Client-to-Server (inbound)

| Endpoint (constant) | Value | Direction | Description |
|---------------------|-------|-----------|-------------|
| `_MSG_GetItem` | 112 \| CLIENT2GAME | client -> server | Player requests to pick up a world item |
| `_MSG_DropItem` | 114 \| CLIENT2GAME | client -> server | Player requests to drop an item into the world |
| `_MSG_UpdateItem` | 116 \| both | client -> server | Player opens/closes an interactive world item (gate/door/treasure) |
| `_MSG_DeleteItem` | 228 \| CLIENT2GAME | client -> server | Player deletes an inventory item (related but not a world-item op) |

### Server-to-Client (outbound)

| Endpoint (constant) | Value | Direction | Description |
|---------------------|-------|-----------|-------------|
| `_MSG_CreateItem` | 110 \| CLIENT2GAME flag | server -> client | Spawn/render a world item at a grid cell |
| `_MSG_DecayItem` | 111 \| GAME2CLIENT | server -> client | Remove/despawn a world item |
| `_MSG_CNFGetItem` | 113 \| GAME2CLIENT | server -> client | Confirm item pickup and destination |
| `_MSG_CNFDropItem` | 117 \| GAME2CLIENT | server -> client | Confirm item drop and placement |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Client item protocol | Internal (client/server) | Pickup, drop, update, spawn, despawn world items | TCP binary messages | C structs (`MSG_GetItem`, etc.) | Silent `return` + `AddCrackError`/`Log` for invalid state; `MSG_DecayItem` to resync client on mismatch |
| `pItemGrid` grid index | Internal (shared state) | Spatial occupancy of world items | In-memory array | `unsigned short[MAX_GRIDY][MAX_GRIDX]` | Cell cleared/overwritten; bounds guarded in `GetEmptyItemGrid` |
| `pHeightGrid` terrain grid | Internal (shared state) | Walkability of cells for placement | In-memory array | `char[MAX_GRIDY][MAX_GRIDX]` (`127` = blocked) | Checked in `GetEmptyItemGrid` |
| `BASE_*` item helpers | Internal (shared module) | Item attributes, clear, ground masks, logging | Function calls | C function signatures | N/A (in-process) |
| Item/Mob/Guild config files | External data | Item definitions, initial items, terrain | Local files | Loaded by `CReadFiles` at startup | Startup log + abort on load failure |
| `CCastleZakum` | Internal (subsystem) | Castle gate integration on item update | Function call | C++ class method | Returns TRUE to short-circuit `Exec_MSG_UpdateItem` |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Anemic Domain Model / Data Holder | `CItem` exposes public fields with no behavior | CItem.h:27-39 | Encapsulate per-item state as a passive record |
| Object Pool | `pItem[MAX_ITEM]` with `GetEmptyItem()` slot allocator | Server.cpp:6754, CItem.h:45 | Reuse a fixed 5000-slot array for world items |
| Registry / Grid Index | `pItemGrid[y][x]` maps cell -> item slot | Server.cpp:364 | Spatial lookup and one-item-per-cell constraint |
| Factory Function | `CreateItem` / `CreateTreasureBox` construct+place items | Server.cpp:7254/9392 | Centralize item creation and initialization |
| Observer / Broadcast | `GridMulticast` fans out `MSG_CreateItem`/`MSG_DecayItem` to nearby players | SendFunc.cpp:620 | Keep clients synchronized with world-item changes |
| Guard Clauses | Each handler validates state before proceeding | _MSG_GetItem.cpp, _MSG_DropItem.cpp | Enforce eligibility business rules |
| Shared Mutable Global State | `pItem` global array mutated by many modules | Server.h:235 | Implicit communication across the game loop (an anti-pattern, see Risks) |

**Architectural decisions:** The design uses a fixed-capacity global object pool with an external grid index and free-function behavior, consistent with the rest of this decompiled legacy codebase (the same pattern as `pMob`). There is no class-level encapsulation, DI, or interface abstraction; the component is a plain C-style data store.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | ProcessDecayItem | Loop counter incremented twice (`for(...i<ItemCount)` + `ItemCount++` inside body), causing a step of 2 and skipped slots; inconsistent wrap cap at `g_dwInitItem + 1` | Some world items may never decay; decay scan is unreliable and O(n) per tick |
| High | Global `pItem` pool | Massive mutable global array shared implicitly across handlers/timers with no ownership or locking | Concurrency hazards, large blast radius, hard to reason about / test |
| High | Array bounds / addressing | Client-facing `ItemID = index + 10000` with index 0 reserved; any off-by-one yields OOB access into `pItem[5000]` | Memory-safety risk if validation is bypassed |
| Medium | `GetEmptyItem` linear scan | O(n) scan over 5000 slots on every item creation | Performance degradation at high item density |
| Medium | Dead fields | `Money`, `Open`, `ItemQuest` (set only in constructor), `Unk[20]` never read | Confusing, indicates incomplete decompilation / reserved space |
| Medium | Duplicated logic | `CreateItem` and `CreateTreasureBox` share ~90% identical initialization | Drift risk between the two paths (e.g., `Delay` differs: 90 vs 0) |
| Medium | Rendering special-cases | sIndex 3145 and 5700 special-cased inside `GetCreateItem` | Hard-coded magic item IDs scatter domain rules across functions |
| Low | `Decay = -1` permanent marker | Branch exists in `ProcessDecayItem` but no assignment to `-1` found | Intended feature (permanent items) is effectively unreachable |
| Low | Prohibited-drop list | Hard-coded item-index list (508,509,522,526-537,446,747,3993,3994) in two places (drop + death) | Magic numbers duplicated; drift risk between drop and death-loss rules |

---

## 11. Test Coverage Analysis

**Result: No automated tests exist anywhere in the project.**

A full-repository search (excluding `.git` and `.opencode`) for test directories and test/spec files returned no matches. There are no unit, integration, or regression tests for `CItem`, `pItem`, `CreateItem`, `UpdateItem`, `ProcessDecayItem`, or any item handler. The only build artifacts are the `TMSrv.vcxproj`/`DBSrv.vcxproj` projects (Visual Studio 2015) and the solution file; none reference a test project. The codegraph index likewise reports "no covering tests found" for the component's functions.

| Component Area | Unit Tests | Integration Tests | Coverage | Test Quality |
|----------------|------------|-------------------|----------|--------------|
| CItem class / constructor | 0 | 0 | 0% | N/A — no tests exist |
| CreateItem / GetEmptyItem | 0 | 0 | 0% | N/A |
| UpdateItem / gate logic | 0 | 0 | 0% | N/A |
| ProcessDecayItem | 0 | 0 | 0% | N/A |
| _MSG_GetItem / _MSG_DropItem | 0 | 0 | 0% | N/A |

**Risk:** The component carries high-complexity business rules (pickup eligibility, coin cap, prohibited drops, keyed gates, decay) with zero automated coverage. Given the identified bugs (double-increment decay loop, magic-number item lists), the lack of tests is a significant reliability risk. Any verification must currently be manual or rely on live server/client testing.

---

## 12. Analysis Limitations

- The project contains Korean/Portuguese comments in `ItemEffect.h` and other files that were not fully transliterated; business-rule intent was inferred from code behavior where comment text was garbled.
- The `Money`, `Open`, `ItemQuest`, and `Unk` fields are documented as unused based on a repo-wide search; if consumed indirectly through memory overlays or external tooling, that usage is outside the analyzed scope.
- No runtime execution was performed (per analysis constraints); all findings are static.

---

*End of Component Deep Analysis Report — CItem*
