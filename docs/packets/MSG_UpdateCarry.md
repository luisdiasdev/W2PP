# MSG_UpdateCarry

## 1. Summary

| Attribute | Value |
|---|---|
| Packet name | `MSG_UpdateCarry` |
| Type constant | `_MSG_UpdateCarry` |
| Hex value | `0x0185` (`133 \| FLAG_GAME2CLIENT` = `0x85 \| 0x100`) |
| Declared at | `legacy/Code/Basedef.h:1455-1463` |
| Flags | `FLAG_GAME2CLIENT` only (`0x0100`) |
| Direction | Outbound TMSrv → client (GAME2CLIENT only) |
| Inbound dispatcher case | **None** — no client→game leg (verified: no `_MSG_UpdateCarry` case in `ProcessClientMessage.cpp`; also absent from `GetFunc.cpp`, `ProcessDBMessage.cpp`) |
| Wire struct size | **528 bytes** (header 12 + Carry 512 + Coin 4), alignment 4 |
| Producer | `SendCarry(int conn)` — `TMSrv/SendFunc.cpp:1534` |
| Purpose | Full refresh of the player's carry inventory (`Carry[64]`) and carried `Coin` |
| Transport | Queued via `CPSock::AddMessage` → `CPSock::SendMessageA` (`CPSock.cpp:513`, `617`) |

Type value derivation: `133 | 0x0100 = 0x85 | 0x100 = 0x0185`. Confirmed at `Basedef.h:1455`.

## 2. Wire Framing

Protocol preamble applies verbatim (no per-packet deviations — standard `_MSG` header, standard obfuscation/checksum in `CPSock::AddMessage`):

- **Magic / INITCODE**: `0x1F11F311` — `CPSock.h:40`.
- **Framing**: stream contains a run of back-to-back messages. Each message's true `Size` (total, header included) is read from the first 2 bytes (`HEADER`). Payload bytes from offset 4 onward are **obfuscated per-byte** XOR/arithmetic keyed by `KeyWord`; the first 4 bytes (Size, KeyWord, CheckSum, Type) are sent plaintext (`CPSock.cpp:586` `memcpy(pSendBuffer + nSendPosition, pMsg, 4)`).
- **Obfuscation** (`CPSock.cpp:554-584`): sender selects `iKeyWord = rand()%256`, looks up `KeyWord = pKeyWord[iKeyWord*2]`. For `i` from 4..Size-1, `pos` advances from `KeyWord`; `Trans = pKeyWord[(pos%256)*2 + 1]`; bytes transformed by `mod = i&0x3`: `mod0 += Trans<<1`, `mod1 -= Trans>>3`, `mod2 += Trans<<2`, `mod3 -= Trans>>5`.
- **CheckSum**: `Sum1` = sum of plaintext bytes (offset 4..Size-1); `Sum2` = sum of transformed bytes; `CheckSum = Sum2 - Sum1` (`CPSock.cpp:583`).
- **Size bound**: `Size` in `[sizeof(_MSG), MAX_MESSAGE_SIZE]`; `MAX_MESSAGE_SIZE = 8192` (`CPSock.h:38`). 528 ≤ 8192, so the packet always fits.
- **Validation**: `BASE_CheckPacket` (`Basedef.cpp:6475`) is **disabled** — it is only invoked inside `#ifdef _PACKET_DEBUG` (`CPSock.cpp:544-552`), and not in the default build.

No per-packet deviations. `MSG_UpdateCarry` uses a normal `_MSG` header with `ClientTick` overwritten by `CurrentTime` in `AddMessage` (`CPSock.cpp:541`).

## 3. Binary Layout

`MSG_UpdateCarry` is declared at `Basedef.h:1455-1463`, **before** the `#pragma pack(push, 1)` at line 1465. It is therefore compiled with the MSVC default alignment (**/Zp8**): each member is aligned to `min(alignof(member), 8)`, and the struct size is rounded to the largest member alignment (4 here). Little-endian x86, LP32.

### 3.1 Header

`_MSG` macro (`Basedef.h:925-930`), 12 bytes, no padding (offsets land naturally).

| Offset | Size | Field | C type | Align | Notes |
|---|---|---|---|---|---|
| 0 | 2 | `Size` | `short` | 2 | Total incl. header = 528 |
| 2 | 1 | `KeyWord` | `char` | 1 | Set by `AddMessage` |
| 3 | 1 | `CheckSum` | `char` | 1 | Set by `AddMessage` |
| 4 | 2 | `Type` | `short` | 2 | `_MSG_UpdateCarry` = 0x0185 |
| 6 | 2 | `ID` | `short` | 2 | Client conn index (`sm.ID = conn`) |
| 8 | 4 | `ClientTick` | `unsigned int` | 4 | Overwritten with `CurrentTime` |

### 3.2 Payload

| Offset | Size | Field | C type | Align | Notes |
|---|---|---|---|---|---|
| 12 | 512 | `Carry[0..63]` | `STRUCT_ITEM[64]` | 2 | 64 × 8 bytes; expanded below |
| 524 | 4 | `Coin` | `int` | 4 | Player's carried gold |

### 3.3 Nested struct expansions

`STRUCT_ITEM` (`Basedef.h:398-412`), **8 bytes**, align 2 — no packing pragma applies (not inside any pack(1) region; pack regions are only 808-835, 1212-1246, 1465-1492, 2063-2097). /Zp8.

| Offset | Size | Field | C type | Align |
|---|---|---|---|---|
| 0 | 2 | `sIndex` | `short` | 2 |
| 2 | 2 | `stEffect[0]` | union `{ short sValue; struct { uchar cEffect; uchar cValue; } }` | 2 |
| 4 | 2 | `stEffect[1]` | union (same) | 2 |
| 6 | 2 | `stEffect[2]` | union (same) | 2 |

`2 + 3×2 = 8` bytes per item; `sizeof(STRUCT_ITEM) = 8`, alignment 2.

### 3.4 Size verification

Offsets computed explicitly:

```
Header (_MSG) ............ 12 bytes   (0 .. 11)
Carry[64] @ 12 ........... 12 + 64*8 = 524   (no pad: 12 % 2 == 0)
Coin @ 524 ............... 524 % 4 == 0 -> no pad
struct size .............. 524 + 4 = 528
largest member align ...... 4 -> 528 % 4 == 0 -> sizeof = 528
```

- `sizeof(MSG_UpdateCarry) = 528` bytes, `_Alignof = 4` (confirmed by a /Zp8-equivalent C test).
- `offsetof(Carry) = 12`, `offsetof(Coin) = 524`.
- Producer sets `sm.Size = sizeof(MSG_UpdateCarry)` (`SendFunc.cpp:1551`) and sends `AddMessage(..., sizeof(MSG_UpdateCarry))` (`SendFunc.cpp:1557`) — **consistent**, no mismatch.
- `Coin` (int, align 4) lands at offset 524 with **zero padding** — the 512-byte carry array is a multiple of 4, so no tail padding is introduced before it.

No UNKNOWN members — every field is named and typed.

## 4. Lifecycle & Flow

Outbound only. The packet is built locally in `SendCarry` and queued directly to the client socket — no inbound switch, no DB round trip.

```
Trigger (server-side event mutates MOB.Carry / MOB.Coin)
   │
   ▼
SendCarry(conn)   TMSrv/SendFunc.cpp:1534
   │  guards: conn in [1,MAX_USER), Mode==USER_PLAY, cSock.Sock!=0  (1536-1543)
   │  MSG_UpdateCarry sm; memset 0 (1545-1547)
   │  sm.ID = conn; sm.Type = _MSG_UpdateCarry; sm.Size = sizeof (1549-1551)
   │  memcpy(sm.Carry, pMob[conn].MOB.Carry, sizeof(STRUCT_ITEM)*MAX_CARRY)  (1553)
   │  sm.Coin = pMob[conn].MOB.Coin  (1555)
   ▼
pUser[conn].cSock.AddMessage((char*)&sm, sizeof(MSG_UpdateCarry))  (1557)
   │  CPSock::AddMessage  CPSock.cpp:513  -> queues + obfuscates + checksum
   ▼
CPSock::SendMessageA  CPSock.cpp:617  -> flushes to client
   │  (or batch-flushed with others; SendCarry alone does not force flush)
```

`SendCarry` producers and the events that trigger them:

| Call site | Trigger |
|---|---|
| `TMSrv/_MSG_UseItem.cpp:5676,5682,5702` | Using bag-expansion item (sIndex 3467): adds carry slot 60/61, then `SendCarry` ×2 (once per slot addition, once after recomputing `MaxCarry`) — `_MSG_UseItem.cpp:5664-5704` |
| `TMSrv/_MSG_Trade.cpp:334,335` | Trade accept: copies traded carry arrays + coins for both sides, then `SendCarry(conn)` and `SendCarry(OpponentID)` — `_MSG_Trade.cpp:328-335` |
| `TMSrv/ProcessDBMessage.cpp:1262,1270` | `_MSG_DBCNFCapsuleCharacterFail` / `Fail2`: DB rejected a capsule-character request → refresh carry + notify (`_NN_NoEmptySlot` / `_NN_CANT_USE_ID`) — `ProcessDBMessage.cpp:1260-1273` |
| `TMSrv/_MSG_Quest.cpp:592` | Jeffi quest: subtracts 1,000,000 coin, then `SendCarry` — `_MSG_Quest.cpp:580-592` |
| `TMSrv/_MSG_Quest.cpp:1602` | Enchant quest: clears 7 crystal slots in carry, then `SendCarry` |
| `TMSrv/_MSG_Quest.cpp:1931,1985,2039,2095` | Various quest rewards / enchant completions: add items via `PutItem`, set affect, `SendScore`, `SendCarry` |

**No client→game leg**: there is no `case _MSG_UpdateCarry` / `_MSG_UpdateCarry` reference in `TMSrv/ProcessClientMessage.cpp`, `TMSrv/GetFunc.cpp`, or `TMSrv/ProcessDBMessage.cpp` (verified by grep — no matches). The client never sends this message.

## 5. Validation & Guards

Server-initiated; the only checks are in `SendCarry` (`SendFunc.cpp:1536-1543`):

| # | Guard | Consequence |
|---|---|---|
| 1 | `conn <= 0 \|\| conn >= MAX_USER` | early `return`, no packet |
| 2 | `pUser[conn].Mode != USER_PLAY` | early `return` (not in play state) |
| 3 | `pUser[conn].cSock.Sock == 0` | early `return` (no live socket) |
| 4 | `AddMessage(...)` returns FALSE (send buffer full / invalid sock) | `CloseUser(conn)` (`SendFunc.cpp:1558`) |

No inbound-side validation (packet never received by server). No `Size`/`CheckSum` runtime validation beyond the (disabled) `BASE_CheckPacket`.

## 6. Game Mechanics & Business Logic

- `SendCarry` performs a **full snapshot** of the player's carry inventory and coin: it `memcpy`s all 64 `STRUCT_ITEM` slots from `pMob[conn].MOB.Carry` and copies `pMob[conn].MOB.Coin` (`SendFunc.cpp:1553-1555`). It is a coarse-grained refresh used when many slots change at once (trade, quest rewards, capsule failure), as opposed to the single-slot `SendItem(conn, ITEM_PLACE_CARRY, ...)` path (`SendFunc.cpp:1055`, `Server.cpp:729`).
- **Coin**: the carried gold `pMob[conn].MOB.Coin` (`STRUCT_MOB.Coin`, `Basedef.h:448`) is what's sent in this packet's `Coin` field.
- **2,000,000,000 coin cap**: enforced where gold is *earned*, not in `SendCarry` itself. Loot gold in `MobKilled.cpp:1750-1756` refuses gains beyond `2000000000` (`_NN_Cant_get_more_than_2G`); the cap value also appears in `CCastleZakum.cpp:246,283` and `CReadFiles.cpp:319,337`. `SendCarry` itself has **no** cap/guard — it merely reflects the authoritative `MOB.Coin` state (which the gain paths already bound to 2,000,000,000).
- **Max carry slots**: `MAX_CARRY = 64` (`Basedef.h:76`); the bag-expansion item (sIndex 3467) grows effective capacity via `pMob[conn].MaxCarry` (`_MSG_UseItem.cpp:5688-5694`), and the full 64-slot array is always transmitted regardless.
- **What the client does with it**: refresh the carry-inventory UI and the displayed coin total (client-side behavior; out of server scope).

## 7. Side Effects

- **State mutation**: none inside `SendCarry` (read-only snapshot). Mutations happen in the *callers* before invoking it (e.g. `MOB.Carry`/`MOB.Coin` updated at `_MSG_Trade.cpp:328-332`, `_MSG_UseItem.cpp:5674-5681`, `_MSG_Quest.cpp` coin subtraction at 592).
- **Outgoing packets**: `MSG_UpdateCarry` (this packet) to the single target `conn`. Callers frequently send companions:
  - `SendScore` → `MSG_UpdateScore` (e.g. `_MSG_Quest.cpp:590`, `_MSG_UseItem.cpp:5685`)
  - `SendEtc` → `MSG_UpdateEtc` (e.g. `_MSG_UseItem.cpp:5686`)
  - `SendClientMessage` (e.g. `_MSG_UseItem.cpp:5668`, `ProcessDBMessage.cpp:1263,1271`)
  - `SendItem(conn, ITEM_PLACE_EQUIP, ...)` (e.g. `_MSG_Quest.cpp:1603`)
- **Logs**: `SendCarry` itself writes no logs. Callers log via `ItemLog`/`Log` (e.g. trade started `_MSG_Trade.cpp:343`, jeffi `_MSG_Quest.cpp:592` context).
- **Connection teardown**: `CloseUser(conn)` if queueing fails (`SendFunc.cpp:1558`).

## 8. Related Packets

| Packet | Type | Relation |
|---|---|---|
| `MSG_SendItem` (`_MSG_SendItem`, `SendItem`, `SendFunc.cpp:1055`) | GAME2CLIENT | Single-slot carry update (`ITEM_PLACE_CARRY`), the fine-grained alternative to the full `SendCarry` refresh; used by `PutItem` (`Server.cpp:729`) |
| `MSG_UpdateScore` (`_MSG_UpdateScore`, `Basedef.h:1466`) | GAME2CLIENT\|CLIENT2GAME | Frequently sent alongside `SendCarry` after inventory-affecting events |
| `MSG_UpdateEtc` (`SendEtc`, `SendFunc.cpp:1195`) | — | Updates coin/etc UI; used for loot-coin gains (e.g. `MobKilled.cpp:1755`) while `SendCarry` is used for full resets |
| `MSG_CNFGetItem` (`_MSG_CNFGetItem`, `Basedef.h:1629`) | GAME2CLIENT | Acknowledges a single item placement into a destination slot — complements `SendCarry`'s bulk refresh |

## 9. Discrepancies & Open Questions

- **Unrelated size typo in neighboring function**: `SendAutoTrade` (`SendFunc.cpp:527`) queues `sizeof(MSG_SendAutoTrade)` bytes but passes **`sizeof(MSG_UpdateCarry)`** to `AddMessage` at `SendFunc.cpp:556`. `MSG_SendAutoTrade` (`Basedef.h:1909-1923`) is larger than `MSG_UpdateCarry` (528), so this truncates the autotrade message. Not part of `MSG_UpdateCarry`'s own path, but it is the same-line source of the only other `sizeof(MSG_UpdateCarry)` reference in the producer file and is a latent bug worth flagging.
- **Coarse vs fine-grained refresh**: `SendCarry` resends all 64 carry slots even when only one slot changed (e.g. quest clears a few crystals at `_MSG_Quest.cpp:1602`). Inefficient but harmless — open question whether a per-slot `SendItem` would be preferable for those cases.
- **`ClientTick`** is always overwritten by `CurrentTime` in `AddMessage` (`CPSock.cpp:541`), so the value the producer might have set is irrelevant — consistent with the rest of the outbound path.
- **Bag-expansion double-send**: `_MSG_UseItem.cpp:5676/5682` and `:5702` send `SendCarry` twice in one handler (once per added slot, once after `MaxCarry` recompute) — redundant but idempotent.

## 10. Source References

| File | Lines | Content |
|---|---|---|
| `legacy/Code/Basedef.h` | 1455-1463 | `_MSG_UpdateCarry` const + `MSG_UpdateCarry` struct |
| `legacy/Code/Basedef.h` | 925-930 | `_MSG` header macro |
| `legacy/Code/Basedef.h` | 932-941 | Flag constants (`FLAG_GAME2CLIENT = 0x0100`) |
| `legacy/Code/Basedef.h` | 398-412 | `STRUCT_ITEM` |
| `legacy/Code/Basedef.h` | 76 | `MAX_CARRY = 64` |
| `legacy/Code/Basedef.h` | 448 | `STRUCT_MOB.Coin` |
| `legacy/Code/Basedef.h` | 1465 / 1492 | pack(1) region boundary (packet is outside it) |
| `legacy/Code/TMSrv/SendFunc.cpp` | 1534-1559 | `SendCarry` producer |
| `legacy/Code/TMSrv/SendFunc.cpp` | 1055-1082 | `SendItem` (single-slot alternative) |
| `legacy/Code/TMSrv/SendFunc.cpp` | 1136 / 1195 | `SendScore` / `SendEtc` |
| `legacy/Code/TMSrv/SendFunc.cpp` | 556 | Stray `sizeof(MSG_UpdateCarry)` in `SendAutoTrade` |
| `legacy/Code/CPSock.cpp` | 513-591 | `AddMessage` framing/obfuscation/checksum |
| `legacy/Code/CPSock.cpp` | 617-642 | `SendMessageA` flush |
| `legacy/Code/CPSock.h` | 38-40 | `MAX_MESSAGE_SIZE`, `INITCODE` |
| `legacy/Code/Basedef.cpp` | 6475 | `BASE_CheckPacket` (disabled) |
| `legacy/Code/TMSrv/_MSG_Trade.cpp` | 328-335 | Trade carry+coin commit → `SendCarry` ×2 |
| `legacy/Code/TMSrv/_MSG_UseItem.cpp` | 5664-5704 | Bag-expansion → `SendCarry` |
| `legacy/Code/TMSrv/_MSG_Quest.cpp` | 592,1602,1931,1985,2039,2095 | Quest rewards → `SendCarry` |
| `legacy/Code/TMSrv/ProcessDBMessage.cpp` | 1260-1273 | Capsule fail → `SendCarry` |
| `legacy/Code/TMSrv/MobKilled.cpp` | 1750-1756 | 2,000,000,000 coin cap (earn-side) |
| `legacy/Code/TMSrv/Server.cpp` | 709-737 | `PutItem` → `SendItem` |
| `legacy/Code/TMSrv/ProcessClientMessage.cpp` | — | No `_MSG_UpdateCarry` case (verified) |
