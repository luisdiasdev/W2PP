# MSG_DropItem

## 1. Summary

| Field | Value |
|---|---|
| **Symbol** | `_MSG_DropItem` |
| **Struct** | `MSG_DropItem` (Basedef.h:1639) |
| **Sequence number** | `114` (0x72) |
| **Flags** | `FLAG_CLIENT2GAME` = 0x200 |
| **Full Type value** | `114 \| 0x200` = **0x272** |
| **Direction** | Client → TMSrv (inbound) |
| **Purpose** | Client asks server to drop an item from the carry/cargo inventory onto the ground |
| **Handler** | `Exec_MSG_DropItem` (_MSG_DropItem.cpp:20) |
| **Dispatch** | `ProcessClientMessage.cpp:198` (`case _MSG_DropItem:`) |
| **Response packet(s)** | `MSG_CNFDropItem` (0x175, to self, `SendOneMessage`) + `MSG_CreateItem` (0x172, `GridMulticast` to area) |
| **Header size** | 12 bytes (`_MSG`) |
| **Total struct size (`sizeof`)** | **32 bytes** |
| **Expected `Size` field** | **32** |
| **Packing** | none — default MSVC /Zp8 (outside all pack regions) |

Aliases / names: `_MSG_DropItem` (Basedef.h:1638), `MSG_DropItem` (Basedef.h:1639),
handler decl `Exec_MSG_DropItem` (ProcessClientMessage.h:103). No other aliases.

---

## 2. Wire Framing (protocol preamble)

The `MSG_DropItem` payload travels inside the generic stream-framing layer
(`CPSock.cpp`). There is **no per-packet deviation** from the standard preamble.

**On-wire frame** (`CPSock.cpp:385-421`, `ReadMessage`):

| Offset | Bytes | Field | Notes |
|---|---|---|---|
| 0 | 4 | Magic `INITCODE` = `0x1F11F311` | first frame only (CPSock.cpp:40, 373) |
| 4 | 2 | `Size` (unsigned short) | total incl. header; validated `[HEADER, MAX_MESSAGE_SIZE]` (CPSock.cpp:397) |
| 6 | 1 | `iKeyWord` | index into `pKeyWord[512]` table (CPSock.cpp:29, 391-392) |
| 7 | 1 | `CheckSum` | `Sum2 - Sum1` (CPSock.cpp:455) |
| 8 | 4 | `SockType` | |
| 12 | 4 | `SockID` | |

**Obfuscation** (CPSock.cpp:430-453, receive): payload bytes from offset 4 are
transformed per-byte XOR-arithmetic keyed by `KeyWord = pKeyWord[iKeyWord*2]`,
modulated by `i&0x3`:
- `mod 0`: `b -= Trans << 1`
- `mod 1`: `b += Trans >> 3`
- `mod 2`: `b -= Trans << 2`
- `mod 3`: `b += Trans >> 5`

where `Trans = pKeyWord[rst*2+1]`, `rst = pos%256`, `pos` starts at `KeyWord`.

**Checksum**: `Sum2` (sum of received bytes 4..Size) minus `Sum1` (sum after
deobfuscation) must equal `CheckSum`; mismatch returns the packet anyway but
signals an error (CPSock.cpp:455-459).

**Size enforcement**: `Size < sizeof(HEADER)` or `Size > MAX_MESSAGE_SIZE` (8192,
CPSock.h:38) rejects the frame (CPSock.cpp:397-406). `BASE_CheckPacket` (which
would enforce `m->Size == sizeof(MSG_DropItem)`, Basedef.cpp:6518) is **DISABLED
for the live path** — it is only referenced on the *send* side and only under
`#ifdef _PACKET_DEBUG` (CPSock.cpp:544-552). Consequently the inbound
`MSG_DropItem` struct size is **not validated** against the actual packet, and
the handler reads `m->SourType/SourPos/...` directly off `pMsg`.

---

## 3. Binary Layout

Packing context: `MSG_DropItem` is declared at Basedef.h:1639, **after** the
`#pragma pack(push,1)` region at 1465-1492 and **before** the next region at
2063-2097. Therefore it uses the default MSVC packing `/Zp8`. Largest member
alignment = 4 (int / unsigned int in `_MSG`), so the struct alignment is 4.

### 3.1 Header (`_MSG` macro, Basedef.h:925-930)

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 2 | short | `Size` |
| 2 | 1 | char | `KeyWord` |
| 3 | 1 | char | `CheckSum` |
| 4 | 2 | short | `Type` |
| 6 | 2 | short | `ID` |
| 8 | 4 | unsigned int | `ClientTick` |

`sizeof(_MSG)` = 12 bytes (offsets 0-11, no padding; next int member lands on
offset 12 which is 4-aligned).

### 3.2 Payload

| Offset | Size | Type | Field |
|---|---|---|---|
| 12 | 4 | int | `SourType` |
| 16 | 4 | int | `SourPos` |
| 20 | 4 | int | `Rotate` |
| 24 | 2 | unsigned short | `GridX` |
| 26 | 2 | unsigned short | `GridY` |
| 28 | 2 | unsigned short | `ItemID` |

Payload size = 18 bytes (offsets 12..29).

### 3.3 Nested struct expansions

`MSG_DropItem` contains no nested structs — only the flat `_MSG` macro and scalar
fields. `STRUCT_ITEM` is **not** a member of this packet (the item is located by
`SourType`/`SourPos` via `GetItemPointer` server-side, not carried in the wire
message). For reference, `sizeof(STRUCT_ITEM)` = 8 (Basedef.h:398: short sIndex
2 + union[3] of short 2 each).

### 3.4 Size verification

Math (default /Zp8, alignment 4):

| Field | Offset | Size | End |
|---|---|---|---|
| `_MSG` (Size..ClientTick) | 0 | 12 | 11 |
| `SourType` | 12 | 4 | 15 |
| `SourPos` | 16 | 4 | 19 |
| `Rotate` | 20 | 4 | 23 |
| `GridX` | 24 | 2 | 25 |
| `GridY` | 26 | 2 | 27 |
| `ItemID` | 28 | 2 | 29 |
| **raw total** | | **30** | |
| pad to alignment 4 | 30 | 2 | 31 |
| **`sizeof(MSG_DropItem)`** | | **32** | |

`sizeof` checks: `Basedef.cpp:6518` (`m->Size != sizeof(MSG_DropItem)` → code=1)
would expect `Size == 32`, but as noted it is DISABLED. `ItemID` (offset 28) is a
declared member that the handler **never reads** — the item identity is taken
from `SrcItem->sIndex` instead → effectively **unused / redundant** field.

---

## 4. Lifecycle & Flow

**Inbound (client → TMSrv):**

```
Client
  │  MSG_DropItem (0x272, 32B)  — obfuscated frame
  ▼
CPSock::ReadMessage (CPSock.cpp)          deobfuscate + checksum + Size gate
  ▼
Server.cpp WSA_READ → ProcessClientMessage(conn, pMsg, FALSE)
  ▼
ProcessClientMessage.cpp:38-66  dispatcher guards
    • ID in [0, MAX_USER)         (line 42)
    • ServerDown < 120            (line 53)
    • LastReceiveTime heartbeat   (line 56-57)
    • Type != _MSG_Ping           (line 59)
    • isServer==FALSE && ClientTick==SKIPCHECKTICK → drop  (line 63)
  ▼
switch(std->Type)  — case _MSG_DropItem:   (ProcessClientMessage.cpp:198-200)
  ▼
Exec_MSG_DropItem(conn, pMsg)             (_MSG_DropItem.cpp:20)
  ▼  (see §5 guards) -> GetEmptyItemGrid + GetItemPointer
  ▼
CreateItem(x, y, SrcItem, Rotate, 1)      (Server.cpp:7254)
  • GetEmptyItem() → pItem[empty] slot
  • pItemGrid[y][x] = empty; fill ITEM/Pos/Rotate/State/Delay/Decay/Height
  • build MSG_CreateItem (0x172) → GridMulticast(x, y, &sm, 0)   (Server.cpp:7314)
  ▼
memset(SrcItem, 0, sizeof(STRUCT_ITEM))   (zero the source slot, _MSG_DropItem.cpp:125)
  ▼
build MSG_CNFDropItem (0x175) → pUser[conn].cSock.SendOneMessage (self, _MSG_DropItem.cpp:127-139)
```

**Outbound:**

```
MSG_CreateItem (0x172)  -> GridMulticast to all in radius (area broadcast)  [Server.cpp:7314]
MSG_CNFDropItem (0x175) -> SendOneMessage to the dropping player only       [_MSG_DropItem.cpp:139]
```

Note on the embedded reference: the spec listed `MSG_UpdateCarry` to self, but
the **actual** code confirms the self-confirmation is `MSG_CNFDropItem`
(0x175), **not** `MSG_UpdateCarry`. `MSG_UpdateCarry` (0x185, Basedef.h:1455) is
a different GAME2CLIENT full-inventory resync and is **not** emitted here.

No `default:` case exists in the dispatcher switch — the packet is matched only
by `case _MSG_DropItem` (ProcessClientMessage.cpp:198). No DBSrv involvement: the
drop path never touches `ProcessDBMessage`/DBSrv; the item is removed only from
in-memory `pItem`/carry state.

---

## 5. Validation & Guards

Executed in order inside `Exec_MSG_DropItem` (_MSG_DropItem.cpp):

| # | Guard | Condition | Action on fail | Line |
|---|---|---|---|---|
| 1 | Alive / mode | `pMob[conn].MOB.CurrentScore.Hp <= 0` OR `pUser[conn].Mode != USER_PLAY` | `AddCrackError(conn,1,14)` + `SendHpMode(conn)` + return | 24-29 |
| 2 | Trade active | `pUser[conn].Trade.OpponentID` | `RemoveTrade(opp)` + `RemoveTrade(conn)` + return | 31-36 |
| 3 | Auto-trade | `pUser[conn].TradeMode` | `SendClientMessage(_NN_CantWhenAutoTrade)` + return | 38-42 |
| 4 | Grid bounds | `m->GridX >= MAX_GRIDX (4096)` OR `m->GridY >= MAX_GRIDY (4096)` | log `"err,wrong drop pos %d %d"` + return | 44-49 |
| 5 | Feature toggle | `isDropItem == 0` | silent return (drops globally disabled, Server.cpp:376) | 51-52 |
| 6 | Empty target cell | `GetEmptyItemGrid(&gridx,&gridy)` returns 0 (cell + 3×3 blocked) | `SendClientMessage(_NN_Cant_Drop_Here)` + return | 57-66 |
| 7 | Source is equip | `m->SourType == ITEM_PLACE_EQUIP (0)` | log `"err,dropitem - sourtype"` + return | 68-72 |
| 8 | Source carry bounds | `SourType == ITEM_PLACE_CARRY (1)` AND (`SourPos<0` OR `SourPos >= pMob[conn].MaxCarry`) | log `"err,dropitem - carry equip"` + return | 74-83 |
| 9 | Source cargo bounds | `SourType != ITEM_PLACE_CARGO (2)` → log `"err,dropitem - sourtype"`; else `SourPos<0 OR >= MAX_CARGO(128)` → log `"err,dropitem - sourpos cargo"` + return | 84-97 |
| 10 | Item resolve | `GetItemPointer(...)` returns NULL (either of two calls) | silent return | 99-104 |
| 11 | Item index valid | `SrcItem->sIndex <= 0` OR `>= MAX_ITEMLIST (6500)` | silent return | 106-107 |
| 12 | Non-droppable list | `sIndex ∈ {508,509,522,446,747,3993,3994} ∪ [526..537]` | `SendClientMessage(_NN_Guild_Medal_Cant_Be_Droped)` + return (guild medals/emblems) | 109-110, 143 |
| 13 | World item create | `CreateItem(...)` returns `<=0` OR `>= MAX_ITEM (5000)` | `SendClientMessage("Can't create object(item)")` + return | 112-118 |

Notes:
- Guards 8-9 combined with `GetItemPointer` (Basedef.cpp:2340) which independently
  bounds-checks `MAX_CARRY(64)/MAX_CARGO(128)/MAX_EQUIP(16)` and rejects
  `sIndex` out of `[0,MAX_ITEMLIST)`.
- Guard 12 is the only anti-drop restriction — a hard-coded whitelist of
  guild-medal/emblem item IDs. There is **no** generic bound/non-tradeable or
  coin special-casing in this handler.
- Anti-cheat: only `AddCrackError(conn,1,14)` on the Hp/mode guard. No distance,
  cooldown, or movement checks.
- Guard 13's `drop >= MAX_ITEM` branch is effectively dead — `CreateItem`
  returns `TRUE(1)`/`FALSE(0)` (Server.cpp:7316), never a value ≥ 5000.

---

## 6. Game Mechanics & Business Logic

**How the drop works** (follow the calls):

1. The client sends `SourType`/`SourPos` identifying the source slot and a
   requested `GridX`/`GridY` plus `Rotate`.
2. `GetEmptyItemGrid(&gridx,&gridy)` (GetFunc.cpp:1876) first tests the requested
   cell; if occupied or on an unspawnable tile (`pHeightGrid == 127`), it scans
   the 3×3 neighborhood and relocates `gridx/gridy` to the first free cell.
   Bounds are checked in the loop; failure returns FALSE → "Can't drop here".
3. `GetItemPointer` (Basedef.cpp:2340) resolves the source `STRUCT_ITEM` from
   `pMob[conn].MOB.Carry` (CARRY) or `pUser[conn].Cargo` (CARGO). Equip is
   rejected outright (guard 7) — equipment cannot be dropped.
4. `CreateItem(x, y, SrcItem, Rotate, 1)` (Server.cpp:7254):
   - re-validates `sIndex`, re-runs `GetEmptyItemGrid`,
   - fails if `pItemGrid[y][x]` occupied,
   - allocates `pItem[empty] = GetEmptyItem()` (Server.cpp:6754, first free
     `Mode==0` slot in `[1,MAX_ITEM)`),
   - sets `Mode=1`, `PosX/Y`, `memcpy`s the `STRUCT_ITEM`, `Rotate`,
     `State=STATE_OPEN`, `Delay=90`, `Decay=0`,
     `GridCharge = BASE_GetItemAbility(item, EF_GROUND)` (Server.cpp:7285),
   - records `pItemGrid[y][x] = empty`, `Height = pHeightGrid[y][x]`,
   - broadcasts `MSG_CreateItem` (`ItemID = empty + 10000`, `ID = ESCENE_FIELD`,
     `GridMulticast`).
5. On success the source slot is zeroed (`memset(SrcItem,0,8)`, line 125) — the
   item is removed from carry/cargo in memory.
6. A `MSG_CNFDropItem` confirmation is sent to the dropping player.

**Business rules observed:**
- Drops only permitted while alive and in `USER_PLAY` mode.
- Active trade / auto-trade blocks dropping (trade cancelled first).
- Cannot drop equipment (`ITEM_PLACE_EQUIP`).
- Guild medals/emblems (IDs 508,509,522,526-537,446,747,3993,3994) cannot be
  dropped — special hard-coded list.
- Coin items are not special-cased: they drop as ordinary `STRUCT_ITEM`s.
- Global switch `isDropItem` (default 0, set from server config at
  Server.cpp:1109) can disable drops entirely.

---

## 7. Side Effects

**State mutation (server memory):**
- `SrcItem` slot (carry/cargo) zeroed via `memset(SrcItem, 0, sizeof(STRUCT_ITEM))`
  (_MSG_DropItem.cpp:125).
- `pItem[empty]` initialized (Mode=1, PosX/Y, ITEM copy, Rotate, State=STATE_OPEN,
  Delay=90, Decay=0, GridCharge, Height) (Server.cpp:7272-7290).
- `pItemGrid[y][x] = empty` (Server.cpp:7287).

**Outgoing packets:**
- `MSG_CreateItem` (0x172) — `GridMulticast(x,y,&sm,0)` to all clients in radius
  (Server.cpp:7314). Fills Type/Size/ID=ESCENE_FIELD/ItemID=empty+10000/Item
  copy/GridX/GridY/Rotate/State (Server.cpp:7291-7314).
- `MSG_CNFDropItem` (0x175) — `SendOneMessage` to self only, fields
  SourType/SourPos/Rotate/GridX/GridY (based on the possibly-relocated grid)
  (_MSG_DropItem.cpp:127-139).
- `SendHpMode(conn)` on guard-1 failure.

**Logs (with format strings):**
- `"err,wrong drop pos %d %d"` (GridX, GridY) — AccountName/IP (_MSG_DropItem.cpp:46-47)
- `"err,dropitem - sourtype"` — equip source / bad type (lines 70, 88)
- `"err,dropitem - carry equip"` — carry slot OOB (line 80)
- `"err,dropitem - sourpos cargo"` — cargo slot OOB (line 93)
- `"dropitem, %s"` — `BASE_GetItemCode(SrcItem, tmplog)` item descriptor via
  `ItemLog` (lines 120-123)

**Client messages (strings):**
- `_NN_CantWhenAutoTrade` (line 40), `_NN_Cant_Drop_Here` (line 64),
  `_NN_Guild_Medal_Cant_Be_Droped` (line 143), literal `"Can't create object(item)"`
  (line 116).

---

## 8. Related Packets

| Packet | Type | Direction | Relation |
|---|---|---|---|
| `_MSG_CreateItem` / `MSG_CreateItem` | 0x172 (110\|0x200) | GAME→clients | emitted to area on successful drop (Server.cpp:7314) |
| `_MSG_CNFDropItem` / `MSG_CNFDropItem` | 0x175 (117\|0x100) | GAME→client | self-confirmation (Basedef.h:1850; _MSG_DropItem.cpp:139) |
| `_MSG_GetItem` / `MSG_GetItem` | 0x172 = 112\|0x200 | client→GAME | inverse — picking the dropped item up (dispatched ProcessClientMessage.cpp:202) |
| `_MSG_UpdateCarry` / `MSG_UpdateCarry` | 0x185 (133\|0x100) | GAME→client | full carry resync; NOT used by drop handler (Basedef.h:1455) |
| `_MSG_DecayItem` | 0x171 | GAME→client | ground-item decay lifecycle of dropped items (Basedef.h:1609) |

`sizeof(MSG_CNFDropItem)` = 28 (12 header + 3×int + 2×ushort, already 4-aligned;
enforced at Basedef.cpp:6521). `sizeof(MSG_CreateItem)` = 32.

---

## 9. Discrepancies & Open Questions

1. **`MSG_UpdateCarry` vs `MSG_CNFDropItem`** — the embedded reference stated the
   self-response is `MSG_UpdateCarry`; source proves it is `MSG_CNFDropItem`
   (0x175) via `SendOneMessage` (_MSG_DropItem.cpp:139). `MSG_UpdateCarry` is not
   sent here.
2. **`ItemID` field unused** — `MSG_DropItem.ItemID` (Basedef.h:1647, offset 28)
   is never read by the handler; item identity comes from `SrcItem->sIndex`.
3. **Dead check `drop >= MAX_ITEM`** — `CreateItem` returns `TRUE/FALSE`, so the
   `>= MAX_ITEM (5000)` branch (line 114) is unreachable. On `CreateItem` failure
   the client gets `"Can't create object(item)"` even though several internal
   CreateItem failures (occupied grid, no empty slot) are mundane, not errors.
4. **`BackupItem` redundant** — `SrcItem` and `BackupItem` are both assigned
   `GetItemPointer(...)` of the same slot (lines 99-100); `BackupItem` is only
   NULL-checked and never otherwise used.
5. **`isDropItem` silent return** — when drops are disabled globally (isDropItem==0)
   the handler returns with no client feedback (line 51-52).
6. **No generic bound/non-tradeable check** — only the hard-coded guild-medal
   whitelist (guard 12) blocks drops; bound/non-tradeable and coin items are not
   otherwise restricted here.
7. **`BASE_CheckPacket` disabled** — inbound `Size` is not validated against
   `sizeof(MSG_DropItem)` at runtime (CPSock.cpp:544 `#ifdef _PACKET_DEBUG` send-only),
   so a malformed/truncated 0x272 packet would still reach the handler which reads
   fixed offsets off `pMsg` (potential read past buffer if frame is undersized).
8. **`GetEmptyItemGrid` initial probe unguarded** — the handler guards
   `GridX/Y < MAX_GRIDX/Y` (line 44) but does not guard the lower bound; the
   fields are unsigned so this is safe, yet the pre-loop `pItemGrid[*gridy][*gridx]`
   access relies entirely on that caller check.

---

## 10. Source References

| File | Line(s) | Content |
|---|---|---|
| legacy/Code/Basedef.h | 1638-1648 | `_MSG_DropItem` const + `MSG_DropItem` struct |
| legacy/Code/Basedef.h | 925-930 | `_MSG` header macro |
| legacy/Code/Basedef.h | 932-941 | flags (CLIENT2GAME 0x200) |
| legacy/Code/Basedef.h | 75-77, 86-88, 100-101, 103, 139 | MAX_EQUIP/CARRY/CARGO, ITEM_PLACE_*, MAX_GRIDX/Y, MAX_ITEM, MAX_ITEMLIST |
| legacy/Code/Basedef.h | 1850-1859 | `MSG_CNFDropItem` |
| legacy/Code/Basedef.h | 1592-1607 | `MSG_CreateItem` |
| legacy/Code/Basedef.h | 1455-1463 | `MSG_UpdateCarry` |
| legacy/Code/Basedef.h | 398-412 | `STRUCT_ITEM` |
| legacy/Code/Basedef.cpp | 6518, 6521 | size checks (disabled) |
| legacy/Code/Basedef.cpp | 2340-2372 | `GetItemPointer` |
| legacy/Code/TMSrv/_MSG_DropItem.cpp | 20-146 | `Exec_MSG_DropItem` (primary handler) |
| legacy/Code/TMSrv/ProcessClientMessage.cpp | 38-66, 198-200 | dispatcher guards + `case _MSG_DropItem` |
| legacy/Code/TMSrv/ProcessClientMessage.h | 103 | handler decl |
| legacy/Code/TMSrv/Server.cpp | 7254-7317 | `CreateItem` |
| legacy/Code/TMSrv/Server.cpp | 6754-6763 | `GetEmptyItem` |
| legacy/Code/TMSrv/Server.cpp | 376, 1109 | `isDropItem` |
| legacy/Code/TMSrv/GetFunc.cpp | 1876-1899 | `GetEmptyItemGrid` |
| legacy/Code/TMSrv/SendFunc.cpp | 320-327 | `SendCreateItem` (related) |
| legacy/Code/CPSock.cpp | 29, 385-459, 525-584 | framing, obfuscation, checksum, SendOneMessage |
| legacy/Code/CPSock.h | 38, 40, 42-50 | MAX_MESSAGE_SIZE, INITCODE, HEADER |
