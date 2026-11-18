# Component Deep Analysis Report - GetItem Handler

Component: `GetItem Handler` (`Exec_MSG_GetItem`)
Primary location: `Code/TMSrv/_MSG_GetItem.cpp` (252 lines, 6,797 bytes)
Project: W2PP - decompiled C/C++ (Win32, MSVC) MMORPG game server emulator for W2
Analysis scope: entire project root (`E:\wyd\test-setup\w2pp`), excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
Analysis date: 2026-11-15
Report type: read-only component analysis (no files were modified)

---

## 1. Executive Summary

### 1.1 Purpose

`Exec_MSG_GetItem` is the TMSrv command handler for the client-to-game message `_MSG_GetItem`
(`Code/Basedef.h:1618`, opcode `112 | FLAG_CLIENT2GAME` = `0x0270`). It implements the single
business transaction "move one item object from the world ground grid into the player's
inventory (or the player's purse, for currency objects)". The whole component is one free
function, 252 lines, with no owned state, no class, and no header of its own: it declares
itself only through `Code/TMSrv/ProcessClientMessage.h:104` and is wired into the dispatcher
at `Code/TMSrv/ProcessClientMessage.cpp:202-203`.

The handler is the direct counterpart of `Exec_MSG_DropItem` (`Code/TMSrv/_MSG_DropItem.cpp`):
drop creates and registers a world item (`CreateItem`, `Code/TMSrv/Server.cpp:7254`), pickup
de-registers that same world item, copies its `STRUCT_ITEM` payload into the character's
`Carry[]` array, and broadcasts the removal to every visible client.

### 1.2 Role in the system

- Layer: TMSrv command-handler layer (`Code/TMSrv/_MSG_*.cpp`), one of 48 components listed in
  `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:140`.
- Architectural classification in the architecture report: *Command Handler (inventory)*,
  afferent 1, efferent 14.
- Position in the runtime call chain:
  `WSA_READ (Server.cpp:3919) -> CPSock::ReadMessage (CPSock.cpp:353) -> ProcessClientMessage
  (ProcessClientMessage.cpp:38) -> Exec_MSG_GetItem -> [world grid + inventory + broadcast]`.
- Authoritative ownership of data: it is the **only** writer that ends the lifecycle of a
  ground item object other than `ProcessDecayItem` (`Server.cpp:8353`) and the expiry sweep.
  It is one of a small number of writers of `pMob[conn].MOB.Carry[]` and the only handler that
  adds to `pMob[conn].MOB.Coin` through a ground object.

### 1.3 Key findings

1. **Validate-then-commit with two early-commit exceptions.** 13 guard clauses precede the
   commit block at lines 234-251, so most failures leave world state untouched. However the
   quest-pill branch (lines 94-127) and the out-of-range-coordinate branch (lines 157-162)
   commit and return before the common commit path, producing three distinct exit states.
2. **No ownership model and no pickup lock.** `CItem` (`Code/TMSrv/CItem.h:24-45`) has no
   owner field and the handler performs no owner comparison; `pItem[].Delay` is a decay
   countdown consumed by `ProcessDecayItem` (`Server.cpp:8368-8372`), not a pickup delay.
   Pickup is strictly first-come-first-served.
3. **No weight, no footprint, no stacking.** Capacity is represented by a single flat slot
   index; `DestPos` is only range-checked against `MAX_CARRY` (64) and tested for
   emptiness (`sIndex == 0`). The engine's own footprint validator `BASE_CanCarry`
   (`Basedef.cpp:4413`) is never called by any handler (its only call site is commented out at
   `Server.cpp:8574`). Consequently 2x4-footprint items and stackable potions are stored
   as single flat slots with no merge path anywhere in the component.
4. **Currency pickup bypasses the inventory entirely.** `EF_VOLATILE == 2` routes to the
   purse branch (lines 182-202), which decodes a 16-bit amount from `EF_HWORDCOIN`/`EF_LWORDCOIN`
   and enforces a hard ceiling of 2,000,000,000 coins. Over-cap pickup is rejected with message
   index 273 and the item stays on the ground.
5. **`itemID == 1727` compares a world-slot index, not an item id.** Line 84 gates the pickup on
   `itemID` (the `pItem[]` array slot, already decremented by 10000) while every other place in
   the codebase gates on the item archetype `pItem[..].ITEM.sIndex` (`Server.cpp:8362`). The
   level-1000 restriction therefore hangs off whichever world object happens to occupy slot
   1727, not off the item defined at `sIndex` 1727.
6. **The message-length contract is declared but not enforced.** `BASE_CheckPacket` lists
   `_MSG_GetItem` at `Basedef.cpp:6516`, but the entire body of that function is commented out
   (`Basedef.cpp:6476-6594`) and it unconditionally returns `FALSE`; its only call sites are
   inside `#ifdef _PACKET_DEBUG`. A truncated `_MSG_GetItem` frame is therefore cast to the
   full struct and read past the caller-declared length.
7. **Two client-desync windows.** (a) The rare-item announcement block (lines 129-143) emits a
   decay multicast *before* the remaining validations, so peers erase an item that can still
   fail to be picked up. (b) The occupied-destination path (lines 217-225) returns without any
   pickup-specific response, resynchronising only one other slot.
8. **Audit asymmetry.** Successful item copies are logged with `ItemLog("getitem, ...")`
   (lines 228-231); currency pickups and the quest-pill `sIndex 470` consumption produce no
   `ItemLog` entry at all, leaving the economy/quest paths without an item trail.
9. **Zero automated test coverage.** The repository contains no test project, no test framework
   dependency, no assert call site, and no CI workflow; the component has no executable
   specification of any kind (see Section 11).
10. **Dead/duplicated code inside the component.** Redundant `itemID` re-validation (line 55),
    unused local `error` (line 214), `MSG_CNFGetItem.Item` never populated (lines 234-244),
    duplicate decay-packet construction (lines 62-70, 111-119, 134-142, 148-155).

---

## 2. Data Flow Analysis

### 2.1 Macro flow (network frame to world state)

```
 1. Client sends frame Type=0x0270 (112|FLAG_CLIENT2GAME), Size declared in the frame header
 2. CPSock::ReadMessage (CPSock.cpp:353) frames by the Size field, validates size bounds
    (MAX_MESSAGE_SIZE) and decrypts/verifies the checksum; on checksum mismatch it still
    returns the buffer (CPSock.cpp:457-464)
 3. Server.cpp:3975 (WSA_READ) -> ProcessClientMessage(User, Msg, FALSE)   [Server.cpp:4001]
 4. ProcessClientMessage: ID range gate (cpp:42), ServerDown gate (cpp:53),
    LastReceiveTime stamp (cpp:57), SKIPCHECKTICK gate (cpp:63)
 5. switch(std->Type) -> case _MSG_GetItem -> Exec_MSG_GetItem(conn, pMsg)  [cpp:202-203]
 6. Exec_MSG_GetItem: cast frame to MSG_GetItem*                            [_MSG_GetItem.cpp:22]
 7. Session gate: HP<=0 or Mode!=USER_PLAY -> AddCrackError(1,13) + SendHpMode + return  [24-29]
 8. Trade gate: Trade.OpponentID != 0 -> RemoveTrade(both sides) + return                  [31-36]
 9. AutoTrade gate: TradeMode != 0 -> message _NN_CantWhenAutoTrade + return                [38-42]
10. Destination container gate: DestType != ITEM_PLACE_CARRY -> Log + return                [44-48]
11. Handle decode: itemID = ItemID - 10000; reject 1..MAX_ITEM-1 outside range            [50-53]
12. World existence gate: pItem[itemID].Mode == 0 -> emit decay to the picker only,
    Log "GetItemFail idx/mode", return                                                      [55-72]
13. Proximity gate: |TargetX - PosX| <= 3 and |TargetY - PosY| <= 3, else Log + return     [74-82]
14. Special level gate: itemID == 1727 and Level < 1000 -> silent return                    [84-85]
15. Archetype sanity: 0 < sIndex < MAX_ITEMLIST, else silent return                         [88-92]
16. Quest-pill path: sIndex == 470 -> 470-branch (commit + return)                       [94-127]
17. Announcement band: 490 <= sIndex < 500 -> global SendNotice + early decay multicast  [129-143]
18. Snapshot itemX/itemY; build MSG_DecayItem                                            [145-155]
19. Coordinate integrity: x in [0,MAX_GRIDX), y in (0,MAX_GRIDY) -> else decay + Mode=0   [157-162]
20. Grid registration: pItemGrid[itemY][itemX] must equal itemID, else decay to picker,
    optional self-heal of an empty grid cell, return                                      [164-172]
21. Client attestation: m->GridX/GridY must equal itemX/itemY, else decay to picker       [174-178]
22. Volatility read: Vol = BASE_GetItemAbility(ITEM, EF_VOLATILE) (raw, sanc-free)         [180]
23. Currency path (Vol == 2): decode amount, ceiling check, add to purse, decay to picker,
    BASE_ClearItem                                                                    [182-202]
24. Inventory path (Vol != 2): DestPos range gate, occupancy gate, resync or memcpy +
    ItemLog("getitem, ...")                                                           [204-232]
25. Confirmation: MSG_CNFGetItem {DestType, DestPos, Item(empty)} to the picker        [234-244]
26. Broadcast: GridMulticast(itemX, itemY, MSG_DecayItem) to the 33x33 view            [246]
27. Teardown: pItemGrid[itemY][itemX] = 0; pItem[itemID].Mode = 0                       [248-249]
28. Inventory projection: SendItem(conn, ITEM_PLACE_CARRY, DestPos, &Carry[DestPos])    [251]
```

### 2.2 Data mutation map

| Target | Read | Written | Line(s) | Notes |
|--------|------|---------|---------|-------|
| `pMob[conn].MOB.CurrentScore.Hp` | yes | no | 24 | alive gate |
| `pUser[conn].Mode` | yes | indirectly (via `CharLogOut` in `AddCrackError`) | 24, 26 | crack counter logout path |
| `pUser[conn].Trade.OpponentID` | yes | via `RemoveTrade` | 31-34 | resets `MSG_Trade` + `MSG_SendAutoTrade` |
| `pUser[conn].TradeMode` | yes | via `RemoveTrade` | 38 | auto-trade visual state |
| `pItem[itemID].Mode` | yes | yes (0) | 55, 109, 160, 249 | world occupancy flag (1 = ground item, 2 = treasure box) |
| `pItem[itemID].ITEM` | yes | yes (cleared on currency/quest path only) | 88, 201, 106 | item payload |
| `pItem[itemID].PosX / PosY` | yes | no | 74-82, 108, 119, 142, 145-146 | world coordinates |
| `pItemGrid[itemY][itemX]` | yes | yes (0 or re-registered) | 164-172, 248 | grid holds the *item index* (not +10000) |
| `pMob[conn].TargetX / TargetY` | yes | no | 74-78 | server-side position, not client-supplied |
| `pMob[conn].MOB.CurrentScore.Level` | yes | no | 84 | level-1000 gate |
| `pMob[conn].extra.QuestInfo.Mortal.PilulaOrc` | yes | yes (1) | 96, 105 | one-shot quest flag |
| `pMob[conn].MOB.SkillBonus` | no | yes (+= 9, unsigned short) | 121 | skill-point pill |
| `pMob[conn].MOB.Coin` | yes | yes (+= coin1) | 190, 197 | currency path |
| `pMob[conn].MOB.Carry[DestPos]` | yes | yes (`memcpy`) | 212, 226, 251 | inventory slot |
| `pMob[conn].MaxCarry` | yes | no | 219 | used only in the resync guard |
| `temp[4096]` (global, `Server.h:382`) | yes | yes | 59, 77, 131, 230 | shared scratch buffer |
| `g_pMessageStringTable[]` | yes | no | 40, 103, 131, 194 | loaded from `UI/strdef.bin` (`Basedef.cpp:4932`) |

### 2.3 Interaction state machine (world item lifecycle)

```
CreateItem (Server.cpp:7254)            Exec_MSG_GetItem          ProcessDecayItem (Server.cpp:8353)
Mode 1, Delay 90, Decay 0  -------------> Mode 0, grid 0  ----------> (skipped: Mode == 0)
     |                                        ^                               |
     |  occupied DestPos (no pickup)          |                               |
     +----------------------------------------+                               |
     |                                                                        |
     +------------ Delay counts down to 0, Decay != -1 --------------------->  Mode 0, grid cleared,
                                                                               decay multicast

CreateTreasureBox (Server.cpp:9392) -> Mode 2  (no callers; unreachable in current tree)
```

### 2.4 Exit inventory (13 distinct exits)

| Exit | Line | World state | Client feedback |
|------|------|-------------|-----------------|
| 1 | 28 | unchanged | crack counter log entry + `SendHpMode` |
| 2 | 35 | both trades force-closed | trade reset signal (900) |
| 3 | 41 | unchanged | `_NN_CantWhenAutoTrade` (index 170) |
| 4 | 47 | unchanged | server log `DEBUG:GetItem with wrong desttype` |
| 5 | 53 | unchanged | none |
| 6 | 71 | unchanged | private `_MSG_DecayItem` (ghost cleanup) |
| 7 | 81 | unchanged | server log `GetItemFail posx/posy/tx/ty` |
| 8 | 85 | unchanged | none (silent rejection) |
| 9 | 92 | unchanged | none |
| 10 | 126 | quest pill consumed, grid cleared, +9 skill points | message, emotion 14/3, `SendEtc` |
| 11 | 161 | item deleted (`Mode = 0`) | private `_MSG_DecayItem` |
| 12 | 171 | possibly grid re-registered | private `_MSG_DecayItem` |
| 13 | 177 | unchanged | private `_MSG_DecayItem` |
| 14 | 196 | unchanged (over-cap currency) | `g_pMessageStringTable[273]` |
| 15 | 209 | unchanged | server log `DEBUG:Trading Fails.(Wrong source position)` |
| 16 | 224 | unchanged | `_MSG_SendItem` resync of `DestPos-1` |
| 17 | 252 | item moved to inventory, grid cleared, broadcast | `_MSG_CNFGetItem` + `_MSG_SendItem` |

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Validation / session | Pickup requires `CurrentScore.Hp > 0` and `pUser[conn].Mode == USER_PLAY`; violation adds 1 to a crack counter, resyncs HP/mode, aborts | `_MSG_GetItem.cpp:24-29` |
| BR-02 | Validation / transaction isolation | An open player trade is force-terminated on both sides and the pickup is abandoned | `_MSG_GetItem.cpp:31-36` |
| BR-03 | Validation / trading state | Automatic-trade mode blocks pickup with message `_NN_CantWhenAutoTrade` | `_MSG_GetItem.cpp:38-42` |
| BR-04 | Validation / destination | Only `DestType == ITEM_PLACE_CARRY` (1) is accepted as a pickup destination | `_MSG_GetItem.cpp:44-48` |
| BR-05 | Validation / handle decoding | World item handle is `ItemID - 10000`, valid handle window is `1 .. MAX_ITEM-1` (1..4999) | `_MSG_GetItem.cpp:50-53` |
| BR-06 | Validation / existence | `pItem[itemID].Mode == 0` means the object is gone: emit a private decay packet, log, abort | `_MSG_GetItem.cpp:55-72` |
| BR-07 | Business logic / distance | Pickup range is a 7x7 box: `|TargetX-PosX| <= 3` and `|TargetY-PosY| <= 3` | `_MSG_GetItem.cpp:74-82` |
| BR-08 | Business logic / progression | World slot `itemID == 1727` requires character level >= 1000; otherwise silent reject | `_MSG_GetItem.cpp:84-85` |
| BR-09 | Validation / referential integrity | Item archetype `sIndex` must satisfy `0 < sIndex < MAX_ITEMLIST` (6500) | `_MSG_GetItem.cpp:88-92` |
| BR-10 | Business logic / quest item | Archetype 470 is a one-shot skill-point pill: +9 `SkillBonus`, sets `PilulaOrc`, never enters inventory | `_MSG_GetItem.cpp:94-127` |
| BR-11 | Business logic / notification | Archetypes 490..499 broadcast a server-wide notice and multicast decay before validation completes | `_MSG_GetItem.cpp:129-143` |
| BR-12 | Validation / coordinate integrity | Server-side item coordinates must satisfy `0 <= x < 4096` and `0 < y < 4096`; violation deletes the object | `_MSG_GetItem.cpp:157-162` |
| BR-13 | Business logic / grid ownership | The item must be the registered occupant of `pItemGrid[y][x]`; an empty cell is re-registered ("self-heal") | `_MSG_GetItem.cpp:164-172` |
| BR-14 | Validation / client attestation | Client-supplied `GridX/GridY` must equal the server-side item position | `_MSG_GetItem.cpp:174-178` |
| BR-15 | Business logic / currency | `EF_VOLATILE == 2` objects are currency: amount = `(HWORD << 8) + LWORD`, purse ceiling 2,000,000,000 | `_MSG_GetItem.cpp:180-202` |
| BR-16 | Validation / inventory index | Inventory destination must satisfy `0 <= DestPos < MAX_CARRY` (64) | `_MSG_GetItem.cpp:206-210` |
| BR-17 | Business logic / slot occupancy | Destination slot must be empty (`sIndex == 0`); if occupied, resync the preceding slot and abort | `_MSG_GetItem.cpp:212-225` |
| BR-18 | Business logic / transfer semantics | Transfer is a raw `memcpy` of `STRUCT_ITEM`: no stacking, no merge, no footprint check, no weight | `_MSG_GetItem.cpp:226` |
| BR-19 | Compliance / audit | Successful item transfer writes one `ItemLog("getitem, <code>")` record | `_MSG_GetItem.cpp:228-231` |
| BR-20 | Protocol / confirmation | The picker receives `_MSG_CNFGetItem {DestType, DestPos, Item(empty)}` | `_MSG_GetItem.cpp:234-244` |
| BR-21 | Business logic / world teardown | After a successful transfer the object is cleared from the grid, `Mode` reset to 0, and a `_MSG_DecayItem` broadcast in the 33x33 view | `_MSG_GetItem.cpp:246-251` |
| BR-22 | Business logic / ownership (absent) | No owner, no entitlement, no pickup delay: any player in range may take any unowned ground object | `CItem.h:24-45`, `_MSG_GetItem.cpp` (absence of check) |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 - Session State Gate (Alive and In-Game)

**Overview**:
Every state-mutating handler in TMSrv opens with the identical "can this connection act"
gate. For pickup, the gate reads the authoritative HP of the character bound to the connection
slot and the connection's own mode, and refuses to touch the world when either is wrong.

**Detailed description**:
The condition at `_MSG_GetItem.cpp:24` is `pMob[conn].MOB.CurrentScore.Hp <= 0 ||
pUser[conn].Mode != USER_PLAY`. The first operand is the *derived* score (`CurrentScore`, not
`BaseScore`), meaning buffs, equipment and affects are already folded in; a character whose HP
was set to zero by combat resolution in `_MSG_Attack` or by `GetCurrentScore` is rejected even
if the character select screen still shows it as valid. The second operand distinguishes the
in-game mode (`USER_PLAY == 22`, `CUser.h:36`) from the other nine connection modes
(`USER_EMPTY 0`, `USER_ACCEPT 1`, `USER_LOGIN 2`, `USER_SELCHAR 11`, `USER_CHARWAIT 12`,
`USER_WAITDB 13`, `USER_SAVING4QUIT 24`). Because the dispatcher already rejected frames whose
`std->ID` is out of `0..MAX_USER-1` (`ProcessClientMessage.cpp:42`), the gate is not a
defensive bounds check but a true state check: it stops a client from picking items on the
character-select screen or while the DB layer is still flushing a logout.

The violation path is deliberately heavier than for other guards. `AddCrackError(conn, 1, 13)`
(`Server.cpp:684`) logs a "cra point" line for every type except 3, 8 and 15, increments
`pUser[conn].NumError` by one, and force-logs-out the connection when the counter reaches
2,000,000,000. Because this handler contributes exactly 1 per violation, and because
`NumError` is reset to zero on character login (`ProcessDBMessage.cpp:822`), on `_MSG_Restart`
(`_MSG_Restart.cpp:28`) and on several attack paths (`_MSG_Attack.cpp:1039, 1249, 1259`), the
counter is in practice unreachable: a client would need two billion rejected pickups without a
single relog. `SendHpMode` (`SendFunc.cpp:1622`) then re-projects HP and `Mode` back to the
client, which is the generic "re-synchronise the client's view of the character" mechanism used
by 23 handlers.

There is a subtle interaction with the drop/pickup pairing: the same Type=13 argument is used
for the pickup gate while `Exec_MSG_DropItem` uses 14 (`_MSG_DropItem.cpp:26`). The second
argument therefore encodes the handler identity in the audit trail, not the rule identity; a
log analysis that groups by type can distinguish "pickup by a dead/absent character" from
"drop by a dead/absent character".

**Rule workflow**:
```
if (pMob[conn].MOB.CurrentScore.Hp <= 0 || pUser[conn].Mode != USER_PLAY)
    AddCrackError(conn, 1, 13)      // log + NumError += 1 + possible CharLogOut
    SendHpMode(conn)                // re-project HP/Mode to the client
    return                          // no world mutation
```

---

### Business Rule: BR-02 - Open Trade Force-Termination

**Overview**:
A pickup request arriving while the character is inside a player-to-player trade window does
not participate in the trade invariants; the server closes both sides of the trade and drops the
request.

**Detailed description**:
`pUser[conn].Trade.OpponentID` is the index of the partner connection in an active
`MSG_Trade` exchange (`CUser.h:59`). When it is non-zero the handler calls
`RemoveTrade(pUser[conn].Trade.OpponentID)` and then `RemoveTrade(conn)` and returns
(`_MSG_GetItem.cpp:31-36`). `RemoveTrade` (`Server.cpp:7319`) is the canonical teardown: it
memsets the trade structure, resets every `Trade.InvenPos[i]` to -1, clears the auto-trade
structure and its `CarryPos[]` array to -1, sends client signal 900 to the local client, and - if
the character was in auto-trade visual mode - broadcasts a fresh `MSG_CreateMob` to rebuild the
character in the view of nearby players.

The ordering of the two calls is significant. The partner is torn down *first*, so the partner
receives the 900 signal even when the local `RemoveTrade` early-returns. The local call can
early-return at three points: `conn <= 0 || conn >= MAX_USER`, the state clearing still happens
before that check, and `pUser[conn].Mode != USER_PLAY` returns after the arrays are cleared. In
practice this means the trade structures are always cleared for both parties, but the trade-window
signal and the visual rebuild may be skipped for a connection that is already leaving.

The rule is a transaction-isolation choice rather than a validation: the pickup is not queued or
deferred until the trade ends, and the client receives no explicit "pickup failed because of
trade" message. The player observes the trade window closing instead. This matches the pattern
used by `_MSG_DropItem` (`_MSG_DropItem.cpp:31-36`) and the shop handlers, so the pickup rule is
part of a cross-handler convention: *any* inventory-mutating request implicitly cancels a trade.

**Rule workflow**:
```
if (pUser[conn].Trade.OpponentID != 0)
    RemoveTrade(partner)     // clears MSG_Trade + MSG_SendAutoTrade, signal 900, rebuild visuals
    RemoveTrade(conn)        // same for the requester
    return                   // request discarded, never retried
```

---

### Business Rule: BR-03 - Auto-Trade (Vending) Mode Lock

**Overview**:
Characters standing in automatic-trade mode cannot pick items up; the request is refused with
the same localized message that blocks selling, dropping, questing and item use.

**Detailed description**:
`pUser[conn].TradeMode` is the flag that keeps the character in the "open shop / vending"
pose. While it is set, the character is expected to be immobile and to interact only through
`_MSG_SendAutoTrade`-family messages. The handler therefore rejects the pickup with
`g_pMessageStringTable[_NN_CantWhenAutoTrade]` where `_NN_CantWhenAutoTrade == 170`
(`Language.h:190`) and returns (`_MSG_GetItem.cpp:38-42`). The identical message index is used at
`_MSG_Attack.cpp:29`, `_MSG_Sell.cpp:40`, `_MSG_DropItem.cpp:40`, `_MSG_Quest.cpp:44`,
`_MSG_SendAutoTrade.cpp:41` and `_MSG_TradingItem.cpp:40`, which makes this a project-wide rule:
auto-trade freezes all normal interactions.

The placement of the check matters relative to BR-02. A character that is trading *and* in auto
trade mode is handled by the trade branch first (both trades closed, silent return), so the
message is only produced when `Trade.OpponentID == 0` but `TradeMode != 0` - i.e. the character
is vending without an open trade window. Unlike BR-02, this branch does not clear `TradeMode`
and does not call `RemoveTrade`; the vending state survives the rejected request.

The message is delivered through `SendClientMessage` (`SendFunc.cpp:27`), which packs the text
into a fixed `MSG_MessagePanel` of `MESSAGE_LENGTH` bytes and forces the last two bytes to zero.
Since the table strings are loaded from `UI/strdef.bin` through an XOR-0x5A decode
(`Basedef.cpp:4932-4958`), the actual natural-language text is data, not code, and cannot be
verified from this repository.

**Rule workflow**:
```
if (pUser[conn].TradeMode != 0)         // only reached when no trade opponent
    SendClientMessage(conn, table[170]) // "cannot do this while auto-trading"
    return                              // vending state preserved
```

---

### Business Rule: BR-04 - Destination Container Restriction

**Overview**:
A pickup may only target the inventory ("carry") container. Equipment slots and cargo/storage
are invalid destinations and are rejected before any world object is touched.

**Detailed description**:
`MSG_GetItem.DestType` (`Basedef.h:1623`) tells the server which container the client wants the
item placed in. The engine defines three containers (`Basedef.h:86-88`):
`ITEM_PLACE_EQUIP 0`, `ITEM_PLACE_CARRY 1`, `ITEM_PLACE_CARGO 2`. The handler accepts exactly
`ITEM_PLACE_CARRY` and logs everything else as `"DEBUG:GetItem with wrong desttype"`
(`_MSG_GetItem.cpp:44-48`). This prevents a client from using the pickup packet to equip an item
directly (bypassing `BASE_CanEquip`, `Basedef.cpp:4293`) or to write into the cargo container
(whose slot count `MAX_CARGO` is unrelated to `MAX_CARRY`).

The rejection path is a log-and-return with no client feedback, which is consistent with the
treatment of an internally impossible request: the shipped client never sends anything but
`DestType = 1` for this opcode. Because it is the very first check that reads a *payload* field
(other than through the shared session gate), it is also the earliest point at which a malformed
packet can be distinguished from a legitimate one.

One consequence is that `DestPos` has container-specific meaning but is validated later
(BR-16) against the carry array only. There is no code path in this component where
`DestPos` is interpreted as an equipment slot or a cargo slot, so the later
`pMob[conn].MOB.Carry[m->DestPos]` access (line 212) is never reached with a non-carry
interpretation.

**Rule workflow**:
```
if (m->DestType != ITEM_PLACE_CARRY /* 1 */)
    Log("DEBUG:GetItem with wrong desttype", account, ip)
    return
```

---

### Business Rule: BR-05 - World Item Handle Decoding and Range

**Overview**:
The wire field `ItemID` is an opaque handle, not an index; it is the world-object slot offset by
10000. Only handles in `10001..14999` can address a real object.

**Detailed description**:
`CreateItem` publishes each ground object with `sm.ItemID = empty + 10000`
(`Server.cpp:7298`), and `ProcessDecayItem` uses the same offset when it announces expiry
(`Server.cpp:8391`). The handler reverses that transform at line 50 (`int itemID = m->ItemID -
10000;`) and then requires `itemID > 0 && itemID < MAX_ITEM` where `MAX_ITEM == 5000`
(`Basedef.h:103`). The lower bound excludes slot 0, which `GetEmptyItem` never returns
(`Server.cpp:6756` starts at 1), and the upper bound keeps every subsequent
`pItem[itemID]` access inside the global array `pItem[MAX_ITEM]` (`Server.h:235`,
`CItem.h:45`).

The arithmetic is done in `int` after promotion of the `unsigned short` field, so a handle below
10000 becomes negative and is rejected by the same comparison; a handle of 0 becomes -10000.
There is no attempt to normalise or to answer the client, which means a forged handle produces
silence rather than an error message - a deliberate anti-enumeration choice consistent with
BR-09 and BR-08.

The immediate re-test at line 55 repeats the identical `itemID <= 0 || itemID >= MAX_ITEM`
condition inside a larger disjunction. Because lines 52-53 already returned for those values,
the repeated operands are unreachable and only `pItem[itemID].Mode == 0` can fire. This is a
decompilation artefact that nevertheless documents the original author's intent to have a single
existence check that is safe on its own; the effective rule is therefore "handle decodes to a
valid slot **and** that slot is occupied by a live ground object".

**Rule workflow**:
```
itemID = (int)m->ItemID - 10000
if (itemID <= 0 || itemID >= 5000) return          // silent, no world access
if (itemID <= 0 || itemID >= 5000 || pItem[itemID].Mode == 0)   // second test: Mode only
    ... ghost cleanup path (BR-06) ...
```

---

### Business Rule: BR-06 - World Object Existence and Ghost-Item Cleanup

**Overview**:
If the addressed world slot is not occupied (`Mode == 0`), the pickup fails and the server sends
a *private* decay packet to the requester so the client erases the object it still believes in.

**Detailed description**:
`pItem[].Mode` is the world-object occupancy flag maintained by `CItem` (`CItem.h:28`):
`CreateItem` sets 1 (`Server.cpp:7272`), `CreateTreasureBox` sets 2 (`Server.cpp:9408`), and both
`ProcessDecayItem` (`Server.cpp:8382`) and this handler (lines 109, 160, 249) set 0. A value of 0
therefore means "the object has already been picked up, decayed, or never existed". The handler
treats this as a client-state defect rather than a cheat: it builds a `MSG_DecayItem`
(`Basedef.h:1609-1616`) with `ID = ESCENE_FIELD` (30000, the server-as-actor sentinel,
`Basedef.h:170`), `ItemID = m->ItemID` and `unk = 0`, and pushes it directly to the requesting
socket with `pUser[conn].cSock.AddMessage` (`_MSG_GetItem.cpp:62-70`). No other player is told
anything; the item was never visible to anyone else.

The logging inside this branch is conditional in a way that is easy to misread: the outer
condition already implies `pItem[itemID].Mode == 0`, so the inner `if (pItem[itemID].Mode)` at
line 57 is always false through this entry point. The `"GetItemFail idx:%d mode:%d"` line
therefore never executes in practice. Its intended purpose - recording a *rejected* pickup of an
object that exists but cannot be taken - is not served by this code path; the equivalent
diagnostics live in BR-07 (`GetItemFail pos...`) and BR-16 (`Trading Fails`).

The client-side effect of the private decay is a local, non-broadcast removal. Two players
clicking the same object in the same tick therefore produce: winner commits (broadcast decay),
loser receives a private decay for an object that is already gone from its own screen. The loser
sees nothing anomalous, which is why the rule is expressed as a resynchronisation rather than an
error.

**Rule workflow**:
```
if (pItem[itemID].Mode == 0)                // object gone
    if (pItem[itemID].Mode) Log(...)        // dead branch (never taken here)
    sm_deci = {Type=_MSG_DecayItem, ID=ESCENE_FIELD, ItemID=m->ItemID}
    AddMessage(conn, sm_deci)               // private, not GridMulticast
    return                                  // world untouched
```

---

### Business Rule: BR-07 - Proximity Requirement (7x7 Pickup Box)

**Overview**:
A player may only pick up an object inside a 7x7 box centred on the object: at most 3 tiles of
absolute difference on each axis, using the server's authoritative target position.

**Detailed description**:
The comparison uses `pMob[conn].TargetX` / `TargetY` - the position the server has accepted for
the character through movement handling - and *not* any client-supplied coordinate from
`MSG_GetItem`. This is the reason the pickup packet carries `GridX/GridY` (the item's tile for
attestation, BR-14) but no player coordinate: the client cannot assert where it stands. The
geometry is a Chebyshev-style box, not a radius, because the four comparisons are independent:
`TargetX < PosX-3 || TargetY < PosY-3 || TargetX > PosX+3 || TargetY > PosY+3`.

The 3-tile allowance is generous relative to the 33x33 view grid (`VIEWGRIDX/Y`,
`Basedef.h:95-96`) and to `HALFGRIDX/Y` of 16, so an object that is visible is nearly always
reachable; the gate exists to stop teleport-distance pickups from a modified client rather than
to enforce realistic reach. Note the deliberate asymmetry with the coordinate-integrity rule
(BR-12): the item position is trusted here for arithmetic even before its range is verified,
because the range check at line 74 happens *before* line 157. For a corrupt `PosX` of, say,
-20000 the comparison still behaves sanely (all four comparisons simply fail for a normal
`TargetX`), so no out-of-bounds access occurs at this point.

The failure path writes a formatted diagnostic that has a duplicated field name:
`"GetItemFail posx:%d posx:%d tx:%d ty:%d"` with arguments `(PosX, PosY, TargetX, TargetY)`
(line 77). The second `posx` is a literal typo for `posy`, so automated log parsing cannot
separate the two coordinates by name. The record is written through `Log(temp, account, ip)`
(`Server.cpp:6701`), which prefixes a timestamp and dotted IP.

**Rule workflow**:
```
if (TargetX < PosX-3 || TargetY < PosY-3 || TargetX > PosX+3 || TargetY > PosY+3)
    Log("GetItemFail posx:%d posx:%d tx:%d ty:%d", PosX, PosY, TargetX, TargetY)
    return
```

---

### Business Rule: BR-08 - Level-1000 Restriction on World Slot 1727

**Overview**:
A ground object whose *world slot index* equals 1727 may only be picked up by a character of
level 1000 or higher.

**Detailed description**:
Line 84 reads `if (itemID == 1727 && pMob[conn].MOB.CurrentScore.Level < 1000) return;`. The
left operand is the decoded world-object slot computed at line 50, not the archetype
`ditem->sIndex`. The evidence that the author intended the archetype is external and strong:
`ProcessDecayItem` skips the decay of items with `pItem[ItemCount].ITEM.sIndex == 1727`
(`Server.cpp:8362`), which is the only other place in the codebase that treats 1727 specially.
The two checks would only agree when the object with archetype 1727 happens to be stored in world
slot 1727.

Two behavioural consequences follow. First, the rule is unstable: whether a pickup is gated
depends on global allocation order, so the same character and the same item type can be blocked
or allowed depending on how many other ground objects exist at that moment. Second, the intended
protection (whatever archetype 1727 is - the item list is not in the repository, so the
archetype cannot be named) is not enforced at all, while an unrelated object inherits a
level-1000 requirement whenever it lands in that slot.

The rejection is completely silent: no log, no message, no decay, no resync. A legitimate
character below the threshold thus experiences a pickup that produces no visible response of any
kind, which is indistinguishable from packet loss. This is the only guard in the handler that
neither logs nor notifies. Confidence in the mismatch interpretation: **high** for the code
fact (slot vs archetype comparison, verified by the two call sites); the *intent* behind the
number 1727 is inferred and cannot be confirmed without `itemlist.csv`.

**Rule workflow**:
```
if (itemID == 1727)                     // world slot, not sIndex
    if (CurrentScore.Level < 1000)
        return                          // silent
```

---

### Business Rule: BR-09 - Item Archetype Sanity

**Overview**:
The object's archetype index must be a valid entry of the static item list before any item
semantics (currency, quest pill, announcement band) are evaluated.

**Detailed description**:
`ditem->sIndex` is a `short` read out of the world object (`STRUCT_ITEM.sIndex`,
`Basedef.h:400`). The guard requires `sIndex > 0 && sIndex < MAX_ITEMLIST`
(`MAX_ITEMLIST == 6500`, `Basedef.h:139`). This is the precondition for every later
`g_pItemList[sIndex]` access - directly at line 131 for the announcement text and indirectly
through `BASE_GetItemAbility` (`Basedef.cpp:1647`, which repeats the same range test at line
1653) and through `BASE_GetItemCode` (`Basedef.cpp:1479`). Because negative `short` values are
possible (a corrupted or hostile `STRUCT_ITEM` copied into a world object), the lower bound is
not redundant.

The failure is silent. A hand-crafted object with an invalid archetype can therefore never be
picked up: the transfer path is unreachable, so an invalid archetype cannot enter an inventory
through this handler. This is the component's most important referential-integrity property: it
makes `Carry[]` entries written by pickup always reference a defined archetype, which downstream
consumers (`_MSG_UseItem`, `_MSG_Sell`, `BASE_CanEquip`) rely on.

`BASE_GetItemAbility` is called twice in the money path with the redundant `(STRUCT_ITEM *)ditem`
cast (lines 184, 187), even though `ditem` is already declared as `STRUCT_ITEM *` (line 88). The
cast is a decompiler artefact and has no runtime effect; it does, however, document that the
money decode intentionally goes through the generic ability layer rather than reading
`stEffect[]` directly, so any future rule added to `BASE_GetItemAbility` for effects 36/37
propagates into pickup automatically.

**Rule workflow**:
```
sIndex = pItem[itemID].ITEM.sIndex
if (sIndex <= 0 || sIndex >= 6500) return      // silent, no item semantics evaluated
```

---

### Business Rule: BR-10 - Archetype 470: One-Shot Skill-Point Pill

**Overview**:
Archetype 470 is not an inventory item at all; picking it up consumes it immediately, awards
+9 skill points, sets a permanent per-character quest flag, and broadcasts the consumption. It is
idempotent per character: a second attempt is refused with a message.

**Detailed description**:
The branch at lines 94-127 is the first of the two early-commit paths. It is selected purely by
archetype (`ditem->sIndex == 470`), before the announcement band, before coordinate validation,
and before the currency/inventory split. Inside, the handler branches on
`pMob[conn].extra.QuestInfo.Mortal.PilulaOrc` - a `char` field in the nested quest structure
`QuestInfo.Mortal` (`Basedef.h:494-504`). If it is already set, the player receives
`_NN_Youve_Done_It_Already` (index 71) via `BASE_GetLanguage` (`Basedef.cpp:1258`) and the
object is left untouched on the ground: the branch returns without clearing anything, so the
pill remains available to other characters.

On the first pick, five mutations happen in a fixed order. (1) `_NN_Get_Skill_Point` (index 1)
is sent. (2) `PilulaOrc = 1`, making the award permanent. (3) `BASE_ClearItem(ditem)`
(`Basedef.cpp:4281`) zeroes `sIndex` and all three effect pairs. (4)
`pItemGrid[PosY][PosX] = 0` and `pItem[itemID].Mode = 0` retire the world object. (5)
`SkillBonus += 9` where `SkillBonus` is an `unsigned short` (`Basedef.h:467`), so the increment
wraps at 65536 rather than saturating. The order means the flag is committed before the world
object is retired, so a crash between (2) and (4) would leave a consumed pill on the ground for
a character that can no longer benefit from it.

Client-visible effects are richer than for a normal pickup: `GridMulticast(PosX, PosY, ...)`
with the decay packet (`SendFunc.cpp:843`), `SendEmotion(conn, 14, 3)` (`SendFunc.cpp:825`,
motion 14 parameter 3 - the same pair used for level-up at `SendFunc.cpp:947`), and `SendEtc`
(`SendFunc.cpp:1195`) to refresh `Exp/SpecialBonus/Learn/ScoreBonus/SkillBonus/Coin/Hold/Magic`.
`SendEtc` is essential here because the skill-point total is not part of any inventory message.
No `ItemLog` record is written for this consumption, which is the audit gap noted in Section 1.3.

Two robustness observations follow from the code as written. The grid clear at line 108 indexes
`pItemGrid[PosY][PosX]` directly, without the coordinate range validation that the generic path
performs at line 157, so a world object with corrupt coordinates takes an out-of-bounds write
into the 4096x4096 `pItemGrid` array (`Server.h:55`) instead of being safely discarded. And the
`sIndex == 470` test happens before `sIndex < MAX_ITEMLIST`? No - the archetype sanity rule
(BR-09) already returned for invalid archetypes, so 470 is guaranteed to be a declared entry.

**Rule workflow**:
```
if (sIndex == 470)
    if (QuestInfo.Mortal.PilulaOrc != 0)
        SendClientMessage(conn, table[71])      // already done
    else
        SendClientMessage(conn, table[1])       // "you got skill points"
        QuestInfo.Mortal.PilulaOrc = 1
        BASE_ClearItem(ITEM)
        pItemGrid[PosY][PosX] = 0
        pItem[itemID].Mode = 0
        GridMulticast(PosX, PosY, _MSG_DecayItem)
        MOB.SkillBonus += 9                     // unsigned short wrap
        SendEmotion(conn, 14, 3)
        SendEtc(conn)
    return                                      // never reaches the generic commit
```

---

### Business Rule: BR-11 - Archetypes 490..499: Global Announcement Band

**Overview**:
Objects in archetype range 490-499 trigger a server-wide "player X got item Y" notice and an
early decay multicast, but do **not** bypass the normal pickup rules - the function continues
into the generic path afterwards.

**Detailed description**:
The band test is `sIndex >= 490 && sIndex < 500`, i.e. ten archetypes (lines 129-143). The text
is built with `sprintf(temp, g_pMessageStringTable[_SSD_S_get_S], MobName, g_pItemList[sIndex].Name)`
where `_SSD_S_get_S == 198` (`Language.h:218`) and the format string itself comes from the
message table. `SendNotice` (`SendFunc.cpp:47`) prefixes the text with `"not "` into a 512-byte
local buffer, writes it to the system log, and re-sends it to every connection in `USER_PLAY`.
Both `MobName` (character name, up to `NAME_LENGTH`) and the item name are interpolated into a
format string that is supplied at runtime, so a malformed or hostile `strdef.bin` entry with
extra conversion specifiers would read stack arguments; the table is a server-side asset, not
client input, so the practical exposure is limited to operator-supplied data.

The decisive structural property is the **absence of a `return`** at the end of the block. After
announcing, the handler rebuilds its own `MSG_DecayItem` (lines 134-142) and multicasts it to the
33x33 view, then falls through to the common code. The object is therefore visually removed from
every nearby screen at this point, while the authoritative removal happens much later at lines
246-249. If any subsequent guard fails - coordinate integrity (161), grid registration (171),
client attestation (177), occupied inventory slot (224), or the currency ceiling (196) - the
object stays alive in `pItem[]` with `Mode == 1` and remains registered in `pItemGrid`, but every
client that received the early multicast no longer renders it. The result is a "ghost" object
that occupies a world slot and a grid cell and can never be picked up again until it decays: the
`pItemGrid[y][x] == itemID` test at line 164 still succeeds, but the client will not send a
pickup for an item it cannot see.

The duplicate multicast is also a bandwidth anomaly: peers in range receive `_MSG_DecayItem` for
the same `ItemID` twice on the success path (once at line 142, once at line 246). Re-sending a
decay for an already-removed object is harmless to the client, but it doubles the packet count
for the most expensive notification class in the handler.

**Rule workflow**:
```
if (sIndex >= 490 && sIndex < 500)
    sprintf(temp, table[198], MobName, g_pItemList[sIndex].Name)
    SendNotice(temp)                            // all USER_PLAY clients + system log
    GridMulticast(PosX, PosY, _MSG_DecayItem)   // EARLY visual removal
    // no return - continues to the generic path
```

---

### Business Rule: BR-12 - Server-Side Coordinate Integrity

**Overview**:
Before an object may be placed in an inventory, its own world coordinates must be inside the
world grid: `0 <= PosX < MAX_GRIDX` and `0 < PosY < MAX_GRIDY`. A violation deletes the object
outright and tells only the requester.

**Detailed description**:
`MAX_GRIDX` and `MAX_GRIDY` are both 4096 (`Basedef.h:100-101`), matching the declaration of
`pItemGrid[MAX_GRIDY][MAX_GRIDX]` (`Server.h:55`). The check at line 157 reproduces `PosX` and
`PosY` into locals `itemX`, `itemY` (lines 145-146) and then applies
`itemX < 0 || itemX >= MAX_GRIDX || itemY <= 0 || itemY >= MAX_GRIDY`. The `y` comparison uses
`<= 0` while the `x` comparison uses `< 0`, so row 0 is treated as invalid while column 0 is
valid. `CreateItem` and `GetEmptyItemGrid` (`GetFunc.cpp:1876`) never produce row 0, so the
asymmetry is currently invisible in normal operation; it becomes observable only for objects
placed by direct memory manipulation, by map-editor tooling, or by a future world layout that
uses row 0.

The violation path is the second early-commit site: it sends the requester a private
`_MSG_DecayItem` and sets `pItem[itemID].Mode = 0`, but it does **not** clear the corresponding
`pItemGrid` cell. That cell is, by definition, not the item's own coordinates (which are
invalid), so the intent appears to be "abandon the corrupt object without touching any grid
data". If the corrupt object had previously been registered under a valid cell, that cell keeps
pointing at `itemID` while `Mode` is 0, and a later pickup of *that* cell will pass BR-06 (Mode
must be non-zero to reach here) - so the cell becomes permanently dead until `CreateItem`
overwrites it.

Because `itemX`/`itemY` have already been copied into the outgoing `MSG_DecayItem`
(`sm_deci.ItemID = m->ItemID`, lines 148-155), the packet identifies the object by handle, not by
position, so the client can remove it regardless of whether it believed in the corrupt
coordinates. The ordering - decay packet constructed before validation, sent only after
validation fails - is a small but deliberate read of the code.

**Rule workflow**:
```
itemX = PosX; itemY = PosY
build MSG_DecayItem{ID=ESCENE_FIELD, ItemID=m->ItemID}
if (itemX < 0 || itemX >= 4096 || itemY <= 0 || itemY >= 4096)
    AddMessage(conn, sm_deci)      // private removal
    pItem[itemID].Mode = 0         // object abandoned (grid cell not touched)
    return
```

---

### Business Rule: BR-13 - World Grid Ownership and Self-Healing Registration

**Overview**:
The object must be the occupant of the grid cell at its own coordinates. If the cell is empty
the handler re-registers the object (a repair path); if the cell holds a different object the
pickup is refused and only the requester is told to erase the item.

**Detailed description**:
`pItemGrid[y][x]` stores the *item index* (`CreateItem`: `pItemGrid[y][x] = empty`,
`Server.cpp:7287`) - not the plus-10000 handle - so the comparison at line 164 is
`pItemGrid[itemY][itemX] != itemID` with both sides in the same index space. This is the
component's invariant that "an object is pickable only from the cell it is registered in". It
prevents a modified client from claiming `GridX/GridY` of a *different* cell (though BR-14
already forces the client coordinates to match the object's own), and, more importantly, it
detects engine-level divergence caused by the known index-order defect in `ProcessDecayItem`,
which clears `pItemGrid[itemPosX][itemPosY]` (`Server.cpp:8381`) using swapped axes relative to
every other writer.

The repair sub-rule is the interesting part. If the cell is empty (`!pItemGrid[itemY][itemX]`),
the handler writes `pItemGrid[itemY][itemX] = itemID` before returning (lines 168-169). The
current request still fails - the client receives the private decay - but the world state is
restored so that a subsequent pickup succeeds. This is a self-healing registration: the object
becomes reachable again after exactly one wasted request. If the cell is occupied by a different
`itemID`, no repair is attempted, because overwriting would destroy the other object's
registration; the object stays orphaned in `pItem[]` until `ProcessDecayItem` retires it.

Note that the refusal does not set `Mode = 0`, unlike BR-12. The object therefore survives with
a live `Mode` but an unusable registration - a third distinct "half-alive" state, and the
mechanism by which a ghost object can persist for up to its remaining decay time (up to
`Delay 90` seconds from `CreateItem`, `Server.cpp:7282`).

**Rule workflow**:
```
if (pItemGrid[itemY][itemX] != itemID)
    AddMessage(conn, sm_deci)                  // private decay to the requester
    if (pItemGrid[itemY][itemX] == 0)
        pItemGrid[itemY][itemX] = itemID       // self-heal registration
    return                                     // Mode stays 1
```

---

### Business Rule: BR-14 - Client Grid Attestation

**Overview**:
The client must state the tile the item appears on, and that tile must equal the server's own
record. A mismatch is treated as a client-side inconsistency and answered with a private decay.

**Detailed description**:
`MSG_GetItem` carries `unsigned short GridX, GridY` (`Basedef.h:1626`). The comparison at line
174 is `itemX != m->GridX || itemY != m->GridY`, i.e. exact equality; there is no tolerance,
because these are the item's coordinates rather than the player's. The purpose is not to
authorise the transfer (the server already knows where the item is) but to detect and repair a
desynchronised client: if the client believes the item is on a different tile than the server,
the safest response is to erase it locally and force the client to rebuild from the next
`_MSG_CreateItem`.

Because `GridX`/`GridY` are `unsigned short`, the comparison with the `int` locals promotes them
to non-negative values, so no negative client value can slip through; the incoming 0..65535 range
is fully handled. A client that sends coordinates for a *different* legitimate item still fails
here, since BR-13 has already established that `itemID` owns its own cell.

The rule is the last of the four "world object is where we think it is" gates and the last guard
before the item's payload (volatility) is even read. Only after this point does the handler
decide whether the target is currency or an inventory item.

**Rule workflow**:
```
if (itemX != m->GridX || itemY != m->GridY)
    AddMessage(conn, sm_deci)     // private decay, no state change
    return
```

---

### Business Rule: BR-15 - Currency Pickup: 16-Bit Decode and 2,000,000,000 Ceiling

**Overview**:
Objects whose `EF_VOLATILE` ability equals 2 are money. Their amount is a 16-bit big-endian pair
of effects; the pickup is refused if it would push the purse to or beyond 2,000,000,000 coins.

**Detailed description**:
The volatility class is read once at line 180 through `BASE_GetItemAbility(ditem, EF_VOLATILE)`
where `EF_VOLATILE == 38` (`ItemEffect.h:77`). The effect-code comment in `ItemEffect.h`
documents the class table (0..7) whose meaning is "which use-handler applies", and
`BASE_GetItemAbility` deliberately excludes `EF_VOLATILE` from sanctification scaling
(`Basedef.cpp:1813`), so the returned value is the raw effect byte - a money object cannot be
turned into a non-money object by sanctification. Class 2 is the currency class; classes 1
(potion), 3, 4, 5, 6 and 7 all fall through to the inventory path, which is why potions picked up
from the ground occupy a slot instead of stacking.

The amount is reconstructed as `(EF_HWORDCOIN << 8) + EF_LWORDCOIN` using the effect constants 36
and 37 (`ItemEffect.h:75-76`) - a 16-bit big-endian value, so a single ground money object
carries at most 65,535 coins. The sum is computed with `BASE_GetItemAbility` twice, i.e. the
generic effect accumulation (static list effects plus the three instance effects, `Basedef.cpp`
1698-1788) rather than a direct `stEffect[]` read, so a money item defined with static list
effects sums both sources.

The ceiling rule is `int tcoin = coin1 + MOB.Coin; if (tcoin >= 2000000000) { SendClientMessage(
table[273]); return; }`. The comparison is `>=`, so exactly 2,000,000,000 is already too much;
the same constant appears as a clamp in `_MSG_Sell.cpp:215-218` and
`_MSG_UseItem.cpp:2263-2275`, establishing 2e9 as the domain-wide purse maximum. Because the
check precedes the mutation, the over-cap case is completely side-effect free: the money object
stays on the ground with `Mode == 1` and its grid registration intact, and the player can retry
after spending coins. Message index 273 has no symbolic name in `Language.h` and its text is in
`UI/strdef.bin`, so the wording cannot be quoted from source; only the index is verifiable.

On success the purse is credited, the requester receives the private decay packet, and
`BASE_ClearItem(ditem)` zeroes the payload. Notably, the handler does **not** call `SendEtc` in
this branch - unlike `_MSG_Sell.cpp:233` - so the client's coin display is only refreshed by
whatever projection follows (there is none in this component). No `ItemLog` record is written for
currency, so the economy path is invisible in `item_log`. Both the flow then continues to the
shared commit block (lines 234-251), which broadcasts the decay and retires the object.

**Rule workflow**:
```
Vol = BASE_GetItemAbility(ITEM, EF_VOLATILE)      // raw, no sanc scaling
if (Vol == 2)
    coin = (BASE_GetItemAbility(ITEM, EF_HWORDCOIN) << 8)
         +  BASE_GetItemAbility(ITEM, EF_LWORDCOIN)
    if (MOB.Coin + coin >= 2000000000)
        SendClientMessage(conn, table[273])
        return                                    // item stays on the ground
    MOB.Coin += coin
    AddMessage(conn, sm_deci)                     // private decay
    BASE_ClearItem(ITEM)
    // falls through to the shared commit (broadcast + teardown)
```

---

### Business Rule: BR-16 - Inventory Destination Range

**Overview**:
The destination slot must be a valid flat index into `Carry[]`: `0 <= DestPos < MAX_CARRY` (64).

**Detailed description**:
`DestPos` is a signed `int` from the packet (`Basedef.h:1624`) and is checked against
`MAX_CARRY == 64` (`Basedef.h:76`), which is the declared size of `STRUCT_MOB.Carry[MAX_CARRY]`
(`Basedef.h:459`). The check is container-agnostic - it does not consult `pMob[conn].MaxCarry` -
so all 64 slots are addressable even though the usable inventory is smaller. `MaxCarry` is
computed as 30 plus 15 per "carry expansion" item (archetype 3467) found in slots 60 and 61
(`ProcessDBMessage.cpp:751-757`, recomputed in `_MSG_UseItem.cpp:5688-5694` and
`ProcessSecMinTimer.cpp:576-590`), giving a maximum of 60.

The gap between 64 and 30..60 is a genuine rule hole rather than an accident of the check: slots
`MaxCarry..63` are the expansion/pouch area, and slots 60 and 61 hold the expansion items
themselves. A pickup into an empty slot 45 with `MaxCarry == 30` succeeds, producing an item that
the client's inventory window does not draw and that the player cannot normally move. The
failure path is a log with the message text `"DEBUG:Trading Fails.(Wrong source position)"`
(line 208) - the wording is copied from the trading handler and does not describe a pickup.

The `log`-only rejection means an attacker probing the slot space learns nothing from the
protocol; but it also means a legitimate client bug produces no user-visible feedback.

**Rule workflow**:
```
if (m->DestPos < 0 || m->DestPos >= 64)
    Log("DEBUG:Trading Fails.(Wrong source position)", account, ip)
    return
```

---

### Business Rule: BR-17 - Destination Slot Occupancy and Occupied-Slot Resynchronisation

**Overview**:
The destination slot must be empty. If it is occupied, the pickup is refused and the server
re-projects the *preceding* slot to the client instead of the pickup result.

**Detailed description**:
Emptiness is tested as `bItem->sIndex == 0` (line 215), i.e. only the archetype field is
consulted; a slot with zero archetype but non-zero effects is treated as empty and overwritten
by the subsequent `memcpy`. When the slot is occupied, the handler does **not** send any pickup
failure message. Instead it checks `m->DestPos > 0 && m->DestPos <= pMob[conn].MaxCarry` and, if
true, decrements `m->DestPos` in place and calls `SendItem(conn, ITEM_PLACE_CARRY, m->DestPos,
&pMob[conn].MOB.Carry[m->DestPos])` (lines 219-223). In other words it projects slot
`DestPos-1` and leaves the packet's own `DestPos` field mutated for no further use.

The intent, read from the effect, is a "nudge the client's slot model" repair: the client is told
the authoritative content of the slot *before* the contested one, which forces it to redraw the
region. As written the rule has three anomalies. (1) The bound is `<= MaxCarry`, so a contested
slot at exactly `MaxCarry` projects `MaxCarry-1`, while a contested slot above `MaxCarry` (which
BR-16 allowed) produces no projection at all. (2) `DestPos == 0` produces no projection, since
the guard requires `> 0`; a contested slot 0 therefore yields complete silence. (3) The mutation
of `m->DestPos` writes into the receive buffer, which is reused by `CPSock` for subsequent
frames; the value is not read again in this handler, so the effect is contained, but the write
does contradict the otherwise read-only treatment of the packet.

Because the return at line 224 happens before `MSG_CNFGetItem`, the client receives no
confirmation for the pickup. A client that optimistically moved the item will resynchronise from
the `_MSG_SendItem` projection if one was sent, and otherwise remain optimistic until another
inventory update arrives. There is no server-side search for an alternative empty slot: capacity
management is entirely the client's responsibility, which is consistent with the absence of a
weight/space model (BR-18).

**Rule workflow**:
```
bItem = &MOB.Carry[DestPos]
can = (bItem->sIndex == 0)
if (!can)
    if (DestPos > 0 && DestPos <= MaxCarry)
        m->DestPos -= 1
        SendItem(conn, ITEM_PLACE_CARRY, m->DestPos, &Carry[m->DestPos])   // resync previous slot
    return                                                                // no CNF, no pickup
```

---

### Business Rule: BR-18 - Transfer Semantics: Raw Copy, No Stacking, No Footprint, No Weight

**Overview**:
The transfer is a `memcpy` of the 8-byte `STRUCT_ITEM` into the destination slot. There is no
merge with an existing stack, no footprint validation, and no weight or capacity accounting.

**Detailed description**:
`STRUCT_ITEM` is `short sIndex` plus three `short` effect unions (`Basedef.h:398-412`), i.e. 8
bytes plus padding; `memcpy(bItem, ditem, sizeof(STRUCT_ITEM))` (line 226) copies archetype and
all three effect pairs verbatim. Stacking is structurally impossible in this design: a stack is
represented by an amount effect inside a single item (see `BASE_GetItemAmount` usage in
`_MSG_UseItem.cpp:95, 139-143`), and the pickup path never looks at that amount, never adds it to
an existing same-archetype slot, and never splits it. Two potion objects on the ground become two
occupied slots. Since the destination must be empty (BR-17), a merge could not occur even if the
amount were read.

Footprint is likewise ignored. Items carry a 2x4 footprint pattern through `EF_GRID`
(`ItemEffect.h:72`, values clamped to 0..7 in `BASE_GetItemAbility`, `Basedef.cpp:1835-1839`), and
`BASE_CanCarry` (`Basedef.cpp:4413`) exists to test whether that footprint fits at a 9x7
inventory coordinate without overlapping existing items. `BASE_CanCarry` has no live call site:
its only reference outside its own definition is commented out (`Server.cpp:8574`). Pickup
therefore places a multi-cell item into a single flat index, and the client's own layout
algorithm has to reconcile the resulting overlap. The flat index space is 9 columns by 7 rows
(`CARRYGRIDX 9`, `CARRYGRIDY 7`, `Basedef.h:92-93`), so `DestPos` doubles as `row * 9 + col` in
the client's model - a convention this component never converts to or from.

Weight does not exist anywhere in the model: `STRUCT_MOB` has no weight field
(`Basedef.h:438-481`), `CItem` has none (`CItem.h:24-45`), and no handler in the tree consults
one. "Space" in this component is therefore exactly one boolean: is the requested flat slot's
`sIndex` zero.

**Rule workflow**:
```
bItem = &MOB.Carry[DestPos]
if (bItem->sIndex != 0) -> BR-17 refusal
memcpy(bItem, ditem, sizeof(STRUCT_ITEM))     // verbatim; no merge, no footprint, no weight
BASE_GetItemCode(bItem, itemLog)              // canonical "<name> : idx.e0.v0.e1.v1.e2.v2"
sprintf(temp, "getitem, %s", itemLog)
ItemLog(temp, account, ip)
```

---

### Business Rule: BR-19 - Item Audit Trail

**Overview**:
Every successful *item* transfer writes exactly one `item_log` line beginning with `getitem,`
containing the canonical item code and the account/IP of the taker.

**Detailed description**:
The log payload is produced by `BASE_GetItemCode` (`Basedef.cpp:1479`), which formats
`" %s : %d.%d.%d.%d.%d.%d.%d"` from the archetype name and the six effect bytes, after
zero-terminating the output when the archetype is out of range. It is then prefixed with
`"getitem, "` into the shared 4096-byte `temp` and written via
`ItemLog(temp, pUser[conn].AccountName, pUser[conn].IP)` (`Server.cpp:9264`), which stamps a
date/time and dotted IP and appends to the daily file opened by `StartItemLog`
(`Server.cpp:9247`, pattern `.\item_log\TM_dd_mm_yyyy_hh_mm_ss.txt`).

The complementary writer is `_MSG_DropItem.cpp:120-123`, which produces `dropitem, <code>`; the
two records are designed to be paired by archetype code, so a drop/get pair proves an item
travelled from one inventory to the ground and into another. Three gaps break that pairing:
currency pickups (BR-15) produce no record; the archetype-470 consumption (BR-10) produces no
record; and the `sIndex 490..499` announcement path logs only the notice
(`SendNotice` → `Log("not ...", "-system", 0)`) plus the normal `ItemLog` at line 231, so those
items are doubly logged while money is not logged at all.

Type-safety note: `itemLog` is a 2048-byte stack buffer and `BASE_GetItemCode` writes a bounded
string (`g_pItemList[].Name` is `ITEMNAME_LENGTH`), so the copy is bounded in practice, but the
`sprintf` into `temp[4096]` uses no width specifier.

**Rule workflow**:
```
itemLog[2048]
BASE_GetItemCode(bItem, itemLog)
sprintf(temp, "getitem, %s", itemLog)
ItemLog(temp, AccountName, IP)        // timestamped, daily file, no rotation guard on write
```

---

### Business Rule: BR-20 - Pickup Confirmation Message

**Overview**:
The picker receives `_MSG_CNFGetItem` with the destination type and slot echoed back. The
struct's `Item` field is declared but never populated.

**Detailed description**:
`MSG_CNFGetItem` is `_MSG` + `int DestType` + `int DestPos` + `STRUCT_ITEM Item`
(`Basedef.h:1629-1636`). The handler zeroes the whole struct, sets `Type`, `Size`, `ID =
ESCENE_FIELD`, then assigns only `DestPos` and `DestType` from the request and sends it to the
client (lines 234-244). `Item` therefore always arrives as eight zero bytes. Whether clients
tolerate that depends on client code outside this repository; within the server tree there is no
other producer of `_MSG_CNFGetItem`, so the empty field is the de-facto contract. The subsequent
`SendItem` (line 251) carries the authoritative slot content, which is presumably what clients
consume; acting on `Item` from the confirmation would be incorrect.

`DestPos` echoed here is the *validated* value. In the occupied-slot path (BR-17) the handler
mutates `m->DestPos` and returns before building this message, so the mutation never reaches the
client. `DestType` is necessarily 1 because BR-04 rejected everything else.

The message is queued via `pUser[conn].cSock.AddMessage` rather than `SendOneMessage`, so it is
flushed with the rest of the outbound buffer in the socket's write phase (`CPSock.cpp:586-588`).
The `ID = ESCENE_FIELD` value marks it as server-authored, the same convention used by
`_MSG_DecayItem` and `_MSG_CreateItem`.

**Rule workflow**:
```
MSG_CNFGetItem cnfGet = {0}
cnfGet.Type = _MSG_CNFGetItem; cnfGet.Size = sizeof(MSG_CNFGetItem); cnfGet.ID = ESCENE_FIELD
cnfGet.DestPos = m->DestPos; cnfGet.DestType = m->DestType
AddMessage(conn, cnfGet)              // cnfGet.Item stays zeroed
```

---

### Business Rule: BR-21 - World Teardown and View Broadcast

**Overview**:
After a successful transfer the server removes the object from the world grid, marks the world
slot free, broadcasts the removal to the 33x33 view, and re-projects the destination slot to the
picker.

**Detailed description**:
The shared commit block runs for both the currency and the inventory path: `GridMulticast(itemX,
itemY, (MSG_STANDARD*)&sm_deci, 0)` (line 246, `send` at `SendFunc.cpp:843`), then
`pItemGrid[itemY][itemX] = 0` and `pItem[itemID].Mode = 0` (lines 248-249), then `SendItem` of
the destination slot (line 251). `GridMulticast` walks `pMobGrid[y][x]` over
`VIEWGRIDX*VIEWGRIDY = 33*33` cells centred on the item, clamps at the world edges, and pushes
the packet only to connections whose mob mode is not `MOB_EMPTY` and whose index is below
`MAX_USER` (i.e. players, `SendFunc.cpp:875-971`). The `skip` argument is 0, so the picker also
receives the broadcast in addition to its private confirmation.

Grid bookkeeping uses `[Y][X]` order here, matching `CreateItem` (`Server.cpp:7287`) and
`GetEmptyItemGrid` (`GetFunc.cpp:1878`), and *not* matching `ProcessDecayItem`'s swapped
`[itemPosX][itemPosY]` write (`Server.cpp:8381`). One of the two conventions must be producing
stale cells, and this component's BR-13 mismatch test is what makes the divergence observable.

`SendItem` (`SendFunc.cpp:1055`) is guarded on the connection index, `USER_PLAY` mode, and a
non-zero socket, then sends `_MSG_SendItem {invType, Slot, item}` with the full `STRUCT_ITEM`.
The same projection is used by 385 call sites across 28 files, making it the canonical way to
tell a client "this is what slot N really contains". Because it is the last statement of the
function, its failure (buffer full) is expressed by `AddMessage` returning false internally and,
in other callers, by `CloseUser`; here the return value is ignored.

**Rule workflow**:
```
GridMulticast(itemX, itemY, _MSG_DecayItem, skip=0)   // all players in the 33x33 view
pItemGrid[itemY][itemX] = 0
pItem[itemID].Mode = 0
SendItem(conn, ITEM_PLACE_CARRY, m->DestPos, &MOB.Carry[m->DestPos])
```

---

### Business Rule: BR-22 - Ownership: Absent by Design (First-Come-First-Served)

**Overview**:
There is no ownership, entitlement, protection window, or kill-credit gate on ground objects.
Any character that is alive, in `USER_PLAY`, not trading or vending, within the 7x7 box, and
carrying a valid destination slot may take any object.

**Detailed description**:
The evidence is structural rather than a single statement, which is why the rule is recorded as
an explicit absence. `CItem` (`CItem.h:24-45`) declares `ITEM`, `Mode`, `PosX`, `PosY`,
`GridCharge`, `State`, `Rotate`, `Delay`, `Decay`, `Height`, `Money`, `Open`, `ItemQuest` and
`Unk[20]` - there is no owner, party, guild or timestamp-of-drop field. `CreateItem`
(`Server.cpp:7254-7317`) sets `Mode = 1`, `Rotate`, `State = STATE_OPEN`, `Delay = 90`,
`Decay = 0`, `GridCharge` and `Height`, and no ownership data, so nothing exists that this
handler could compare against.

`Delay` is frequently mistaken for a protection timer, but `ProcessDecayItem`
(`Server.cpp:8368-8372`) decrements it once per sweep and only then allows the `Decay` sweep to
retire the object; it is the object's lifetime head-start, not a lock. A pickup at the instant of
the drop is therefore fully legal, and the first player in range wins. The only ordering
protection comes from the grid/object consistency gates (BR-06, BR-13) and the natural
serialisation of the single-threaded Win32 message loop (`Server.cpp:3919-4004` reads and
dispatches on the UI thread), which means two requests in the same drain are processed
sequentially: the first sets `Mode = 0`, the second fails BR-06 and receives a private decay.

Aggravating factors are worth recording because they are consequences of this rule rather than
separate ones: the 3-tile range includes tiles the character may not be able to stand on; there
is no map/region or height check (`pHeightGrid` is consulted by `CreateItem` only through
`GetEmptyItemGrid`, and `GetFunc.cpp:1878` uses the magic value 127); and the level-1000 gate
(BR-08) is attached to a world slot rather than an archetype, so it does not function as an
entitlement filter.

**Rule workflow**:
```
// no ownership fields exist in CItem and no owner comparison appears in the handler
// effective entitlement = (alive) AND (USER_PLAY) AND (no trade) AND (not TradeMode)
//                       AND (within 3 tiles) AND (valid archetype) AND (valid destination)
```

---

## 4. Component Structure

### 4.1 Physical structure

```
Code/TMSrv/_MSG_GetItem.cpp                      # 252 lines, 6,797 bytes; the entire component
├── lines 1-17    GPLv3 header (2015, "VK, Charles TheHouse")
├── line 18       #include "ProcessClientMessage.h"    (sole include)
└── lines 20-252  void Exec_MSG_GetItem(int conn, char *pMsg)
    ├── 22           frame cast to MSG_GetItem*
    ├── 24-48        session / trade / vending / destination gates
    ├── 50-92        handle decode, existence, distance, special level, archetype sanity
    ├── 94-127       archetype 470 branch (early commit)
    ├── 129-143      archetype 490..499 announcement (early multicast, no return)
    ├── 145-178      coordinate integrity, grid ownership, client attestation
    ├── 180-232      volatility split: currency path / inventory path
    └── 234-251      shared commit: confirmation, broadcast, teardown, projection
```

Related declarations and registration:

```
Code/TMSrv/ProcessClientMessage.h:104            void Exec_MSG_GetItem(int conn, char *pMsg);
Code/TMSrv/ProcessClientMessage.cpp:202-204      case _MSG_GetItem: Exec_MSG_GetItem(conn, pMsg);
Code/TMSrv/TMSrv.vcxproj:137                     <ClCompile Include="_MSG_GetItem.cpp" />
Code/TMSrv/TMSrv.vcxproj.filters:255             <ClCompile Include="_MSG_GetItem.cpp">
Code/Basedef.h:1618-1627                         opcode constant + MSG_GetItem struct
Code/Basedef.h:1629-1636                         MSG_CNFGetItem (response)
Code/Basedef.h:1609-1616                         MSG_DecayItem (response)
```

### 4.2 The component's true boundary

The file is one translation unit with one function, but the *analysis* boundary is wider,
because the business logic is distributed across three collaborators it calls:

| Concern | Where the logic actually lives | Lines |
|---------|-------------------------------|-------|
| World-object allocation and identity | `CreateItem`, `GetEmptyItem`, `GetEmptyItemGrid` | `Server.cpp:7254`, `Server.cpp:6754`, `GetFunc.cpp:1876` |
| World-object retirement | `ProcessDecayItem` | `Server.cpp:8353-8396` |
| Item ability / volatility decode | `BASE_GetItemAbility`, `BASE_ClearItem`, `BASE_GetItemCode` | `Basedef.cpp:1647`, `4281`, `1479` |
| Inventory projection | `SendItem` | `SendFunc.cpp:1055` |
| View broadcast | `GridMulticast` | `SendFunc.cpp:843-972` |
| Trade teardown | `RemoveTrade` | `Server.cpp:7319-7352` |
| Anti-cheat counter | `AddCrackError` | `Server.cpp:684-707` |
| Character/mode resync | `SendHpMode`, `SendEtc` | `SendFunc.cpp:1622`, `1195` |
| Localized strings | `g_pMessageStringTable`, `UI/strdef.bin` | `Basedef.cpp:4932`, `Language.h` |

`_MSG_GetItem.cpp` owns **no** state, no memory, and no initialization order. All of its data is
borrowed from globals (`pItem`, `pItemGrid`, `pMob`, `pUser`, `g_pItemList`,
`g_pMessageStringTable`, `temp`), which is the dominant architectural characteristic of the
whole handler layer.

### 4.3 Size and complexity indicators (measured)

| Metric | Value | Method |
|--------|-------|--------|
| Lines | 252 | file length |
| Executable statements in the function | ~120 | counted manually |
| Cyclomatic complexity (approx.) | 34 | 1 + 33 decision points (`if` 20, compound operands 9, `?:` 1, else-if chain 3) |
| Maximum nesting depth | 3 | `if (can == 0) { if (DestPos...) { } }` at 217-225 |
| Distinct exit points | 17 | see Section 2.4 |
| Local variables | 10 (`itemID`, `ditem`, `sIndex`, `itemX`, `itemY`, `Vol`, `HWORDCOIN`, `coin1`, `tcoin`, `error`, `can`, `bItem`, `itemLog`) | lines 50-230 |
| Unused locals | 1 (`error`, line 214) | no read after assignment |
| Magic numbers | 10000, 1727, 470, 490, 500, 3, 2000000000, 273, 9 (skill points), 14/3 (emotion) | throughout |
| Global scratch usage | 1 (`temp`) | lines 59, 77, 131, 230 |

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains (efferent, outbound)

```
Exec_MSG_GetItem  (Code/TMSrv/_MSG_GetItem.cpp:20)
├── AddCrackError ──────────────► Log, SendClientMessage, CharLogOut
│                                  └── SaveUser, DeleteMob, SendClientSignal, SendBilling, RemoveTrade
├── SendHpMode ─────────────────► CPSock::AddMessage, CloseUser
├── RemoveTrade ────────────────► SendClientSignal, GetCreateMob, GridMulticast
├── SendClientMessage ──────────► CPSock::AddMessage
├── BASE_GetLanguage ───────────► g_pMessageStringTable (UI/strdef.bin)
├── Log ────────────────────────► fLogFile (fprintf), SetWindowText
├── BASE_GetItemAbility ────────► g_pItemList, g_pMountBonus, g_pMountTempBonus, BASE_GetItemSanc
├── BASE_ClearItem ─────────────► (leaf: zeroes STRUCT_ITEM)
├── BASE_GetItemCode ───────────► g_pItemList[].Name
├── ItemLog ────────────────────► fItemLogFile (fprintf), localtime
├── SendNotice ─────────────────► Log, SendClientMessage (fan-out to every USER_PLAY conn)
├── GridMulticast ──────────────► pMobGrid, pUser[].cSock.AddMessage, GetCreateMob, CheckGetLevel,
│                                  SetCircletSubGod, SendClientMessage, SendScore, SendEmotion,
│                                  SendEtc, GetPKPoint, SetPKPoint, DoItemLevel
├── SendEmotion ────────────────► GridMulticast
├── SendEtc ────────────────────► CPSock::AddMessage, CloseUser
└── SendItem ───────────────────► CPSock::AddMessage
```

Runtime transitive depth: 4 levels below the handler
(`Exec_MSG_GetItem → GridMulticast → SendEmotion → GridMulticast` is the worst cycle-free
nesting; `GridMulticast` can itself trigger level-up side effects at `SendFunc.cpp:920-966`,
which is where the fan-out to 11 further functions comes from).

### 5.2 External and platform dependencies

| Dependency | Kind | Use in this component | Notes |
|-----------|------|-----------------------|-------|
| Winsock 2 (`winsock2.h` via `Windows.h`) | Platform API | indirect, through `CPSock::AddMessage` | single-threaded message pump |
| Win32 `SetWindowText` (user32) | Platform API | indirect, in `Log` | log line mirrored to the server console window |
| MSVC CRT `sprintf`, `memcpy`, `memset` | Runtime library | direct (lines 59, 77, 131, 149, 226, 230, 235) | unsafe variants; `_CRT_SECURE_NO_WARNINGS` configured |
| `UI/strdef.bin` | Data asset | supplied by `BASE_ReadMessageBin` to `g_pMessageStringTable` | XOR 0x5A obfuscation; **absent from the repository** |
| `itemlist.csv` (`../../Common/`) | Data asset | `g_pItemList[].Name`, abilities | **absent from the repository** |
| `item_log/TM_*.txt` | Filesystem | `ItemLog` output | opened per day by `StartItemLog` |
| `TMSrv.log` (`fLogFile`) | Filesystem | `Log` output | global handle |

No third-party library, no network service, no database, and no IPC endpoint is contacted
directly by this component. Its only I/O is the outbound player socket and two log files.

### 5.3 Global state dependency list

| Symbol | Declared | Accessed as | Mode |
|--------|----------|-------------|------|
| `pItem[MAX_ITEM]` | `Server.h:235`, `CItem.h:45` | `pItem[itemID].Mode/.ITEM/.PosX/.PosY` | read + write |
| `pItemGrid[MAX_GRIDY][MAX_GRIDX]` | `Server.h:55` (also re-declared at `Server.h:310`) | `pItemGrid[y][x]` | read + write |
| `pMob[MAX_MOB]` | `CMob.h:122` | `pMob[conn].MOB.*`, `.TargetX/Y`, `.MaxCarry`, `.extra.QuestInfo` | read + write |
| `pUser[MAX_USER]` | `Server.h:64` | `pUser[conn].Mode/.Trade/.TradeMode/.cSock/.AccountName/.IP` | read + write |
| `g_pItemList[MAX_ITEMLIST]` | `Basedef.h:2467` | `g_pItemList[sIndex].Name` (via `SendNotice` arg) | read |
| `g_pMessageStringTable[MAX_STRING][128]` | `Basedef.h:2461` | indices 1, 71, 170, 198, 273 | read |
| `temp[4096]` | `Server.h:382`, `Server.cpp:277` | log/format scratch | read + write |

### 5.4 Dependency direction assessment

The component depends only on lower layers (Basedef primitives, SendFunc projections, engine
helpers in `Server.cpp`) and on no other `_MSG_*` handler. That makes it a leaf of the handler
graph in the *reverse* direction: nothing in `Code/TMSrv` includes `_MSG_GetItem.cpp` and nothing
calls `Exec_MSG_GetItem` except the dispatcher. There is no cycle, and the only lateral
coupling is through shared globals, which is where the real risk lies (Section 6).

---

## 6. Afferent and Efferent Coupling

C++ units in this codebase are free functions, classes and global arrays. The table below uses
functions as the unit of measure, with measured reference counts (`grep`/AST counts over
`Code/**/*.cpp` and `Code/**/*.h`, so counts are lower bounds where a symbol is also called from
`Server.cpp`'s monolithic functions).

### 6.1 Component-level (module granularity)

| Component | Afferent (units that depend on it) | Efferent (units it depends on) | Instability (Ce/Co) | Criticality |
|-----------|-----------------------------------|-------------------------------|---------------------|-------------|
| GetItem Handler (`Exec_MSG_GetItem`) | 1 (`ProcessClientMessage.cpp:203`) + 1 declaration (`ProcessClientMessage.h:104`) | 16 project symbols across 4 files + 3 CRT calls | 16 / (16+1) = 0.94 | High |
| `pItem[]` world-object array | 10 files / 157 references | 0 | 0.00 | High |
| `pItemGrid` | 8 files / 27 references | 0 | 0.00 | High |
| `MOB.Carry[]` (inventory) | 29 files / 473 references | 0 | 0.00 | High |
| `BASE_GetItemAbility` | 18 files / 120 references | 6 (`g_pItemList`, `g_pMountBonus`, `g_pMountTempBonus`, `BASE_GetItemSanc`, `BASE_CanEquip` callers) | 0.25 | High |
| `GridMulticast` | 21 files / 106 references | 11 | 0.34 | High |
| `SendItem` | 28 files / 385 references | 1 (`CPSock::AddMessage`) | 0.03 | Medium |
| `SendEtc` | 22 files / 57 references | 2 | 0.08 | Medium |
| `RemoveTrade` | 27 files / 91 references | 3 | 0.10 | Medium |
| `ItemLog` | 6 files (economy handlers) | 1 (`fprintf`) | 0.14 | Low |
| `BASE_ClearItem` | 20 files | 0 | 0.00 | Low |

Interpretation: the handler itself is maximally unstable (nothing depends on it except the
dispatcher), yet it writes to the three most-depended-on globals in the server. This is the
classic "stable data, unstable behaviour" inversion: the logic is easy to change, but every
change is immediately visible to 29 other files through `Carry[]`.

### 6.2 Symbol-level coupling of the handler

| Direction | Symbol | Count | Where |
|-----------|--------|-------|-------|
| Efferent (outbound) | `AddCrackError` | 1 | `_MSG_GetItem.cpp:26` |
| | `SendHpMode` | 1 | `:27` |
| | `RemoveTrade` | 1 (called twice) | `:33-34` |
| | `SendClientMessage` | 2 | `:40`, `:103` |
| | `Log` | 3 | `:46`, `:60`, `:80`, `:208` (4 call sites) |
| | `BASE_GetLanguage` | 1 | `:98` |
| | `sprintf` / `memset` / `memcpy` | 4 / 4 / 1 | CRT |
| | `CPSock::AddMessage` | 5 | `:70`, `:159`, `:166`, `:176`, `:199`, `:244` |
| | `BASE_ClearItem` | 2 | `:106`, `:201` |
| | `GridMulticast` | 3 | `:119`, `:142`, `:246` |
| | `SendNotice` | 1 | `:132` |
| | `BASE_GetItemAbility` | 3 | `:180`, `:184`, `:187` |
| | `SendEmotion` | 1 | `:123` |
| | `SendEtc` | 1 | `:124` |
| | `BASE_GetItemCode` | 1 | `:229` |
| | `ItemLog` | 1 | `:231` |
| | `SendItem` | 2 | `:222`, `:251` |
| Afferent (inbound) | Dispatcher call | 1 | `ProcessClientMessage.cpp:203` |
| | Linkage declaration | 1 | `ProcessClientMessage.h:104` |
| | Build registration | 2 | `TMSrv.vcxproj:137`, `.filters:255` |

The measured efferent total is **16 distinct project-defined callables** across four files
(`Server.cpp`, `SendFunc.cpp`, `Basedef.cpp`, `CPSock.cpp`), which reconciles with the
architecture report's "efferent 14" if `CPSock::AddMessage` (a method reached through a global
`pUser[].cSock`) and one overload-resolution duplicate (`BASE_GetItemAbility` counted once per
call site) are excluded. Both figures describe the same fan-out; this report uses 16.

### 6.3 Cohesion assessment

| Cohesion dimension | Assessment | Evidence |
|--------------------|------------|----------|
| Functional cohesion | Mixed | The function implements one user-visible transaction (pickup), but embeds four unrelated sub-transactions: quest-pill consumption (94-127), global announcement (129-143), currency deposit (182-202), inventory copy (204-232) |
| Temporal cohesion | High | All steps must happen in the given order; the ordering of the announcement multicast before validation is the single most consequential sequence decision |
| Logical cohesion | Low | Currency and inventory paths share almost no logic beyond the four preamble gates |
| Data cohesion | Low | No local aggregate; 7 globals touched |
| LCOM (estimate) | High | Each of the four sub-transactions touches a disjoint global set (quest: `extra.QuestInfo`, `SkillBonus`; currency: `MOB.Coin`; inventory: `Carry[]`) |

The quest-pill branch is the clearest cohesion defect: it is a *use* operation (consume an item
for an effect) implemented inside a *move* operation. `_MSG_UseItem` already owns that semantic
and has a full volatility dispatch table (`_MSG_UseItem.cpp:94-...`), yet archetype 470 is
special-cased here because the pill is only ever encountered as a ground object.

---

## 7. Endpoints

The component exposes no HTTP, GraphQL or gRPC surface. Its endpoint is a binary TCP opcode on
the game protocol.

| Endpoint | Direction | Opcode | Declared | Handler | Payload |
|----------|-----------|--------|----------|---------|---------|
| `_MSG_GetItem` | client -> game | `112 \| FLAG_CLIENT2GAME` = `0x0270` (624) | `Basedef.h:1618` | `_MSG_GetItem.cpp:20` | `int DestType; int DestPos; unsigned short ItemID; unsigned short GridX, GridY` (`Basedef.h:1619-1627`) |
| `_MSG_CNFGetItem` | game -> client | `113 \| FLAG_GAME2CLIENT` = `0x0171` (369) | `Basedef.h:1629` | emitted at `:234-244` | `int DestType; int DestPos; STRUCT_ITEM Item` |
| `_MSG_DecayItem` | game -> client (direct + broadcast) | `111 \| FLAG_GAME2CLIENT` = `0x016F` (367) | `Basedef.h:1609` | emitted at `:62-70`, `:111-119`, `:134-142`, `:199`, `:246` | `short ItemID; short unk` |
| `_MSG_SendItem` | game -> client | `130 \| FLAG_GAME2CLIENT` = `0x0182` (386) | `Basedef.h:1672` | emitted at `:222`, `:251` via `SendFunc.cpp:1055` | `invType, Slot, STRUCT_ITEM` |
| `_MSG_MessagePanel` | game -> client | `1 \| FLAG_GAME2CLIENT` = `0x0101` (257) | `Basedef.h:1194` | emitted at `:40`, `:103`, `:194` via `SendClientMessage` | `char String[MESSAGE_LENGTH]` |
| `_MSG_Motion` | game -> client (broadcast) | `106 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME` | `Basedef.h:1710` | emitted at `:123` via `SendEmotion` | `Motion, Parm` |
| `_MSG_UpdateEtc` | game -> client | `55 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME` | `Basedef.h:1494` | emitted at `:124` via `SendEtc` | `Exp, SpecialBonus, Learn, ScoreBonus, SkillBonus, Coin, Hold, Magic` |
| `_MSG_SetHpMode` | game -> client | `146 \| FLAG_CLIENT2GAME` | `Basedef.h:1811` | emitted at `:27` via `SendHpMode` | `Hp, Mode` |

### 7.1 Wire-format notes

- `_MSG` expands to `short Size; char KeyWord; char CheckSum; short Type; short ID;
  unsigned int ClientTick;` (`Basedef.h:925-930`), so the fixed header is 12 bytes.
  `sizeof(MSG_GetItem)` is 28 bytes with 4-byte alignment (26 bytes of content plus 2 bytes of
  tail padding); the declared size check at `Basedef.cpp:6516` compares against that value.
- `ID` is a `short` in the header while `ProcessClientMessage.cpp:42` compares it against
  `MAX_USER` (1000), so the effective valid range is 0..999.
- `ClientTick` is checked by the dispatcher: a non-server frame with
  `ClientTick == SKIPCHECKTICK` is dropped (`ProcessClientMessage.cpp:63-64`).
- Framing is length-driven (`CPSock.cpp:390-411`): the `Size` field must be between
  `sizeof(HEADER)` and `MAX_MESSAGE_SIZE` and must not exceed the buffered byte count, so a
  truncated `_MSG_GetItem` (e.g. `Size` = 12) is delivered to the handler as a 12-byte buffer
  that the handler then reads as a 28-byte struct (Section 10).
- The checksum mismatch path returns the buffer anyway (`CPSock.cpp:457-464`), and the caller
  logs but does not terminate (`Server.cpp:3990-3999`), so corrupt frames reach handlers.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol / Mechanism | Data Format | Error Handling |
|-------------|------|---------|----------------------|-------------|----------------|
| Player client socket | External peer | Receive `_MSG_GetItem`; emit 7 response/broadcast message types | TCP, custom binary frame (12-byte header + payload), XOR-obfuscated body | `MSG_GetItem`, `MSG_CNFGetItem`, `MSG_DecayItem`, `MSG_SendItem`, `MSG_MessagePanel`, `MSG_Motion`, `MSG_UpdateEtc`, `MSG_SetHpMode` | No retry; invalid requests logged or silently dropped; builder-buffer overflow handled inside `CPSock::AddMessage` (`CPSock.cpp:513-545`) |
| `pItemGrid` world map | In-process global | Authoritative 4096x4096 object registration | direct array access | `unsigned short` item index (0 = free) | Range-checked on read (BR-12); write unchecked in the archetype-470 branch |
| `pItem[]` object table | In-process global | Object payload + lifetime flags | direct struct access | `CItem` (8-byte item + 12 int fields) | Slot range checked (BR-05); `Mode` semantics enforced (BR-06) |
| Character inventory `MOB.Carry[]` | In-process global | Destination of transferred items | `memcpy` + projections | `STRUCT_ITEM[64]` | Slot range checked; occupancy checked; no footprint/weight validation |
| Character purse `MOB.Coin` | In-process global | Currency destination | integer addition | `int` | Ceiling enforced at 2e9 |
| Quest flags `extra.QuestInfo.Mortal` | In-process global | One-shot pill marker | `char` flag | `STRUCT_MOBEXTRA` | Idempotency checked |
| DB persistence (DBSrv) | Out-of-process (indirect) | Persist `MOB` after pickup | no direct call from this component; `MSG_DBSaveMob` via `SaveUser` (`ProcessDBMessage.cpp`) | `MSG_DBSaveMob` | Not handled here; `AddCrackError` logout path triggers `SaveUser` |
| `g_pItemList` (itemlist.csv) | Data asset | Archetype names, abilities, footprints | loaded by `BASE_ReadItemListFile` (`Basedef.cpp:4963+`) | CSV -> `STRUCT_ITEMLIST[6500]` | Not validated by this component beyond `sIndex < MAX_ITEMLIST` |
| `g_pMessageStringTable` (UI/strdef.bin) | Data asset | All localized texts | XOR 0x5A decoded binary (`Basedef.cpp:4932`) | `char[MAX_STRING][128]` | Index bounds checked in `BASE_GetLanguage`; direct `table[...]` uses at 40/103/194 are unchecked constants |
| `Log` / `item_log` files | Filesystem | Diagnostics and audit | `fprintf` to `fLogFile`, `fItemLogFile` | plain text | No error check on write; daily file naming by `StartItemLog` |
| Server console window | Win32 UI | Live log mirror | `SetWindowText(hWndMain, LogTemp)` | text | none |

---

## 9. Design Patterns & Architecture

### 9.1 Patterns observed

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Command Handler (message-per-function) | `Exec_MSG_GetItem` registered by opcode in a switch | `_MSG_GetItem.cpp:20`, `ProcessClientMessage.cpp:202` | 1:1 mapping opcode -> free function; no interface, no polymorphism |
| Guard clause chain (early return) | 13 sequential `if (...) return;` preambles | `_MSG_GetItem.cpp:24-178` | Flatten nesting; keep the happy path at the bottom |
| Validate-then-commit (transactional tail) | All world mutation deferred to lines 234-251 | `_MSG_GetItem.cpp:234-251` | Single commit point; failed requests are side-effect free (except BR-10/BR-12/BR-11) |
| Authoritative re-derivation | Client sends only a handle and an attestation tile; server re-reads position, archetype, volatility and occupancy from `pItem[]` | `:50-180` | Trust boundary: client cannot assert item identity, position or content |
| Resynchronisation via projections | Private `_MSG_DecayItem`, `_MSG_SendItem`, `_MSG_SetHpMode` used as corrective state pushes instead of error codes | `:62-70`, `:159-178`, `:222`, `:27` | Self-healing client after a divergence; the protocol has no error channel |
| Correlation handle with offset | World identity = `array index + 10000` | `:50`, `Server.cpp:7298`, `Server.cpp:8391` | Distinguishes "world object" namespace from "archetype" and from "inventory slot" |
| Grid registry (spatial index) | `pItemGrid[y][x]` holds the item index, parallel to `pMobGrid[y][x]` | `Server.cpp:7287`, `:164-172` | O(1) spatial lookup; also the duplicate-registration detector |
| Broadcast fan-out (spatial) | `GridMulticast(itemX, itemY, msg, 0)` iterating the 33x33 view | `SendFunc.cpp:843` | View-scoped multicast without per-player filtering data |
| Event-log / audit trail | `ItemLog("getitem, <code>")` paired with `ItemLog("dropitem, <code>")` | `:228-231`, `_MSG_DropItem.cpp:120-123` | Reconstructable item economy |
| Anti-cheat counter (tarpit) | `NumError += 1`, logout at 2e9, per-handler type tag 13 | `Server.cpp:684-707`, `:26` | Rate-limited bot/forgery detection |
| Global singleton state | `pMob`, `pUser`, `pItem`, `pItemGrid` arrays | `Server.h`, `CMob.h`, `CUser.h`, `CItem.h` | Whole-server state without dependency injection |
| Copy-paste message construction | `MSG_DecayItem` built identically in 4 places | `:62-70`, `:111-119`, `:134-142`, `:148-155` | Decompiler artefact; no shared builder |

### 9.2 Architectural decisions recorded

1. **The client is never trusted for item identity.** Only a handle and an attestation tile
   cross the boundary; everything else is re-derived server-side. This is the component's
   strongest design property and it is enforced consistently across BR-06 to BR-14.
2. **The protocol has no negative acknowledgement.** Every failure is expressed as silence, a log
   line, or a corrective state projection. Consequence: client developers must treat "nothing
   happened" as a valid response class, and operators must read server logs to diagnose pickups.
3. **World objects are handles into a global array, not objects with lifetime management.**
   `Mode` is the only lifetime flag, and three components (`CreateItem`, `ProcessDecayItem`,
   this handler) mutate it with no ownership protocol. There is no generation counter, so a
   stale handle can be recycled onto a new object within one `GetEmptyItem` scan
   (`Server.cpp:6756` scans from index 1 upward, so low indices are reused quickly).
4. **Inventory is a flat 64-slot array, not a spatial container.** Footprint, stacking and
   capacity are delegated to the client; the server enforces only index bounds and emptiness.
   This is a deliberate simplification whose cost is the unreachable `BASE_CanCarry` validator.
5. **Notifications are emitted eagerly, validation is completed late.** The rare-item band
   (BR-11) breaks the validate-then-commit rule and is therefore the component's main
   consistency hazard.
6. **Single-threaded actor model.** All handlers run on the Win32 message thread
   (`Server.cpp:3929-4002`), so the read-modify-write of `pItem`/`Carry`/`Coin` is atomic by
   construction; no locking exists anywhere in the component.

---

## 10. Technical Debt & Risks

### 10.1 Risk register

| Risk | Level | Component area | Issue | Impact | Evidence |
|------|-------|----------------|-------|--------|----------|
| R-01 | High | Frame length trust | Whole body of `BASE_CheckPacket` is commented out and returns `FALSE`; `_MSG_GetItem`'s declared size check at `Basedef.cpp:6516` never runs, and the only call sites are `#ifdef _PACKET_DEBUG` | A truncated `_MSG_GetItem` frame (e.g. `Size` 12) is cast to a 28-byte struct; `DestType`, `DestPos`, `ItemID`, `GridX`, `GridY` are read from stale buffer bytes, letting the tail of a previous frame influence the pickup | `Basedef.cpp:6475-6596`; `Server.cpp:3980-3988`; `CPSock.cpp:390-421` |
| R-02 | High | BR-08 level gate | `itemID == 1727` compares the *world array slot*, not `ITEM.sIndex`; the same literal is treated as an archetype in `ProcessDecayItem` | The intended archetype-1727 protection is bypassable, while an arbitrary unrelated object inherits a level-1000 requirement depending on allocation order | `_MSG_GetItem.cpp:84` vs `Server.cpp:8362` |
| R-03 | High | BR-11 announcement | `GridMulticast` of `_MSG_DecayItem` at line 142 precedes the remaining validations and the function does not return | Peers permanently erase an item that may fail to be picked up (occupied slot, currency cap, grid mismatch), producing a world object that exists but is invisible and unpickable until decay | `_MSG_GetItem.cpp:129-143` vs `:157-178`, `:217-225` |
| R-04 | High | BR-10 grid write | `pItemGrid[PosY][PosX] = 0` without the coordinate guard that the generic path applies at line 157 | A world object with corrupt coordinates drives an out-of-bounds write into the 32 MB `pItemGrid` array | `_MSG_GetItem.cpp:108` vs `:157-162`; `Server.h:55` |
| R-05 | Medium | BR-16/BR-17 capacity | Destination validated against `MAX_CARRY` (64), not `MaxCarry` (30..60); slots above the usable range are writable | Items can be placed in slots the client does not render, creating invisible items and inventory desync | `_MSG_GetItem.cpp:206`, `:219`; `CMob.h:96`; `ProcessDBMessage.cpp:751-757` |
| R-06 | Medium | BR-17 resync | Occupied slot resends `DestPos-1` and mutates `m->DestPos` in the receive buffer; slot 0 and slots above `MaxCarry` produce no response at all | No deterministic client recovery for a contested pickup; receive-buffer mutation is an unexplained side effect | `_MSG_GetItem.cpp:217-225` |
| R-07 | Medium | BR-18 stacking/footprint | `memcpy` into a verified-empty slot with no merge and no `BASE_CanCarry` call; `BASE_CanCarry` has zero live call sites | Stackable items fragment across slots; multi-cell items overlap in the client grid; the engine's own invariant validator is dead code | `_MSG_GetItem.cpp:226`; `Basedef.cpp:4413`; `Server.cpp:8574` |
| R-08 | Medium | BR-19 audit | Currency pickups and archetype-470 consumptions produce no `ItemLog`; currency also skips `SendEtc` | Economy movements and quest-item consumptions are not reconstructable from `item_log`; coin display relies on some later projection | `_MSG_GetItem.cpp:182-202`, `:94-127` vs `:228-231`; contrast `_MSG_Sell.cpp:233` |
| R-09 | Medium | BR-13 grid divergence | This handler uses `[y][x]`; `ProcessDecayItem` clears `[itemPosX][itemPosY]` | Stale grid registrations accumulate, and this handler's mismatch test (line 164) turns them into unpickable ghost objects | `_MSG_GetItem.cpp:248` vs `Server.cpp:8381` |
| R-10 | Medium | BR-01 anti-cheat | `NumError` threshold of 2,000,000,000 with increments of 1 and resets on login/restart | The counter is practically unreachable; the "crack" mechanism functions only as a log tag | `Server.cpp:692-704`; `ProcessDBMessage.cpp:822`; `_MSG_Restart.cpp:28` |
| R-11 | Medium | BR-14/BR-06 diagnostics | `GetItemFail idx/mode` log is unreachable (inner `if` contradicts the outer condition); `GetItemFail posx..posx` has a duplicated field name | Operator cannot distinguish ghost-item classes from logs; automated log parsing of coordinates is ambiguous | `_MSG_GetItem.cpp:57-61`, `:77` |
| R-12 | Low | BR-20 confirmation | `MSG_CNFGetItem.Item` is declared but always zeroed | Any client implementation that reads the item from the confirmation receives an empty item; the authoritative payload is only in the following `_MSG_SendItem` | `Basedef.h:1635`; `_MSG_GetItem.cpp:234-244` |
| R-13 | Low | Format strings | `sprintf(temp, g_pMessageStringTable[_SSD_S_get_S], MobName, itemName)` and `BASE_GetLanguage` use table strings as format strings | If a table entry contains unexpected specifiers, stack arguments are consumed; the table is a server asset so exposure is operator-controlled | `_MSG_GetItem.cpp:131`; `Basedef.cpp:1258-1279` |
| R-14 | Low | Currency wrap | `SkillBonus` is `unsigned short`; `+= 9` wraps at 65536 | A character at/near the maximum loses accumulated points on pill pickup | `Basedef.h:467`; `_MSG_GetItem.cpp:121` |
| R-15 | Low | Object recycling | World handles are plain array indices with no generation counter; `GetEmptyItem` reuses the lowest free index | A delayed or replayed pickup can address a *newly created* object that occupied the same slot | `Server.cpp:6754-6763`; `_MSG_GetItem.cpp:50` |
| R-16 | Low | Height/terrain | No `pHeightGrid` check in the pickup path, while `CreateItem`'s own height check is commented out | Objects on impassable tiles (height 127) are still pickable; half of the terrain rule exists only at creation | `Server.cpp:7261-7265`; `GetFunc.cpp:1878` |

### 10.2 Code smells

| Smell | Instance |
|-------|----------|
| Dead branch | `if (pItem[itemID].Mode)` at line 57 can never be true (outer condition forces `Mode == 0`) |
| Redundant condition | Duplicated `itemID <= 0 \|\| itemID >= MAX_ITEM` at lines 52 and 55 |
| Unused variable | `int error = -2;` at line 214; never read |
| Magic numbers | 10000, 1727, 470, 490, 500, 3, 2000000000, 273, 9, 14, 3, 0 as "empty slot" |
| Copy-paste | `MSG_DecayItem` construction repeated 4x (62-70, 111-119, 134-142, 148-155) |
| Comment-free domain logic | No comment explains 470, 490-499, 1727, or the 2e9 ceiling |
| Decompiler artefacts | `(STRUCT_ITEM *)ditem` cast of an already-typed pointer (184, 187); unreachable duplicated guards |
| Message text wrong for context | `"DEBUG:Trading Fails.(Wrong source position)"` (208) and `"DEBUG:GetItem with wrong desttype"` (46) |
| Inconsistent bounds style | `itemX < 0` vs `itemY <= 0` (157); `DestPos <= MaxCarry` (219) vs `>= MAX_CARRY` (206) |
| Split personality | Quest consumption and currency deposit implemented inside a transfer handler |
| Shadowed/in-band error signalling | Failures expressed as silence, no return codes |
| Global scratch coupling | `temp[4096]` used at 59, 77, 131, 230 - any nested call that also writes `temp` corrupts the message |

### 10.3 Security posture summary

| Property | Assessment |
|----------|------------|
| Input trust for item identity | Strong: handle -> server-side re-read of position, archetype, volatility, occupancy |
| Input trust for item content | Strong: `STRUCT_ITEM` never comes from the client on this path |
| Input trust for frame length | **Weak**: size validation disabled (R-01) |
| Input trust for destination slot | **Medium**: range-checked but not capacity-checked against `MaxCarry` (R-05) |
| Replay / race resistance | Adequate: single-threaded dispatch, and `Mode = 0` commits before the next frame is processed |
| Duplication resistance | **Medium**: no per-handle generation counter (R-15); announcement path can leave half-alive objects (R-03) |
| Privilege model | None: no ownership, no guild/party/level entitlement except the misapplied slot-1727 check (BR-22, R-02) |
| Auditability | **Partial**: items logged, money and quest pills not (R-08) |
| Crash resistance | Adequate for valid data; OOB write possible on corrupt `PosX/PosY` in the 470 branch (R-04) |

---

## 11. Test Coverage Analysis

### 11.1 Direct answer: there are no tests

No test file, test project, test harness, mock, fixture or assertion exists for this component or
for any other part of the project. Evidence, collected over the whole project root with the
`ignore-folders` set applied:

| Check | Result |
|-------|--------|
| Files whose name matches `test`, `spec`, `mock`, `fixture` | **0** (recursive scan of the entire tree, excluding `.git`, `.codegraph`, `.opencode`, `.agents`, `.vs`) |
| Test projects in the solution | **0** - `W2PP Code Project.sln` contains only `Code/TMSrv/TMSrv.vcxproj` and `Code/DBSrv/DBSrv.vcxproj` |
| `.vcxproj` files in the repository | 2 (`TMSrv`, `DBSrv`), both server executables, neither with a test configuration |
| Test framework headers/identifiers (`gtest`, `CppUnit`, `catch2`, `doctest`, `BOOST_TEST`, `TEST_F`) | **0** matches across `Code/**/*.cpp`, `Code/**/*.h`, `Code/**/*.vcxproj` |
| `assert()` call sites in any `.cpp` | **0** |
| CI configuration (`.github/`, `*.yml` outside `openspec/`) | **0** (only `openspec/config.yaml`, which is workflow tooling, not CI) |
| CMake / Makefile / package.json | **0** |
| Test data or golden files for pickup scenarios | **0** |
| Existing `docs/reports/component-deep-analyzer/*.md` references to tests | 6 prior component reports, all concluding "no tests found" |

There is also no *runtime* test surface: the handlers are free functions bound to Win32 global
arrays (`pMob`, `pUser`, `pItem`, `pItemGrid` are 32 MB+ of static storage, `Server.h:55`,
`Basedef.h:56/107`), the transport is a raw Winsock socket, and there is no seam to substitute
the socket, the clock (`CurrentTime`, `SecCounter`) or the item list. Nothing in the component is
reachable without a live `TMSrv` process.

### 11.2 Coverage table

| Component area | Unit tests | Integration tests | Coverage | Test quality |
|----------------|-----------|-------------------|----------|--------------|
| `Exec_MSG_GetItem` (whole function) | 0 | 0 | 0% | None - no executable specification |
| BR-01 session gate | 0 | 0 | 0% | None |
| BR-02 trade teardown | 0 | 0 | 0% | None |
| BR-03 vending lock | 0 | 0 | 0% | None |
| BR-04 destination container | 0 | 0 | 0% | None |
| BR-05 handle decode/range | 0 | 0 | 0% | None |
| BR-06 ghost-object cleanup | 0 | 0 | 0% | None |
| BR-07 distance box | 0 | 0 | 0% | None |
| BR-08 slot-1727 level gate | 0 | 0 | 0% | None |
| BR-09 archetype sanity | 0 | 0 | 0% | None |
| BR-10 quest pill (470) | 0 | 0 | 0% | None |
| BR-11 announcement band | 0 | 0 | 0% | None |
| BR-12 coordinate integrity | 0 | 0 | 0% | None |
| BR-13 grid ownership/self-heal | 0 | 0 | 0% | None |
| BR-14 client attestation | 0 | 0 | 0% | None |
| BR-15 currency pickup/cap | 0 | 0 | 0% | None |
| BR-16 destination range | 0 | 0 | 0% | None |
| BR-17 occupancy/resync | 0 | 0 | 0% | None |
| BR-18 transfer semantics | 0 | 0 | 0% | None |
| BR-19 audit logging | 0 | 0 | 0% | None |
| BR-20 confirmation message | 0 | 0 | 0% | None |
| BR-21 teardown/broadcast | 0 | 0 | 0% | None |
| Collateral: `CreateItem` | 0 | 0 | 0% | None |
| Collateral: `ProcessDecayItem` | 0 | 0 | 0% | None |
| Collateral: `BASE_GetItemAbility` | 0 | 0 | 0% | None |
| Collateral: `GridMulticast` | 0 | 0 | 0% | None |
| Collateral: `SendItem` | 0 | 0 | 0% | None |

### 11.3 Why the component is currently untestable

1. **No linkage seam.** `_MSG_GetItem.cpp:18` includes only `ProcessClientMessage.h`, which
   includes the global headers. `Exec_MSG_GetItem` cannot be compiled without `pItem`,
   `pItemGrid`, `pMob`, `pUser`, `g_pItemList`, `g_pMessageStringTable`, `temp`, `Log`,
   `ItemLog` and the Win32 socket wrapper - i.e. without effectively linking the whole server.
2. **Static, huge state.** `pItemGrid` is `unsigned short[4096][4096]` = 32 MB
   (`Basedef.h:100-101`, `Server.h:55`); `pMob` is `CMob[25000]`; these are compiled-in globals
   initialized by `WinMain`, not constructible per test.
3. **Filesystem side effects at import time.** `Log` opens `fLogFile`, `ItemLog` opens a daily
   file through `StartItemLog`, and message texts require `UI/strdef.bin`, which is **not in the
   repository** (verified: no `strdef*` file exists anywhere in the tree). Any test would need to
   fabricate the asset.
4. **Item semantics are data-driven.** `g_pItemList` is loaded from `../../Common/itemlist.csv`
   (`Basedef.cpp:4963+`), also **absent from the repository**. The archetype semantics this
   component branches on (470, 490-499, 1727, "volatile == 2") therefore cannot be verified at
   all from source, only observed on a running deployment.
5. **No time or randomness abstraction.** The decay path depends on `CurrentTime`/`SecCounter`
   and `ProcessDecayItem`'s sweep; pickup ordering depends on the message-loop drain.

### 11.4 Observable behavioural evidence in lieu of tests

Because no test can be cited, the following behaviours are asserted only by reading the code and
should be treated as *unverified* until exercised:

| Claim | Confidence | Basis |
|-------|-----------|-------|
| Two clients clicking one item in the same drain: first wins, second gets a private decay | High | single-threaded dispatch (`Server.cpp:3973-4002`) + BR-06 |
| Over-cap currency leaves the money object on the ground and mutable | High | ceiling check precedes mutation (`:192-197`) |
| Rare-item announcement can strand an item invisible to all clients | High | eager multicast (`:142`) precedes `:157-224` failure paths |
| Slot-1727 gate applies to the wrong object | High (code), Medium (intent) | `:84` vs `Server.cpp:8362` |
| Items can be placed above `MaxCarry` | High | `:206` checks `MAX_CARRY`, not `MaxCarry` |
| Money pickup emits no `ItemLog` and no `SendEtc` | High | absence of both calls in `:182-202` |
| Message size is not enforced for this opcode | High | `Basedef.cpp:6476` `/*` ... `:6594` `*/`, `:6595 return FALSE` |
| A failed `DestPos` projection for slot 0 produces no feedback | High | `:219` requires `DestPos > 0` |

### 11.5 Coverage gaps expressed as behaviours that would need to be pinned

Descriptive only; no implementation guidance is implied.

- The exact set of client messages emitted for each of the 17 exit paths (Section 2.4), which is
  the component's only contract.
- The interaction of `DestPos` above `MaxCarry` with the client's inventory rendering.
- Whether any client reads `MSG_CNFGetItem.Item` (currently always zero).
- Whether the currency ceiling message (index 273) is the same text as the sell-clamp message
  path in `_MSG_Sell.cpp:215-218`.
- The behaviour of `BaseScore`/`CurrentScore` level for the BR-08 gate after a level-down or a
  temporary affect.
- The fate of a ghost object (BR-13 mismatch) until `ProcessDecayItem` retires it.

---

## 12. Analysis Limitations

1. **Data assets absent.** `UI/strdef.bin` (all localized texts, XOR 0x5A) and
   `../../Common/itemlist.csv` (all archetype semantics) are not in the repository. Message
   indices 1, 71, 170, 198 and 273 are referenced numerically only; the archetypes 470, 490-499
   and 1727 cannot be named, and the definition of a "money item" (`EF_VOLATILE == 2` template)
   cannot be confirmed. No `heightmap.dat` is present either.
2. **Intent vs implementation for BR-08.** The slot-vs-archetype mismatch is established from the
   code; the intended semantics of 1727 is an inference supported only by `Server.cpp:8362`.
3. **Decompiled source.** The code carries decompiler artefacts (duplicated guards, redundant
   casts, unused locals, unreachable log branches). Where a construct is unreachable, this report
   describes the reachable behaviour and notes the artefact; it does not attempt to reconstruct
   the original binary's intent beyond what the artefacts imply.
4. **Client counterpart unavailable.** No client source is in the repository, so the client's
   handling of `_MSG_CNFGetItem` (empty `Item`), the double `_MSG_DecayItem` in the announcement
   band, and the `DestPos-1` resync cannot be verified.
5. **Single-threaded assumption.** The absence of locking is described as safe because dispatch
   runs on the Win32 message thread (`Server.cpp:3973-4002`). If any future path dispatches
   `ProcessClientMessage` from another thread, all `pItem`/`Carry`/`Coin` reasoning in this
   report would need to be revisited. No such path was found in the current tree.
6. **Cross-handler comparison is limited to the handlers inspected for context**
   (`_MSG_DropItem`, `_MSG_Sell`, `_MSG_UseItem`, `_MSG_Attack`, `_MSG_Quest`) plus the specific
   `Server.cpp`/`Basedef.cpp`/`SendFunc.cpp`/`GetFunc.cpp` function bodies cited.
7. **No execution.** No server was built or run and no packet was replayed; every claim about
   runtime behaviour is derived from static reading.

---

## 13. Summary of Referenced Files

### 13.1 The component

| File | Lines referenced | Role |
|------|------------------|------|
| `Code/TMSrv/_MSG_GetItem.cpp` | 1-252 (whole file; key ranges 24-48, 50-92, 94-127, 129-143, 145-178, 180-208, 212-232, 234-251) | The handler |

### 13.2 Declarations and registration

| File | Lines | Role |
|------|-------|------|
| `Code/Basedef.h` | 76, 86-88, 92-93, 95-101, 103, 139, 170, 398-412, 438-481, 459, 467, 483-504, 882-904, 925-946, 1609-1636, 1618-1627, 1629-1636, 2391-2394, 2461, 2467 | Constants, structs, opcodes, prototypes |
| `Code/TMSrv/ProcessClientMessage.h` | 104 | Linkage declaration |
| `Code/TMSrv/ProcessClientMessage.cpp` | 38-64, 195-204, 202-203 | Dispatcher and routing |
| `Code/TMSrv/TMSrv.vcxproj` | 137 | Build registration |
| `Code/TMSrv/TMSrv.vcxproj.filters` | 255 | Filter registration |
| `Code/TMSrv/Server.h` | 54-56, 235, 309-311, 382 | Global externs |
| `Code/TMSrv/CItem.h` | 24-45 | World-object schema (no owner field) |
| `Code/TMSrv/CUser.h` | 26-37, 39-121 | Connection schema, mode constants |
| `Code/TMSrv/CMob.h` | 24-35, 37-122, 96 | Character/mob schema, `MaxCarry` |
| `Code/TMSrv/Language.h` | 21, 43, 91, 190, 218 | Message indices used by the handler |
| `Code/ItemEffect.h` | 65-78 | `EF_HWORDCOIN` 36, `EF_LWORDCOIN` 37, `EF_VOLATILE` 38, `EF_GRID` 33 |

### 13.3 Collaborators (read for behaviour)

| File | Lines | Role |
|------|-------|------|
| `Code/TMSrv/Server.cpp` | 684-707 (`AddCrackError`), 6701-6727 (`Log`), 6754-6763 (`GetEmptyItem`), 7254-7317 (`CreateItem`), 7319-7352 (`RemoveTrade`), 8353-8396 (`ProcessDecayItem`), 8362, 8381, 9247-9290 (`StartItemLog`/`ItemLog`), 9392-9449 (`CreateTreasureBox`), 3919-4004 (WSA_READ dispatch), 8574 | Engine helpers the handler depends on |
| `Code/TMSrv/SendFunc.cpp` | 27-45 (`SendClientMessage`), 47-62 (`SendNotice`), 843-972 (`GridMulticast`), 825-841 (`SendEmotion`), 1055-1082 (`SendItem`), 1195-1231 (`SendEtc`), 1622-1645 (`SendHpMode`) | Outbound projections |
| `Code/Basedef.cpp` | 742, 1258-1279 (`BASE_GetLanguage`), 1479-1495 (`BASE_GetItemCode`), 1647-1842 (`BASE_GetItemAbility`; 1813 volatility exclusion, 1835-1839 grid clamp), 4281-4291 (`BASE_ClearItem`), 4293-4310 (`BASE_CanEquip`), 4413-4478 (`BASE_CanCarry`, unused), 4932-4961 (`BASE_ReadMessageBin`), 4963+ (`BASE_InitializeItemList`), 6475-6596 (`BASE_CheckPacket`, disabled), 6516 | Item/ability primitives, packet validation |
| `Code/TMSrv/GetFunc.cpp` | 1195 (`GetCreateItem`), 1876-1899 (`GetEmptyItemGrid`) | Grid helpers |
| `Code/TMSrv/ProcessDBMessage.cpp` | 751-757, 822 | `MaxCarry` derivation, `NumError` reset |
| `Code/TMSrv/_MSG_UseItem.cpp` | 85-204, 5680-5705 | Comparable volatility dispatch, `MaxCarry` recompute, coin clamp |
| `Code/TMSrv/_MSG_Sell.cpp` | 125-236 | Comparable currency clamp (2e9) and `SendEtc` pattern |
| `Code/TMSrv/_MSG_DropItem.cpp` | 20-146 | Counterpart handler (drop), audit pairing |
| `Code/CPSock.cpp` | 353-467 (`ReadMessage`), 513-545 (`AddMessage`) | Framing and outbound queue |
| `Code/CPSock.h` | 76 | `ReadMessage` declaration |

### 13.4 Context documents

| File | Use |
|------|-----|
| `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` | Component inventory (line 140), coupling figures, global risk context (lines 293-301, 362-371) |
| `docs/reports/MANIFEST.md` | Component numbering ("GetItem Handler" = #16) |
| `README.md`, `W2PP Code Project.sln` | Build surface (2 projects only; no test project) |

---

End of report.
