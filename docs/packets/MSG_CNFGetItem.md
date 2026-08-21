# MSG_CNFGetItem

## 1. Summary

| Property | Value |
|---|---|
| Wire type constant | `_MSG_CNFGetItem` = `(113 \| FLAG_GAME2CLIENT)` = `113 \| 0x0100` = `0x0171` (369) |
| Declaration | `Basedef.h:1629` (const), `Basedef.h:1630-1636` (struct) |
| Direction | TMSrv (game) → client. **GAME2CLIENT only** — no client→game inbound leg. |
| Purpose | Confirmation sent to the client after a ground item is successfully picked up (the `MSG_GetItem` response), carrying the item and its destination slot. |
| Producer | `TMSrv/_MSG_GetItem.cpp:234-244` (`Exec_MSG_GetItem`, pickup-success path) |
| Inbound dispatcher | **None.** `ProcessClientMessage.cpp` has `case _MSG_GetItem` (the *request*) at `:202-204`, but no `case _MSG_CNFGetItem` (verified). |
| Struct size (`sizeof`) | 28 bytes |
| Header size | 12 bytes (`_MSG` / `HEADER`) |
| Payload size | 16 bytes |
| Producer-sent `Size` | `sizeof(MSG_CNFGetItem)` = 28 (`_MSG_GetItem.cpp:238`) |
| Alias | none (unique constant) |
| Flags mask | GAME2CLIENT `0x0100` — see `Basedef.h:932` |

## 2. Wire Framing (protocol preamble)

Global framing lives in `CPSock.cpp`/`CPSock.h`; `MSG_CNFGetItem` does not deviate from it.

- **Session init magic:** on connection the server first sends `INITCODE = 0x1F11F311` as a raw 4-byte value (`CPSock.cpp:249-250`; defined `CPSock.h:40`). The peer authenticates this before any message (`CPSock.cpp:366-383`).
- **Message envelope** (`HEADER`, `CPSock.h:42-50` — identical to `_MSG`, `Basedef.h:925-930`), 12 bytes:
  - `Size` short @0, `KeyWord` char @2, `CheckSum` char @3, `Type` short @4, `ID` short @6, `ClientTick` uint @8.
- **Size bounds:** `Size` must satisfy `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE` (= `8192`, `CPSock.h:38`) — `CPSock.cpp:397`.
- **KeyWord / obfuscation:** `AddMessage` picks `iKeyWord = rand()%256`, `KeyWord = pKeyWord[iKeyWord*2]` (`CPSock.cpp:535-536`). Payload bytes from offset **4** are obfuscated per-byte, XOR-style keyed by `KeyWord`, with byte-index-dependent transforms (`CPSock.cpp:558-581`, receive inverse `CPSock.cpp:430-453`):
  - `i&3==0`: `+ (Trans<<1)`; `i&3==1`: `- (Trans>>3)`; `i&3==2`: `+ (Trans<<2)`; `i&3==3`: `- (Trans>>5)`, where `Trans = pKeyWord[(KeyWord+i)%256*2+1]`.
  - First 4 bytes (`Size`, `KeyWord`, `CheckSum`) are copied verbatim after computing `CheckSum` (`CPSock.cpp:586`).
- **CheckSum:** `CheckSum = Sum2 - Sum1`, where `Sum1` = sum of plaintext payload bytes (offset 4..Size), `Sum2` = sum of obfuscated payload bytes (`CPSock.cpp:583-584`; verified on receive `CPSock.cpp:425-455`).
- **ClientTick** = `CurrentTime` (`CPSock.cpp:541`).
- **BASE_CheckPacket:** compiled out — its body is entirely commented out (`Basedef.cpp:6475-6476`), so the `_MSG_CNFGetItem` size check at `Basedef.cpp:6517` is inert. (Only active under `_PACKET_DEBUG`, `CPSock.cpp:544-552`.)

No per-packet framing deviation: `MSG_CNFGetItem` uses the standard preamble exactly.

## 3. Binary Layout

### Packing context

`MSG_CNFGetItem` is declared at `Basedef.h:1630`, which is **outside** every `#pragma pack(push,1)` region. The pack(1) regions in `Basedef.h` are `808-835`, `1212-1246`, `1465-1492`, `2063-2097`. Offset 1629-1636 falls after `1492` and before `2063`, so it compiles under the **MSVC default `/Zp8`**. Nested `STRUCT_ITEM` (`Basedef.h:398-412`) is likewise at `/Zp8`. Little-endian x86, LP32.

### 3.1 Header (12 bytes, from `_MSG`, Basedef.h:925-930)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 2 | `Size` | short | total packet size incl. header = 28 |
| 2 | 1 | `KeyWord` | char | index into `pKeyWord` (obfuscation) |
| 3 | 1 | `CheckSum` | char | `Sum2 - Sum1` |
| 4 | 2 | `Type` | short | `_MSG_CNFGetItem` = 0x0171 |
| 6 | 2 | `ID` | short | `ESCENE_FIELD` = 30000 (`Basedef.h:170`) |
| 8 | 4 | `ClientTick` | unsigned int | `CurrentTime` |

### 3.2 Payload (16 bytes, struct `MSG_CNFGetItem`, Basedef.h:1630-1636)

| Offset | Size | Field | Type | Producer value |
|---|---|---|---|---|
| 12 | 4 | `DestType` | int | `m->DestType` — must be `ITEM_PLACE_CARRY` (=1, `Basedef.h:87`) |
| 16 | 4 | `DestPos` | int | `m->DestPos` — destination slot in `MOB.Carry[]` |
| 20 | 8 | `Item` | STRUCT_ITEM | **left zeroed by producer (see §9)** |

### 3.3 Nested struct expansions

`STRUCT_ITEM` (`Basedef.h:398-412`), all 2-byte members → no padding, **8 bytes**:

| Offset (rel. struct) | Size | Field | Type |
|---|---|---|---|
| 0 | 2 | `sIndex` | short |
| 2 | 2 | `stEffect[0]` (union `sValue` / `{cEffect,cValue}`) | short |
| 4 | 2 | `stEffect[1]` | short |
| 6 | 2 | `stEffect[2]` | short |

The union is 2 bytes: `short sValue` overlays `{unsigned char cEffect; unsigned char cValue;}`.

### 3.4 Size verification (math)

Alignment under `/Zp8` = `min(member size, 8)`; alignment of `STRUCT_ITEM` = largest member (short) = 2.

```
offset  field          size  running  padding
0       Size (short)    2     2
2       KeyWord (char)  1     3
3       CheckSum (char) 1     4
4       Type (short)    2     6
6       ID (short)      2     8
8       ClientTick (uint) 4   12
12      DestType (int)  4     16
16      DestPos (int)   4     20
20      Item (STRUCT_ITEM) 8  28
                                    0 padding bytes
sizeof(MSG_CNFGetItem) = 28
```

- Producer cross-check: `cnfGet.Size = sizeof(MSG_CNFGetItem)` (`_MSG_GetItem.cpp:238`) and `AddMessage((char*)&cnfGet, sizeof(MSG_CNFGetItem))` (`:244`) → **28, matches, no mismatch.**
- `sizeof(STRUCT_ITEM) = 8` — used as the `memcpy` length in `_MSG_GetItem.cpp:226` and `SendFunc.cpp:1079`.
- **No UNKNOWN members** in this struct (all fields named and populated — see §9 for the one dead field).

## 4. Lifecycle & Flow

```
 client                  TMSrv (game)                        ground grid
   │ MSG_GetItem (0x112, CLIENT2GAME)                          │
   │──────────────────────► ProcessClientMessage.cpp:202       │
   │                    Exec_MSG_GetItem (_MSG_GetItem.cpp)     │
   │                      • guards (HP, trade, mode, desttype)  │
   │                      • locate pItem[itemID] (Mode)         │
   │                      • range check (3-cell)                │
   │                      • coin branch OR slot-write branch    │
   │                      • memcpy bItem←ditem (:226)           │
   │ ◄─────────────────── MSG_CNFGetItem (0x171) AddMessage:244 │
   │ ◄─────────────────── MSG_DecayItem  AddMessage:199/GridMulti:246
   │                      pItemGrid[Y][X]=0; pItem[].Mode=0 (248-249)
   │ ◄─────────────────── SendItem → MSG_SendItem (0x130) :251  │
```

- **Producer:** `Exec_MSG_GetItem` in `TMSrv/_MSG_GetItem.cpp:20`. The confirmation is built at `:234-244`:
  1. `MSG_CNFGetItem cnfGet; memset(&cnfGet,0,sizeof)` (`:234-235`)
  2. set `Type = _MSG_CNFGetItem`, `Size = sizeof`, `ID = ESCENE_FIELD` (`:237-239`)
  3. set `DestPos = m->DestPos`, `DestType = m->DestType` (`:241-242`)
  4. send via `pUser[conn].cSock.AddMessage((char*)&cnfGet, sizeof)` (`:244`) — **direct queue to the single client socket** (not a multicast; only the picker sees it).
- **Send path:** `AddMessage` (`CPSock.cpp:513-591`) frames + obfuscates into `pSendBuffer`, later flushed by `SendMessageA` (`CPSock.cpp:617+`).
- **Companion sends on the same pickup:** `MSG_DecayItem` (removes the ground sprite) to the picker (`:199` coin path / `:159,166,176` early-outs) and via `GridMulticast(itemX,itemY,…)` (`:246`) to nearby players; `SendItem(...)` → `MSG_SendItem` (`:251`) refreshes the carry slot on the client.
- **No client→game leg:** there is no inbound dispatcher case for `_MSG_CNFGetItem`. The GAME2CLIENT flag (0x0100) makes it impossible for the game's receive loop to route it (client messages carry CLIENT2GAME 0x0200). `ProcessClientMessage.cpp` only dispatches `_MSG_GetItem` (`:202-204`). `ProcessDBMessage.cpp` has no reference.

## 5. Validation & Guards

The producer validates *before* emitting `MSG_CNFGetItem`:

| # | Guard | Location | Behavior on failure |
|---|---|---|---|
| 1 | Player alive & `Mode == USER_PLAY` | `_MSG_GetItem.cpp:24` | `AddCrackError` + `SendHpMode`, return |
| 2 | Not trading (no opponent) | `:31` | `RemoveTrade`, return |
| 3 | Not in auto-trade mode | `:38` | send "can't when auto trade", return |
| 4 | `m->DestType == ITEM_PLACE_CARRY` | `:44` | log "wrong desttype", return |
| 5 | `itemID` in `[1, MAX_ITEM)` | `:52` | return |
| 6 | `pItem[itemID].Mode != 0` (item exists) | `:55` | `MSG_DecayItem` to picker, return |
| 7 | Target within 3 cells of `TargetX/Y` | `:74` | log "GetItemFail posx", return |
| 8 | Quest-pill / other item special cases | `:84,94,129` | various returns |
| 9 | Coin cap (`tcoin >= 2,000,000,000`) | `:192` | message, return |
| 10 | `0 <= m->DestPos < MAX_CARRY` (64) | `:206` | log "Trading Fails", return |
| 11 | Destination carry slot empty (`sIndex==0`) | `:215` | if valid slot, re-send it via `SendItem` and return (item NOT granted) |

Wire-level `Size`/`CheckSum`/bounds validation happens in `CPSock::ReadMessage` (`CPSock.cpp:390-467`); `BASE_CheckPacket` is disabled (`Basedef.cpp:6475`).

## 6. Game Mechanics & Business Logic

- `MSG_CNFGetItem` is the **server→client ack** for a successful ground-item pickup (the reply to `MSG_GetItem`, 0x112).
- **What it carries:** `DestType` (inventory class — always `ITEM_PLACE_CARRY` = 1 for ground pickup) and `DestPos` (the target slot index in the player's `MOB.Carry[64]` array, `Basedef.h:76`). Field semantics per `Basedef.h:1633-1634` and the producer (`_MSG_GetItem.cpp:241-242`).
- **The `Item` field** is declared (`Basedef.h:1635`) but the producer never populates it — the struct is `memset` to 0 and only Type/Size/ID/DestPos/DestType are set (see §9).
- **Slot-write branch (`:204-232`):** only writes to the destination if that carry slot is empty (`bItem->sIndex == 0`, `:215`); the ground item is copied with `memcpy(bItem, ditem, sizeof(STRUCT_ITEM))` (`:226`), an audit line `"getitem, <code>"` is written via `ItemLog` (`:228-231`).
- **Coin branch (`:182-202`):** volatile/hwordcoin/lwordcoin items are summed into `pMob[conn].MOB.Coin` (capped at 2,000,000,000) instead of going into a carry slot; the confirmation is still emitted afterward (`:234-244`) with the original `DestPos/DestType`.
- **Ground-cleanup side effect:** after confirming, `pItemGrid[itemY][itemX] = 0` and `pItem[itemID].Mode = 0` (`:248-249`), and the ground sprite is removed via `MSG_DecayItem` (`:246`).

## 7. Side Effects

1. **State mutation:** picked item removed from world grid (`pItemGrid[itemY][itemX]=0`, `:248`) and `pItem[itemID].Mode=0` (`:249`).
2. **Player inventory mutation:** item `memcpy` into `MOB.Carry[m->DestPos]` (`:226`); coins added for the coin branch (`:197`).
3. **Outgoing packets to the picker:** `MSG_CNFGetItem` (`:244`), `MSG_DecayItem` (`:199` coin path; `:246` multicast), `MSG_SendItem` via `SendItem` (`:251`) and the slot-collision path (`:222`).
4. **Outgoing packets to nearby players:** `MSG_DecayItem` via `GridMulticast` (`:246`), removing the ground sprite for all.
5. **Audit log:** `ItemLog("getitem, …")` (`:230-231`).
6. **No DB round-trip** — `MSG_CNFGetItem` never touches `ProcessDBMessage.cpp` / DBSrv; it is purely a game→client notification.

## 8. Related Packets

| Packet | Type | Direction | Relation |
|---|---|---|---|
| `MSG_GetItem` | `0x112` (`Basedef.h:1618`) | CLIENT2GAME | The request that triggers this confirmation; dispatched at `ProcessClientMessage.cpp:202`. |
| `MSG_DecayItem` | `0x111` (`Basedef.h:1609`) | GAME2CLIENT | Sent alongside to remove the ground sprite (`_MSG_GetItem.cpp:246`). |
| `MSG_SendItem` | `0x130` (`Basedef.h:1672`) | GAME2CLIENT | `SendItem` refresh of the carry slot after pickup (`_MSG_GetItem.cpp:251`). |
| `MSG_CNFDropItem` | `0x115` (`Basedef.h:1638` area, DropItem family) | GAME2CLIENT | Inverse operation (drop confirmation). |
| `MSG_CreateItem` | (`Basedef.h:6514` size check) | GAME2CLIENT | Spawns ground items that this pickup later consumes. |
| `MSG_UpdateCarry` / `SendCarry` | — | GAME2CLIENT | Bulk inventory refresh complementing the single-slot `MSG_SendItem`. |

## 9. Discrepancies & Open Questions

1. **Dead `Item` field (confirmed):** The producer `memset`s `cnfGet` and never assigns `cnfGet.Item` (`_MSG_GetItem.cpp:234-244`). On the wire, `Item` (offset 20-27) is always all-zero for this producer. The client therefore cannot learn the item from `MSG_CNFGetItem`; it gets the real item through the separate `MSG_SendItem` (`:251`). The field's presence is vestigial/for client-side display assumptions. **Open:** whether the client actually reads `Item`; if so, this is a latent bug (client would see a zeroed item).
2. **Confirmation sent on the coin path with a slot index:** when the ground item is volatile/coin (`:182-202`), the confirmation still carries the original `DestPos`/`DestType` even though no carry slot is written — the client may interpret this as an inventory write that did not occur. **Open:** client handling of coin pickups vs. slot pickups.
3. **Slot-collision returns without confirmation:** when the destination slot is occupied (`:215-224`), no `MSG_CNFGetItem` is sent — the client only receives `MSG_SendItem` for the existing slot. A client expecting an ack may stall.
4. `BASE_CheckPacket` size guard for this type is defined but dead (`Basedef.cpp:6517`, whole function commented out at `:6476`).

## 10. Source References

- `legacy/Code/Basedef.h:87` — `ITEM_PLACE_CARRY = 1`
- `legacy/Code/Basedef.h:170` — `ESCENE_FIELD = 30000`
- `legacy/Code/Basedef.h:398-412` — `STRUCT_ITEM`
- `legacy/Code/Basedef.h:925-930` — `_MSG` macro (header)
- `legacy/Code/Basedef.h:1618-1627` — `MSG_GetItem`
- `legacy/Code/Basedef.h:1629-1636` — `_MSG_CNFGetItem` / `MSG_CNFGetItem`
- `legacy/Code/Basedef.cpp:6475-6476, 6517` — `BASE_CheckPacket` (disabled) + size check
- `legacy/Code/CPSock.h:38,40,42-50` — `MAX_MESSAGE_SIZE`, `INITCODE`, `HEADER`
- `legacy/Code/CPSock.cpp:249-250,366-383,390-467,513-591` — framing/obfuscation/`AddMessage`
- `legacy/Code/TMSrv/_MSG_GetItem.cpp:20-252` — producer `Exec_MSG_GetItem`; confirmation at `:234-244`
- `legacy/Code/TMSrv/ProcessClientMessage.cpp:202-204` — `case _MSG_GetItem` (only inbound leg; no `_MSG_CNFGetItem`)
- `legacy/Code/TMSrv/SendFunc.cpp:1055-1082` — `SendItem` → `MSG_SendItem`
