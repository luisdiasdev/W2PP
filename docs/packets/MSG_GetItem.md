# MSG_GetItem

## 1. Summary

| Property | Value |
|---|---|
| Wire type constant | `_MSG_GetItem` = `(112 \| FLAG_CLIENT2GAME)` = `112 \| 0x0200` = `0x0270` (624) |
| Declaration | `Basedef.h:1618` (const), `Basedef.h:1619-1627` (struct) |
| Direction | client → TMSrv (game). **CLIENT2GAME only** — the client's request to pick up a ground item. |
| Purpose | Inbound request to pick a ground item into a carry slot (or as coins). The game's reply is `MSG_CNFGetItem` (0x171) plus `MSG_DecayItem` / `MSG_SendItem`. |
| Inbound dispatcher | `ProcessClientMessage.cpp:202-204` — `case _MSG_GetItem: Exec_MSG_GetItem(conn, pMsg)`. |
| Handler | `Exec_MSG_GetItem` in `TMSrv/_MSG_GetItem.cpp:20-252` |
| Struct size (`sizeof`) | 28 bytes |
| Header size | 12 bytes (`_MSG` / `HEADER`) |
| Payload size | 14 bytes of data, padded to 16 (offsets 12–27) |
| Expected `Size` field | `sizeof(MSG_GetItem)` = 28 (inbound; not validated against `sizeof` — see §9) |
| Alias | none (unique constant) |
| Flags mask | CLIENT2GAME `0x0200` — see `Basedef.h:933` |

## 2. Wire Framing (protocol preamble)

Global framing lives in `CPSock.cpp`/`CPSock.h`; `MSG_GetItem` does not deviate from it.

- **Session init magic:** on connection the server first sends `INITCODE = 0x1F11F311` as a raw 4-byte value (`CPSock.cpp:249-250`; defined `CPSock.h:40`). The peer authenticates this before any message (`CPSock.cpp:366-383`).
- **Message envelope** (`HEADER`, `CPSock.h:42-50` — identical to `_MSG`, `Basedef.h:925-930`), 12 bytes:
  - `Size` short @0, `KeyWord` char @2, `CheckSum` char @3, `Type` short @4, `ID` short @6, `ClientTick` uint @8.
- **Size bounds:** `Size` must satisfy `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE` (= `8192`, `CPSock.h:38`) — `CPSock.cpp:397`.
- **KeyWord / obfuscation:** receive path reads `iKeyWord`, derives `KeyWord = pKeyWord[iKeyWord*2]` (`CPSock.cpp:391-392`). Payload bytes from offset **4** are de-obfuscated per-byte, XOR-style keyed by `KeyWord`, with byte-index-dependent transforms (`CPSock.cpp:430-453`):
  - `i&3==0`: `- (Trans<<1)`; `i&3==1`: `+ (Trans>>3)`; `i&3==2`: `- (Trans<<2)`; `i&3==3`: `+ (Trans>>5)`, where `Trans = pKeyWord[(KeyWord+i)%256*2+1]`.
  - First 4 bytes (`Size`, `KeyWord`, `CheckSum`) are read verbatim.
- **CheckSum:** `CheckSum = Sum2 - Sum1`, where `Sum1` = sum of plaintext payload bytes (offset 4..Size), `Sum2` = sum of obfuscated payload bytes (`CPSock.cpp:425-455`; produced on send `CPSock.cpp:583-584`).
- **ClientTick:** on send `CurrentTime` (`CPSock.cpp:541`); the dispatcher rejects `ClientTick == SKIPCHECKTICK` (an anti-cheat marker) for client-originated packets (`ProcessClientMessage.cpp:63`).
- **BASE_CheckPacket:** compiled out — its body is entirely commented out (`Basedef.cpp:6475-6476`), so any per-type `Size` validation is inert. (Only active under `_PACKET_DEBUG`, `Server.cpp:3980-3988`.)

No per-packet framing deviation: `MSG_GetItem` uses the standard preamble exactly.

## 3. Binary Layout

### Packing context

`MSG_GetItem` is declared at `Basedef.h:1619`, which is **outside** every `#pragma pack(push,1)` region. The pack(1) regions in `Basedef.h` are `808-835`, `1212-1246`, `1465-1492`, `2063-2097`. Offset 1619-1627 falls after `1492` and before `2063`, so it compiles under the **MSVC default `/Zp8`**. Nested `STRUCT_ITEM` (`Basedef.h:398-412`) is likewise at `/Zp8`. Little-endian x86, LP32.

### 3.1 Header (12 bytes, from `_MSG`, Basedef.h:925-930)

| Offset | Size | Field | Type | Notes |
|---|---|---|---|---|
| 0 | 2 | `Size` | short | total packet size incl. header = 28 |
| 2 | 1 | `KeyWord` | char | index into `pKeyWord` (obfuscation) |
| 3 | 1 | `CheckSum` | char | `Sum2 - Sum1` |
| 4 | 2 | `Type` | short | `_MSG_GetItem` = 0x0270 |
| 6 | 2 | `ID` | short | player conn index (guarded `[0, MAX_USER)` at `ProcessClientMessage.cpp:42`) |
| 8 | 4 | `ClientTick` | unsigned int | `CurrentTime`; must != `SKIPCHECKTICK` (235543242, `Basedef.h:172`) |

### 3.2 Payload (16 bytes, struct `MSG_GetItem`, Basedef.h:1619-1627)

| Offset | Size | Field | Type | Meaning (from handler) |
|---|---|---|---|---|
| 12 | 4 | `DestType` | int | destination inventory class; must be `ITEM_PLACE_CARRY` = 1 (`Basedef.h:87`) — `_MSG_GetItem.cpp:44` |
| 16 | 4 | `DestPos` | int | destination slot in `MOB.Carry[MAX_CARRY]` (0..63) — `_MSG_GetItem.cpp:206,212` |
| 20 | 2 | `ItemID` | unsigned short | **ground-item slot index + 10000** — handler does `itemID = m->ItemID - 10000` (`:50`); matches the `+10000` spawn convention (`Server.cpp:8391`) |
| 22 | 2 | `GridX` | unsigned short | expected ground X; must equal `pItem[itemID].PosX` (`:174`) |
| 24 | 2 | `GridY` | unsigned short | expected ground Y; must equal `pItem[itemID].PosY` (`:174`) |
| 26 | 2 | — | (padding) | struct-tail alignment padding; not a named field |

Payload data is 14 bytes; the struct is padded to 16 so `sizeof(MSG_GetItem) = 28`.

### 3.3 Nested struct expansions

No `STRUCT_*` is nested inside `MSG_GetItem` itself. The related reply `MSG_CNFGetItem` (which embeds `STRUCT_ITEM`) is documented in `MSG_CNFGetItem.md`. For completeness, the `STRUCT_ITEM` used by the ground item (`pItem[itemID].ITEM`, `CItem.h:27`) is, all 2-byte members → no padding, **8 bytes** (`Basedef.h:398-412`):

| Offset (rel. struct) | Size | Field | Type |
|---|---|---|---|
| 0 | 2 | `sIndex` | short |
| 2 | 2 | `stEffect[0]` (union `sValue` / `{cEffect,cValue}`) | short |
| 4 | 2 | `stEffect[1]` | short |
| 6 | 2 | `stEffect[2]` | short |

The union is 2 bytes: `short sValue` overlays `{unsigned char cEffect; unsigned char cValue;}`.

### 3.4 Size verification (math)

Alignment under `/Zp8` = `min(member size, 8)`.

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
20      ItemID (ushort) 2     22
22      GridX (ushort)  2     24
24      GridY (ushort)  2     26
26      (tail pad)      2     28   2 bytes pad to 4-align
sizeof(MSG_GetItem) = 28
```

- Expected wire `Size` = `sizeof(MSG_GetItem)` = **28**. The handler never assigns `Size` (it is inbound); there is no in-handler `sizeof(MSG_GetItem)` cross-check. Size/checksum/bounds are validated only in `CPSock::ReadMessage` (`CPSock.cpp:390-407,425-455`).
- `itemID = m->ItemID - 10000` is used as the index into `pItem[MAX_ITEM]` (`MAX_ITEM = 5000`, `Basedef.h:103`), bounded by `itemID >= MAX_ITEM` at `_MSG_GetItem.cpp:52,55`.
- **No UNKNOWN members**: every named field's role is resolved from the handler.

## 4. Lifecycle & Flow

```
 client                  TMSrv (game)                        ground grid (pItem[])
   │ MSG_GetItem (0x270, CLIENT2GAME)                            │
   │──────────────────────► CPSock.ReadMessage (unobfuscate)     │
   │                    Server.cpp WSA_READ → ProcessClientMessage (:4001)
   │                      dispatcher guards (:42-64)              │
   │                    case _MSG_GetItem (ProcessClientMessage.cpp:202)
   │                    Exec_MSG_GetItem (_MSG_GetItem.cpp:20)    │
   │                      • alive/mode/trade/desttype guards      │
   │                      • itemID = ItemID-10000; pItem[] lookup │
   │                      • range check (Target ±3)               │
   │                      • grid-consistency checks               │
   │                      • coin branch OR carry-slot write       │
   │                      memcpy bItem←ditem (:226)               │
   │ ◄─────────────────── MSG_CNFGetItem (0x171) AddMessage:244   │
   │ ◄─────────────────── MSG_DecayItem  AddMessage:199 / GridMulticast:246
   │                      pItemGrid[Y][X]=0; pItem[].Mode=0 (:248-249)
   │ ◄─────────────────── SendItem → MSG_SendItem (0x130) :251    │
   │                      area broadcast: MSG_DecayItem (GridMulticast :246)
```

- **Receive:** `CPSock::Receive` (`CPSock.cpp:336-347`) buffers; `ReadMessage` (`CPSock.cpp:353-467`) auths INITCODE, validates `Size`, and de-obfuscates the payload before returning a pointer.
- **Dispatch:** `Server.cpp` `WSA_READ` → `ReadMessage` loop → `ProcessClientMessage(User, Msg, FALSE)` (`Server.cpp:3975,4001`).
- **Dispatcher guards** (`ProcessClientMessage.cpp:38-66`): `ID in [0, MAX_USER)` (`:42`); `ServerDown >= 120` drop (`:53`); `ClientTick == SKIPCHECKTICK` drop for client packets (`:63`). Then `switch(std->Type)` → `case _MSG_GetItem` → `Exec_MSG_GetItem` (`:202-204`). **No `default:` case** in the switch (verified — unmatched types are silently ignored).
- **Handler** `Exec_MSG_GetItem(conn, pMsg)` (`_MSG_GetItem.cpp:20`): casts `pMsg` to `MSG_GetItem*` (`:22`) and runs the guards, lookup, range check, and the coin/slot write branches described in §5–§7.

## 5. Validation & Guards

Guards execute in the following order inside `Exec_MSG_GetItem`:

| # | Guard | Location | Behavior on failure |
|---|---|---|---|
| 1 | Player alive (`Hp > 0`) and `Mode == USER_PLAY` (22, `CUser.h:36`) | `_MSG_GetItem.cpp:24` | `AddCrackError(conn,1,13)` + `SendHpMode`, return |
| 2 | Not trading (`Trade.OpponentID == 0`) | `:31` | `RemoveTrade` both sides, return |
| 3 | Not in auto-trade (`TradeMode == 0`) | `:38` | `SendClientMessage(_NN_CantWhenAutoTrade)`, return |
| 4 | `m->DestType == ITEM_PLACE_CARRY` (1) | `:44` | log `"DEBUG:GetItem with wrong desttype"`, return |
| 5 | `itemID = m->ItemID - 10000` in `[1, MAX_ITEM)` | `:50-53` | return |
| 6 | `pItem[itemID].Mode != 0` (ground item exists) | `:55` | `MSG_DecayItem` to picker (`:62-70`), return |
| 7 | Player within ±3 cells of `pItem[itemID]` position | `:74-76` | log `"GetItemFail posx…"`, return (anti-cheat range gate) |
| 8 | Item 1727 (god-level) requires `Level >= 1000` | `:84-85` | return |
| 9 | `sIndex` in `[1, MAX_ITEMLIST)` (6500, `Basedef.h:139`) | `:91-92` | return |
| 10 | Coin cap: `coin + pMob[conn].MOB.Coin >= 2,000,000,000` | `:192` | `SendClientMessage(_NN_Cant_get_more_than_2G)` (index 273, `Language.h:293`), return |
| 11 | `0 <= m->DestPos < MAX_CARRY` (64, `Basedef.h:76`) | `:206` | log `"DEBUG:Trading Fails.(Wrong source position)"`, return |
| 12 | Destination carry slot empty (`bItem->sIndex == 0`) | `:215` | if `DestPos in (0, MaxCarry]`, decrement and re-send slot via `SendItem` (`:221-223`); return (item NOT granted) |

Grid-consistency checks inside the flow (not "guard-and-return" in the same sense but early-outs): invalid grid coords (`:157`) clears item & returns `MSG_DecayItem`; `pItemGrid[itemY][itemX] != itemID` (`:164`) sends `MSG_DecayItem` and (re)writes the grid cell then returns; `itemX != m->GridX || itemY != m->GridY` (`:174`) sends `MSG_DecayItem` and returns.

Wire-level validation (`Size`, `CheckSum`, bounds) is in `CPSock::ReadMessage` (`CPSock.cpp:390-407,425-455`); `BASE_CheckPacket` is disabled (`Basedef.cpp:6475`).

## 6. Game Mechanics & Business Logic

- **Ground-item identity:** the client does not send the `STRUCT_ITEM` itself — it sends `ItemID = ground-slot-index + 10000` (`_MSG_GetItem.cpp:50`). The server subtracts 10000 and indexes the global `pItem[MAX_ITEM]` (`CItem.h:45`). Same `+10000` convention used when spawning ground items (`Server.cpp:8391`) and in `MSG_DecayItem.ItemID`.
- **Ground-item match:** the handler re-validates the item exists (`pItem[itemID].Mode != 0`, `:55`), is near the player (`:74`), and that the reported `GridX/GridY` match `pItem[itemID].PosX/PosY` and the `pItemGrid` occupancy (`:157-178`) before granting.
- **Coin branch (`Vol == 2`):** `Vol = BASE_GetItemAbility(ditem, EF_VOLATILE)` (`EF_VOLATILE = 38`, `ItemEffect.h:77`). Coin is reconstructed as `EF_HWORDCOIN << 8 + EF_LWORDCOIN` (`EF_HWORDCOIN = 36`, `EF_LWORDCOIN = 37`, `ItemEffect.h:75-76`) summed into `pMob[conn].MOB.Coin` (`:180-197`), capped at 2,000,000,000 (`:192`). The ground item is cleared with `BASE_ClearItem` (`:201`) and the picker gets `MSG_DecayItem` (`:199`).
- **Carry-slot branch (else):** checks `DestPos` bounds (`:206`), requires an empty slot (`bItem->sIndex == 0`, `:215`), then `memcpy(bItem, ditem, sizeof(STRUCT_ITEM))` (`:226`) into `MOB.Carry[m->DestPos]`.
- **Special item cases:**
  - `sIndex == 470` (PilulaOrc quest pill): if already done, sends `_NN_Youve_Done_It_Already`; else grants a skill point — sets `PilulaOrc = 1`, clears the item and grid cell (`:106-109`), multicasts `MSG_DecayItem` (`:119`), `SkillBonus += 9`, `SendEmotion(14,3)` and `SendEtc` (`:121-124`).
  - `sIndex` in `[490,500)` (rare drops): broadcasts a server notice `_SSD_S_get_S` via `SendNotice` (`:129-132`) and multicasts `MSG_DecayItem` (`:142`).
- **Full-inventory handling:** there is no bulk "inventory full" scan; the slot write succeeds only if `bItem->sIndex == 0` (`:215`). If occupied, the slot is re-sent (`SendItem`, `:222`) and the pickup is refused — the item stays on the ground.
- **Item ownership:** none enforced — any player within range can pick up any `pItem[]` slot; there is no owner/looter timestamp check.
- **Follow calls:** `BASE_GetItemAbility` (`Basedef.cpp:1647`), `BASE_ClearItem` (`Basedef.cpp:4281`), `BASE_GetItemCode` (`Basedef.cpp:1479`), `SendItem` (`SendFunc.cpp:1055`), `GridMulticast` (`SendFunc.cpp:843`), `SendEmotion`/`SendEtc` (`SendFunc.cpp:825,1195`), `SendClientMessage`/`SendNotice` (`SendFunc.cpp:27,47`), `SendHpMode` (`SendFunc.cpp:1622`), `AddCrackError`/`RemoveTrade`/`ItemLog` (`Server.cpp:684,7319,9264`).

## 7. Side Effects

1. **Ground grid mutation:** on success `pItemGrid[itemY][itemX] = 0` (`:248`) and `pItem[itemID].Mode = 0` (`:249`); the ground sprite is removed for everyone via `GridMulticast` `MSG_DecayItem` (`:246`). Special-case paths also clear the cell (`:108-109`, `:160`, `:169`).
2. **Player inventory mutation:** carry-slot branch `memcpy` into `MOB.Carry[m->DestPos]` (`:226`); coin branch adds to `pMob[conn].MOB.Coin` (`:197`).
3. **Outgoing packets to the picker:**
   - `MSG_CNFGetItem` (0x171) — success ack, `AddMessage` (`:244`); producer at `_MSG_GetItem.cpp:234-244` (see `MSG_CNFGetItem.md`).
   - `MSG_DecayItem` — coin path (`:199`) and various early-outs (`:159,166,176`).
   - `MSG_SendItem` via `SendItem(conn, ITEM_PLACE_CARRY, slot, …)` — carry-slot refresh (`:251`) and the occupied-slot path (`:222`).
4. **Outgoing packets to the area:** `MSG_DecayItem` via `GridMulticast(itemX, itemY, …, 0)` (`:246`), plus the special-case multicasts (`:119,142`).
5. **Logs / messages:**
   - `ItemLog("getitem, <itemcode>", …)` (`:228-231`) — `BASE_GetItemCode` formats `" <Name> : <sIndex>.<c0>.<v0>.<c1>.<v1>.<c2>.<v2>"` (`Basedef.cpp:1494`).
   - `Log("DEBUG:GetItem with wrong desttype", …)` (`:46`); `Log("GetItemFail idx:%d mode:%d", …)` (`:59`); `Log("GetItemFail posx:%d posx:%d tx:%d ty:%d", …)` (`:77`); `Log("DEBUG:Trading Fails.(Wrong source position)", …)` (`:208`).
   - `SendClientMessage` for auto-trade refusal (`:40`), coin cap (`:194`), quest-pill states (`:98,103`); `SendNotice` for rare drops (`:132`); `SendEmotion`/`SendEtc` for the quest pill (`:123-124`).
6. **No DB round-trip** — `MSG_GetItem` is fully handled in TMSrv game memory; it never reaches `ProcessDBMessage.cpp` / DBSrv. (No reference to `_MSG_GetItem` in `ProcessDBMessage.cpp`.)

## 8. Related Packets

| Packet | Type | Direction | Relation |
|---|---|---|---|
| `MSG_CNFGetItem` | `0x171` (`Basedef.h:1629`) | GAME2CLIENT | Success ack for this pickup (`_MSG_GetItem.cpp:234-244`). |
| `MSG_DecayItem` | `0x111` (`Basedef.h:1609`) | GAME2CLIENT | Removes the ground sprite after pickup / on early-outs (`_MSG_GetItem.cpp:199,246`). |
| `MSG_SendItem` | `0x130` (`Basedef.h:1672`) | GAME2CLIENT | Single-slot carry refresh via `SendItem` (`_MSG_GetItem.cpp:251`). |
| `MSG_UpdateCarry` | `0x133` (`Basedef.h:1455`) | GAME2CLIENT | Bulk carry+coin sync (`SendFunc.cpp:1545-1558`); complement to `MSG_SendItem`, not sent by this handler directly. |
| `MSG_CreateItem` | `0x110` (`Basedef.h:1592`) | CLIENT2GAME | Spawns the ground items this pickup consumes; shares the `+10000` `ItemID` convention (`Server.cpp:8391`). |
| `MSG_DropItem` | `0x114` (`Basedef.h:1638`) | CLIENT2GAME | Inverse operation (dropping an item to the ground), dispatched at `ProcessClientMessage.cpp:198`. |

## 9. Discrepancies & Open Questions

1. **No in-handler `Size` cross-check:** `MSG_GetItem` is inbound and the handler never compares the packet's `Size` against `sizeof(MSG_GetItem)` (unlike outbound packets). Size validity relies solely on `CPSock::ReadMessage` bounds (`CPSock.cpp:397`); `BASE_CheckPacket` size guards are disabled (`Basedef.cpp:6475`).
2. **`itemID` derived from `ItemID` is used as a raw `pItem[]` index** (`:50`), i.e. the wire carries a server-side array index disguised as an ID. A malformed/forged `ItemID` is bounded by `[1, MAX_ITEM)` (`:52,55`), but within that range the client can address **any** live ground slot — **no ownership check** (any player may take any ground item in range). Possibly intentional for a private server; flagging as a design point.
3. **Coin-cap check occurs *after* item-existence/range validation but the coin value is computed from item effects** (`:180-197`); the 2,000,000,000 cap is enforced on the *sum* (`tcoin`), not per-item.
4. **`DestPos` slot-collision returns with no `MSG_CNFGetItem`** (`:215-224`) — the client gets only a re-sent `MSG_SendItem`; an ack-dependent client may stall.
5. **Quest-pill (`sIndex==470`) and rare-drop (`490..499`) paths return before the normal confirm/grant flow** (`:94-143`) — no `MSG_CNFGetItem` is emitted for them.
6. **`m->GridX/GridY` are validated but redundant** with `pItem[itemID].PosX/PosY` (`:174`); they appear to be a client-authoritative cross-check that can be rejected on mismatch.
7. The coin path sends the confirmation with the original `DestPos/DestType` even though **no carry slot is written** (`:234-244` after `:182-202`) — same observation as documented in `MSG_CNFGetItem.md` §9.

## 10. Source References

- `legacy/Code/Basedef.h:87` — `ITEM_PLACE_CARRY = 1`
- `legacy/Code/Basedef.h:100-103,139,170,172` — `MAX_GRIDX/MAX_GRIDY`, `MAX_ITEM`, `MAX_ITEMLIST`, `ESCENE_FIELD`, `SKIPCHECKTICK`
- `legacy/Code/Basedef.h:398-412` — `STRUCT_ITEM`
- `legacy/Code/Basedef.h:925-930` — `_MSG` macro (header)
- `legacy/Code/Basedef.h:1618-1627` — `_MSG_GetItem` / `MSG_GetItem`
- `legacy/Code/Basedef.h:1609-1616,1629-1636,1672-1682,1455-1463` — `MSG_DecayItem`, `MSG_CNFGetItem`, `MSG_SendItem`, `MSG_UpdateCarry`
- `legacy/Code/Basedef.cpp:6475-6476` — `BASE_CheckPacket` (disabled)
- `legacy/Code/Basedef.cpp:1479,1647,4281` — `BASE_GetItemCode`, `BASE_GetItemAbility`, `BASE_ClearItem`
- `legacy/Code/CItem.h:24-45` — `CItem` (ground item `ITEM`/`Mode`/`PosX`/`PosY`), `pItem[MAX_ITEM]`
- `legacy/Code/CPSock.h:38,40,42-50` — `MAX_MESSAGE_SIZE`, `INITCODE`, `HEADER`
- `legacy/Code/CPSock.cpp:336-347,353-467` — `Receive` / `ReadMessage` (INITCODE, Size, de-obfuscation, CheckSum)
- `legacy/Code/ItemEffect.h:75-77` — `EF_HWORDCOIN`, `EF_LWORDCOIN`, `EF_VOLATILE`
- `legacy/Code/TMSrv/_MSG_GetItem.cpp:20-252` — handler `Exec_MSG_GetItem`
- `legacy/Code/TMSrv/ProcessClientMessage.cpp:38-66` — dispatcher guards; `:202-204` `case _MSG_GetItem`
- `legacy/Code/TMSrv/Server.cpp:3975,4001,4004` — `WSA_READ` → `ProcessClientMessage`
- `legacy/Code/TMSrv/SendFunc.cpp:27,47,825,1055,1195,1622,843` — `SendClientMessage`, `SendNotice`, `SendEmotion`, `SendItem`, `SendEtc`, `SendHpMode`, `GridMulticast`
- `legacy/Code/TMSrv/Server.h:55` — `extern unsigned short pItemGrid[MAX_GRIDY][MAX_GRIDX]`
- `legacy/Code/TMSrv/CUser.h:36` — `USER_PLAY = 22`
- `legacy/Code/TMSrv/Language.h:293` — `_NN_Cant_get_more_than_2G` (index 273)
- `legacy/Code/TMSrv/Server.cpp:684,7319,9264` — `AddCrackError`, `RemoveTrade`, `ItemLog`
- `docs/packets/MSG_CNFGetItem.md` — companion reply packet
