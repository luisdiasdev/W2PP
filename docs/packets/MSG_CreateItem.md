# MSG_CreateItem

Server-to-client packet that spawns a **ground item** (an entry of the world `pItem[MAX_ITEM]` item grid) into the field, so that clients render a pickable/usable object (loot drops, treasure boxes, gates/doors, quest objects).

---

## 1. Summary

| Property | Value |
|---|---|
| Constant | `_MSG_CreateItem` (Basedef.h:1592) |
| Expression | `(110 \| FLAG_CLIENT2GAME)` = `110 \| 0x0200` |
| Hex Type | `0x026E` |
| Flags | `FLAG_CLIENT2GAME` (0x0200) only |
| Struct | `MSG_CreateItem` (Basedef.h:1593-1607) |
| `sizeof(MSG_CreateItem)` | **32** bytes (computed, see §3.4) |
| Wire `Size` field | `sizeof(MSG_CreateItem)` = 32 (set by every producer) |
| Struct packing | MSVC default `/Zp8` (NOT in a `pack(1)` region) |
| Wire direction (actual) | **Game → Client** (built server-side, broadcast) |
| Wire direction (flag) | declared CLIENT2GAME — **discrepancy**, see §9 |
| Inbound dispatcher | **NONE** (no `case _MSG_CreateItem` in `ProcessClientMessage.cpp`) |
| Delivery | `GridMulticast` (area broadcast) — except `SendCreateItem` (per-conn) |
| Producers | `GetCreateItem` (GetFunc.cpp:1195), `CreateItem` (Server.cpp:7254), `CreateTreasureBox` (Server.cpp:9392), `ProcessMinTimer` respawn (ProcessSecMinTimer.cpp:2187), GM `imple.cpp` gate commands |

---

## 2. Wire Framing

`MSG_CreateItem` uses the standard CPSock framing (no per-packet deviation).

- **Session init magic**: connection is authenticated by a leading `INITCODE = 0x1F11F311` (CPSock.h:40); receive side checks it once before parsing messages (CPSock.cpp:371-379).
- **Header** `HEADER` (CPSock.h:42-50) == `_MSG` (Basedef.h:925-930): `Size(short,2)@0, KeyWord(char,1)@2, CheckSum(char,1)@3, Type(short,2)@4, ID(short,2)@6, ClientTick(uint,4)@8`. 12 bytes.
- **Obfuscation**: bytes from offset **4** (past Size+KeyWord+CheckSum) are transformed per-byte with a keyed XOR-ish additive cipher derived from `pKeyWord`, selected by `KeyWord` (CPSock.cpp:558-581 send; 430-453 recv). `Size` itself is **not** obfuscated; `Type` is obfuscated.
- **Checksum**: `CheckSum = Sum2 - Sum1` where Sum1 is the sum of the *plain* payload and Sum2 the sum of the *obfuscated* payload (CPSock.cpp:583-584). Verified on receive (CPSock.cpp:455-464).
- **Size bounds**: receive requires `HEADER ≤ Size ≤ MAX_MESSAGE_SIZE(8192)` (CPSock.cpp:397-406).
- **`BASE_CheckPacket`** (size-vs-`sizeof` cross-check incl. `_MSG_CreateItem`, Basedef.cpp:6514) is **commented out / DISABLED** — the function body is wrapped in `/* ... */` (Basedef.cpp:6476-6477).
- **Per-packet deviation**: none. `Type = _MSG_CreateItem (0x026E)`; `Size = sizeof(MSG_CreateItem) = 32`; `ID = ESCENE_FIELD = 30000` (Basedef.h:170) for all server broadcasts; `ClientTick` filled by CPSock::AddMessage (CPSock.cpp:541).

---

## 3. Binary Layout

Packing context: `MSG_CreateItem` is declared at Basedef.h:1593 — **outside** every `#pragma pack(push,1)` region (808-835, 1212-1246, 1465-1492, 2063-2097; Basedef.h:808/835/1212/1246/1465/1492/2063/2097). Therefore both the struct and its nested `STRUCT_ITEM` use MSVC **default `/Zp8`** packing. Alignment rules: natural alignment (short=2, char=1, uint=4), struct alignment = 4 (largest member).

### 3.1 Header (`_MSG`, 12 bytes)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 2 | `Size` | short | = 32 |
| 2 | 1 | `KeyWord` | char | obfuscation key index |
| 3 | 1 | `CheckSum` | char | Sum2-Sum1 |
| 4 | 2 | `Type` | short | 0x026E (obfuscated on wire) |
| 6 | 2 | `ID` | short | 30000 (ESCENE_FIELD) |
| 8 | 4 | `ClientTick` | unsigned int | filled by AddMessage |

Header size = 12.

### 3.2 Payload (18 bytes)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 12 | 2 | `GridX` | unsigned short | item grid X |
| 14 | 2 | `GridY` | unsigned short | item grid Y |
| 16 | 2 | `ItemID` | unsigned short | slot idx + 10000 |
| 18 | 8 | `Item` | STRUCT_ITEM | see §3.3 |
| 26 | 1 | `Rotate` | unsigned char | facing |
| 27 | 1 | `State` | unsigned char | STATE_OPEN/CLOSED/LOCKED |
| 28 | 1 | `Height` | unsigned char | -204 (0x34) for boxes/gates |
| 29 | 1 | `Create` | unsigned char | create-mode flag |
| 30 | 2 | *(padding)* | — | trailing pad to 8-aligned struct size |

Payload = 18 bytes (12→30); struct total 30 → padded to **32**.

### 3.3 Nested struct expansion — `STRUCT_ITEM` (Basedef.h:398-412)

| Offset (within Item) | Size | Field | Type |
|---|---|---|---|
| 0 | 2 | `sIndex` | short |
| 2 | 2 | `stEffect[0]` | union { short sValue; { uchar cEffect; uchar cValue; } } |
| 4 | 2 | `stEffect[1]` | union (same) |
| 6 | 2 | `stEffect[2]` | union (same) |

`sizeof(STRUCT_ITEM) = 8`, alignment 2, no padding. Union members are same 2-byte size (short vs two chars), so no internal padding. In `MSG_CreateItem` it lands at offset 18 (even → 2-aligned), so no padding before it. `stEffect[i].cEffect` and `.cValue` alias the low/high byte of `sValue` (little-endian). Absolute offsets: `sIndex@18`, `stEffect[0]@20-21`, `stEffect[1]@22-23`, `stEffect[2]@24-25`.

### 3.4 Size verification

| Producer | Field | Code | Cross-check |
|---|---|---|---|
| GetFunc.cpp:1195 | `sm->Size = sizeof(MSG_CreateItem)` | GetFunc.cpp:1198 | ✓ 32 |
| Server.cpp:7254 `CreateItem` | `sm.Size = sizeof(MSG_CreateItem)` | Server.cpp:7295 | ✓ 32 |
| Server.cpp:9392 `CreateTreasureBox` | `sm.Size = sizeof(MSG_CreateItem)` | Server.cpp:9431 | ✓ 32 |
| ProcessSecMinTimer.cpp | `sm.Size = sizeof(MSG_CreateItem)` | ProcessSecMinTimer.cpp:2190 | ✓ 32 |
| SendFunc.cpp:320 | `AddMessage(..., sizeof(MSG_CreateItem))` | SendFunc.cpp:327 | ✓ 32 |

Math:
```
Header : 2+1+1+2+2+4 = 12
GridX  : 2
GridY  : 2
ItemID : 2
Item   : 8  (STRUCT_ITEM)
Rotate : 1
State  : 1
Height : 1
Create : 1
Sum    : 12 + 2+2+2+8+1+1+1+1 = 12 + 18 = 30
Align to 4 (struct align) → 32
```
All field offsets are naturally aligned under `/Zp8` (no inter-field padding). `BASE_CheckPacket` would have required `Size == sizeof(MSG_CreateItem)` (Basedef.cpp:6514) but is disabled. **UNKNOWN**: the exact client meaning of `Create` (server sets it 0 for open items; the field is declared but never read server-side — see §9).

---

## 4. Lifecycle & Flow

`MSG_CreateItem` is produced **only by the game server (TMSrv)**; there is **no inbound client→game leg**.

### Producers

**A. `GetCreateItem(idx, sm)` — canonical builder** (GetFunc.cpp:1195-1237):
- `ID=ESCENE_FIELD`, `Size=sizeof`, `Type=_MSG_CreateItem`.
- `GridX/Y = pItem[idx].PosX/PosY`, `ItemID = idx + 10000`, `Rotate`, `Item = pItem[idx].ITEM`, `State`, `Height = -204`.
- Special-cases: guild-flags item `sIndex==3145` sets victory flag + guild charge into `stEffect` (GetFunc.cpp:1215-1227); `sIndex==5700` returns early (no broadcast, GetFunc.cpp:1229-1230); if `State==STATE_OPEN` then `Height = pItem[idx].Height` and `Create = 0` (GetFunc.cpp:1232-1236).

**B. `CreateItem(x, y, item, rotate, Create)`** (Server.cpp:7254-7317) — general ground-item spawn:
1. Guard `item->sIndex` in `[1, MAX_ITEMLIST)` (7256).
2. `GetEmptyItemGrid` snaps to a free walkable cell (7259, GetFunc.cpp:1876).
3. Guard `pItemGrid[y][x]` free (7264); `GetEmptyItem` slot (7267, Server.cpp:6754).
4. Fills `pItem[empty]` (Mode=1, Pos, ITEM, Rotate, State=STATE_OPEN, Delay=90, Decay=0, GridCharge=BASE_GetItemAbility(EF_GROUND), Height from `pHeightGrid`) (7272-7289).
5. Builds `MSG_CreateItem` and **`GridMulticast(x,y,&sm,0)`** (7291-7314). `Create==2` (Bau/box) → `Height=-204`.

**C. `CreateTreasureBox(x, y, item, rotate, State)`** (Server.cpp:9392-9449) — identical to `CreateItem` but `Mode=2`, takes `State` param, `Height=-204`, `GridMulticast`.

**D. `ProcessMinTimer` item respawn** (ProcessSecMinTimer.cpp:2019, block 2142-2208): for init items with a KEY ability (`iKey != 0`), after an open item's `Delay` elapses it sets `State=STATE_LOCKED`, `Delay=0` and re-broadcasts a fresh `MSG_CreateItem` (2187-2206) to re-spawn doors/boxes.

**E. Gate/door state changes** — when a door goes from OPEN to a closed/locked state the server re-sends `MSG_CreateItem` (instead of `MSG_UpdateItem`): `SetColoseumDoor/2`, `SetArenaDoor`, `SetCastleDoor` (Server.cpp:5924-5929, 5972-5977, 6048-6053, 6097-6102), and GM commands `cgate/ngate/mgate/openarmia/closearmia/creategate/destroygate` in `imple.cpp` (277-427).

**F. `SendCreateItem(conn, item, bSend)`** (SendFunc.cpp:320-331) — unicast variant for a single connection via `GetCreateItem` + `AddMessage`.

```
                 +--------------------------------------------+
                 |  TMSrv  (game server)                       |
                 |  pItem[MAX_ITEM] world item grid            |
                 |                                            |
   mob death     |                                            |
   quest reward  |   CreateItem / CreateTreasureBox           |
   player drop   |      |                                    |
   door/gate st. |   GetCreateItem  <-- ProcessMinTimer respawn
   init items    |      |                                    |
                 +------+-------------------------------------+
                        | MSG_CreateItem (Type=0x026E)
                        | Size=32, ID=ESCENE_FIELD
                        v
                 GridMulticast(x, y, &sm, 0)   (area broadcast)
                        |
                        v
                 +------------------+   +---------------------+
                 | clients in grid  |   | SendCreateItem conn |
                 | (per-conn AddMsg)|   | (unicast)           |
                 +------------------+   +---------------------+

   inbound?  client --(no case _MSG_CreateItem)--> ProcessClientMessage
              [confirmed absent, see §9]
```

**Inbound confirmation**: `ProcessClientMessage.cpp` dispatches on `switch(std->Type)` (line 66) with cases including `_MSG_DropItem` (198) and `_MSG_GetItem` (202) but **no `case _MSG_CreateItem`**; a repo-wide grep for `_MSG_CreateItem` matches only Basedef.h, the server producers, and the disabled `BASE_CheckPacket` — no inbound handler exists.

---

## 5. Validation & Guards

| # | Guard | Location | Action on fail |
|---|---|---|---|
| 1 | `item->sIndex <= 0 \|\| >= MAX_ITEMLIST` | Server.cpp:7256, 9394 | return FALSE |
| 2 | `pItemGrid[y][x]` occupied | Server.cpp:7264, 9400 | return FALSE |
| 3 | no free item slot `GetEmptyItem()==0` | Server.cpp:7269, 9403 | return FALSE |
| 4 | `GetEmptyItemGrid` finds no free walkable cell | Server.cpp:7259 → GetFunc.cpp:1876 | caller aborts (DropItem) |
| 5 | `item->sIndex == 5700` | GetFunc.cpp:1229 | GetCreateItem returns (no broadcast) |
| 6 | GM gate create: `gateid` in `[1, MAX_ITEM)` | imple.cpp:274 | return |
| 7 | (disabled) `Size != sizeof(MSG_CreateItem)` | Basedef.cpp:6514 | BASE_CheckPacket (disabled) |

---

## 6. Game Mechanics & Business Logic

- **Ground item spawn**: `MSG_CreateItem` is the *create* half of the world-item lifecycle (`CreateItem` → clients see object → `MSG_GetItem` pickup or `MSG_DecayItem` removal). It carries the full `STRUCT_ITEM` (index + up to 3 effect/value pairs) so clients can render and interact with the object.
- **Item ID encoding**: on-wire `ItemID = slot_index + 10000`; receivers decode with `itemID = m->ItemID - 10000` and index `pItem[itemID]` (e.g. _MSG_GetItem.cpp:50). `GetEmptyItem` scans slots `1..MAX_ITEM-1` (Server.cpp:6756); `MAX_ITEM=5000` (Basedef.h:103).
- **Position**: `GridX/GridY` come from the world grid; `CreateItem` relocates via `GetEmptyItemGrid` and records in `pItemGrid[y][x] = empty` (Server.cpp:7287).
- **State/Height**: `State` = `STATE_OPEN(1)/STATE_CLOSED(2)/STATE_LOCKED(3)` (Basedef.h:1835-1837). `Height=-204` (0x34) marks boxes/gates (CreateItem Create==2, CreateTreasureBox, GetCreateItem); open items get the actual terrain `Height` and `Create=0` (GetFunc.cpp:1232-1236).
- **Decay timing**: `CreateItem` sets `pItem[empty].Decay=0`, `Delay=90` (Server.cpp:7282-7283); decay is enforced elsewhere (item grid scanning in the minute/second timer) — the packet itself carries no timer.
- **Guild flag (3145)**: `GetCreateItem` overrides `sIndex` with the winning-guild variant and packs guild charge into `stEffect[0..1]` (GetFunc.cpp:1215-1227).

---

## 7. Side Effects

- **Broadcast**: `GridMulticast(x, y, (MSG_STANDARD*)&sm, 0)` to every client in the grid cell's area (Server.cpp:7314, 9446; SendFunc.h:47) — or per-connection via `SendCreateItem` (SendFunc.cpp:327).
- **World state written before broadcast**: `pItem[empty]` fields (Mode, Pos, ITEM, Rotate, State, Delay, Decay, GridCharge, Height) and `pItemGrid[y][x]=empty` (Server.cpp:7272-7289, 9423-9425).
- **GridCharge**: `BASE_GetItemAbility(item, EF_GROUND)` set on the spawned item (Server.cpp:7285, 9421).
- **Related state transitions**: door/gate state changes may also call `UpdateItem` before broadcasting (ProcessSecMinTimer.cpp:2166; imple.cpp:315, 339, 363, 378, 400, 415).
- **Item grid**: `pItemGrid` occupancy changes, which later blocks/blocks-others' `CreateItem` (guard #2) and `GetEmptyItemGrid`.

---

## 8. Related Packets

| Packet | Constant | Role |
|---|---|---|
| `MSG_UpdateItem` | `_MSG_UpdateItem` | server→client state change of an existing ground item (vs. create) — used for OPEN door transitions; `CreateItem` counterpart for CLOSED/LOCKED (Server.cpp:5911-5929) |
| `MSG_DecayItem` | `_MSG_DecayItem` (Basedef.h:1609) | server→client remove/expire of a ground item on pickup/clear (e.g. _MSG_GetItem.cpp:62-70, 111-119, 134-142) |
| `MSG_GetItem` | `_MSG_GetItem` | client→game pickup request (ProcessClientMessage.cpp:202); decodes `ItemID-10000` against `pItem` (inverse of the `+10000` encoding) |
| `MSG_CNFGetItem` | `_MSG_CNFGetItem` | server→client pickup confirmation |
| `MSG_DropItem` | `_MSG_DropItem` | client→game drop request; ultimately calls `CreateItem` → triggers `MSG_CreateItem` (ProcessClientMessage.cpp:198, _MSG_DropItem.cpp:112) |
| `MSG_CNFDropItem` | `_MSG_CNFDropItem` | server→client drop confirmation (sent alongside the CreateItem broadcast, _MSG_DropItem.cpp:127-139) |
| `MSG_CreateMob` | `_MSG_CreateMob` | analogous world-spawn for mobs (mob → item drop path) |

---

## 9. Discrepancies & Open Questions

1. **Direction-flag mismatch (primary)**: `_MSG_CreateItem` is declared with `FLAG_CLIENT2GAME` (Basedef.h:1592), yet the codebase **only builds it server-side and broadcasts it to clients** (G2C behavior). There is **no** `case _MSG_CreateItem` inbound handler in `ProcessClientMessage.cpp`, and no other consumer decodes it from a client. The flag is inconsistent with actual usage (legacy flag likely inherited from the original client/server contract).
2. **`Create` field** is set only in `GetCreateItem` (0 for open items, GetFunc.cpp:1235) and left 0 by the memset in other producers; its client-side semantics are undocumented and **UNKNOWN** (server never reads it back).
3. **`Height=-204`** is a hardcoded magic for boxes/gates; its interpretation (signed char 0x34 vs visual offset) is **UNKNOWN** beyond matching box/treasure rendering.
4. **`sIndex==5700`** special-case (GetFunc.cpp:1229) returns without setting/broadcasting — reason (a non-spawnable item id) is **UNKNOWN**.
5. **Disabled validation**: `BASE_CheckPacket` — which would enforce `Size == sizeof(MSG_CreateItem)` — is entirely commented out (Basedef.cpp:6476), so malformed sizes are not caught.
6. The world-item `Decay`/`Delay` timers are not carried by this packet; client-side decay/removal relies on the separate `MSG_DecayItem`, so timing consistency is split between server timers and packet stream.

---

## 10. Source References

- `legacy/Code/Basedef.h:1592` — `_MSG_CreateItem = (110 | FLAG_CLIENT2GAME)` → `0x026E`
- `legacy/Code/Basedef.h:1593-1607` — `struct MSG_CreateItem`
- `legacy/Code/Basedef.h:398-412` — `struct STRUCT_ITEM` (8 bytes)
- `legacy/Code/Basedef.h:925-930` — `_MSG` header macro
- `legacy/Code/Basedef.h:932-941` — flag constants
- `legacy/Code/Basedef.h:170` — `ESCENE_FIELD=30000`; `:103` `MAX_ITEM=5000`; `:139` `MAX_ITEMLIST=6500`; `:1835-1837` STATE_*
- `legacy/Code/CPSock.h:38-50` — `MAX_MESSAGE_SIZE`, `INITCODE=0x1F11F311`, `HEADER`
- `legacy/Code/CPSock.cpp:249,371-379` — INITCODE auth; `:397-406` size bounds; `:513-591` AddMessage (obfuscation, checksum); `:430-466` recv deobfuscation
- `legacy/Code/Basedef.cpp:6475-6514` — `BASE_CheckPacket` (disabled), `:6514` CreateItem size check
- `legacy/Code/TMSrv/GetFunc.cpp:1195-1237` — `GetCreateItem`
- `legacy/Code/TMSrv/Server.cpp:7254-7317` — `CreateItem` (builder @7291-7314)
- `legacy/Code/TMSrv/Server.cpp:9392-9449` — `CreateTreasureBox` (builder @9427-9446)
- `legacy/Code/TMSrv/Server.cpp:5924-5929,5972-5977,6048-6053,6097-6102` — gate/door state re-create
- `legacy/Code/TMSrv/Server.cpp:6754-6763` — `GetEmptyItem`
- `legacy/Code/TMSrv/GetFunc.cpp:1876-1899` — `GetEmptyItemGrid`
- `legacy/Code/TMSrv/ProcessSecMinTimer.cpp:2019,2142-2208` — item respawn (`ProcessMinTimer`)
- `legacy/Code/TMSrv/imple.cpp:277-427` — GM gate commands using GetCreateItem/CreateItem
- `legacy/Code/TMSrv/SendFunc.cpp:320-331` — `SendCreateItem` (unicast)
- `legacy/Code/TMSrv/ProcessClientMessage.cpp:66,198,202` — dispatcher: has DropItem/GetItem, **no CreateItem**
- `legacy/Code/TMSrv/_MSG_DropItem.cpp:112` — drop → `CreateItem`
- `legacy/Code/TMSrv/_MSG_GetItem.cpp:50,62-142` — pickup (`ItemID-10000`), `MSG_DecayItem`
- `legacy/Code/TMSrv/CItem.h:24-44` — `class CItem` (world item record)
