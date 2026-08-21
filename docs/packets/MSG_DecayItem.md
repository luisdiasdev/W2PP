# MSG_DecayItem

Server→Client notification that a **ground item has been removed** (decayed after its lifetime expiry, or consumed by a pickup / game rule). It tells every client in the area grid to despawn the item at `ItemID`.

## 1. Summary

| Attribute | Value |
|---|---|
| Macro name | `_MSG_DecayItem` |
| Definition | `Basedef.h:1609` — `(111 \| FLAG_GAME2CLIENT)` |
| Raw value (decimal) | `111 \| 0x0100 = 367` |
| Raw value (hex) | `0x016F` |
| Direction | **GAME2CLIENT only** (server → client) |
| Struct | `MSG_DecayItem` — `Basedef.h:1610-1617` |
| Wire `Size` (sizeof) | **16 bytes** |
| Producer sites | `SendFunc.cpp:512` (`SendRemoveItem`), `_MSG_GetItem.cpp:62,111,134,148`, `Server.cpp:8384` (`ProcessDecayItem`) |
| Inbound dispatcher | **None** — no `case _MSG_DecayItem` in `ProcessClientMessage.cpp` (verified, no match) |
| Aliases | none (not referenced via `MSG_STANDARD`/`MSG_STANDARDPARM`) |

`FLAG_GAME2CLIENT = 0x0100` (`Basedef.h:932`). `111 | 0x0100 = 0x6F | 0x0100 = 0x016F = 367`. Confirmed against `Basedef.h:1609`.

## 2. Wire Framing (protocol preamble)

All packets share the framing applied in `CPSock.cpp`:

- **Magic / handshake:** `INITCODE = 0x1F11F311` validated at connection setup (`CPSock.cpp:249,373`). Not part of the per-packet body.
- **Header** `_MSG` (`Basedef.h:925-930`) = 12 bytes:
  | Offset | Size | Field | Notes |
  |---|---|---|---|
  | 0 | 2 | `Size` | total packet size incl. header |
  | 2 | 1 | `KeyWord` | index into `pKeyWord` table (cipher key selector) |
  | 3 | 1 | `CheckSum` | `Sum2 - Sum1` obfuscation checksum |
  | 4 | 2 | `Type` | `_MSG_DecayItem = 0x016F` |
  | 6 | 2 | `ID` | `ESCENE_FIELD = 30000` (`Basedef.h:170`) |
  | 8 | 4 | `ClientTick` | server timestamp, `CPSock.cpp:541` |
- **Obfuscation:** payload from offset 4 is XOR/arithmetic-transformed per byte, keyed by `pKeyWord[KeyWord]` (`CPSock.cpp:556-575`, `mod = i&0x3` branch table). `CheckSum = Sum2 - Sum1` (`CPSock.cpp:583-584`). First 4 bytes copied verbatim (`CPSock.cpp:580`).
- **Validation:** `BASE_CheckPacket` (`Basedef.cpp:6475`) is **entirely commented out** → size/type validation is DISABLED. No per-packet guard on `MSG_DecayItem`.

Per-packet deviations for `MSG_DecayItem`: none. It is sent via the standard `AddMessage` path (`CPSock.cpp:523-589`) with `Size = 16`. `Type` and `ID` are set manually by each producer; `Size`, `KeyWord`, `CheckSum`, `ClientTick` are filled by `AddMessage` (`CPSock.cpp:535-541`).

## 3. Binary Layout

Packing context: `Basedef.h` uses `#pragma pack(push,1)`/`pop` regions only at `808-835`, `1212-1246`, `1465-1492`, `2063-2097`. `MSG_DecayItem` is declared at `1609-1617` — **after** the `1465-1492` region, **before** the `2063-2097` region → it compiles under **MSVC default `/Zp8`** (natural alignment, max 8).

### 3.1 Header (`_MSG`, 12 bytes — `Basedef.h:925-930`)

| Field | Type | Size | Offset | Align | Padding |
|---|---|---|---|---|---|
| `Size` | short | 2 | 0 | 2 | – |
| `KeyWord` | char | 1 | 2 | 1 | – |
| `CheckSum` | char | 1 | 3 | 1 | – |
| `Type` | short | 2 | 4 | 2 | – |
| `ID` | short | 2 | 6 | 2 | – |
| `ClientTick` | unsigned int | 4 | 8 | 4 | – |
| **Subtotal** | | **12** | | | **0** |

### 3.2 Payload (4 bytes)

| Field | Type | Size | Offset | Align | Padding |
|---|---|---|---|---|---|
| `ItemID` | short | 2 | 12 | 2 | – |
| `unk` | short | 2 | 14 | 2 | – |
| **Subtotal** | | **4** | | | **0** |

### 3.3 Nested struct expansions

No nested structs. `_MSG` is a macro (not a member object) expanded inline, so the 12 header bytes + 4 payload bytes are contiguous with no sub-struct alignment boundary.

### 3.4 Size verification

```
sizeof(_MSG)            = 12
ItemID  @ 12  (short, 2)  → ends 14
unk     @ 14  (short, 2)  → ends 16
sizeof(MSG_DecayItem)   = 16
expected Size field     = 16
```

Cross-checked against every producer — all set `sm.Size = sizeof(MSG_DecayItem)`:
- `SendFunc.cpp:516` (`SendRemoveItem`)
- `_MSG_GetItem.cpp:66,115,138,152`
- `Server.cpp:8388` (`ProcessDecayItem`)

All consistent; **no mismatch** between `sizeof()` usage and the computed /Zp8 layout.

| Member | Offset | Verified meaning |
|---|---|---|
| `ItemID` | 12 | `10000 + ground-item array index` (client subtracts 10000). Set as `10000+itemid` in `SendFunc.cpp:519`, `ItemCount+10000` in `Server.cpp:8392`, raw `m->ItemID` (already offset) in `_MSG_GetItem.cpp:70,118,140,154`. |
| `unk` | 14 | Always written as `0` (`SendFunc.cpp:520`, `_MSG_GetItem.cpp:71,119,141,155`; not set in `Server.cpp:8387-8392`, left `0` by the preceding `memset`). Purpose **UNKNOWN** — unused by server, likely reserved/legacy. |

## 4. Lifecycle & Flow

**GAME2CLIENT only.** There is no client→game inbound leg: `ProcessClientMessage.cpp` has no `case _MSG_DecayItem` (verified by grep — no match), and `ProcessDBMessage.cpp` has none either. The client never sends this packet; it only receives it. The complementary inbound packet is `_MSG_GetItem` (`case _MSG_GetItem` at `ProcessClientMessage.cpp:202-203` → `Exec_MSG_GetItem`).

The packet is emitted from three server code paths, all broadcasting to the surrounding area grid:

```text
 [Pickup: client sends _MSG_GetItem]
        │  ProcessClientMessage.cpp:202 → Exec_MSG_GetItem (_MSG_GetItem.cpp)
        ▼
 _MSG_GetItem.cpp:148-159  sm_deci.ItemID=m->ItemID; GridMulticast(itemX,itemY,&sm_deci,0)
        │  (also 62,111,134 for fail/early/coin/quest branches — some send only to conn)
        ▼
 [Periodic decay: ProcessSecMinTimer.cpp:1102, every 2 s]
        ▼
 Server.cpp:8353 ProcessDecayItem()
        │  if Mode!=0/2, Delay-- → 0, Decay!=-1 → clear item
        │  Server.cpp:8384-8393 sm.ItemID=ItemCount+10000; GridMulticast(itemPosX,itemPosY,&sm,0)
        ▼
 [Area range refresh: SendFunc.cpp:740]
        │  SendRemoveItem(conn,titem,0) — SendFunc.cpp:510-523, direct AddMessage to one conn
        ▼
 AddMessage → CPSock.cpp:523  (Size=16, KeyWord/CheckSum/ClientTick set, payload obfuscated)
        ▼
 GridMulticast (SendFunc.cpp:620) → fans out to every user in VIEWGRID area around (tx,ty)
```

`GridMulticast` (`SendFunc.cpp:620`) rebroadcasts the passed `MSG_STANDARD*` to all connected users whose grid cell falls inside the `VIEWGRIDX × VIEWGRIDY` window centered on `(tx,ty)` (`SendFunc.cpp:660-700`). It is invoked with `(MSG_STANDARD*)&sm_deci` so the `MSG_DecayItem` is treated as a standard header + payload blob.

## 5. Validation & Guards

Wire-level validation is disabled (`BASE_CheckPacket` commented out, `Basedef.cpp:6475`). The guards below are **server-side business logic** that decide whether a decay packet is emitted:

| # | Guard | Location | Outcome |
|---|---|---|---|
| 1 | `pItem[id].Mode == 0 \|\| Mode == 2` → skip | `Server.cpp:8368` | Ground-only (`Mode==1`) items eligible for periodic decay |
| 2 | `Delay >= 1` → decrement, skip this tick | `Server.cpp:8370-8373` | Lifetime countdown (created with `Delay=90`, `Server.cpp:7283`) |
| 3 | `Decay != -1` | `Server.cpp:8374` | Always true in this tree (`Decay` only ever set to 0, `Server.cpp:7283,9419`) → all ground items eventually decay |
| 4 | item out of grid bounds / not on grid / grid mismatch | `_MSG_GetItem.cpp:163-196` | Fallback: send decay to the single `conn` (not multicast) |
| 5 | `itemID<=0 \|\| itemID>=MAX_ITEM` | `_MSG_GetItem.cpp:52-53` | Drop, no packet |
| 6 | distance > 3 tiles from pickup target | `_MSG_GetItem.cpp:77-86` | Drop, no packet |
| 7 | `sIndex<=0 \|\| sIndex>=MAX_ITEMLIST` | `_MSG_GetItem.cpp:95-96` | Drop, no packet |
| 8 | volatile coin overflow (>= 2,000,000,000) | `_MSG_GetItem.cpp:185-189` | No decay packet, keep item |

There is **no** inbound validation for `MSG_DecayItem` because there is no inbound case.

## 6. Game Mechanics & Business Logic

- **Purpose:** force all nearby clients to remove a ground item (`pItemGrid[y][x]`) from their local scene. The client despawns the entity identified by `ItemID` (index = `ItemID - 10000`).
- **Periodic decay (lifetime expiry):** `ProcessDecayItem` (`Server.cpp:8353`) runs every 2 s (`ProcessSecMinTimer.cpp:1102`, gated by `SecCounter % 2 == 0`). For each live ground item it decrements `Delay` (a per-item lifetime counter, initialized to `90` at creation, `Server.cpp:7283`); when `Delay` reaches 0 and `Decay != -1`, it clears the item (`BASE_ClearItem`), zeroes the grid slot, sets `Mode=0`, and multicasts `MSG_DecayItem` to the area (`Server.cpp:8378-8393`).
- **Pickup removal:** `Exec_MSG_GetItem` (`_MSG_GetItem.cpp`) removes the ground item on successful pickup and multicasts `MSG_DecayItem` to the area (`_MSG_GetItem.cpp:245-246`), then zeroes the grid and sets `Mode=0` (`_MSG_GetItem.cpp:247-248`). It is also emitted in branch paths: invalid item index/mode (`:62-71`), orc-pill quest item (`:111-122`), and notice items sIndex 490-499 (`:134-143`) — those use `GridMulticast`; the volatile-coin and early-fail branches send only to the requesting connection (`:159,166,176`).
- **Area refresh:** `SendRemoveItem` (`SendFunc.cpp:510-523`) is called from the grid-render range walk (`SendFunc.cpp:740`) to notify a single client to drop items that have left its view window (`ItemID = 10000 + titem`).
- **`ID` field** is always `ESCENE_FIELD = 30000` (`Basedef.h:170`), the shared identifier for server-sent messages; clients use it to distinguish scene messages from chat/trade/etc.

## 7. Side Effects

- `pItem[ItemID-10000].ITEM` cleared via `BASE_ClearItem` (`Server.cpp:8379`, `_MSG_GetItem.cpp:103,235`).
- `pItemGrid[y][x] = 0` — grid slot freed (`Server.cpp:8380`, `_MSG_GetItem.cpp:247`).
- `pItem[ItemID-10000].Mode = 0` — item marked dead (`Server.cpp:8381`, `_MSG_GetItem.cpp:248`).
- All clients in the surrounding area view remove the item entity from their scene.
- On pickup path only: item added to player carry (`memcpy` into `MOB.Carry[DestPos]`, `_MSG_GetItem.cpp:222`), `MSG_CNFGetItem` confirmation sent (`_MSG_GetItem.cpp:228-242`), `SendItem` refresh (`_MSG_GetItem.cpp:249`), optional coin gain / skill-point grant (`_MSG_GetItem.cpp:180-199`).
- Item index `1727` is explicitly exempt from periodic decay (`Server.cpp:8366`).

## 8. Related Packets

| Packet | Definition | Relation |
|---|---|---|
| `_MSG_GetItem` | `Basedef.h:1620` (112 \| CLIENT2GAME) | Inbound request that triggers pickup-decay emission |
| `_MSG_CNFGetItem` | `Basedef.h:1628` (113 \| GAME2CLIENT) | Pickup confirmation sent alongside the decay |
| `_MSG_CreateItem` | (creation packet) | Complementary create; item spawn sets the `Delay`/`Decay` state that `MSG_DecayItem` later reverses |
| `MSG_STANDARD` | `Basedef.h:943` | `GridMulticast` casts decay to this for area fan-out |

## 9. Discrepancies & Open Questions

- `unk` (offset 14) is always `0` and never read by the server — reserved/legacy, purpose **UNKNOWN**.
- `Decay` field is only ever initialized to `0` and never set to `-1` in the tree (`Server.cpp:7283,9419`), so the `Decay != -1` branch (`Server.cpp:8374`) is effectively unconditional — item lifetime is governed solely by `Delay`. It is unclear whether the original client/server used `Decay` for a distinct expiry mechanism.
- `ProcessDecayItem` increments the loop index `ItemCount` then bounds-checks (`Server.cpp:8354-8360`) rather than the reverse; the `if (ItemCount >= 5000) ItemCount = g_dwInitItem+1;` line inside the loop is suspicious (re-initializes mid-iteration) but matches upstream style — potential off-by-one / index-reset quirk.
- The coin-overflow branch (`_MSG_GetItem.cpp:185-189`) returns **without** sending a decay packet, leaving the volatile-coin item in the world; the not-full-bag fallback path (`:198-203`) likewise does not always send.

## 10. Source References

| File | Lines | Content |
|---|---|---|
| `Basedef.h` | 1609-1617 | `_MSG_DecayItem` macro + `MSG_DecayItem` struct |
| `Basedef.h` | 925-930 | `_MSG` header macro |
| `Basedef.h` | 932-941 | flag constants (GAME2CLIENT = 0x100) |
| `Basedef.h` | 808,835,1212,1246,1465,1492,2063,2097 | `#pragma pack` regions |
| `Basedef.h` | 170 | `ESCENE_FIELD = 30000` |
| `CPSock.cpp` | 523-589 | `AddMessage`: framing, obfuscation, checksum |
| `CPSock.cpp` | 249,373 | `INITCODE` handshake |
| `Basedef.cpp` | 6475 | `BASE_CheckPacket` (disabled) |
| `SendFunc.cpp` | 510-523 | `SendRemoveItem` producer |
| `SendFunc.cpp` | 620-700 | `GridMulticast` area fan-out |
| `SendFunc.cpp` | 740 | area-range refresh call to `SendRemoveItem` |
| `_MSG_GetItem.cpp` | 62,111,134,148 | pickup-decay producers |
| `Server.cpp` | 8353-8393 | `ProcessDecayItem` producer |
| `Server.cpp` | 7283,9419 | item creation (`Decay=0`) |
| `ProcessSecMinTimer.cpp` | 1102 | periodic call to `ProcessDecayItem` |
| `ProcessClientMessage.cpp` | 202-203 | inbound `_MSG_GetItem` dispatch (no `_MSG_DecayItem` case) |
| `CItem.h` | 26-44 | `CItem` fields incl. `Delay`, `Decay`, `Mode` |
