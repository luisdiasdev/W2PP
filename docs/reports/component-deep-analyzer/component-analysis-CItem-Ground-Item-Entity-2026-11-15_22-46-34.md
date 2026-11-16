# Component Deep Analysis Report

**Component:** CItem Ground Item Entity
**Primary location:** `Code/TMSrv/CItem.h`, `Code/TMSrv/CItem.cpp`
**Project:** W2PP - decompiled W2 MMORPG server emulator (C/C++, Win32, MSVC)
**Host process:** `TMSrv` (game/world server)
**Analysis scope:** entire project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
**Report generated:** 2026-11-15 22:46:34

---

## 1. Executive Summary

`CItem` is the world-object record for every item that exists on the ground of the TMSrv map. It is a plain data holder (`Code/TMSrv/CItem.h:24-44`) with a constructor/destructor pair (`Code/TMSrv/CItem.cpp:22-35`) and no behaviour of its own: no methods other than construction, no validation, no serialization. All life-cycle logic lives outside the class, in four ownership clusters inside `Code/TMSrv/Server.cpp` - the allocator `GetEmptyItem` (`:6754`), the factories `CreateItem` (`:7254`) and `CreateTreasureBox` (`:9392`), the decay sweeper `ProcessDecayItem` (`:8353`), and the state mutator `UpdateItem` (`:7472`).

The entity is instantiated once as a fixed global array: `CItem pItem[MAX_ITEM]` (`Code/TMSrv/Server.cpp:279`, declared `extern` in `Code/TMSrv/CItem.h:45` and again in `Code/TMSrv/Server.h:235`). With `MAX_ITEM == 5000` (`Code/Basedef.h:103`) and a 76-byte record (8-byte `STRUCT_ITEM` + 12 `int` fields + `char Unk[20]`, 4-byte aligned), the array occupies approximately 380,000 bytes of static memory. There is no heap allocation, no pool, no persistence: ground items are pure in-memory state that is lost on restart.

Key findings of this analysis:

* **Fan-in is large and unguarded, fan-out is one.** 10 translation units read or write `pItem` directly, plus 5 header-only includers; the type itself depends only on `Code/Basedef.h` (`STRUCT_ITEM`, `MAX_ITEM`). Every consumer reaches into the global array with hand-rolled bounds checks that differ from each other.
* **Field semantics are heavily overloaded.** `Mode` is an object-state discriminator (free / ground object / treasure box), `Delay` is simultaneously a decay countdown and a gate one-tick latch, `State` is a door/gate state machine (1 open, 2 closed, 3 locked), `GridCharge` is an item-ability-derived mask index, `Height` is a terrain sample that is later overwritten by a mask-computed value, and `Decay` is a kill switch that no code ever sets.
* **Three of the declared fields are dead.** `Money`, `Open` and `ItemQuest` have zero references anywhere in `Code/` outside the constructor assignment of `ItemQuest` (`Code/TMSrv/CItem.cpp:29`). Gold on the ground is **not** stored in `CItem::Money`; it is encoded inside the `STRUCT_ITEM` payload as item effects 36/37 under `EF_VOLATILE == 2` and absorbed on pickup (`Code/TMSrv/_MSG_GetItem.cpp:180-202`).
* **The decay mechanism contains a swapped-index defect.** `ProcessDecayItem` releases the occupancy cell with `pItemGrid[itemPosX][itemPosY]` (`Code/TMSrv/Server.cpp:8381`) while every other access in the codebase is `pItemGrid[y][x]`, so the wrong cell is cleared on decay, leaving a stale grid entry that still resolves to the now-free slot.
* **There is no owner/isolation model.** The component has no owner, no account, no party, no timestamp and no lock-out field. Any player who passes the proximity and state checks can take any object, including in PvP loot scenarios; the only protection is the 90-second `Delay` immunity window and the per-object 3-cell distance rule.
* **No test coverage exists anywhere in the repository.** The solution contains exactly two projects (`Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj`), there is no test project, no test framework reference (no gtest/Catch2/CppUTest/Boost.Test), no `TEST_CASE`/`TEST_F` symbol, no fixture directory and no CI configuration.

The component exposes **no network endpoints**; it is an in-process domain entity whose state is projected onto the wire only through other components (`GetCreateItem` in `Code/TMSrv/GetFunc.cpp:1195`, `SendCreateItem` in `Code/TMSrv/SendFunc.cpp:320`). Consequently the "Endpoints" section of the analysis template is intentionally omitted.

---

## 2. Data Flow Analysis

### 2.1 Creation path (drop, loot, boot, GM)

```
1. Caller supplies a STRUCT_ITEM payload and a target cell
   (_MSG_DropItem.cpp:112, MobKilled.cpp:2193/2327/2384, ProcessSecMinTimer.cpp:2273,
    Server.cpp:6306/7173/8588, imple.cpp:272)
2. CreateItem(x, y, item, rotate, Create)                       [Server.cpp:7254]
3.   payload validity gate: item->sIndex in 1..MAX_ITEMLIST-1   [Server.cpp:7256]
4.   GetEmptyItemGrid(&x, &y) - 3x3 fallback relocation         [GetFunc.cpp:1876]
5.   occupancy re-test: pItemGrid[y][x] != 0 -> FALSE           [Server.cpp:7264]
6.   GetEmptyItem() - linear scan of pItem[1..4999].Mode == 0   [Server.cpp:6754]
7.   stamp record: Mode=1, PosX, PosY, ITEM (memcpy), Rotate,
     State=STATE_OPEN(1), Delay=90, Decay=0, GridCharge=EF_GROUND,
     Height=pHeightGrid[y][x]                                   [Server.cpp:7272-7289]
8.   claim occupancy: pItemGrid[y][x] = slot                    [Server.cpp:7287]
9.   build MSG_CreateItem (ItemID = slot + 10000, State, Rotate,
     Height = -204 unless Create == 2)                          [Server.cpp:7291-7313]
10.  GridMulticast(x, y, MSG_CreateItem)                        [Server.cpp:7314]
11.  return TRUE (1) - callers compare the value to MAX_ITEM    [Server.cpp:7316]
```

### 2.2 Decay path (time-driven)

```
1. WM_TIMER(TIMER_SEC, 500 ms) -> ProcessSecTimer()             [Server.cpp:3725-3726, 3631]
2. SecCounter++                                                 [ProcessSecMinTimer.cpp:123]
3. every second (SecCounter % 2 == 0) ProcessDecayItem()        [ProcessSecMinTimer.cpp:1101-1102]
4.   for cursor ItemCount (global, starts 100)                  [Server.cpp:328, 8355-8360]
5.     skip if ITEM.sIndex == 1727                              [Server.cpp:8362]
6.     skip if Mode == 0 (free) or Mode == 2 (treasure box)     [Server.cpp:8365]
7.     if Delay >= 1 { Delay--; continue; }                     [Server.cpp:8368-8372]
8.     if Decay != -1:
         BASE_ClearItem(&ITEM)                                  [Server.cpp:8379]
         pItemGrid[PosX][PosY] = 0     <-- swapped indices       [Server.cpp:8381]
         Mode = 0                                               [Server.cpp:8382]
         GridMulticast(posX, posY, MSG_DecayItem, ItemID + 10000) [Server.cpp:8384-8393]
```

### 2.3 Pickup path (client driven)

```
1. Exec_MSG_GetItem(conn, pMsg)                                 [_MSG_GetItem.cpp:20]
2.   alive / USER_PLAY guard                                    [:24-29]
3.   trade and auto-trade guards                                [:31-42]
4.   DestType must be ITEM_PLACE_CARRY                          [:44-48]
5.   itemID = ItemID - 10000; range 1..4999                     [:50-53]
6.   pItem[itemID].Mode == 0 -> send MSG_DecayItem to client    [:55-72]
7.   Chebyshev distance <= 3 from TargetX/TargetY               [:74-82]
8.   slot 1727 requires level >= 1000                           [:84-85]
9.   payload sIndex re-validation 1..MAX_ITEMLIST-1             [:89-92]
10.  sIndex 470 (quest pill): one-shot +9 SkillBonus, clear     [:94-127]
11.  sIndex 490..499: server-wide notice, then normal pickup    [:129-143]
12.  grid sanity: coordinates in range, pItemGrid[y][x]==itemID,
     client-supplied GridX/GridY must match stored position     [:157-178]
13.  EF_VOLATILE == 2 -> coin = (EF_HWORDCOIN<<8) + EF_LWORDCOIN,
     cap 2,000,000,000, clear payload, no carry insert          [:180-202]
14.  else -> memcpy payload into MOB.Carry[DestPos], item log   [:204-232]
15.  MSG_CNFGetItem to client, MSG_DecayItem to grid,
     pItemGrid[itemY][itemX] = 0, Mode = 0, SendItem             [:234-251]
```

### 2.4 Gate / door state path (interaction driven)

```
1. Exec_MSG_UpdateItem -> gateid = ItemID - 10000               [_MSG_UpdateItem.cpp:42-49]
2. CCastleZakum::OpenCastleGate (keyid 10..14) short-circuit     [CCastleZakum.cpp:72-103]
3. state = pItem[gateid].State; target = m->State               [_MSG_UpdateItem.cpp:54-55]
4. key requirement when either side is STATE_LOCKED(3):
     EF_KEYID of the object must equal EF_KEYID of a carried item [:_MSG_UpdateItem.cpp:57-87]
5. UpdateItem(gateid, STATE_OPEN, &height)                      [_MSG_UpdateItem.cpp:98]
6.   BASE_UpdateItem(GridCharge, State, newState, PosX, PosY,
                     pHeightGrid, Rotate, &height)              [Server.cpp:7476]
7.   on success: State = newState, Height = height, Delay = 0    [Server.cpp:7481-7487]
8.   broadcast MSG_UpdateItem / MSG_CreateItem to the grid       [Server.cpp:5918-5929 etc.]
```

### 2.5 Projection path (view synchronisation)

```
1. Player enters view -> SendGridMob / SendGridItem             [SendFunc.cpp:560-618, 720-760]
2.   for each cell: titem = pItemGrid[y][x]                     [SendFunc.cpp:596]
3.   SendCreateItem(conn, titem) -> GetCreateItem(titem, &sm)   [SendFunc.cpp:320-326]
4.     GridX/GridY/Rotate/Item/State/Height copied out          [GetFunc.cpp:1202-1236]
5.     sIndex 3145 -> sIndex + g_pGuildZone[zone].Victory,
       State forced to STATE_CLOSED, flag charge injected in
       effects 56/57 - only in the outbound copy                [GetFunc.cpp:1215-1227]
6.     sIndex 5700 -> function returns without emitting          [GetFunc.cpp:1229-1230]
7.     State == STATE_OPEN -> real Height, Create = 0            [GetFunc.cpp:1232-1236]
```

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | State model | `Mode` is a three-value object discriminator: 0 free, 1 ordinary ground object, 2 treasure box | `Code/TMSrv/CItem.h:28`; `Server.cpp:6758`, `7272`, `8365`, `9408` |
| BR-02 | Capacity | At most 4,999 concurrent ground objects; allocation starts at index 1 and returns 0 on exhaustion | `Server.cpp:6754-6763` |
| BR-03 | Reserved range | Index 0 is a permanent null sentinel; indices `1..g_dwInitItem` hold boot-placed map objects and are excluded from decay | `Server.cpp:8359-8360`; `ProcessSecMinTimer.cpp:2142` |
| BR-04 | Validation | Payload `sIndex` must satisfy `1 <= sIndex < MAX_ITEMLIST (6500)` | `Server.cpp:7256`, `9394`; `_MSG_GetItem.cpp:91` |
| BR-05 | Placement | Preferred cell wins only if unoccupied and terrain height != 127; otherwise a 3x3 neighbourhood scan relocates the object | `GetFunc.cpp:1876-1899` |
| BR-06 | Invariant | `pItemGrid[y][x] == slot` is the bidirectional link between world cell and entity | `Server.cpp:7287`, `8381`; `_MSG_GetItem.cpp:164-172`, `248` |
| BR-07 | Creation | Factory stamps Mode=1, position, payload, Rotate, State=OPEN, Delay=90, Decay=0, GridCharge, Height | `Server.cpp:7272-7289` |
| BR-08 | Creation variant | Treasure boxes use Mode=2, Delay=0, caller-supplied State and a fixed wire height of -204 | `Server.cpp:9408-9444` |
| BR-09 | Lifecycle | `Delay` is a countdown decremented once per decay sweep; each unit is one second at the nominal 500 ms timer | `Server.cpp:8368-8372`; `ProcessSecMinTimer.cpp:1101` |
| BR-10 | Exemption | Destruction is skipped when `Decay == -1` (never written) and for payload `sIndex == 1727` | `Server.cpp:8374`, `8362` |
| BR-11 | Destruction | Decay clears the payload, releases the cell, sets Mode=0 and broadcasts `_MSG_DecayItem` (111) | `Server.cpp:8379-8393` |
| BR-12 | Access control | Pickup requires alive player, no trade, CARRY destination, slot 1..4999, Mode!=0, distance <= 3, valid payload | `_MSG_GetItem.cpp:24-92` |
| BR-13 | Money | `EF_VOLATILE == 2` objects are absorbed into `MOB.Coin` instead of the inventory, capped at 2,000,000,000 | `_MSG_GetItem.cpp:180-202` |
| BR-14 | Money encoding | Ground money value = `(EF_HWORDCOIN << 8) + EF_LWORDCOIN`, therefore <= 65,535 per object; `CItem::Money` is unused | `_MSG_GetItem.cpp:184-190`; `CItem.h:36` |
| BR-15 | Projection | Outbound create message rewrites height to -204 for non-open states, suppresses sIndex 5700 and virtualises sIndex 3145 | `GetFunc.cpp:1213-1236` |
| BR-16 | State machine | Item state transitions allowed only 1<->2 and 1<->3; 2<->3 is refused; requires `GridCharge` in 0..5; key check on state 3 | `Server.cpp:7476-7505`; `Basedef.cpp:5292-5306` |
| BR-17 | Derivation | `GridCharge` is sampled once at creation from the `EF_GROUND` item ability and never recomputed | `Server.cpp:7285`, `9421`, `7476` |
| BR-18 | Automation | Open gates whose `EF_KEYID < 15` are auto-closed by the minute timer after a one-sweep latch on `Delay` | `ProcessSecMinTimer.cpp:2142-2209` |
| BR-19 | Presentation | `Rotate` is a visual orientation (0..3 by convention) stored as `int` and emitted as `unsigned char`, never validated | `Server.cpp:7279`; `GetFunc.cpp:1207`; `Basedef.h:1602` |
| BR-20 | Terrain | `Height` starts as the terrain sample at placement and is overwritten by the mask computation; it is truncated to a byte on the wire | `Server.cpp:7289`, `7482`; `GetFunc.cpp:1234`; `Basedef.h:1604` |
| BR-21 | Unimplemented | `Open`, `ItemQuest` and `Unk[20]` are declared but carry no behaviour; no quest binding exists at entity level | `CItem.h:37-39`; `CItem.cpp:29` |
| BR-22 | Identity | The wire identity of an object is `slot + 10000`; there is no generation counter, so handles alias across recycling | `Server.cpp:7298`, `8391`; `GetFunc.cpp:1205` |

---

### Detailed breakdown of the business rules

---

#### Business Rule: BR-01 - Slot lifecycle and object modes

**Overview**
Every slot of `pItem` is in exactly one of three states expressed by the `Mode` field: `0` = free/allocatable, `1` = ordinary ground object subject to decay, `2` = treasure box exempt from decay.

**Detailed description**
`Mode` is the only liveness flag in the component and the allocator is defined entirely by it: `GetEmptyItem` returns the first index `i` in `1..MAX_ITEM-1` for which `pItem[i].Mode == 0` (`Code/TMSrv/Server.cpp:6754-6763`). The value `1` is stamped by `CreateItem` for every ordinary object - player drops, mob loot, arena rewards, dungeon prizes and boot-placed map objects - and it is the value that makes the record eligible for the decay sweep (`Server.cpp:7272`, `8365`). The value `2` is stamped only by `CreateTreasureBox` (`Server.cpp:9408`) and marks objects that must never expire, which is why the decay loop explicitly skips both `0` and `2` (`Server.cpp:8365`). Freeing is done by writing `0` after clearing the payload - either by the decay sweep (`Server.cpp:8382`), by the pickup handler (`_MSG_GetItem.cpp:109`, `160`, `249`), or by the GM `destroygate` command which bypasses the whole protocol with `memset(&pItem[ival2], 0, sizeof(CItem))` (`Code/TMSrv/imple.cpp:308`).

The mode value is also load-bearing for every reader. `SendFunc` will only project a cell whose slot has a non-zero `Mode` and a non-zero payload index (`Code/TMSrv/SendFunc.cpp:611`, `739`, `754`); `_MSG_GetItem` rejects `Mode == 0` and logs the distinction between "free" and "occupied but unusable" (`_MSG_GetItem.cpp:55-61`); `MobKilled` guards the castle-gate reopen path with `pItem[ItemID].Mode` (`Code/TMSrv/MobKilled.cpp:728`). Because `Mode` is an `int` with no accessor and no enum, any of these sites can write any value; nothing enforces that only 0, 1 and 2 appear.

A notable consequence is that the treasure-box mode is vestigial in the current tree. `CreateTreasureBox` has no caller anywhere in `Code/` (only its definition at `Server.cpp:9392` and its declaration at `Server.h:186`), so in practice `Mode == 2` never occurs, the `Mode == 2` branch of the decay test at `Server.cpp:8365` is dead, and the `Create == 2` branch that forces the -204 wire height in `CreateItem` (`Server.cpp:7311-7312`) is unreachable as well.

**Rule workflow**
```
Mode == 0  --(CreateItem, success)-->  Mode == 1  --(ProcessDecayItem, Delay exhausted)--> Mode == 0
Mode == 0  --(CreateTreasureBox, no callers)-->  Mode == 2 (never decays)
Mode == 1  --(pickup: carry or coin)-->  Mode == 0
Mode == 1  --(GM destroygate: memset)-->  Mode == 0
```

---

#### Business Rule: BR-02 - Capacity limit and allocation failure

**Overview**
The world can hold at most 4,999 ground objects (indices 1..4999). Allocation failure is reported by returning `0`, which callers must treat as "no object created".

**Detailed description**
`GetEmptyItem` performs a full linear scan from index 1 to `MAX_ITEM - 1` on every call and returns the first slot whose `Mode` is 0; if the scan completes it returns `0` as an error sentinel (`Code/TMSrv/Server.cpp:6754-6763`). `CreateItem` and `CreateTreasureBox` both abort with `FALSE` when the allocator returns 0 (`Server.cpp:7269-7270`, `9405-9406`). Because the scan is unconditional and starts from 1, allocation exhibits a lowest-free-index policy with no free list and no generation counter; cost is O(5000) per creation, which is acceptable at drop rates but is paid again by every `CreateItem` called from the loot loops of `MobKilled` (up to 64 iterations for a Kefra corpse, `MobKilled.cpp:695-707`).

The failure contract is only partially honoured. `_MSG_DropItem` treats a non-positive return as failure and tells the player "Can't create object(item)" (`Code/TMSrv/_MSG_DropItem.cpp:112-118`), which is correct. In contrast, several callers misinterpret the `BOOL` return as a slot index: `int ID = CreateItem(...)` followed by `if (ID >= MAX_ITEM || ID <= 0) continue;` at `Server.cpp:7173-7176`, `int gateid = CreateItem(...)` at `imple.cpp:272-275`, and the dead `#ifdef PKDrop` block which tests `create >= MAX_ITEM` (`MobKilled.cpp:2327-2331`, `2384-2385`). Those comparisons can never fail, because the function returns 1 or 0 and never a slot.

The 4,999-object ceiling is a hard, unconfigurable limit. It is also inconsistent with the file-search limit: the allocator bounds itself with `MAX_ITEM` while the decay cursor uses the literal `5000` (`Server.cpp:8359`), so a future change to `MAX_ITEM` would silently desynchronise the two.

**Rule workflow**
```
slot = GetEmptyItem()
   scan i = 1 .. MAX_ITEM-1
   first pItem[i].Mode == 0 -> return i
   none found -> return 0
CreateItem: slot == 0 -> return FALSE (no object)
            slot != 0 -> occupy slot, return TRUE (1)
```

---

#### Business Rule: BR-03 - Reserved index range and boot-placed map objects

**Overview**
Index 0 is a permanent null slot. Indices `1..g_dwInitItem` are occupied at start-up by fixed map objects (gates, doors, arena mechanisms) and are excluded from the decay sweep and from recycling.

**Detailed description**
At start-up the server loads `g_pInitItem` (`Code/Basedef.cpp:5913-5938`) and replays it through `CreateItem` in array order, so the boot objects occupy slots `1..g_dwInitItem` in the exact order of the init file (`Code/TMSrv/Server.cpp:7166-7186`). Because the allocator is first-free, this mapping is stable only if every init entry succeeds; if any entry fails the payload gate or the placement gate, all subsequent objects shift down by one and every hardcoded slot constant in the codebase becomes wrong.

Those hardcoded constants are pervasive: colosseum doors are slots 12 and 13 (`Server.cpp:5889-5897`), colosseum door set two is 14..18 (`Server.cpp:5941-5945`), arena doors are `3*i + j + 1` (`Server.cpp:6018`), castle doors are 36..39 (`Server.cpp:6064`), the GM gate-relocation command writes absolute slot 45 (`imple.cpp:414-425`), and the Kefra loot path derives items from cells rather than slots (`MobKilled.cpp:695`). The decay cursor is what protects this range: when the sweep reaches 5000 it resets to `g_dwInitItem + 1` (`Server.cpp:8359-8360`), so slots at or below the boot count are never visited, never decremented and never destroyed. That is the mechanism - accidental or not - that keeps map gates alive forever despite them being created with `Delay = 90` like any other object.

The same mechanism creates an asymmetry: the decay cursor starts life at 100 (`Server.cpp:328`), so on a server with `g_dwInitItem < 100` the very first sweeps begin above the boot range and no boot object is ever threatened, but on a server with `g_dwInitItem > 100` the first sweeps visit slots `100..g_dwInitItem`, decrementing their `Delay` - the objects survive only because the cursor then wraps to `g_dwInitItem + 1` and never returns below it. Conversely, a GM `destroygate` (`imple.cpp:308`) frees a reserved slot, after which the first-free allocator will hand that index to an ordinary drop, and the hardcoded `UpdateItem(45, ...)` style calls will then operate on that unrelated object.

**Rule workflow**
```
boot: for i in 0..g_dwInitItem-1: CreateItem(g_pInitItem[i]) -> slots 1..g_dwInitItem
decay cursor: ItemCount starts at 100
              if ItemCount >= 5000 -> ItemCount = g_dwInitItem + 1
              => slots 1..g_dwInitItem never decay
GM destroygate: memset(&pItem[n], 0) -> slot n re-enters the allocatable range
```

---

#### Business Rule: BR-04 - Payload validity gate

**Overview**
An object may only be created if its `STRUCT_ITEM.sIndex` lies in `1..MAX_ITEMLIST-1`; the same range is re-validated on every read path.

**Detailed description**
`CreateItem` opens with `if (item->sIndex <= 0 || item->sIndex >= MAX_ITEMLIST) return FALSE;` (`Code/TMSrv/Server.cpp:7256`), and `CreateTreasureBox` repeats the check identically (`Server.cpp:9394`). `MAX_ITEMLIST` is 6500 (`Code/Basedef.h:139`) and indexes the static item definition table `g_pItemList` (`Basedef.h:2467`), so the gate is really "the payload must be a known item template". Nothing validates the three effect slots of the payload, the count byte, or the durability value; a payload with valid `sIndex` and arbitrary effects is accepted verbatim by the `memcpy` at `Server.cpp:7277`.

Readers repeat the check defensively rather than trusting the stored record. `_MSG_GetItem` re-validates `sIndex` after reading it from the slot (`_MSG_GetItem.cpp:89-92`) and bails silently if it fails, which covers the case of a slot whose payload was never initialised or was partially cleared. The gate loops that iterate boot objects validate `ITEM.sIndex <= 0 || ITEM.sIndex >= MAX_ITEMLIST` before touching state (`Server.cpp:5897`, `5945`, `6021`, `6066`) and `ProcessSecMinTimer` does the same (`ProcessSecMinTimer.cpp:2148`). `SendFunc` uses a weaker test - only `pItem[titem].ITEM.sIndex` non-zero (`SendFunc.cpp:739`, `754`).

The asymmetry matters because the constructor does not initialise `ITEM` at all (`Code/TMSrv/CItem.cpp:22-30` assigns only `Mode`, `GridCharge`, `State`, `Rotate`, `Delay`, `ItemQuest`). For the global array this is harmless because static storage is zero-filled, but any `CItem` created as a local or as a member of a heap object would start with an indeterminate `STRUCT_ITEM` and could therefore pass or fail the payload gate unpredictably.

**Rule workflow**
```
create: sIndex <= 0 or sIndex >= 6500 -> FALSE, no slot consumed
read  : sIndex <= 0 or sIndex >= 6500 -> treat as absent (silent return / skip)
```

---

#### Business Rule: BR-05 - Placement resolution and the 3x3 relocation scan

**Overview**
A requested drop cell is used only if it is free and not terrain-blocked; otherwise the object is relocated to the first acceptable neighbour, and if no neighbour qualifies in the 3x3 window the placement fails.

**Detailed description**
`GetEmptyItemGrid` (`Code/TMSrv/GetFunc.cpp:1876-1899`) implements a two-stage policy. It first checks the requested cell: free iff `pItemGrid[y][x] == 0` and walkable iff `pHeightGrid[y][x] != 127`. If both hold it returns `TRUE` with the coordinates untouched. Otherwise it scans the 3x3 block centred on the request with the row loop outermost (`y` from `y-1` to `y+1`, `x` from `x-1` to `x+1`), skipping out-of-range cells and again requiring empty and not 127, and writes the first hit back through the pointers. Order of preference inside the window is therefore top-left first, giving a deterministic bias toward lower coordinates.

Three details are worth recording. First, the preferred-cell test at `GetFunc.cpp:1878` performs **no** bounds check, so it dereferences whatever coordinates the caller supplies; the 3x3 fallback does clamp (`:1885-1886`). Callers are expected to have validated already - `_MSG_DropItem` rejects `GridX >= MAX_GRIDX || GridY >= MAX_GRIDY` (`_MSG_DropItem.cpp:44-49`) and `_MSG_Attack` rejects `<0`/`>=4096` before using the cell as an item lookup (`_MSG_Attack.cpp:164-168`) - but `CreateItem` itself does not validate, and its callers pass mob positions and init-file positions unchecked.

Second, the sentinel `127` is the only terrain exclusion. A commented-out rule at `Server.cpp:7261-7262` shows that an earlier variant refused drops outside a height band (`>= -15 || >= 90`, itself a tautological condition); it is disabled, so items may now be placed on any terrain whose height byte is not the 127 sentinel.

Third, the handler and the factory both call the resolver: `_MSG_DropItem` resolves first (`_MSG_DropItem.cpp:57`) and `CreateItem` resolves again (`Server.cpp:7259`), so a drop performs two window scans and the second one operates on coordinates already rewritten by the first. That duplication is the reason the "can't drop here" message can be emitted while `m->GridX/GridY` have already been modified in the client's copy of the request.

**Rule workflow**
```
GetEmptyItemGrid(&x, &y):
  if pItemGrid[y][x] == 0 and pHeightGrid[y][x] != 127 -> TRUE (same cell)
  for yy in y-1..y+1:
    for xx in x-1..x+1:
      if xx,yy out of 0..4095 -> continue
      if pItemGrid[yy][xx] == 0 and pHeightGrid[yy][xx] != 127 -> write xx,yy; TRUE
  return FALSE
CreateItem: if pItemGrid[y][x] != 0 -> FALSE
```

---

#### Business Rule: BR-06 - Grid/entity bijection invariant

**Overview**
For every live object, the world grid must contain exactly one cell whose value is that slot index, and `pItem[slot].PosX/PosY` must name the same cell. The invariant is assumed by the pickup handler and by the view projection but is not enforced by the component.

**Detailed description**
The link is established at creation by `pItemGrid[y][x] = empty` (`Code/TMSrv/Server.cpp:7287`) immediately after the position fields are stamped, and it is assumed by every consumer. The pickup handler goes as far as to require the client to echo the stored coordinates (`_MSG_GetItem.cpp:174-178`) and to repair a broken link defensively: if `pItemGrid[itemY][itemX] != itemID` it re-writes the cell when it is empty, sends a decay packet and aborts the pickup (`:164-172`). The view layer treats a cell pointing at a slot whose `Mode` is 0 as stale and zeroes it in place (`SendFunc.cpp:613-614`, `759-760`), which is a self-healing path for the case where the entity was freed without the grid being released.

The invariant is nevertheless broken by the component's own decay routine. `ProcessDecayItem` releases occupancy with `pItemGrid[itemPosX][itemPosY] = 0` (`Server.cpp:8381`), i.e. x first, while the array is declared `pItemGrid[MAX_GRIDY][MAX_GRIDX]` (`Server.cpp:364`) and every other site indexes `[y][x]`. The result of a decay is therefore a cleared cell at the transposed coordinate plus a surviving cell at the true coordinate that still resolves to the now-free slot. The surviving cell makes the slot un-reusable through that cell (the next `CreateItem` there fails the occupancy test at `:7264`) and makes the freed index reachable through a stale handle; the transposed cell may additionally free or claim a cell belonging to a different object. The only mitigating factor is that the pickup handler's repair branch re-seats the cell when it is empty.

A second, weaker break comes from the GM path: `destroygate` multicasts a `_MSG_CreateItem` for the object and then zeroes the record without touching `pItemGrid` (`imple.cpp:304-308`), leaving the cell pointing at slot 0-`Mode`. The pickup handler's "grid cell is not this slot" branch and the `SendFunc` stale-cell repair are the only recovery mechanisms, and both are client-driven or view-driven rather than authoritative.

**Rule workflow**
```
create : stamp PosX/PosY -> pItemGrid[PosY][PosX] = slot
pickup : verify pItemGrid[PosY][PosX] == slot else repair/abort
         on success pItemGrid[PosY][PosX] = 0 and Mode = 0
decay  : pItemGrid[PosX][PosY] = 0   (transposed - defect) and Mode = 0
view   : cell -> slot -> Mode == 0 -> cell = 0 (repair)
```

---

#### Business Rule: BR-07 - Creation stamping contract

**Overview**
`CreateItem` writes a fixed field set: `Mode = 1`, target coordinates, full payload copy, `Rotate` from the caller, `State = STATE_OPEN`, `Delay = 90`, `Decay = 0`, `GridCharge` from the payload's `EF_GROUND` ability, and `Height` from the terrain grid.

**Detailed description**
The stamping block is `Server.cpp:7272-7289` and it defines the initial value of every live field except `Money`, `Open`, `ItemQuest` and `Unk`. `State = STATE_OPEN` means every freshly dropped item is in the "open" state of the door state machine; for ordinary items this field is purely decorative because nothing reads it except the projection (`GetFunc.cpp:1211`) and the gate paths, but it matters for items that share the gate plumbing. `GridCharge` is the single most consequential derived field: it is the `EF_GROUND` ability of the payload (effect 34, `Code/ItemEffect.h:73`), sampled once, stored, and later used as the first index of `g_pGroundMask` during state transitions (`Server.cpp:7476`, `Basedef.cpp:5312`). Because effect 34 is normally absent from decorative ground items, `GridCharge` is normally 0.

Two fields are notable by omission. `Money` is never written (`CItem.h:36` is the only occurrence of the name outside this report's references), and `PosX/PosY/Decay/Height/Money/Open/Unk` are not initialised by the constructor, so a re-used slot inherits leftovers from its previous occupant for those fields until they are re-stamped. `Delay = 90` combined with the one-per-second sweep yields the ~90 second lifetime described in BR-09, and also serves as the "grace period" during which a dropped object cannot expire even if the dropper is standing on it.

The return contract is `TRUE/FALSE` (`Server.cpp:7316`) and the wire side effect is a `_MSG_CreateItem` multicast with `ItemID = slot + 10000`, the payload, coordinates, `Rotate`, `State` and a height that is -204 unless the caller passed `Create == 2` (`Server.cpp:7291-7314`). Callers that interpret the boolean as an index are documented under BR-02.

**Rule workflow**
```
if sIndex invalid -> FALSE
GetEmptyItemGrid(&x,&y)
if pItemGrid[y][x] != 0 -> FALSE
slot = GetEmptyItem(); if slot == 0 -> FALSE
Mode=1; PosX=x; PosY=y; ITEM=*item; Rotate=rotate; State=1
Delay=90; Decay=0; GridCharge=EF_GROUND(item); Height=pHeightGrid[y][x]
pItemGrid[y][x]=slot
multicast _MSG_CreateItem(ItemID=slot+10000, Height=-204 unless Create==2)
return TRUE
```

---

#### Business Rule: BR-08 - Treasure-box creation variant

**Overview**
`CreateTreasureBox` produces a non-decaying object (`Mode = 2`) with no immunity delay, a caller-specified state and a fixed client-side height marker of -204. The function currently has no callers.

**Detailed description**
The factory mirrors `CreateItem` (`Code/TMSrv/Server.cpp:9392-9449`) and differs in four places: `Mode = 2` instead of 1 (`:9408`), `Delay = 0` instead of 90 (`:9418`), `State` taken from the parameter instead of forced to `STATE_OPEN` (`:9416`), and an unconditional `sm.Height = -204` (`:9444`). The -204 value is the client's rendering marker for a "boxed" object, the same marker `GetCreateItem` uses as its default (`GetFunc.cpp:1213`).

The Mode=2 choice is what makes the object immortal: the decay sweep skips both 0 and 2 (`Server.cpp:8365`). That is the intended "static interactive object" category, distinct from boot objects which are immortal through the cursor reset (BR-03). The absence of `Delay` immunity is consistent with immortality: there is no need to protect an object from a decay that will never run.

No caller exists in the analysed tree - a repository-wide search for `CreateTreasureBox(` returns only the definition (`Server.cpp:9392`) and the declaration (`Server.h:186`). The component therefore contains a complete but unreachable second object class, and the `Mode == 2` conditions scattered through `ProcessDecayItem` (`:8365`) are defensive rather than functional.

**Rule workflow**
```
CreateTreasureBox(x,y,item,rotate,State):
  validate sIndex; resolve grid; test occupancy; allocate slot
  Mode=2; PosX/PosY/ITEM/Rotate; State=State; Delay=0; Decay=0
  GridCharge=EF_GROUND(item); Height=pHeightGrid[y][x]
  pItemGrid[y][x]=slot
  multicast _MSG_CreateItem with Height=-204
```

---

#### Business Rule: BR-09 - Decay countdown semantics and effective lifetime

**Overview**
`Delay` is a per-object countdown measured in decay sweeps. A shipped object starts at 90, each sweep decrements by one, and the object is destroyed on the first sweep that finds it at 0.

**Detailed description**
The decrement is the first action of the sweep body: `if (pItem[ItemCount].Delay >= 1) { pItem[ItemCount].Delay--; continue; }` (`Code/TMSrv/Server.cpp:8368-8372`). The sweep is invoked from `ProcessSecTimer` guarded by `SecCounter % 2 == 0` (`ProcessSecMinTimer.cpp:1101-1102`), and `ProcessSecTimer` is driven by the Win32 `TIMER_SEC` timer installed with a 500 ms period (`Server.cpp:3631`, dispatched at `3725-3726`). One sweep therefore occurs every two timer ticks, i.e. every ~1000 ms, and a `Delay` of 90 corresponds to a nominal ~90 second lifetime, plus up to one extra sweep because the object is destroyed on the visit *after* the countdown reaches 0.

Two operational variants disturb that arithmetic. During the shutdown countdown the timer is re-armed at 200 ms (`ProcessSecMinTimer.cpp:113-118`), so decay runs ~2.5x faster for the remainder of the process life. And the sweep cursor is the **global** `ItemCount` that is incremented inside the loop body as well as by the loop's own `i++` (`Server.cpp:8355-8360`): the loop condition compares an incrementing `i` against a global that advances at the same rate, which makes the number of iterations depend on the current cursor rather than on `MAX_ITEM`, and the wrap assigns `ItemCount = g_dwInitItem + 1` instead of 0. Empirically each call still walks the range `(cursor, 4999]` and wraps once, so every slot above `g_dwInitItem` receives exactly one decrement per sweep - but that outcome is emergent, not stated, and any change to the initial cursor (`Server.cpp:328`, currently 100) or to the wrap rule changes the lifetime of the entire world's items.

`Delay` doubles as a latch in two other places. `UpdateItem` sets `Delay = 0` after every accepted state transition (`Server.cpp:7487`), and the gate setters set it to 0 unconditionally (`Server.cpp:5931`, `5979`, `6055`, `6104`). `ProcessSecMinTimer` sets it to 1 as a "wait one sweep" flag before closing a gate (`ProcessSecMinTimer.cpp:2159-2165`), relying on the decay sweep to bring it back to 0 (`:2185`). Any consumer that reads `Delay` as a pure lifetime must therefore know which object class it is looking at.

**Rule workflow**
```
every ~1000 ms: ProcessDecayItem()
  slot in scan range:
    sIndex == 1727            -> skip (BR-10)
    Mode == 0 or Mode == 2    -> skip
    Delay >= 1                -> Delay--, skip
    Decay != -1               -> destroy (BR-11)
lifetime = (Delay + 1) sweeps ~= 91 s for a drop; 0 sweeps for a treasure box
```

---

#### Business Rule: BR-10 - Decay exemptions

**Overview**
Two exemptions exist: `Decay == -1` disables destruction for an object, and payload `sIndex == 1727` is never destroyed regardless of `Delay`.

**Detailed description**
The `Decay` field is documented by its use: it is tested as `if (pItem[ItemCount].Decay != -1)` before the destruction block (`Code/TMSrv/Server.cpp:8374`), i.e. `-1` is intended as the "permanent object" marker. In the analysed tree, however, `-1` is never assigned: the only writes are `Decay = 0` in `CreateItem` (`:7283`), `Decay = 0` in `CreateTreasureBox` (`:9419`), the zeroing performed by `memset(&pItem[ival2], 0, sizeof(CItem))` (`imple.cpp:308`) and the implicit zero of static storage. The documented "permanent" capability is therefore unreachable, and the condition is effectively `if (true)`. Permanence is instead achieved by exclusion from the scan range (BR-03) or by `Mode == 2` (BR-01).

The second exemption is genuine and payload-driven: `if (pItem[ItemCount].ITEM.sIndex == 1727) continue;` (`Server.cpp:8362`) runs before the `Mode` and `Delay` tests, so an object whose payload index is 1727 is never decremented and never destroyed, no matter what its `Mode` or `Delay` are. Item 1727 has no other privileged treatment except a pickup restriction that tests the wrong quantity: `_MSG_GetItem` gates on the **slot index** (`if (itemID == 1727 && ...)`, `_MSG_GetItem.cpp:84`) rather than on the payload index, so the intended "level 1000 to touch this item" rule applies to whichever object happens to sit in slot 1727 rather than to item 1727. The two sites use the same magic number for two different fields.

The interaction of the two exemptions with the scan-range reset means the world contains three distinct object-permanence mechanisms - cursor exclusion, `Mode == 2`, payload 1727 - none of them documented, one of them dead.

**Rule workflow**
```
ProcessDecayItem(slot):
  if ITEM.sIndex == 1727     -> continue (immortal, no countdown)
  if Mode == 0 or Mode == 2  -> continue
  if Delay >= 1              -> Delay--
  if Decay != -1             -> destroy
  // Decay == -1 branch is unreachable: -1 never written
```

---

#### Business Rule: BR-11 - Destruction procedure and broadcast

**Overview**
Destruction clears the payload with `BASE_ClearItem`, releases the occupancy cell, sets `Mode = 0`, and multicasts a `_MSG_DecayItem` (opcode 111) carrying `slot + 10000`.

**Detailed description**
The block at `Code/TMSrv/Server.cpp:8374-8394` is the component's only autonomous state change. It snapshots `PosX/PosY` first (so the grid release and the broadcast use the pre-destruction position), clears the payload structure (not the whole record - `Mode`, `Delay`, `Decay`, `Height`, `GridCharge`, `Rotate`, `State`, and the previously noted dead fields keep their last values), releases occupancy, sets `Mode = 0`, and emits `MSG_DecayItem` with `Type = _MSG_DecayItem`, `Size`, `ID = ESCENE_FIELD`, `ItemID = ItemCount + 10000` (`:8384-8393`).

The protocol consequence is that clients learn about expiry through the same opcode used for "this object is gone" in every other removal path: pickup sends the same message to the grid on success (`_MSG_GetItem.cpp:246`), and `SendRemoveItem` (`SendFunc.cpp:510-525`) wraps the same packet for view exits. The client therefore treats `_MSG_DecayItem` as an unconditional "the object with this handle no longer exists", which is what makes the slot-recycling alias described in BR-22 observable.

The procedure has two correctness defects. The grid release uses transposed indices (`pItemGrid[itemPosX][itemPosY]`, `:8381`) as described in BR-06, and the record is not fully zeroed, so the freed slot retains a stale `ITEM` (cleared, so safe), a stale `Height` and `GridCharge`, and, critically, `Delay = 0` - meaning that if the same slot is later re-used by a path that does not stamp every field, the object inherits a countdown of zero and expires on its next sweep.

**Rule workflow**
```
destroy(slot):
  x = pItem[slot].PosX; y = pItem[slot].PosY
  BASE_ClearItem(&pItem[slot].ITEM)
  pItemGrid[x][y] = 0            // transposed - see BR-06
  pItem[slot].Mode = 0
  GridMulticast(x, y, MSG_DecayItem{ItemID = slot+10000})
```

---

#### Business Rule: BR-12 - Pickup eligibility

**Overview**
Picking up an object requires: the character alive and in play, not trading, a CARRY-type destination, a slot in `1..4999` whose `Mode != 0`, a Chebyshev distance of at most 3 cells, a valid payload index, and a matching grid cell and client echo.

**Detailed description**
The guard sequence in `Exec_MSG_GetItem` is layered from session to world. Session guards come first: `pMob[conn].MOB.CurrentScore.Hp <= 0` or `pUser[conn].Mode != USER_PLAY` triggers anti-cheat accounting (`AddCrackError(conn, 1, 13)`) and a state resend (`Code/TMSrv/_MSG_GetItem.cpp:24-29`); an open trade is force-closed for both parties (`:31-36`); auto-trade mode is refused with a panel message (`:38-42`); a destination type other than `ITEM_PLACE_CARRY` is logged and dropped (`:44-48`). Object guards follow: `itemID = ItemID - 10000` must fall in `1..MAX_ITEM-1` (`:50-53`), `pItem[itemID].Mode` must be non-zero (`:55-72`), the payload index must be in `1..MAX_ITEMLIST-1` (`:89-92`).

The positional rule is a Chebyshev (square) window of radius 3 around the character's **target** cell, not its current cell: `TargetX/TargetY` compared against `PosX/PosY` with `±3` on both axes (`:74-82`). Violations are logged with the object and character coordinates but produce no client feedback, which makes out-of-range pickup attempts look to the player like an ignored click. Two special payload rules sit inside the same handler: `sIndex == 470` is a one-shot quest reward granting `+9` `SkillBonus`, guarded by a per-character flag `QuestInfo.Mortal.PilulaOrc` and accompanied by an emotion and an `SendEtc` refresh (`:94-127`); `sIndex` in `490..499` triggers a server-wide "player X obtained item Y" notice before continuing with the normal pickup (`:129-143`). The `itemID == 1727` level gate (`:84-85`) compares the slot, not the item (BR-10).

The final consistency gate is the most interesting: the client's `GridX/GridY` must equal the stored `PosX/PosY` (`:174-178`), and the stored cell must resolve back to this slot (`:164-172`). This is an anti-desync measure that also protects against a client addressing a recycled slot at a stale position - but only when the grid link is intact, which BR-06 shows it may not be after a decay.

**Rule workflow**
```
reject if: Hp <= 0 | not USER_PLAY | trade open | autotrade | DestType != CARRY
reject if: itemID not in 1..4999 | Mode == 0 | payload sIndex invalid
reject if: |TargetX-PosX| > 3 or |TargetY-PosY| > 3
reject if: slot == 1727 and Level < 1000
special  : sIndex 470 -> one-shot quest reward (no inventory insert)
special  : sIndex 490..499 -> global notice, then continue
reject if: coordinates out of range | pItemGrid[PosY][PosX] != itemID | client echo mismatch
```

---

#### Business Rule: BR-13 - Pickup effect: inventory insert versus money absorb

**Overview**
If the payload's `EF_VOLATILE` ability equals 2 the object is money and its value is added to `MOB.Coin`; otherwise the payload is copied into the requested inventory slot.

**Detailed description**
The branch is `int Vol = BASE_GetItemAbility(ditem, EF_VOLATILE);` followed by `if (Vol == 2)` (`Code/TMSrv/_MSG_GetItem.cpp:180-202`). In the money branch the handler computes `coin = (EF_HWORDCOIN << 8) + EF_LWORDCOIN`, reads the current balance, refuses the pickup with message 273 if `coin + MOB.Coin >= 2000000000`, and on success adds the amount, sends the decay packet to the client that initiated the pickup, clears the payload with `BASE_ClearItem` and proceeds to the common confirmation tail (`:190-202`). No inventory slot is involved, so the `DestPos` field is ignored for money, and the balance change is not written to any log in this path (contrast the item branch, which emits an `ItemLog` line via `BASE_GetItemCode`).

The item branch (`:204-232`) validates `DestPos` against `MAX_CARRY` (64), refuses if the target slot is occupied - and in that case performs a client-side correction by resending the *previous* inventory slot (`:217-225`) - then `memcpy` the payload into `MOB.Carry[DestPos]` and writes an item log line. After either branch the handler emits `MSG_CNFGetItem`, multicasts `MSG_DecayItem`, clears the occupancy cell, sets `Mode = 0` and sends the resulting item to the client (`:234-251`).

Two boundary observations belong to this rule. The 2,000,000,000 ceiling is enforced as a refusal (`>= 2000000000`), so a player at 1,999,999,000 cannot absorb a 65,535 coin pile even though it would not overflow a 32-bit signed `int` until 2,147,483,647; the stricter limit matches the deposit and trade limits used elsewhere. And the money branch clears the payload *before* the common tail, so an exception in the tail would leave an object counted as money-received but still occupying its cell - the asymmetry between "payload cleared" and "Mode cleared" is a small window of inconsistency inherent to the two-phase teardown.

**Rule workflow**
```
if EF_VOLATILE(ditem) == 2:
    coin = (EF_HWORDCOIN<<8) + EF_LWORDCOIN
    if MOB.Coin + coin >= 2,000,000,000 -> message 273, abort (object untouched)
    MOB.Coin += coin
    MSG_DecayItem to initiator, BASE_ClearItem(ditem)
else:
    if DestPos out of 0..63 -> log, abort
    if Carry[DestPos] occupied -> resend slot, abort
    Carry[DestPos] = ditem, item log
common: MSG_CNFGetItem, multicast MSG_DecayItem, pItemGrid=0, Mode=0, SendItem
```

---

#### Business Rule: BR-14 - Money encoding on the ground

**Overview**
There is no dedicated money field on the ground entity. Money is a `STRUCT_ITEM` whose `EF_VOLATILE` ability is 2 and whose value is split across two byte-sized effects, `EF_HWORDCOIN` (36) and `EF_LWORDCOIN` (37), yielding at most 65,535 coins per object.

**Detailed description**
`STRUCT_ITEM` (`Code/Basedef.h:398-412`) is a 2-byte index plus three `union { short sValue; struct { unsigned char cEffect; unsigned char cValue; } }` slots. `BASE_GetItemAbility` resolves an effect number by scanning those three slots and summing `cValue` where `cEffect` matches (`Code/Basedef.cpp` effect-scan loop), after consulting the static template for template-driven effects. Money therefore occupies one combined 16-bit quantity split in two bytes: the high byte is effect 36 and the low byte is effect 37 (`Code/ItemEffect.h:75-76`), read back as `(high << 8) + low` on pickup (`_MSG_GetItem.cpp:184-190`).

The consequence is a hard ceiling of 65,535 coins per ground money object, which explains why large payouts are implemented as item transfers, guild-zone state or direct balance arithmetic rather than as ground piles. It also means a money object is indistinguishable from an ordinary item at the record level: `Mode` is 1, `Decay` is 0, `Delay` is 90, it participates in the ordinary decay sweep, and it can only be recognised by evaluating its effects. The payload of a money object is authored in the item template table (or generated by whatever code sets effects 36/37 on a payload), not by the `CItem` component: no site in `Code/` writes effects 36/37 on a ground payload, so money objects can only come from the static item list replayed through `CreateItem`.

`CItem::Money` (`Code/TMSrv/CItem.h:36`) is present with a 4-byte width - enough for the real 2,000,000,000 balance range - and is **never read or written** anywhere in `Code/`. The architecture report describes the component as carrying "money", which is accurate about the declared shape of the record but not about its behaviour: the money path bypasses the field entirely. Any analysis or extension that assumes `pItem[n].Money` holds the value of a ground money drop would be wrong.

**Rule workflow**
```
ground money object: ITEM.sIndex = <template>, ITEM.stEffect[i].cEffect = 36 / 37, EF_VOLATILE = 2
pickup: coin = (cValue(36) << 8) + cValue(37)   // <= 65535
        MOB.Coin += coin (subject to 2e9 cap)
CItem.Money: unused
```

---

#### Business Rule: BR-15 - Client projection rules

**Overview**
The outbound snapshot of an object is produced by `GetCreateItem`, which defaults the wire height to -204, exposes the real height only for state 1, suppresses payload 5700 entirely and rewrites payload 3145 into a per-zone guild flag.

**Detailed description**
`GetCreateItem` (`Code/TMSrv/GetFunc.cpp:1195-1237`) fills a `MSG_CreateItem`: `ID = ESCENE_FIELD`, fixed `Size`/`Type`, `GridX/GridY` from the record, `ItemID = idx + 10000`, `Rotate`, the payload by value, `State`, and `Height = -204` as the default (`:1213`). Only when `State == STATE_OPEN` is the stored `Height` substituted and `Create` cleared (`:1232-1236`), so closed and locked doors always render with the "boxed" marker while open doors render at terrain height. Two payload-specific rewrites follow the defaults.

Payload 3145 is the guild-flag object. For it the projection substitutes `sIndex = 3145 + g_pGuildZone[zone].Victory` (the zone's owning guild's flag tier), forces `State = STATE_CLOSED`, and injects effects 56 and 57 carrying the guild charge split across the server index and the low byte (`:1215-1227`). This is a projection-only transformation: the stored `pItem[idx].ITEM` keeps 3145 and its original effects, so the same record renders differently to different zones and is re-derived on every view. Payload 5700 causes the function to `return` before the height logic (`:1229-1230`), producing a structurally valid but semantically empty message; the caller `SendCreateItem` has already allocated the packet on the stack, so the client receives a zeroed `_MSG_CreateItem` rather than nothing.

The truncation surface is significant here: `CItem::Height` is an `int` that may legitimately hold negative terrain values or a mask result, while `MSG_CreateItem::Height` is an `unsigned char` (`Code/Basedef.h:1604`). The -204 marker relies on that conversion (`-204 & 0xFF == 52`), and a genuine height above 255 or below 0 is likewise mangled. `Rotate` (int) is emitted as `unsigned char` (`Basedef.h:1602`), which is safe only because the field is never validated to the 0..3 range that `g_pGroundMask` expects (BR-19).

**Rule workflow**
```
GetCreateItem(idx, sm):
  sm.GridX/Y = PosX/PosY; sm.ItemID = idx+10000; sm.Rotate = Rotate
  sm.Item = ITEM; sm.State = State; sm.Height = -204
  if ITEM.sIndex == 3145: sIndex += zone.Victory; State = CLOSED; effects 56/57 = guild charge
  if ITEM.sIndex == 5700: return
  if State == STATE_OPEN: sm.Height = Height; sm.Create = 0
```

---

#### Business Rule: BR-16 - Gate/door state machine

**Overview**
The `State` field implements a three-state machine (`1 = open`, `2 = closed`, `3 = locked`). Only transitions between open and each of the other two are representable; closed and locked cannot be reached from each other. Every transition recomputes the object's height through the ground mask and clears `Delay`.

**Detailed description**
The states are constants `STATE_OPEN = 1`, `STATE_CLOSED = 2`, `STATE_LOCKED = 3` (`Code/Basedef.h:1835-1837`) and the transition legality is decided in the domain layer, not in `CItem`: `BASE_UpdateItem` computes `delta = +1` for `1 -> 2|3`, `delta = -1` for `2|3 -> 1`, and returns `FALSE` when `delta` stays 0, which covers both no-op transitions and the `2 <-> 3` pair (`Code/Basedef.cpp:5292-5306`). The same function rejects any `maskidx` outside `0..MAX_GROUNDMASK-1` (i.e. `GridCharge` outside `0..5`) before doing any work (`:5297-5298`), and walks the 6x6 mask applying `delta` to the height grid, clamping each cell to `0..255` and reporting the last written value as the object's new height (`:5308-5338`).

`UpdateItem` (`Code/TMSrv/Server.cpp:7472-7505`) is the wrapper that keeps the record coherent with that computation: it passes `GridCharge`, the current `State`, the requested state, `PosX/PosY`, the height grid, `Rotate` and an out-parameter; on success it writes `State`, writes `Height`, sets `Delay = 0`, re-checks the mask index range and, when the effective state equals the requested state (i.e. `BASE_UpdateItem` succeeded), special-cases `EF_KEYID == 15` on a `1 -> 3` transition by spawning a `"GATE"` NPC at the object's position (`:7495-7504`). Note that `UpdateItem` reads its height out-parameter into `Height` *before* validating the mask index range, so an out-of-range `GridCharge` still mutates `State`/`Height`/`Delay` and returns `TRUE` from the later branch.

Access control is layered on top. `Exec_MSG_UpdateItem` requires an `EF_KEYID` match against a carried item whenever the current or requested state is `3` (locked), except for payload 773 which is exempt from the "no key" message (`_MSG_UpdateItem.cpp:54-87`). `CCastleZakum::OpenCastleGate` intercepts keyed objects with ids in `10..14` and additionally requires an `EF_QUEST` level match, except for id 10 which is the castle entrance (`CCastleZakum.cpp:72-103`). Boot gates with `EF_KEYID` in `1..14` are locked at start-up by `UpdateItem(ID, STATE_LOCKED, &Height)` (`Server.cpp:7178-7185`) - although, as noted in BR-02, the `ID` passed there is the boolean result of `CreateItem`, so the lock is applied to slot 1 rather than to the gate just created.

**Rule workflow**
```
UpdateItem(gate, newState, &height):
  rotate = pItem[gate].Rotate
  ok = BASE_UpdateItem(GridCharge, State, newState, PosX, PosY, pHeightGrid, rotate, &height)
  if !ok -> FALSE
  State = newState; Height = height; Delay = 0
  if GridCharge outside 0..5 -> log, TRUE (after mutation)
  if keyid(gate) == 15 and newState == 3 and reachable-from-open -> spawn "GATE" npc
  return TRUE
```

---

#### Business Rule: BR-17 - GridCharge derivation and its role as mask index

**Overview**
`GridCharge` is the payload's `EF_GROUND` ability sampled once at creation. It is used as the outer index of `g_pGroundMask[maskidx][rotate][6][6]` and must stay within `0..5` for any state transition to be possible.

**Detailed description**
Both factories assign `GridCharge = BASE_GetItemAbility(item, EF_GROUND)` (`Code/TMSrv/Server.cpp:7285`, `9421`) from effect 34 (`Code/ItemEffect.h:73`). The field is never recomputed after creation, so a payload mutated in place after creation - which the component permits, since `ITEM` is public and several call sites `memcpy` over it - would leave a stale mask index. `MAX_GROUNDMASK` is 6 (`Basedef.h:84`) and the mask table is `int g_pGroundMask[6][4][6][6]` (`Basedef.h:2446`, defined `Basedef.cpp:173`).

The mask index is validated twice on every transition: `BASE_UpdateItem` rejects `maskidx >= 6 || maskidx < 0` (`Basedef.cpp:5297`) and `UpdateItem` re-checks the same condition after the height has already been written (`Server.cpp:7489-7493`). Because the table is never populated anywhere in the tree - `g_pGroundMask` has a definition and reads only (`Basedef.cpp:5312`, `5331`, `5352`, `Server.cpp:7514`) and no writer - every mask cell is zero, so `BASE_UpdateItem` performs no height mutation and reports `*height = 0` while still returning `TRUE` for legal transitions. The gate mechanism therefore currently toggles state and rewrites `Height` to 0 without any terrain deformation.

A side effect worth flagging is that `Rotate`, the second index of the same table, is not validated at all. It is stored verbatim from the creation call (`Server.cpp:7279`) where it originates from a client-supplied `unsigned char` in the drop packet (`_MSG_DropItem.cpp:112` passes `m->Rotate`), so values up to 255 reach `g_pGroundMask[maskidx][rotate][y][x]` with a declared second dimension of 4. Once the table is ever populated this becomes an out-of-bounds read (and, in the `BASE_UpdateItem` variant that writes, a potential out-of-bounds effect on the height grid).

**Rule workflow**
```
create: GridCharge = cValue(EF_GROUND)   // 0 when the effect is absent
update: BASE_UpdateItem(GridCharge, ...) -> FALSE if GridCharge not in 0..5
        table is all zeros today -> height result always 0, no deformation
rotate: unvalidated index into the [4] dimension (defect)
```

---

#### Business Rule: BR-18 - Automatic closing of open gates

**Overview**
The minute timer walks the boot-object range and closes any object that is open and keyed with `EF_KEYID < 15`, using `Delay` as a one-tick latch so a freshly opened door is not slammed immediately.

**Detailed description**
The routine is the "Close Gates" region of `ProcessMinTimer` (`Code/TMSrv/ProcessSecMinTimer.cpp:2141-2209`), driven by `TIMER_MIN` at a 12,000 ms period (`Server.cpp:3632`) with `MinCounter` incremented per invocation (`ProcessSecMinTimer.cpp:2282`) - so "minute" here means a 12-second unit. For each index `i` in `17..g_dwInitItem` it resolves the object through `g_pInitItem[i].PosX/PosY` and `pItemGrid[y][x]`, validates the payload index, reads `EF_KEYID`, and, when the object is `STATE_OPEN` with `keyid != 0` and `keyid < 15`, applies the latch: if `Delay == 0` it sets `Delay = 1` and skips (`:2159-2165`), otherwise it calls `UpdateItem(ipg, 3, &pItem[ipg].Height)` and then explicitly sets `STATE_LOCKED`, `Delay = 0` and rebroadcasts a `_MSG_CreateItem` (`:2166-2206`).

The latch depends on the decay sweep, not on a dedicated timer: `Delay = 1` is decremented to 0 by `ProcessDecayItem` within the next second, so the gate survives one timer period and closes on the following one - a minimum open duration of roughly 12 to 24 seconds. Because the latch and the decay countdown share the same field, the gate's `Delay` no longer carries its creation value and no longer expires, which is exactly the intent for boot objects (they are outside the decay scan range anyway, so the decrement never happens for them - the latch therefore relies on `UpdateItem`'s own `Delay = 0` write rather than on the sweep). The lower loop bound of 17 is a hardcoded assumption about the init list layout, consistent with the hardcoded slot constants catalogued in BR-03.

The effect is a self-healing world: any keyed door left open by a player, a GM `ogate` command (`imple.cpp:375-395`) or a castle event is reverted to locked within one to two timer periods, and clients are told via a create-message rather than a state-update message so the door is redrawn from scratch.

**Rule workflow**
```
every 12,000 ms (MinCounter++):
  for i in 17..g_dwInitItem-1:
    cell = pItemGrid[g_pInitItem[i].PosY][g_pInitItem[i].PosX]
    if payload index invalid -> continue
    key = EF_KEYID(payload)
    if key == 0 or key >= 15 -> continue
    if State != STATE_OPEN -> continue
    if Delay == 0 -> Delay = 1; continue          // one-tick latch
    UpdateItem(cell, STATE_LOCKED, &Height); State = 3; Delay = 0
    multicast _MSG_CreateItem
```

---

#### Business Rule: BR-19 - Rotation semantics and validation gap

**Overview**
`Rotate` is the visual orientation of the object, conventionally `0..3`, stored as `int` and emitted as `unsigned char`. It is never range-validated at any layer.

**Detailed description**
The field is set from the creation parameter without a mask or assertion (`Code/TMSrv/Server.cpp:7279`), and its sources are: the drop packet's `Rotate` byte (`_MSG_DropItem.cpp:112`, `unsigned char` per `MSG_DropItem`), the init item's `Rotate` (`Server.cpp:7173` from `g_pInitItem[i].Rotate`), and internal `rand() % 4` choices in the loot and event paths (`MobKilled.cpp:2193`, `ProcessSecMinTimer.cpp:2271`). The first source is client-controlled and reaches 255, so the field's runtime domain is the full byte range even though both the ground mask (`g_pGroundMask[...][4][...]`, `Basedef.h:2446`) and the client's rendering assume four orientations.

It is read in four places: the create/update projections (`GetFunc.cpp:1207`, `Server.cpp:7305`, `ProcessSecMinTimer.cpp:2200`), and the state transition (`Server.cpp:7474`). Only the last one has a consequence beyond appearance, because it indexes the mask. The GM `gate` command also takes `ival5` straight from the console (`imple.cpp:272`) with no bound check.

**Rule workflow**
```
create: Rotate = caller value (unvalidated, 0..255 reachable)
emit  : MSG_CreateItem.Rotate = (unsigned char)Rotate
update: g_pGroundMask[GridCharge][Rotate][y][x]  // Rotate >= 4 addresses outside the [4] dimension
```

---

#### Business Rule: BR-20 - Height semantics

**Overview**
`Height` records the object's vertical placement. It begins as the terrain sample at creation, is overwritten by the mask computation on state transitions, and is transmitted as a single byte.

**Detailed description**
At creation the field receives `pHeightGrid[y][x]` (`Code/TMSrv/Server.cpp:7289`, `9425`), a signed `char` grid (`Server.h:56`) that holds terrain elevation with a special `127` blocking sentinel (BR-05). On a successful state transition `UpdateItem` writes the out-parameter of `BASE_UpdateItem` into the field (`Server.cpp:7482`), and several call sites pass the field's own address as that out-parameter, e.g. `UpdateItem(ival2, STATE_LOCKED, &pItem[ival2].Height)` (`imple.cpp:315`), which is legal but couples read and write in one expression. When `g_pGroundMask` is empty - which it always is today (BR-17) - the resulting height is 0, so any gate interaction resets the object's height from terrain elevation to 0.

The value is only ever consumed by the projection, where it is cast to `unsigned char` (`GetFunc.cpp:1234`, `MSG_CreateItem::Height`), and only when the object is open (`:1232`); otherwise the -204 marker is used. That combination means the height a client sees is a lossy function of the stored value, and the same record can present different heights depending on its state at projection time.

**Rule workflow**
```
create    : Height = pHeightGrid[PosY][PosX]
transition: Height = *BASE_UpdateItem(...)   // 0 while g_pGroundMask is empty
project   : State == 1 -> Height (truncated to uchar); else -204
```

---

#### Business Rule: BR-21 - Declared but unimplemented fields

**Overview**
`Open`, `ItemQuest` and `Unk[20]` have no behaviour. `ItemQuest` is the only one that is written (to 0 in the constructor); `Open` and `Unk` are never referenced at all.

**Detailed description**
A repository-wide search over `Code/**/*.cpp` and `Code/**/*.h` finds zero occurrences of `.Open` and zero occurrences of `.Unk[` outside the declarations themselves, and exactly one occurrence of `ItemQuest` - the constructor assignment `ItemQuest = 0` (`Code/TMSrv/CItem.cpp:29`). `Open` (`CItem.h:37`) most plausibly once denoted whether an openable/closed container was open, a role now taken by `State`; `ItemQuest` (`CItem.h:38`) most plausibly denoted a quest binding, a role that in this tree is carried by the payload's `EF_QUEST` ability instead (`_MSG_UpdateItem.cpp:76`, `CCastleZakum.cpp:89`). `Unk[20]` (`CItem.h:39`) is an unclaimed 20-byte tail, presumably reserved by the original binary for fields the decompilation could not name; it inflates the record to 76 bytes and keeps the struct layout compatible with whatever the original snapshot expected.

The practical implication is that the component embodies **no quest semantics at object level**. A quest item on the ground is just an item with a payload; the quest state lives on the mob (`QuestInfo`), on the payload effect, or on the character's flag snapshot (`_MSG_GetItem.cpp:96-105` for the `sIndex 470` one-shot). Any analysis that attributes "quest flag" behaviour to the ground entity (as the architecture summary does) should be read as "the field exists but the behaviour was relocated into the item payload".

**Rule workflow**
```
CItem ctor: Mode=0; GridCharge=0; State=0; Rotate=0; Delay=0; ItemQuest=0
Open      : never read, never written
ItemQuest : written once (0), never read
Unk[20]   : never read, never written
```

---

#### Business Rule: BR-22 - Object identity and handle recycling

**Overview**
The wire identity of a ground object is its array index plus 10000. There is no generation counter, so a recycled slot silently becomes a new object under an unchanged handle.

**Detailed description**
`MSG_CreateItem.ItemID` and `MSG_DecayItem.ItemID` are computed as `slot + 10000` in every producer (`Code/TMSrv/Server.cpp:7298`, `8391`, `9434`; `GetFunc.cpp:1205`; `SendFunc.cpp:518`), and the client returns the same value in `MSG_GetItem` / `MSG_UpdateItem`, from which handlers subtract 10000 (`_MSG_GetItem.cpp:50`, `_MSG_UpdateItem.cpp:42`). Because `GetEmptyItem` returns the lowest free index (BR-02), a slot freed at time *t* by decay or pickup is typically re-issued at the next creation in that neighbourhood, and the client cannot distinguish the two objects: the handle is identical.

Three defences exist and they are all partial. The pickup handler re-validates the payload index and the position echo before granting (`_MSG_GetItem.cpp:89-92`, `174-178`), which rejects most stale requests because position and payload rarely coincide. The grid link check (`:164-172`) rejects a request whose cell no longer maps to the slot. And `SendFunc` repairs cells that point at free slots (`SendFunc.cpp:613-614`). None of these prevents a replayed request that happens to match the new occupant's position and a valid payload - the residual exposure is bounded by the requirement that the new object be at the same coordinates the client last saw.

The absence of a generation counter also affects logging and forensics: all item logs (`ItemLog`/`BASE_GetItemCode`, e.g. `_MSG_GetItem.cpp:228-231`) record the payload, never the handle, so a recycled slot cannot be traced.

**Rule workflow**
```
handle H = slot + 10000
destroy(slot) -> Mode = 0, H becomes dangling
create(slot)  -> new payload, same H
client request(H): dereferenced as slot = H - 10000 with no generation check
```

---

## 4. Component Structure

```
Code/TMSrv/
├── CItem.h                                    # 47 lines - the entity
│   ├── class CItem                            # :24  record only, no methods beyond ctor/dtor
│   │   ├── STRUCT_ITEM ITEM                   # :27  payload (sIndex + 3 x {cEffect,cValue})
│   │   ├── int Mode                           # :28  0 free / 1 object / 2 treasure box
│   │   ├── int PosX, PosY                     # :29  world cell
│   │   ├── int GridCharge                     # :30  EF_GROUND of payload -> ground-mask index
│   │   ├── int State                          # :31  1 open / 2 closed / 3 locked
│   │   ├── int Rotate                         # :32  visual orientation, unvalidated
│   │   ├── int Delay                          # :33  decay countdown AND gate latch
│   │   ├── int Decay                          # :34  -1 would mean permanent (never written)
│   │   ├── int Height                         # :35  terrain sample / mask result
│   │   ├── int Money                          # :36  DEAD FIELD
│   │   ├── int Open                           # :37  DEAD FIELD
│   │   ├── int ItemQuest                      # :38  DEAD FIELD (ctor writes 0 only)
│   │   ├── char Unk[20]                       # :39  DEAD FIELD (layout padding)
│   │   ├── CItem();  ~CItem();                # :42-43
│   │   └── extern CItem pItem[MAX_ITEM];      # :45  the single global instance
│   └── includes ../Basedef.h                  # :22
│
├── CItem.cpp                                  # 35 lines - 9 executable lines
│   ├── CItem::CItem()                         # :22-30 initialises Mode, GridCharge, State,
│   │                                          #        Rotate, Delay, ItemQuest (6 of 13 fields)
│   └── CItem::~CItem()                        # :32-35 empty
│
├── Server.cpp                                 # host of all component behaviour
│   ├── CItem pItem[MAX_ITEM]                  # :279  definition, static storage (~380 KB)
│   ├── int ItemCount = 100                    # :328  decay cursor (global, mutated in-loop)
│   ├── GetEmptyItem()                         # :6754-6763 first-free allocator, 0 = failure
│   ├── LoadInitItems / CreateItem replay      # :7166-7186 boot objects -> slots 1..g_dwInitItem
│   ├── CreateItem()                           # :7254-7317 ordinary factory (+ _MSG_CreateItem)
│   ├── UpdateItem()                           # :7472-7505 state machine wrapper
│   ├── ProcessDecayItem()                     # :8353-8396 decay sweep (transposed grid release)
│   ├── CreateTreasureBox()                    # :9392-9449 Mode=2 factory (NO CALLERS)
│   └── SetColoseumDoor/2, SetArenaDoor,
│       SetCastleDoor                          # :5887-6106 hardcoded slot 12/13, 14..18, 1..12, 36..39
│
├── GetFunc.cpp
│   ├── GetCreateItem()                        # :1195-1237 outbound projection (+3145/5700 rules)
│   └── GetEmptyItemGrid()                     # :1876-1899 3x3 placement resolution
│
├── SendFunc.cpp
│   ├── SendCreateItem()                       # :320-326  thin wrapper over GetCreateItem
│   ├── SendRemoveItem()                       # :510-525  _MSG_DecayItem to one client
│   ├── SendGridMob()                          # :560-618  initial view population
│   └── SendGridItem()                         # :720-760  incremental view population
│
├── _MSG_GetItem.cpp                           # :20-252 pickup handler (money, quest 470, notices)
├── _MSG_DropItem.cpp                          # :30-146 drop handler (calls CreateItem)
├── _MSG_UpdateItem.cpp                        # :40-105 gate interaction (key check -> UpdateItem)
├── _MSG_Attack.cpp                            # :164-178 mortar skill validates a ground payload (746)
├── CCastleZakum.cpp                           # :72-205 castle gate orchestration (keyid 10..14)
├── MobKilled.cpp                              # :727-750 gate reopen on boss death; :2185-2193 arena
│                                              #   reward drop; drop loops (partly disabled)
├── ProcessSecMinTimer.cpp                     # :1101-1102 decay tick; :2141-2209 gate auto-close;
│                                              #   :2273 dungeon prize drops
└── imple.cpp                                  # :264-428 GM gate/door commands (absolute slots)

Code/Basedef.h                                 # STRUCT_ITEM :398, STATE_* :1835-1837,
                                               # MAX_ITEM :103, MAX_ITEMLIST :139,
                                               # MAX_GROUNDMASK :84, MSG_CreateItem :1593,
                                               # MSG_DecayItem :1610, pItemGrid/pHeightGrid sizes
Code/Basedef.cpp                               # BASE_UpdateItem :5292, g_pGroundMask :173 (all zero)
Code/ItemEffect.h                              # EF_GROUND 34, EF_HWORDCOIN 36, EF_LWORDCOIN 37,
                                               # EF_VOLATILE 38, EF_KEYID 39
Code/TMSrv/Server.h                            # :143 CreateItem, :146 UpdateItem, :158 ProcessDecayItem,
                                               # :186 CreateTreasureBox, :235 extern pItem, :284 ItemCount
```

---

## 5. Dependency Analysis

### Internal dependencies

```
CItem (type)
 └─> Code/Basedef.h :: STRUCT_ITEM, MAX_ITEM                     (compile-time, only dependency)

pItem (global instance)
 ├─> Server.cpp          definition + allocator + factories + decay + state mutation + door setters
 ├─> GetFunc.cpp         projection (GetCreateItem), placement (GetEmptyItemGrid)
 ├─> SendFunc.cpp        view synchronisation (SendCreateItem / SendGridMob / SendGridItem)
 ├─> _MSG_GetItem.cpp    pickup, money absorb, quest 470, notices
 ├─> _MSG_UpdateItem.cpp gate interaction
 ├─> _MSG_Attack.cpp     mortar target validation (payload 746)
 ├─> CCastleZakum.cpp    castle gate orchestration
 ├─> MobKilled.cpp       boss-death gate reopen, arena reward drop, loot drops
 ├─> ProcessSecMinTimer.cpp decay tick driver, gate auto-close, dungeon prizes
 └─> imple.cpp           GM gate/door commands by absolute slot index

Runtime coupling (not visible to the compiler, but required for correctness)
 pItem[x].PosX/PosY  <->  pItemGrid[y][x]        bijection, see BR-06
 pItem[x].GridCharge <->  g_pGroundMask[..]      mask index, see BR-17
 pItem[x].Rotate     <->  g_pGroundMask[..][4]   table dimension, see BR-19
 pItem[x].Height     <->  pHeightGrid[y][x]      terrain sample, see BR-20
 pItem[x].ITEM       <->  g_pItemList[sIndex]    template lookup via BASE_GetItemAbility
 ItemCount (cursor)  <->  g_dwInitItem           reserved-range boundary, see BR-03
```

### External dependencies

| Dependency | Kind | Purpose in this component | Contract used |
|------------|------|---------------------------|---------------|
| `Code/Basedef.h` / `Basedef.cpp` | In-project domain layer | `STRUCT_ITEM` layout, `BASE_UpdateItem`, `BASE_GetItemAbility`, `BASE_ClearItem`, `BASE_GetItemCode`, `g_pGroundMask` | Struct layout, mask signature `(maskidx, curState, nextState, x, y, heightGrid, rotate, &height)`, ability reader returning summed `cValue` |
| `Code/ItemEffect.h` | In-project constant dictionary | `EF_GROUND(34)`, `EF_HWORDCOIN(36)`, `EF_LWORDCOIN(37)`, `EF_VOLATILE(38)`, `EF_KEYID(39)` | Effect numbers embedded as payload byte pairs |
| Win32 timer (`SetTimer`/`WM_TIMER`) | OS | Drives both the decay sweep (500 ms, `TIMER_SEC`) and the gate auto-close (12,000 ms, `TIMER_MIN`) | Coarse, non-reentrant tick; no drift compensation |
| Win32 API: `memcpy`/`memset`/`sprintf`/`fopen`/`rand` | CRT | Payload copy, record teardown, logging, breakpoint debug file | — |
| `CPSock` (`pUser[].cSock.AddMessage` / `SendOneMessage`) | In-project transport | Delivery of `_MSG_CreateItem`, `_MSG_DecayItem`, `_MSG_CNFGetItem` | Fire-and-forget buffered sends |
| `g_pItemList[MAX_ITEMLIST]` | In-memory item template table loaded from disk | Static abilities consulted by `BASE_GetItemAbility` | Indexed by payload `sIndex` |
| `g_pInitItem` / `g_dwInitItem` | Persisted map-object file (`Basedef.cpp:5913-5938`) | Boot placement of gates/doors | Array order determines slot numbers |
| Flat-file logs (`Log`, `ItemLog`) | File system | Failure diagnostics (`GetItemFail`, `err,updateItem maskidx illegal`) and item audit | Unstructured text |

### Notes on dependency direction

* The dependency graph is **inverted relative to the class**: `CItem` knows nothing about the world (efferent = 1), while the world knows everything about `CItem` (afferent ≈ 10 TUs / ≈ 20 including header includers and entity-pool users). This is the classic "anemic data record at the centre of a procedural monolith" shape.
* No abstraction boundary exists between the component and its consumers: there is no accessor, no validation function, no `CItem` method, and no interface. Every consumer re-implements bounds checks with slightly different bounds (`MAX_ITEM` vs `5000` vs `4096`, `MAX_ITEMLIST` vs explicit literals).
* `pItem` is co-declared in two headers (`CItem.h:45`, `Server.h:235`) and `pItemGrid`/`pHeightGrid` in three (`Server.h:55-56`, `Server.h:310-311`, `ProcessClientMessage.h:59-60`), so any consumer can obtain the globals through multiple include paths - including the umbrella `ProcessClientMessage.h:34`, which makes all 60 handler translation units compile-time dependants of the entity.

---

## 6. Afferent and Efferent Coupling

Units below are C++ classes `CItem` and the functions that constitute the component (C++ / procedural paradigm, so the coupling unit is class + function, not struct).

| Component unit | Afferent (in) | Efferent (out) | Critical | Notes |
|----------------|---------------|----------------|----------|-------|
| `class CItem` (record) | 6 (declaration/includers: `Server.h`, `ProcessClientMessage.h`, `ProcessDBMessage.h`, `CWarTower.cpp`, `ProcessClientMessage.cpp`, `ProcessDBMessage.cpp`) | 1 (`Basedef.h`) | Low | No behaviour, no invariants, no accessors |
| `pItem[MAX_ITEM]` (global instance) | 10 (Server, GetFunc, SendFunc, _MSG_GetItem, _MSG_UpdateItem, _MSG_Attack, CCastleZakum, MobKilled, ProcessSecMinTimer, imple) + 4 header-level includers | 3 (`pItemGrid`, `pHeightGrid`, `pMob` for coin) | High | Direct field access from every consumer; single point of truth for world objects |
| `CreateItem` | 7 call sites in 5 files (`_MSG_DropItem:112`, `imple:272`, `MobKilled:2193/2327/2384`, `ProcessSecMinTimer:2273`, `Server:6306/7173/8588`) | 7 (`GetEmptyItemGrid`, `GetEmptyItem`, `BASE_GetItemAbility`, `GridMulticast`, `pItem`, `pItemGrid`, `pHeightGrid`) | High | Also a de-facto part of `SendFunc`'s contract via `_MSG_CreateItem` |
| `CreateTreasureBox` | 0 call sites | 7 (same set as `CreateItem`) | Low | Dead code |
| `GetEmptyItem` | 2 (`CreateItem:7267`, `CreateTreasureBox:9403`) | 1 (`pItem`) | High | 4,999-object ceiling; O(n) scan |
| `GetEmptyItemGrid` | 3 (`_MSG_DropItem:57`, `CreateItem:7259`, `CreateTreasureBox:9397`) | 2 (`pItemGrid`, `pHeightGrid`) | High | Sole placement policy for all world objects |
| `UpdateItem` | 12 call sites in 6 files (`_MSG_UpdateItem:98`, `CCastleZakum`, `MobKilled:731`, `ProcessSecMinTimer:2166`, `imple:315/339/363/378/400/415`, `Server:5903/5951/6027/6074/7184`) | 3 (`BASE_UpdateItem`, `pItem`, `CreateMob`) | High | Mutates `State`, `Height`, `Delay` together |
| `ProcessDecayItem` | 1 (`ProcessSecMinTimer:1102`) | 4 (`pItem`, `pItemGrid`, `BASE_ClearItem`, `GridMulticast`) | High | Owns `ItemCount` cursor; only autonomous mutator |
| `GetCreateItem` | 7 (`SendFunc:325`, `imple:304/320/344/368/405/423`, `Server:5927/5975/6051/6100`) | 4 (`pItem`, `BASE_GetItemAbility`, `BASE_GetVillage`, `g_pGuildZone`) | High | Only path by which the record reaches the wire |
| `CItem::CItem` / `~CItem` | 2 (static init of `pItem`, any `CItem` local - none exist) | 0 | Low | Partial initialisation of 6 of 13 fields |

Aggregate shape: **afferent ≈ 20 dependants, efferent = 1-3**, high instability (`I = Ce/(Ca+Ce) ≈ 0.05-0.13`) with high abstractness of zero - the classic "stable but not abstract" quadrant risk (a stable dependency that cannot be extended). Cohesion inside the file pair is trivially maximal (one class, no methods); cohesion of the *component's behaviour* is low because it is spread across 10 files with duplicated validation.

---

## 7. Integration Points

| Integration | Type | Purpose | Protocol / Mechanism | Data Format | Error Handling |
|-------------|------|---------|----------------------|-------------|----------------|
| World item grid (`pItemGrid[4096][4096]`) | In-process global state | Occupancy index, cell -> slot resolution, view queries | Direct array access, no synchronization | `unsigned short` per cell (slot index) | Unchecked on read in several paths; stale-cell repair in `SendFunc`/`_MSG_GetItem` |
| Terrain height grid (`pHeightGrid[4096][4096]`) | In-process global state | Placement blocking (`127` sentinel), height sampling, mask target | Direct array access | signed `char` per cell | No validation at factory level; `127` is the only exclusion |
| Ground mask (`g_pGroundMask[6][4][6][6]`) | In-process global table | Door height deformation | `BASE_UpdateItem` | `int` per mask cell | Empty (all zeros) in this tree; index bounds checked for `maskidx` only, not `rotate` |
| Item template table (`g_pItemList[6500]`) | In-process, loaded from disk | Static abilities (`EF_GROUND`, `EF_KEYID`, `EF_VOLATILE`, coin bytes) and item names | `BASE_GetItemAbility` | `STRUCT_ITEMLIST` per index | Index validated `1..MAX_ITEMLIST-1`, returns 0 otherwise |
| Client protocol `_MSG_CreateItem` (110 \| 0x0200) | Game-client message | Announce object creation / re-draw doors | `GridMulticast`, `cSock.AddMessage` | Fixed struct: `GridX/Y`, `ItemID`, `Item`, `Rotate`, `State`, `Height`, `Create` | Fire-and-forget; no acknowledgement |
| Client protocol `_MSG_DecayItem` (111 \| 0x0100) | Game-client message | Announce object removal (decay, pickup, view exit) | `GridMulticast`, `SendRemoveItem`, direct `AddMessage` | Fixed struct with `ItemID` and `unk` | Fire-and-forget |
| Client protocol `_MSG_CNFGetItem` / `_MSG_CNFDropItem` | Game-client message | Confirm pickup / drop to the initiator | `cSock.AddMessage` | `DestPos`, `DestType` / `SourType`, `SourPos`, `GridX/Y` | Fire-and-forget |
| Database (`DBSrv`) | **none** | Ground objects are never persisted | — | — | Restart loses all ground objects (by design) |
| Text logs (`Log`, `ItemLog`, `TesteDie.txt`) | File system | Diagnostics for failed pickups, illegal mask indices, item audit trails | `sprintf` + function calls / `fopen("a+")` | Unstructured lines | No rotation, no flush control |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Object pool / fixed array | `CItem pItem[5000]` + `GetEmptyItem` first-free scan | `Server.cpp:279`, `6754-6763` | Pre-allocated, allocator-free entity storage; avoids heap fragmentation |
| Anemic domain record | Class with only fields, ctor and dtor | `CItem.h:24-44` | Pure data carrier; all logic external |
| Global accessor-free singleton array | `extern CItem pItem[MAX_ITEM]` in two headers | `CItem.h:45`, `Server.h:235` | Any module may read/write world objects directly |
| Factory function | `CreateItem` / `CreateTreasureBox` | `Server.cpp:7254`, `9392` | Two creation policies (decaying object, immortal box) |
| State machine (data-encoded) | `State` 1/2/3 with transition table in `BASE_UpdateItem` | `Basedef.cpp:5292-5306` | Door/gate open/close/lock |
| Two-phase destroy (logical then physical) | `Mode = 0` after payload clear + grid release + broadcast | `Server.cpp:8379-8393`; `_MSG_GetItem.cpp:201-249` | Ordering chosen so clients are told before the slot is reusable |
| Lazy sweep / round-robin cursor | `ProcessDecayItem` with persistent `ItemCount` | `Server.cpp:8353-8396` | Amortised expiry without per-object timers |
| Tick latch | `Delay = 1` used as a one-sweep delay flag | `ProcessSecMinTimer.cpp:2159-2165` | Defers door close by one timer period |
| Projection / DTO mapping | `GetCreateItem` builds a wire struct from the record | `GetFunc.cpp:1195-1237` | Isolates wire format from storage |
| Virtualised projection | Payload 3145 rewritten per zone; payload 5700 suppressed | `GetFunc.cpp:1215-1230` | Per-viewer presentation without mutating storage |
| Sentinel values | `Mode == 0` free, `PosX/PosY == 0`, `Height == 127` blocked, `ItemID = slot + 10000`, `Decay == -1` permanent | multiple | Absence encoding (mostly untyped `int`) |
| Feature-flag dead code | `#ifdef PKDrop` block with a signature mismatch | `Basedef.h:29` (commented), `MobKilled.cpp:2305-2418` | Player-kill item drop, disabled |

Architectural observations:

* The component is a **record in a transaction-script architecture**. There is no repository, no unit of work, no domain service; the "model" is a global array and the "logic" is a set of free functions in the server TU.
* **Ownership is implicit and positional.** Entity existence is encoded in a `Mode` integer, cell ownership in a parallel 16 MB grid, and identity in an arithmetic offset. No type encodes any of these relationships, so no compiler check can detect a violated invariant.
* **Synchronisation is single-threaded by construction**: everything runs on the Win32 message loop of the TMSrv window (`Server.cpp:3723-3729`), so no locking exists anywhere in the component. Any move to multi-threading (e.g. per-map threads) would race on `pItem`, `pItemGrid`, `pHeightGrid` and `ItemCount`.
* **No persistence boundary**: there is no serialization of `CItem`, no snapshot, no undo. Combined with in-place `memset` teardown (`imple.cpp:308`), the component has no audit trail of its own state transitions.

---

## 9. Technical Debt & Risks

| Risk | Area | Issue | Impact | Evidence |
|------|------|-------|--------|----------|
| High | Decay sweep | Grid release uses transposed indices (`pItemGrid[PosX][PosY]`) while every other site uses `[y][x]` | Wrong cell cleared; stale cell keeps a freed slot reachable; may clear another object's cell; next `CreateItem` in the true cell fails | `Server.cpp:8381` vs `Server.cpp:364`, `7287`, `_MSG_GetItem.cpp:164-172`, `248` |
| High | Decay sweep | Loop condition and cursor both mutate the global `ItemCount`; wrap target is `g_dwInitItem + 1` and the literal `5000` duplicates `MAX_ITEM` | Scan coverage depends on emergent behaviour; lifetime arithmetic is fragile; any change to the start value (100) silently changes world-wide item lifetimes; decouples from `MAX_ITEM` | `Server.cpp:8355-8360`, `328`, `103` |
| High | Decay configuration | `Decay == -1` ("permanent") is never written anywhere | The documented immortality switch is unreachable; all non-boot, non-1727 objects unavoidably expire after ~91 s | `Server.cpp:8374`, `7283`, `9419`, `imple.cpp:308` |
| High | State machine | `Rotate` is never validated yet indexes `g_pGroundMask[maskidx][rotate][...]` with a declared `[4]` dimension; source is a client byte | Out-of-bounds read (and, in the writing variant, out-of-bounds height-grid effect) once the mask table is populated | `Server.cpp:7279`, `7474`; `Basedef.h:2446`; `_MSG_DropItem.cpp:112` |
| High | API contract | `CreateItem` returns `BOOL` but four call sites treat the result as a slot index; one compares against `MAX_ITEM`, another passes it to `UpdateItem` | Boot-time gate locking is applied to slot 1 instead of the created gate; the `ID` guards are no-ops | `Server.cpp:7173-7185`, `imple.cpp:272-275`, `MobKilled.cpp:2327-2331`, `2384-2385` |
| High | Dead but uncompilable code | `#ifdef PKDrop` block calls `CreateItem` with 4 arguments against a 5-parameter declaration | Enabling `PKDrop` (`Basedef.h:29`) breaks the build; the drop logic is unverifiable | `MobKilled.cpp:2327`, `2384`; `Server.h:143` |
| High | Money identification | Money is recognised only by `EF_VOLATILE == 2` on the payload; no code in the tree creates coin effects 36/37 | Ground money can only originate from data files; `CItem::Money` is dead, so the architecture description of the field is misleading | `_MSG_GetItem.cpp:180-190`; `CItem.h:36` |
| Medium | Magic-number field confusion | Decay exempts payload `sIndex == 1727`; pickup gates **slot** `1727` for level 1000 | The intended level gate protects an arbitrary slot instead of the special item | `Server.cpp:8362` vs `_MSG_GetItem.cpp:84` |
| Medium | Reserved range | Boot objects depend on `g_pInitItem` order to land in slots 1..N, while gates are addressed by hardcoded constants (12, 13, 14..18, 1..12, 36..39, 45) and the auto-close loop starts at index 17 | One failed init entry shifts every slot and silently re-targets all gate logic; a GM `destroygate` frees a reserved slot into the general allocator | `Server.cpp:5889`, `5943`, `6018`, `6064`, `7166-7186`; `imple.cpp:308`, `414`; `ProcessSecMinTimer.cpp:2142` |
| Medium | Grid invariant | No single owner of `pItemGrid`; three repair strategies (silent zero, re-seat, abort) in three files | Desync between entity position and occupancy is detectable only indirectly (failed drops, ghost objects) | `SendFunc.cpp:613-614`, `759-760`; `_MSG_GetItem.cpp:164-172`; `imple.cpp:308` |
| Medium | Constructor incompleteness | `CItem::CItem` initialises 6 of 13 fields and not `ITEM`, `PosX/PosY`, `Decay`, `Height`, `Money`, `Open`, `Unk` | Safe only because the sole instance is a zero-initialised global; any stack/heap `CItem` starts with indeterminate state | `CItem.cpp:22-30` |
| Medium | Partial teardown | Slots freed by pickup keep their payload and `Delay = 0`; decay clears payload but keeps `State`/`Height`/`GridCharge`/`Decay` | A re-used slot lacking a full re-stamp inherits an expired countdown or stale state | `_MSG_GetItem.cpp:249`; `Server.cpp:8379-8382` |
| Medium | Protocol identity | No generation counter on `ItemID = slot + 10000` while allocation re-uses the lowest free slot | Delayed/replayed client requests can address a newly created object at the same coordinates | `Server.cpp:6754-6763`, `7298`, `8391` |
| Medium | Wire truncation | `Height` (int) and `Rotate` (int) are emitted as `unsigned char`; the -204 marker relies on that conversion | Heights outside 0..255 are silently mangled; the marker is indistinguishable from a legitimate byte value | `Basedef.h:1602-1604`; `GetFunc.cpp:1234`; `Server.cpp:7311-7312` |
| Medium | Unreachable object class | `CreateTreasureBox` has no callers; `Mode == 2` therefore never occurs | Dead branch in the decay test; `Mode` is effectively binary in practice | `Server.cpp:9392`, `8365`; `Server.h:186` |
| Medium | Effect-table emptiness | `g_pGroundMask` is defined but never written; every cell is 0 | Door state changes reset `Height` to 0 and never deform terrain, so the mask plumbing is untested and unverifiable | `Basedef.cpp:173`; readings only at `5312`, `5331`, `5352`, `Server.cpp:7514` |
| Low | Disabled rule | Height-band drop restriction commented out, and its condition was tautological (`>= -15 || >= 90`) | Objects can be placed on terrain previously excluded; intent unrecoverable | `Server.cpp:7261-7262` |
| Low | Duplicated placement | Drop handler resolves the cell, then `CreateItem` resolves it again | Two window scans per drop; the client's request cell is mutated before the failure message is sent | `_MSG_DropItem.cpp:57`; `Server.cpp:7259` |
| Low | Duplicated declarations | `pItem` declared `extern` in two headers; `pItemGrid`/`pHeightGrid` in three | Any header change ripples to all consumers | `CItem.h:45`, `Server.h:235`, `55-56`, `310-311`, `ProcessClientMessage.h:59-60` |
| Low | Logging inconsistency | `_MSG_GetItem` logs a duplicate format bug (`"posx:%d posx:%d"` for x then y) and silently drops several violations | Diagnostics cannot distinguish rejection causes; player gets no feedback | `_MSG_GetItem.cpp:77-78`, `74-82` |
| Low | Dead fields | `Money`, `Open`, `ItemQuest`, `Unk[20]` unused | 28 bytes per record (~140 KB across the array) reserved for nothing; misleading to readers | `CItem.h:36-39`, `CItem.cpp:29` |

---

## 10. Test Coverage Analysis

### Test inventory (evidence of absence)

| Search performed | Scope | Result |
|------------------|-------|--------|
| Files matching `*test*`, `*Test*`, `*spec*`, `*Spec*` | whole repository minus ignored folders | **0 files** |
| `TEST_CASE`, `TEST_F(`, `gtest`, `CppUTest`, `catch2`, `Boost.Test`, `assert.h` | all `Code/**/*.cpp`, `*.h`, `*.vcxproj` | **0 matches** |
| Solution content | `W2PP Code Project.sln` | Exactly 2 projects: `Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj` - no test project |
| Project file entry for the component | `Code/TMSrv/TMSrv.vcxproj` | `:94 <ClCompile Include="CItem.cpp" />`, `:172 <ClInclude Include="CItem.h" />` - compiled into the server only |
| Test/CI configuration | root and nested (`*.yml`, `*.yaml`, `*.bat`, `*.ps1`, `*.sh`, `CMakeLists.txt`, `Makefile`) | **0 files**; repository root contains only `.gitattributes`, `.gitignore`, `license.txt`, `README.md`, the `.sln` and the `docs/` tree |
| Fixtures / mocks / harnesses | all of `Code/` | none; the only test-adjacent artifact is the runtime debug file `TesteDie.txt` written by `MobKilled.cpp:2176-2182`, which is production diagnostic output, not a test |

Conclusion: **the component has zero automated test coverage - unit, integration, or otherwise.** There is no harness that could even instantiate `CItem` in isolation, because every behaviour lives in `Server.cpp` free functions that reach into `pItemGrid`, `pHeightGrid`, `g_pItemList`, `pMob`, `pUser` and the socket layer.

### Coverage matrix (all zero, with the behaviour that is unverified)

| Unit / behaviour | Unit tests | Integration tests | Coverage | Test quality | Unverified risk |
|------------------|-----------|-------------------|----------|--------------|-----------------|
| `CItem::CItem` / `~CItem` | 0 | 0 | 0% | none | Partial initialisation masked by static storage (BR-07) |
| `GetEmptyItem` | 0 | 0 | 0% | none | Exhaustion path, first-free policy, index-0 sentinel (BR-02) |
| `CreateItem` | 0 | 0 | 0% | none | Payload gate, occupancy gate, stamping, return-type contract (BR-04, BR-07) |
| `CreateTreasureBox` | 0 | 0 | 0% | none | Entire function unexercised (no callers) (BR-08) |
| `GetEmptyItemGrid` | 0 | 0 | 0% | none | 3x3 scan order, `127` sentinel, unguarded preferred-cell dereference (BR-05) |
| `ProcessDecayItem` | 0 | 0 | 0% | none | Countdown arithmetic, cursor/wrap behaviour, transposed grid release, exemptions (BR-06, BR-09, BR-10, BR-11) |
| `UpdateItem` + `BASE_UpdateItem` | 0 | 0 | 0% | none | Transition legality, `maskidx` bounds, post-mutation early return, keyid 15 NPC spawn (BR-16, BR-17) |
| Pickup handler (`_MSG_GetItem`) | 0 | 0 | 0% | none | Distance rule, grid/echo consistency, money cap, quest 470 one-shot, 490..499 notices (BR-12, BR-13) |
| Money encoding path | 0 | 0 | 0% | none | hi/lo computation, 65,535 ceiling, 2e9 refusal boundary (BR-14) |
| Projection (`GetCreateItem`) | 0 | 0 | 0% | none | -204 default, `STATE_OPEN` height substitution, 3145 virtualisation, 5700 suppression (BR-15) |
| Gate auto-close (`ProcessMinTimer` region) | 0 | 0 | 0% | none | One-tick latch, `keyid < 15` filter, index 17 lower bound (BR-18) |
| Slot recycling / identity | 0 | 0 | 0% | none | Stale-handle behaviour, absence of generation counter (BR-22) |
| Dead fields (`Money`, `Open`, `ItemQuest`, `Unk`) | 0 | 0 | 0% | none | Inferred dead from absence of references - no executable proof (BR-21) |

### Testability assessment

The design actively resists testing: (a) behaviour is not attached to the type, so there is no unit under test smaller than a `Server.cpp` function; (b) all state is global, so no fixture isolation is possible without resetting the 380 KB array, two 64 MB grids and the item template table; (c) time is read from Win32 timers rather than injected, so decay timing cannot be driven deterministically; (d) the projection writes through `CPSock` instances owned by the session array, so asserting on outbound messages requires a socket double that does not exist. Any future test effort would first need seam extraction, which is outside the scope of this analysis.

---

*End of report.*
