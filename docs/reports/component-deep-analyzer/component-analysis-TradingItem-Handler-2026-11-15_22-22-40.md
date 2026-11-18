# Component Deep Analysis Report

**Component:** TradingItem Handler (`Exec_MSG_TradingItem`)
**Primary location:** `Code/TMSrv/_MSG_TradingItem.cpp` (430 lines, ~13 KB)
**Declaration:** `Code/TMSrv/ProcessClientMessage.h:130`
**Dispatch registration:** `Code/TMSrv/ProcessClientMessage.cpp:160-161`
**Build inclusion:** `Code/TMSrv/TMSrv.vcxproj:161`, `Code/TMSrv/TMSrv.vcxproj.filters:264`
**Project:** W2PP - decompiled Win32/MSVC C++ MMORPG server emulator (TMSrv game/world server)
**Analyzed scope:** entire repository root `E:\wyd\test-setup\w2pp`
**Ignored folders (per parameter):** `.agents`, `.codegraph`, `.opencode`, `openspec`
**Analysis timestamp:** 2026-11-15 22:22:40
**Mode:** read-only analysis. No project file was modified.

---

## 1. Executive Summary

### 1.1 Purpose

`Exec_MSG_TradingItem` is the TMSrv handler for client message type `_MSG_TradingItem` (118, `FLAG_CLIENT2GAME | FLAG_GAME2CLIENT`, `Code/Basedef.h:1728`). It implements a **server-authoritative two-slot item relocation transaction**: the client supplies a source slot and a destination slot (each qualified by a storage domain), the server validates the request, performs an atomic content exchange between the two item cells, applies a set of item-family side effects (stack merging, timed-item activation, expiry purge, equipment projection refresh, mount resync, warehouse compaction), and echoes the request packet back so the client mirrors the exchange.

Despite the name and despite sitting next to `_MSG_Trade` / `_MSG_QuitTrade`, this handler does **not** implement the trade window. The trade window, its 15-item escrow buffer (`MSG_Trade.Item[MAX_TRADE]` + `InvenPos[MAX_TRADE]`, `Code/Basedef.h:2047-2057`), the money transfer, the two-party confirmation and the atomic commit all live in `Code/TMSrv/_MSG_Trade.cpp`. The only trade-handshake behaviour present here is a **negative** one: while a trade is open, an item-move request tears the trade down instead of moving the item (lines 31-36). The functional reality is best described as "move/swap item between Equip / Carry / Cargo with server validation", which is the primitive the trade window, the warehouse NPC and the private-shop system all depend on but do not themselves implement.

The architecture report labels this component "item-for-item trade handshake" with afferent 1 / efferent 14. The afferent figure is exact. The efferent figure is understated: this analysis measures **23 distinct first-party callees across 6 modules** (plus 4 CRT primitives), enumerated in section 6.

### 1.2 Role in the system

- Entry point family: one of ~48 `Exec_MSG_*` command handlers selected by a single `switch` in `Code/TMSrv/ProcessClientMessage.cpp:66-...`.
- Upstream: `CPSock` transport (`Code/CPSock.cpp:386-466` frame decode + checksum) -> socket loop (`Code/TMSrv/Server.cpp:3973-4002`) -> `ProcessClientMessage`.
- Downstream: `Basedef.cpp` item primitives, `SendFunc.cpp` client projections, `Server.cpp` session/trade/audit utilities, `GetFunc.cpp` guild helpers, `CMob::GetCurrentScore`, `CPSock::AddMessage`.
- State owned: it mutates `pMob[conn].MOB.Equip[16]`, `pMob[conn].MOB.Carry[64]`, `pUser[conn].Cargo[128]` and, indirectly, `pMob[conn].MOB.CurrentScore`, party summons (via `MountProcess`) and the trade/auto-trade state of both participants.

### 1.3 Key findings

| # | Severity | Finding |
|---|----------|---------|
| 1 | High | Warehouse NPC identity guard at line 48 is logically unsatisfiable (`WarpID <= MAX_USER && WarpID >= MAX_MOB` can never be true), so a **negative `WarpID` reaches `pMob[WarpID]` at line 54**, an out-of-bounds array read driven by client-controlled input. |
| 2 | High | **No self-swap rejection.** When `SrcPlace/SrcSlot` equals `DestPlace/DestSlot`, both item pointers alias the same cell and the stack-merge branch (lines 248-285) can inflate a stack (e.g. 100 -> 120 units in one cell), a duplication vector for the whitelisted stackable item families. |
| 3 | High | Rejected swaps (equipment requirement failure) are **silently dropped**: the `else` branch at lines 405-428 restores the snapshot but sends no message, no log entry and no crack point, leaving the client's optimistic view divergent until the next resync. |
| 4 | Medium | The guild-item weekday/level gate at line 175 is **dead code**: `g_Week` is defined once as `0` (`Code/TMSrv/Server.cpp:287`) and never assigned, so the "Sunday only" restriction is never enforced. |
| 5 | Medium | All `GetGuild(...)` calls in this handler (lines 181, 182, 323, 326, 359) are **no-ops**: `GetGuild(int conn)` (`Code/TMSrv/GetFunc.cpp:1862-1867`) computes into unused locals, and `int GetGuild(STRUCT_ITEM*)` return values are discarded. Guild binding is therefore never recomputed on an item move. |
| 6 | Medium | `BASE_CanCargo` contains a **stride defect** (`Code/Basedef.cpp:4531` uses `CARGOGRIDY` where the row stride is `CARGOGRIDX`), which feeds both the warehouse compaction loop (lines 366-403) and trade-space checks elsewhere. |
| 7 | Medium | Two cells of the warehouse array (`MAX_CARGO-2` = 126, `MAX_CARGO-1` = 127) are accepted as legitimate destinations but are outside the 9x14 = 126 renderable grid, are persisted by `SaveUser` (`Code/TMSrv/Server.cpp:7065`) and are only drained opportunistically after a cargo-involving swap. |
| 8 | Medium | Packet-shape validation for this message type is **disabled**: the `_MSG_TradingItem` size check at `Code/Basedef.cpp:6537` sits inside a commented-out block (`BASE_CheckPacket`, lines 6475-6596, returns `FALSE` unconditionally). |
| 9 | Medium | Dead validation branches at lines 194-204 and 214-224 (`if (!Can)` inside `ITEM_PLACE_CARRY` blocks, with `error` frozen at `-2`) never execute; they read as live anti-dupe logic but are inert. |
| 10 | Low | Estimated cyclomatic complexity of the single function is ~127 (59 `if`, 7 `else`, 42 `&&`, 25 `||`), with nesting depth up to 8 and roughly 31 distinct magic item/flag literals. |

### 1.4 Confidence statement

All statements about control flow and callee behaviour are verified against source. Statements about *gameplay intent* (what item 747 is, what class 3 + `LearnedSkill & 0x2000` represents, what the 120 cap means to players) are inferred and flagged with confidence markers. There are no tests, no specification documents and no comments in the component that describe its contract; `Code/Basedef.h:458-459` and `Code/TMSrv/Server.cpp:646` contain the only two useful inline hints ("747 = Crown on itemlist").

---

## 2. Data Flow Analysis

### 2.1 Primary accepted flow (source slot to destination slot)

```
 1. CPSock::ReadMessage decodes one frame from pRecvBuffer: Size bounded to
    [sizeof(HEADER) .. MAX_MESSAGE_SIZE=8192] (Code/CPSock.cpp:386-411),
    key/de-key transform and checksum compare (Code/CPSock.cpp:424-464).
 2. Socket loop dispatches ProcessClientMessage(User, Msg, FALSE)
    (Code/TMSrv/Server.cpp:3975-4001).
 3. ProcessClientMessage validates conn range, ServerDown < 120, refreshes
    LastReceiveTime, rejects ClientTick == SKIPCHECKTICK, then routes
    case _MSG_TradingItem -> Exec_MSG_TradingItem(conn, pMsg)
    (Code/TMSrv/ProcessClientMessage.cpp:42-64, 160-161).
 4. Exec_MSG_TradingItem aliases the receive buffer as MSG_TradingItem (line 22).
 5. Liveness gate: Hp == 0 || Mode != USER_PLAY  ->  SendHpMode + crack point
    (lines 24-29).
 6. Trade-state gate: Trade.OpponentID != 0  ->  RemoveTrade(both) + return
    (lines 31-36).
 7. Auto-trade gate: TradeMode != 0  ->  "cannot while auto-trading" + return
    (lines 38-42).
 8. Warehouse gate (only when a Cargo slot is involved): character privilege
    (lines 44-45) or warehouse-NPC identity (48-58) and 33x33 proximity (60-69).
 9. Mount snapshot: Mount = &pMob[conn].MOB.Equip[14] (line 71).
10. Pointer resolution x4: DestItem/SrcItem and the duplicate pair dItem/sItem
    via GetItemPointer(&MOB, pUser[conn].Cargo, place, slot) (lines 76-80).
11. Slot-domain validation: place in {0,1,2}, per-domain slot bounds, with a
    Log() line on each rejection (lines 83-150).
12. Carry capacity: DestPlace == CARRY && DestSlot >= MaxCarry -> silent return
    (lines 152-153).
13. Resolvability: either pointer NULL -> Log + return (lines 155-159).
14. Crown/cargo suppression test (line 161) - skips the whole swap body when a
    747 crown is combined with a cargo coordinate.
15. Guild-item gate: 508/522/526..537 into an equip slot, weekend + level<1000
    (lines 163-179, dormant because g_Week == 0).
16. Equipment requirement checks: BASE_CanEquip(incoming item, CurrentScore,
    target equip index, Class, Equip vector, extra) (lines 184-225).
17. Snapshot both cells into stack locals save1/src, save2/dest (lines 229-246).
18. Stack merge for families {413, 412, 419, 420, 2390..2419} (lines 248-285).
19. Timed-item activation for 4150..4188 with empty effects: BASE_SetItemDate(30)
    (lines 287-296).
20. Expiry purge for 3980..3989 and 4150..4188 when BASE_CheckItemDate() is true
    (lines 297-307).
21. Audit: BASE_GetItemCode x2 + sprintf + ItemLog("tradingitem, ...") (309-316).
22. Commit: memcpy(SrcItem, &save2); memcpy(DestItem, &save1) (318-319).
23. Guild re-evaluation attempts (no-op) (322-326).
24. Derived score recompute: pMob[conn].GetCurrentScore(conn) (line 328).
25. Confirmation: the request packet itself is queued back to the client
    pUser[conn].cSock.AddMessage(m, sizeof(MSG_TradingItem)) (line 330).
26. Two-handed/second-slot consolidation: Equip[6] empty + Equip[7].EF_POS != 128
    -> move 7 to 6 and emit a synthetic MSG_TradingItem (lines 332-352).
27. Projection refresh: SendEquip when an equip slot was involved (354-355);
    GetGuild(conn) + SendScore when an equip slot was involved (357-361).
28. Mount resync when either resulting item is in 2330..2359: MountProcess
    (lines 363-364).
29. Warehouse compaction: staged cells 126/127 re-placed into grid cells 0..125
    via BASE_CanCargo + SendItem per relocation (lines 366-403).
30. Return.
```

### 2.2 Rejected flow (equipment check fails)

```
 1-17. identical to 2.1
18. Can && YouCan evaluates false -> else branch (lines 405-428)
19. save1/save2 are re-copied back into SrcItem/DestItem (a no-op restore,
    since nothing had been written yet)
20. return with no client message, no Log(), no AddCrackError(), no echo
    -> the client keeps whatever optimistic visual state it applied
```

### 2.3 Flow classification per request shape

| Request shape | Early exit point | Side effects |
|---|---|---|
| Character dead or not `USER_PLAY` | 24-29 | `_MSG_SetHpMode`, crack point 1/19 |
| Trade open with a partner | 31-36 | trade torn down on both sides, signal 900 |
| Auto-trade (private shop) open | 38-42 | message panel only |
| Cargo coordinate without privilege/NPC/range | 50-69 | client signal `ESCENE_FIELD`/412 |
| Cargo coordinate with negative `WarpID` | 54 | out-of-bounds `pMob[...]` read, then either signal 412 or a crash |
| Illegal place/slot | 87-147 | `Log()` line only |
| Carry destination beyond `MaxCarry` | 152-153 | none |
| Unresolvable cell (invalid `sIndex`) | 155-159 | `Log()` line only |
| Crown + cargo coordinate | 161 | nothing at all (silent no-op) |
| Guild item out of window (dormant) | 175-179 | message `_NN_Only_Sunday` |
| Equip requirement failure | 405-428 | silent revert |
| Accepted | 318-330 | commit + side effects + echo |

### 2.4 Ordering property worth noting

For merged stacks the two `SendItem` calls (lines 266-267 / 282-283) are issued **before** the echo at line 330. Because the echo makes the client swap its two cells locally, and the content updates were applied to the pre-swap cells, the client's post-swap state converges with the server's `memcpy` result at lines 318-319. The same pre-swap-then-echo ordering is what makes the `BASE_SetItemDate` / `BASE_ClearItem` sends at lines 290, 295, 300 and 306 land in the correct final cell. This ordering is load-bearing and is not documented in code.

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Validation / authorization | Request requires `Hp != 0` and `Mode == USER_PLAY` | `Code/TMSrv/_MSG_TradingItem.cpp:24-29` |
| BR-02 | Security accounting | Rejected liveness gate accrues 1 crack point of type 19; 2e9 points force logout | `_MSG_TradingItem.cpp:27`, `Code/TMSrv/Server.cpp:684-707` |
| BR-03 | State invariant | An item-move request during an open trade cancels the trade instead of moving the item | `_MSG_TradingItem.cpp:31-36`, `Code/TMSrv/Server.cpp:7319-7352` |
| BR-04 | Validation | No item movement while `TradeMode` (private shop) is active | `_MSG_TradingItem.cpp:38-42` |
| BR-05 | Authorization | Warehouse (Cargo) access is allowed without an NPC when `Class == 3` and `LearnedSkill & 0x2000` | `_MSG_TradingItem.cpp:44-45` |
| BR-06 | Validation | Otherwise Cargo access requires a `MOB_PEACE` mob with `Merchant == 2` identified by `WarpID` | `_MSG_TradingItem.cpp:46-58` |
| BR-07 | Validation / anti-cheat | The warehouse NPC must be within +/-33 grid cells on both axes | `_MSG_TradingItem.cpp:60-69`, `Code/Basedef.h:95-96` |
| BR-08 | Validation | Storage domain must be 0 (Equip), 1 (Carry) or 2 (Cargo) | `_MSG_TradingItem.cpp:83-150`, `Code/Basedef.h:86-88` |
| BR-09 | Validation | Equip destination index must be in [0, 14] | `_MSG_TradingItem.cpp:109-113` |
| BR-10 | Validation | Equip source index must be in [1, 14]; slot 0 is never a source | `_MSG_TradingItem.cpp:143-147` |
| BR-11 | Validation | Carry coordinates must be in [0, 59] (`MAX_CARRY-4`) | `_MSG_TradingItem.cpp:87, 121`, `Code/Basedef.h:76` |
| BR-12 | Validation | Cargo coordinates must be < 128, of which only 0..125 are grid cells | `_MSG_TradingItem.cpp:100, 134`, `Code/Basedef.h:77, 90-91` |
| BR-13 | Validation / capacity | Carry destination must be below `MaxCarry` (30/45/60); the source is not capacity-checked | `_MSG_TradingItem.cpp:152-153`, `Code/TMSrv/ProcessDBMessage.cpp:751-757` |
| BR-14 | Validation | Both participating cells must resolve to a valid `sIndex` in `[0, 6500)` | `_MSG_TradingItem.cpp:155-159`, `Code/Basedef.cpp:2340-2372` |
| BR-15 | Business logic | A 747 crown combined with a cargo coordinate suppresses the swap entirely | `_MSG_TradingItem.cpp:161` |
| BR-16 | Business logic (dormant) | Guild items (508/522/526..537) entering an equip slot are blocked on non-Sunday for level < 1000 | `_MSG_TradingItem.cpp:163-179` |
| BR-17 | Business logic (no-op) | Guild binding is recomputed after the swap via `GetGuild` | `_MSG_TradingItem.cpp:181-182, 322-326, 359`, `Code/TMSrv/GetFunc.cpp:1862-1874` |
| BR-18 | Validation | An item arriving in an equip slot must satisfy `BASE_CanEquip` (slot position, class, master class, mob type, level, str/int/dex/con requirements) | `_MSG_TradingItem.cpp:184-225`, `Code/Basedef.cpp:4293-4411` |
| BR-19 | Business logic | Dual weapon/shield validation uses the pre-swap equipment vector for the opposite hand | `Code/Basedef.cpp:4311-4337`, called from `_MSG_TradingItem.cpp:192, 212` |
| BR-20 | Transaction semantics | The exchange is all-or-nothing; a failed check reverts silently with no client notification | `_MSG_TradingItem.cpp:226, 405-428` |
| BR-21 | Business logic | Stack merging applies only to item families 413, 412, 419, 420 and 2390..2419, and only while both amounts are < 120 | `_MSG_TradingItem.cpp:248-254` |
| BR-22 | Business logic | Stack total is capped at 120; surplus remains in the other cell; an amount <= 0 counts as 1 | `_MSG_TradingItem.cpp:255-284` |
| BR-23 | Business logic | Items 4150..4188 with all three effects empty receive a 30-day lifetime on first movement | `_MSG_TradingItem.cpp:287-296`, `Code/Basedef.cpp:6732-6761` |
| BR-24 | Business logic | Expired 3980..3989 and 4150..4188 items are destroyed by the move | `_MSG_TradingItem.cpp:297-307`, `Code/Basedef.cpp:6763-6780` |
| BR-25 | Auditability | Every accepted swap writes one `tradingitem, ...` item-log record with both item codes | `_MSG_TradingItem.cpp:309-316`, `Code/TMSrv/Server.cpp:9264-9284` |
| BR-26 | Derived state | Character stats are recomputed after every accepted swap | `_MSG_TradingItem.cpp:328`, `Code/TMSrv/CMob.cpp:598` |
| BR-27 | Protocol semantics | The server confirms an accepted swap by echoing the request packet unchanged | `_MSG_TradingItem.cpp:330`, `Code/CPSock.cpp:513-541` |
| BR-28 | Business logic | If equip slot 6 is empty and slot 7 holds an item whose `EF_POS != 128`, slot 7 is relocated to slot 6 | `_MSG_TradingItem.cpp:332-352` |
| BR-29 | Projection | Equipment and score projections are refreshed only when an equip coordinate was involved | `_MSG_TradingItem.cpp:354-361`, `Code/TMSrv/SendFunc.cpp:1084-1190` |
| BR-30 | Business logic | Moving a 2330..2359 item re-runs mount/summon reconciliation | `_MSG_TradingItem.cpp:363-364`, `Code/TMSrv/Server.cpp:4178-4221` |
| BR-31 | Business logic | After any cargo-involving swap, staged cells 126/127 are compacted into the first fitting grid cell | `_MSG_TradingItem.cpp:366-403`, `Code/Basedef.cpp:4480-4537` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 Session liveness and player-mode gate

**Overview**
The handler refuses to process any relocation unless the character is alive and the connection is in the in-game play state.

**Detailed description**

The gate is the first statement of the function (lines 24-29) and reads two independent pieces of session state: `pMob[conn].MOB.CurrentScore.Hp` and `pUser[conn].Mode`. A character whose current HP is exactly zero, or a connection that is not in `USER_PLAY` (22, `Code/TMSrv/CUser.h:36`), cannot move items. The comparison against zero is exact, so the rule is literally "HP is not zero", not "HP is positive"; a hypothetical negative HP value would pass the gate. HP values are produced by the combat path, and this analysis did not find a producer that writes a negative current HP, so the practical difference is nil, but the rule as written is an equality test rather than an inequality.

Rejection is not silent. `SendHpMode(conn)` (`Code/TMSrv/SendFunc.cpp:1622-1645`) emits `_MSG_SetHpMode` (146) carrying the current HP and the mode value, which is the mechanism by which a client whose local HP or UI state drifted is resynchronised before it is told that its request was illegitimate. `AddCrackError(conn, 1, 19)` then accrues one crack point and writes a log line with the account name and IP. The pairing of a resync projection with a penalty record is the standard shape used by every handler family in this codebase.

The gate is weaker than the equivalent gate in `_MSG_Trade` (`Code/TMSrv/_MSG_Trade.cpp:24-30`), which additionally calls `RemoveTrade(conn)` on the rejection path. Here no trade state is cleared. A connection that transitions out of `USER_PLAY` (for example during logout or a save-in-progress transition) while still holding a non-zero `Trade.OpponentID` keeps that stale state until another path clears it. Because `RemoveTrade` is idempotent and bound-checked, the consequence is a lingering trade window rather than memory corruption.

**Rule workflow**
```
request -> Hp == 0 ?  --yes--> SendHpMode + AddCrackError(1, 19) -> discard
                    \--no--> Mode != USER_PLAY ? --yes--> same path
                                              \--no--> continue to BR-03
```

---

### Business Rule: BR-02 Crack-error accounting for rejected requests

**Overview**
Malformed or illegitimate requests contribute to a per-session error budget whose exhaustion forcibly logs the character out.

**Detailed description**

`AddCrackError` (`Code/TMSrv/Server.cpp:684-707`) adds a caller-supplied weight to `pUser[conn].NumError`, writes a log line `cra point: <weight>  type: <type>` unless the type is one of the three excluded log types (3, 8, 15), and tests the counter against `2000000000`. On overflow it sends `_NN_Bad_Network_Packets`, calls `CharLogOut(conn)` and returns `TRUE`. Type 19, used by this handler, is not excluded, so every rejection of a dead/not-playing request produces a log record plus one point. The other handler in the trade family, `_MSG_Trade`, uses a weight of 5 for its equivalent gate (`_MSG_Trade.cpp:27`), so the same class of violation is punished five times harder there.

Inside this component only the liveness gate accrues points. Every other rejection path - illegal place, illegal slot, unresolvable cell, crown suppression, failed equipment check - produces either a `Log()` line or nothing at all, and never touches `NumError`. From an abuse-detection standpoint the interesting signals (a client enumerating illegal slot combinations, or hammering a rejected equip swap) are therefore invisible to the crack subsystem.

There is no decay, no per-window rate limit and no per-handler threshold; the only escalation is the single 2e9 ceiling. Since the counter is incremented by one per request, the ceiling is not reachable through this handler in any realistic session, so the observable effect of BR-02 here is audit-log volume, not lockout.

**Rule workflow**
```
rejection -> AddCrackError(conn, 1, 19)
          -> NumError += 1
          -> log "cra point: 1 type: 19"
          -> NumError >= 2e9 ? --yes--> message + CharLogOut
                              \--no--> return FALSE
```

---

### Business Rule: BR-03 Open-trade precedence (an item move cancels an open trade)

**Overview**
If the player is currently trading, an item-movement request is interpreted as an intent to abort the trade: the trade is destroyed on both sides and no item is moved.

**Detailed description**

The test is `if (pUser[conn].Trade.OpponentID)` (line 31). When non-zero, the handler calls `RemoveTrade` for the opponent and then for the requesting connection, and returns without touching a single item cell. `RemoveTrade` (`Code/TMSrv/Server.cpp:7319-7352`) zeroes the whole `MSG_Trade` block, resets every `InvenPos[i]` to -1, clears the auto-trade buffer and its positions, sends client signal 900 (the trade-window close signal), and, if `TradeMode` was set, recreates the player's private-shop mob. The rule therefore has three effects at once: trade teardown, private-shop teardown and client UI close.

This is the only trade-handshake behaviour this component implements, and it is a purely defensive one. Its purpose is to protect the trade escrow from drifting. The escrow is a set of copies: `pUser[conn].Trade.Item[i]` holds a snapshot of the offered item plus an inventory index, while the real item stays in `MOB.Carry`. `_MSG_Trade` re-validates those snapshots with `memcmp` against live carry slots before committing (`Code/TMSrv/_MSG_Trade.cpp:56-85`). Allowing the relocation handler to run while a trade window is open would create two writers over the same cells, so the design chooses to cancel rather than to reconcile. The escrow itself - capture, two-party check, money, commit, persistence - is entirely outside this file.

Two properties deserve note. First, the rule does not verify mutuality: it never checks that `pUser[opponent].Trade.OpponentID == conn`. `RemoveTrade` is guarded by range checks and is idempotent, so a forged or stale `OpponentID` can close an unrelated player's trade window (that player receives signal 900) but cannot corrupt item state. Second, the relocation request that triggered the cancellation is discarded, so a client that intended both a move and a cancellation loses the move. There is no compensating echo, no message and no log for this path.

**Rule workflow**
```
Trade.OpponentID != 0 ?
  --yes--> RemoveTrade(OpponentID)
           RemoveTrade(conn)          [clears Trade + AutoTrade, signal 900, shop teardown]
           return                     [the requested swap never happens]
  --no---> BR-04
```

---

### Business Rule: BR-04 Auto-trade (private shop) lock

**Overview**
While the player is operating a private shop (`TradeMode`), item relocation is refused with a dedicated client message.

**Detailed description**

`TradeMode` is set to 1 when a player opens an auto-trade shop (`Code/TMSrv/_MSG_SendAutoTrade.cpp:105`) and cleared by `RemoveTrade` (`Server.cpp:7350`) and by character load (`Code/TMSrv/ProcessDBMessage.cpp:845`). The same refusal guard is repeated in at least ten handlers (`_MSG_Action.cpp:46`, `_MSG_Attack.cpp:27`, `_MSG_Buy.cpp:32`, `_MSG_DropItem.cpp:38`, `_MSG_GetItem.cpp:38`, `_MSG_Quest.cpp:42`, `_MSG_ReqBuy.cpp:32`, `_MSG_Sell.cpp:38` for cargo only, `_MSG_SendAutoTrade.cpp:39`, `_MSG_UseItem.cpp:58`). The uniformity is the point: the shop publishes an inventory snapshot to other clients, and every mutation path must be closed while that snapshot is live.

The snapshot is consumed by the browsing side: `_MSG_ReqTradeList.cpp:36` rejects queries against players whose `TradeMode` is zero, and `SendFunc.cpp:304, 535` only routes shop traffic for `TradeMode == 1` sessions. If the owner could move items mid-listing, a buyer would price an item from the listing and then receive a different cell content at purchase time. The lock is the simplest possible mitigation: no movement at all while the shop exists.

Refusal here is a message panel only (`_NN_CantWhenAutoTrade`, index 170 in `Code/TMSrv/Language.h:190`). No crack point, no trade teardown, no log. The shop survives the rejected request, and the client stays open, which is the correct trade-off for a legitimate-but-blocked action.

**Rule workflow**
```
TradeMode != 0 ?
  --yes--> SendClientMessage(_NN_CantWhenAutoTrade) -> return
  --no---> BR-05 (warehouse gate, only if a cargo coordinate is present)
```

---

### Business Rule: BR-05 Warehouse access privilege

**Overview**
A player may address the account warehouse (Cargo) without standing next to an NPC when the character satisfies a class-plus-skill condition.

**Detailed description**

The condition at lines 44-45 is `(pMob[conn].MOB.Class != 3 || !(pMob[conn].MOB.LearnedSkill & 0x2000))`: the NPC requirements of BR-06 and BR-07 are demanded only when this disjunction is true, i.e. when the character is *not* (class 3 and holding skill bit 0x2000). A qualifying character therefore reaches the warehouse from anywhere on the map. The semantics of `Class == 3` and of `LearnedSkill & 0x2000` are not documented anywhere in the repository; the only other uses of a class-3 test combined with skill bits are `_MSG_Attack.cpp:459` (`Class == 3 && LearnedSkill & 0x200000`) and the wolf/asta checks in `Code/Basedef.cpp:3439-3441`, which use different bits. Confidence that this is a "remote warehouse" privilege (a premium/skill-granted convenience) is medium; confidence that it bypasses the NPC checks is high, since that is what the boolean composition does.

`LearnedSkill` is a `long` bitfield in `STRUCT_MOB` (`Code/Basedef.h:461`), so `0x2000` is a single bit and the test is exact. The privilege is per-character, not per-account: it is not persisted in the account file structure this component touches, it is part of the mob record.

Because the privilege short-circuits the entire block, a qualifying character never has `m->WarpID` dereferenced. That incidentally neutralises the BR-06 defect for those characters only; every other player still supplies the field that reaches the out-of-bounds read described under BR-06.

**Rule workflow**
```
cargo coordinate present ?
  --no---> skip the whole warehouse gate
  --yes--> (Class == 3 && LearnedSkill & 0x2000) ?
             --yes--> skip NPC checks, continue to BR-08
             --no---> BR-06 then BR-07
```

---

### Business Rule: BR-06 Warehouse NPC identity validation

**Overview**
For players without the BR-05 privilege, the `WarpID` field must identify a peaceful mob configured as a merchant of type 2.

**Detailed description**

The intended first check is a range test on `m->WarpID` (an `int` taken directly from the client packet), meant to reject indices that are not spawned mobs. As written, `if (WarpID <= MAX_USER && WarpID >= MAX_MOB)` (line 48) requires a value that is simultaneously at most 1000 and at least 25000; that conjunction is unsatisfiable, so the guard never fires and control always reaches `pMob[WarpID]` at line 54. For `WarpID` in the legitimate mob range the subsequent checks do the real work: `pMob[WarpID].Mode != MOB_PEACE` (4, `Code/TMSrv/CMob.h:30`) or `pMob[WarpID].MOB.Merchant != 2` produce a client signal `ESCENE_FIELD`/412 and a return.

For an out-of-range index the behaviour depends on the value. `WarpID < 0` indexes before the start of the `CMob pMob[MAX_MOB]` array (`Code/TMSrv/CMob.h:122`), which is undefined behaviour: in the best case `Mode`/`Merchant` read garbage and the request is rejected with signal 412, in the worst case the read faults inside a 25,000-element object array and terminates the world server. `WarpID >= MAX_MOB` reads past the end of the array with the same class of consequence. `WarpID` values in `[0, 1000)` alias player slots; a playing character's `Mode` is `USER_PLAY` rather than `MOB_PEACE`, so those are rejected by the second check, though a disconnected-but-allocated slot could in principle present `Mode == 0` and `Merchant == 0`, which also fails the `Merchant != 2` test.

The rule is therefore "correct in intent, broken in range enforcement". It is the single most dangerous defect in the component because the input is fully attacker-controlled, unauthenticated, and dereferenced before any validation of the character's own position; the proximity check of BR-07 comes after and therefore cannot protect it.

**Rule workflow**
```
WarpID = m->WarpID
range guard (WarpID <= 1000 && WarpID >= 25000)   [never true]
-> pMob[WarpID].Mode != MOB_PEACE || Merchant != 2 ?
     --yes--> SendClientSignal(conn, ESCENE_FIELD, 412) -> return
     --no---> BR-07
```

---

### Business Rule: BR-07 Warehouse proximity envelope

**Overview**
The warehouse NPC must be within a 33-by-33 grid envelope centred on the player's own position.

**Detailed description**

Lines 60-69 compare `pMob[conn].TargetX/TargetY` against `pMob[WarpID].TargetX/TargetY` and reject with client signal `ESCENE_FIELD`/412 when any axis exceeds `VIEWGRIDX` (33) or `VIEWGRIDY` (33) (`Code/Basedef.h:95-96`). The envelope is symmetric and inclusive, defined by four separate comparisons (`< tx-33`, `> tx+33`, `< ty-33`, `> ty+33`), so a distance of exactly 33 is accepted. The constants are the same view-grid dimensions used for visibility, which means the warehouse is reachable exactly as far as the player can see the NPC.

The check is a positional anti-cheat measure, not a cost or gameplay measure. Its intent is to stop a client from addressing the warehouse through a spoofed `WarpID` while standing anywhere on the map. Because it uses `TargetX/TargetY` (the destination cell of the current movement) rather than `PosX/PosY`, a player who is mid-step is evaluated at the step target; a player who is teleporting or being pushed may therefore momentarily satisfy the check from a position the server has not yet committed.

Note the ordering: the identity check of BR-06 runs first, so proximity is evaluated against a mob that has already been accepted as `Merchant == 2`. A forged `WarpID` pointing at a genuine warehouse NPC 400 cells away is rejected here; a forged `WarpID` pointing at an unrelated mob is rejected at BR-06.

**Rule workflow**
```
PosX/PosY = pMob[conn].TargetX/TargetY
tx/ty     = pMob[WarpID].TargetX/TargetY
|PosX - tx| > 33 || |PosY - ty| > 33 ?
  --yes--> SendClientSignal(conn, ESCENE_FIELD, 412) -> return
  --no---> BR-08
```

---

### Business Rule: BR-08 Storage domain whitelist

**Overview**
Only the three defined storage domains are addressable, and each has its own slot bound; anything else is rejected before any item is read.

**Detailed description**

The `place` byte of the packet is validated in two mirrored blocks (lines 83-116 for the destination, 117-150 for the source). A non-zero place must be either 1 (`ITEM_PLACE_CARRY`) or 2 (`ITEM_PLACE_CARGO`); any other non-zero value produces `Log("err,trading fails.SwapItem source type")` or `Log("DEBUG:Trading Fails.SwapItem dest type")` and a return. Place 0 (`ITEM_PLACE_EQUIP`) is handled by the `else` arms. The constants are defined at `Code/Basedef.h:86-88`; there is no fourth domain, and the game has no other persistent item container reachable through this message.

The two blocks are structurally identical but not textually identical, which matters for maintenance: the destination block logs `err,trading fails.SwapItem-Carry` / `err,trading fails.SwapItem-Cargo` / `err,trading fails.SwapItem-Equip`, while the source block logs `DEBUG:Trading Fails.Swap-Carr` / `DEBUG:Trading Fails.Swap-Cargo` / `DEBUG:Trading Fails.Swap-Equip`. The differing prefixes mean log-based detection rules must match both families. Neither family increments the crack counter.

The rejection is a hard stop with no client feedback. Because the client's own UI normally cannot produce an illegal place value, this path is only reachable from a crafted packet or a desynchronised client, and it is treated as "drop the request" rather than "tell the user".

**Rule workflow**
```
place == 0 ? --yes--> equip arm  -> BR-09 / BR-10
           \--no--> place == 1 ? --yes--> carry arm -> BR-11
                              \--no--> place == 2 ? --yes--> cargo arm -> BR-12
                                                 \--no--> Log + return
```

---

### Business Rule: BR-09 Equipment destination slot domain

**Overview**
A destination in the equipment domain must fall in `[0, 14]`.

**Detailed description**

Line 109 rejects `m->DestSlot < 0 || m->DestSlot >= MAX_EQUIP-1`, where `MAX_EQUIP` is 16 (`Code/Basedef.h:75`). Since `DestSlot` is `unsigned char`, the `< 0` comparison is always false and is dead; the effective bound is `< 15`, so destinations 0..14 are accepted and 15 is rejected. `MAX_EQUIP-1` is used rather than `MAX_EQUIP`, which excludes index 15. Slot 15 is also excluded inside `BASE_CanEquip` (`Code/Basedef.cpp:4300-4301` returns `FALSE` for `Pos == 15`), so the two layers agree.

The accepted indices map to the equipment layout used by `SendEquip` and by the pawn-shop/visual code: 0 face, 1 weapon, 6 and 7 the paired weapon/shield slots, 12 the accessory handled by the minute timer, 13 the fairy, 14 the mount. This handler does not name those roles; it only enforces the numeric range and then delegates the semantic check to BR-18.

When this arm is taken, the code also sets the local `DestPlace = 1; SrcPlace = 1;` (lines 114-115). Those locals are not storage domains; they are flags used later at lines 354-361 to decide whether equipment projections must be refreshed (see BR-29). The reuse of the constants `ITEM_PLACE_CARRY` for this purpose is the single most confusing construct in the file.

**Rule workflow**
```
DestPlace == 0 ?
  --> DestSlot in [0, 14] ? --no--> Log("...SwapItem-Equip") + return
                            \--yes--> DestPlace = 1; SrcPlace = 1
                                      continue to source validation
```

---

### Business Rule: BR-10 Equipment source slot exclusion

**Overview**
Slot 0 of the equipment vector can be a destination but never a source.

**Detailed description**

Line 143 rejects `m->SrcSlot <= 0 || m->SrcSlot >= MAX_EQUIP-1`, so sources 1..14 are accepted and both 0 and 15 are refused. The asymmetry with BR-09 (which accepts 0 as a destination) is deliberate by construction but undocumented: the only way to read it is to compare the two comparisons. Slot 0 is the face item, which the equipment projection and the appearance system derive from; allowing it to be moved out through a generic swap would let a client cycle appearance items and, more concretely, would let an item enter a slot whose validation path (`BASE_CanEquip` with `Pos == 0`) is the least exercised.

The practical consequence for players is that the face item cannot be removed by dragging; removal must go through whatever dedicated path the client and other handlers provide. Every other equip slot, including the paired 6/7 slots and the mount slot 14, can be the source of a move.

The rejection is a `Log("DEBUG:Trading Fails.Swap-Equip")` line with no client message and no crack point, so a client that offers slot 0 as a source sees the swap simply not happen.

**Rule workflow**
```
SrcPlace == 0 ?
  --> SrcSlot in [1, 14] ? --no--> Log("...Swap-Equip") + return
                           \--yes--> DestPlace = 1; SrcPlace = 1
                                     continue to BR-13
```

---

### Business Rule: BR-11 Carry coordinate bound (60 slots)

**Overview**
Carry-domain coordinates are limited to the first 60 of the 64 inventory cells, on both the source and destination side.

**Detailed description**

Both arms test `slot >= MAX_CARRY-4` (lines 87 and 121), where `MAX_CARRY` is 64 (`Code/Basedef.h:76`), so the accepted range is 0..59. The exclusion of the last four cells is consistent with the rest of the server: `_MSG_Trade.cpp:60, 74, 205` uses the same `MAX_CARRY-4` bound for trade offers, and `Server.cpp:719-725` uses `MaxCarry` when searching for a free cell. The reserved cells 60 and 61 hold the capacity-expansion charms (`sIndex == 3467`) that raise `MaxCarry` from 30 to 45 to 60 (`_MSG_UseItem.cpp:5688-5694`, `ProcessSecMinTimer.cpp:571-596`), and 62/63 are unused padding.

Because the expansion charms live at exactly the boundary, they are structurally immovable through this message: a request naming slot 60 or 61 is rejected before any pointer is taken. Removing or inserting a charm therefore has to happen through a different handler, which is a design decision rather than an oversight - the charms are what define the usable range, so permitting them to move through the same primitive would create a bootstrap problem.

The bound is static (`MAX_CARRY-4`), unlike BR-13 which is dynamic (`MaxCarry`). A character with `MaxCarry == 30` can therefore still move an item *out* of slot 45, because the static bound permits 0..59 and only the destination is capacity-checked. This asymmetry is what allows a player whose capacity shrank (charm expired while items were above slot 29) to recover their items instead of stranding them.

**Rule workflow**
```
place == 1 ?
  --> slot >= 60 ? --yes--> Log("...Swap-Carry"/"...Swap-Carr") + return
                  \--no--> accept coordinate
```

---

### Business Rule: BR-12 Cargo coordinate bound and the two staging cells

**Overview**
Cargo coordinates are accepted up to 127, while only 0..125 correspond to renderable warehouse grid cells; cells 126 and 127 are staging space.

**Detailed description**

The handler tests `m->DestSlot > MAX_CARGO` (line 100) and `m->SrcSlot > MAX_CARGO` (line 134) - comparisons that can never be true because both fields are `unsigned char` - and then relies on `GetItemPointer` for the real bound, `pos >= MAX_CARGO` (`Code/Basedef.cpp:2360-2361`). `MAX_CARGO` is 128 (`Code/Basedef.h:77`) but the warehouse grid is 9 columns by 14 rows (`CARGOGRIDX`/`CARGOGRIDY`, `Code/Basedef.h:90-91`), i.e. 126 cells. Cells 126 and 127 (written in this file as `MAX_CARGO-2` and `MAX_CARGO-1`, lines 366-367) are therefore addressable by any client but invisible to the grid-based UI.

The intended role of those cells is a staging area: after a cargo-involving swap, the compaction loop (BR-31) tries to place them into grid cells 0..125 through `BASE_CanCargo`. Nothing in this handler prevents a request whose destination *is* 126 or 127, and nothing prevents an item from remaining there when the grid is full. Because `SaveUser` persists the entire `Cargo[128]` array (`Code/TMSrv/Server.cpp:7065`, `MSG_DBSaveMob.Cargo[MAX_CARGO]` at `Code/Basedef.h:989`), a stranded staging item survives logout and remains invisible to the player until a later cargo operation drains it. From the player's perspective this is an item hiding in a cell they cannot see; from an anti-duplication perspective it is a second, off-grid capacity of two items.

The wider consequence is that the effective warehouse capacity exceeds what the UI advertises, and that any audit that counts "items in grid" can under-count the account's holdings by two.

**Rule workflow**
```
place == 2 ?
  --> slot <= 128 (dead test) and GetItemPointer(slot < 128) ?
        --no--> Log("...Swap-Cargo") + return
        --yes--> coordinate accepted (126/127 are staging, not grid)
```

---

### Business Rule: BR-13 Carry capacity gate (destination only)

**Overview**
A carry destination must be below the character's current `MaxCarry`, which is 30, 45 or 60 depending on expansion charms; sources are not capacity-checked.

**Detailed description**

Line 152 rejects a `DestPlace == ITEM_PLACE_CARRY` request whose `DestSlot >= pMob[conn].MaxCarry`. `MaxCarry` is derived, not stored: character load sets it to 30 and adds 15 for each `sIndex == 3467` charm found in carry slots 60 and 61 (`Code/TMSrv/ProcessDBMessage.cpp:751-757`), and the same computation is repeated when a charm is consumed or expires (`_MSG_UseItem.cpp:5688-5694`, `Code/TMSrv/ProcessSecMinTimer.cpp:571-596`).

The rule is intentionally one-sided. Source cells are bounded only by the static BR-11 limit of 59, so an item that sits above the current `MaxCarry` can still be moved out. This preserves access to items for a player whose capacity shrank after the items were placed (charm expired, item consumed, capacity recomputed), which would otherwise be a permanent item lockout. The cost is that the invariant "no item exists above `MaxCarry`" is false, so every consumer of `Carry` must use its own bound; the codebase does this inconsistently (`Server.cpp:719-725` uses `MaxCarry`, `_MSG_Trade.cpp:60` uses `MAX_CARRY-4`).

Rejection here is the only silent early exit in the slot-validation section: no `Log`, no message, no crack point (line 152-153). That makes capacity-rejected moves indistinguishable from packet loss in the server's own telemetry.

**Rule workflow**
```
DestPlace == 1 && DestSlot >= MaxCarry ?
  --yes--> return (silent)
  --no---> BR-14
```

---

### Business Rule: BR-14 Source and destination resolvability

**Overview**
Both named cells must resolve through `GetItemPointer` to a legal item record; an invalid `sIndex` in either cell rejects the request.

**Detailed description**

`GetItemPointer` (`Code/Basedef.cpp:2340-2372`) returns `NULL` when the place is unknown, when the index is outside the container, or when the stored `sIndex` is negative or `>= MAX_ITEMLIST` (6500). It returns a valid pointer for an *empty* cell, because `sIndex == 0` satisfies both bounds. That distinction is what makes the handler able to use the same two pointers for "read the occupant" and "write the result". Lines 155-159 reject the request when either pointer is `NULL` and log `DEBUG:Trading Fails.Swap-Equip` (a copy-paste label also used by the equip arms of BR-09/BR-10).

The check runs *after* the slot-domain arms but the pointers were obtained *before* them (lines 76-80). That ordering is harmless because the pointer computation itself performs the container bounds test, and the later arms re-test the same bounds with their own constants; the redundancy means a defect in one layer would not be masked by the other.

Two identical pairs are resolved: `DestItem`/`SrcItem` and `dItem`/`sItem` (lines 76-80) are four `GetItemPointer` calls for two cells. The duplication is a decompilation artefact, not a semantic distinction; `dItem` aliases `DestItem` and `sItem` aliases `SrcItem` for the rest of the function. It doubles the (cheap) validation work and invites divergent edits.

**Rule workflow**
```
DestItem = GetItemPointer(DestPlace, DestSlot)
SrcItem  = GetItemPointer(SrcPlace,  SrcSlot)
DestItem == NULL || SrcItem == NULL ?
  --yes--> Log("...Swap-Equip") + return
  --no---> BR-15
```

---

### Business Rule: BR-15 Crown-in-warehouse swap suppression

**Overview**
When a crown (item 747) and a warehouse coordinate are combined, the swap body is skipped entirely.

**Detailed description**

Line 161 reads `if (DestItem->sIndex != 747 && SrcItem->sIndex != 747 || m->DestPlace != ITEM_PLACE_CARGO && m->SrcPlace != ITEM_PLACE_CARGO)`. By `&&` binding tighter than `||`, the swap executes unless *both* halves are false, i.e. unless (a crown is present) **and** (a cargo coordinate is present). So the suppressed case is exactly "crown involved and warehouse involved", and in that case the function does nothing: no message, no log, no revert, no echo.

The intent is legible from the codebase comment `// 747 = Crown on itemlist` (`Code/TMSrv/Server.cpp:646`): the crown is the guild-master artefact, and `Server.cpp:525-672`, `_MSG_Trade.cpp:147-158` and `_MSG_DropItem.cpp:110` all treat 747 (together with 3993/3994) as an item that must not be traded, dropped or lost. Warehouse deposit of a crown would move it out of the equip projection while the guild still references the holder, so the operation is refused.

That said, the refusal is the weakest possible form. Neither the requested move nor a resync is reported, and no crack point is recorded, so a client that repeatedly attempts the operation learns nothing and the server records nothing. Additionally, the guard does not cover the crown's variants 3993/3994 that the trade and drop handlers do exclude, so the restriction is not uniform across the item family.

**Rule workflow**
```
crown present (Src or Dest sIndex == 747) AND cargo coordinate present ?
  --yes--> do nothing (no message, no log, no echo, return implicitly)
  --no---> BR-16 ... BR-31 (the whole swap body)
```

---

### Business Rule: BR-16 Guild-item weekday restriction (dormant)

**Overview**
Guild artefacts moving into an equip slot are supposed to be restricted to a specific day for characters below level 1000; the rule is currently unreachable.

**Detailed description**

Lines 163-179 compute two flags, `isGuild` and `isGuildAgain`, each true when the respective item is one of 508, 522 or 526..537 and the move is *into* an equip slot from a non-equip slot (`!m->DestPlace && m->SrcPlace`). If either flag is set, `g_Week` is non-zero and `pMob[conn].MOB.BaseScore.Level < 1000`, the handler sends `_NN_Only_Sunday` (`Code/TMSrv/Language.h:286`) and returns. The item family matches the guild-artefact family used by `_MSG_Trade.cpp:160` and by the drop restriction at `_MSG_DropItem.cpp:110`.

`g_Week` is declared `extern` in `Code/TMSrv/Server.h:243`, defined as `int g_Week = 0;` at `Code/TMSrv/Server.cpp:287`, and never assigned anywhere in the repository (verified by exhaustive search of `Code/**/*.cpp`). A separate variable, `WeekMode`, does carry the weekly guild-event state (`Server.cpp:332, 6516-6606`), but it is not read here. The restriction is therefore dead: guild artefacts can be equipped by any level on any day.

Restoring the intended behaviour would require assigning `g_Week`, which is out of scope for this analysis. The finding of record is that the rule exists in source, is documented by a message constant, and has no runtime effect, so any gameplay documentation claiming a Sunday-only restriction for guild items is inconsistent with the deployed code.

**Rule workflow**
```
(isGuild || isGuildAgain) && g_Week && Level < 1000 ?
  --yes--> SendClientMessage(_NN_Only_Sunday) -> return      [unreachable: g_Week == 0]
  --no---> BR-17
```

---

### Business Rule: BR-17 Guild binding recomputation (no-op)

**Overview**
The handler attempts to recompute guild binding before and after the swap; every attempt is a no-op.

**Detailed description**

Lines 181-182 call `GetGuild(SrcItem)` and `GetGuild(DestItem)` before the swap, and lines 322-326 repeat the call for whichever side carries an `sIndex <= 40`. Line 359 calls the connection overload `GetGuild(conn)`. The first form resolves to `int GetGuild(STRUCT_ITEM*)` (`Code/TMSrv/GetFunc.cpp:1869-1874`), which returns `BASE_GetGuild(item)` and has no side effects; the return value is discarded at every call site. The second resolves to `void GetGuild(int conn)` (`GetFunc.cpp:1862-1867`), whose body takes the address of `pMob[conn].MOB.Equip[12]` and the current guild id into locals and then ends without using either.

The consequence is that moving a guild-bound item - for example swapping a guild artefact between the equip vector and the inventory - never updates `pMob[conn].MOB.Guild` or `MOB.GuildLevel`. Whatever holds the guild association must therefore be the item's own effect data, read on demand through `BASE_GetGuild`, plus the checks in `_MSG_Trade.cpp:140-177` and `_MSG_Action.cpp`. Whether that is sufficient is outside this component, but the *calls* here are decorative: they express an intent (recompute binding after the item moved) that the implementation does not fulfil.

The same dead calls appear at `ProcessDBMessage.cpp:777`, `Server.cpp:2761` and `Server.cpp:3138`, and `_MSG_UseItem.cpp:3160`, so the stub is systemic rather than local to this handler.

**Rule workflow**
```
pre-swap : GetGuild(SrcItem); GetGuild(DestItem);        [returns discarded]
post-swap: sIndex <= 40 ? GetGuild(...)                  [returns discarded]
           equip involved ? GetGuild(conn)               [void stub, no effect]
```

---

### Business Rule: BR-18 Equipment requirement enforcement

**Overview**
An item that will occupy an equipment slot must satisfy the full `BASE_CanEquip` rule set: slot compatibility, class, master class, mob type, level and the four stat requirements.

**Detailed description**

The check appears twice, once per direction. If the *source* coordinate is an equip slot, the item that will arrive there is the destination item, so `Can = BASE_CanEquip(dItem, &CurrentScore, m->SrcSlot, Class, MOB.Equip, &extra)` is evaluated (lines 191-192). If the *destination* coordinate is an equip slot, the arriving item is the source item, so `YouCan = BASE_CanEquip(sItem, &CurrentScore, m->DestSlot, ...)` is evaluated (lines 211-212). Both validations are guarded by a non-empty arriving item (`dItem->sIndex` at 187, `sItem->sIndex` at 207), which is correct: if the arriving item is empty, nothing is being equipped. The mapping of item-to-slot is the non-obvious part and it is right.

`BASE_CanEquip` (`Code/Basedef.cpp:4293-4411`) enforces, in order: `sIndex` in `(0, MAX_ITEMLIST)`; `Pos != 15`; the `EF_POS` bit for the target slot; the paired 6/7 exclusivity rules keyed on `nUnique == 46` and the special 64/128 `EF_POS` values; the `EF_CLASS` bit against the character class (with master-class characters deriving the class from `MortalFace/10`); `EF_MOBTYPE` against `MORTAL`/`ARCH`/`CELESTIAL`; the master-class restriction that only 747 or 3500..3507 may occupy slot 1; and finally `EF_LEVEL`, `EF_REQ_STR`, `EF_REQ_INT`, `EF_REQ_DEX`, `EF_REQ_CON` against `CurrentScore`, with a 130% / 150% surcharge for two-handed weapons in slot 7. A `FALSE` from any of these makes `Can` or `YouCan` zero, and the composite test at line 226 fails.

Two nuances matter. First, the validation uses `CurrentScore`, i.e. the score *including* equipment bonuses, so a swap that would remove the very item granting the stat can validate against a score the character will not have after the move; the recompute at line 328 happens after the commit, so a temporarily-invalid state can exist until the next validation pass. Second, if both sides are equip slots, the pair is validated against the pre-swap `Equip` vector, which is what `BASE_CanEquip` needs for the 6/7 exclusivity rule but means the mutually-exclusive pair is never evaluated post-swap.

A project-wide search shows that this handler is the **only** caller of `BASE_CanEquip` (two call sites, lines 192 and 212; definition at `Code/Basedef.cpp:4293`). No other code path - item use, character load, quest reward, shop purchase - re-runs the equipment requirement check. The invariant "a worn item satisfies its level/stat/class requirements" is therefore established exclusively by this function, which raises the cost of any defect in it and makes the two dead `if (!Can)` guards of R-09 more significant than they first appear.

**Rule workflow**
```
SrcPlace == 0 && dItem non-empty ?
  --> Can = BASE_CanEquip(dItem, CurrentScore, SrcSlot, Class, Equip, extra)
DestPlace == 0 && sItem non-empty ?
  --> YouCan = BASE_CanEquip(sItem, CurrentScore, DestSlot, Class, Equip, extra)
Can && YouCan ? --yes--> BR-20 commit path
               \--no---> BR-20 silent revert
```

---

### Business Rule: BR-19 Dual-slot (weapon/shield) validation semantics

**Overview**
The two-handed and off-hand compatibility rules are evaluated during the swap, using the equipment vector as it exists before the swap.

**Detailed description**

`BASE_CanEquip` treats slots 6 and 7 as a mutually constrained pair (`Code/Basedef.cpp:4311-4337`). When either the incoming item or the item in the opposite slot carries the special `EF_POS` value 64, one of them must be `nUnique == 46` and the other must have `EF_POS == 128`; any other combination is rejected. These are the two-handed-weapon versus shield/off-hand rules, expressed positionally rather than by item name.

Because the check is invoked from this handler with `pMob[conn].MOB.Equip` - the live, pre-swap vector - the pair being evaluated is (incoming item, item currently in the opposite slot), not (incoming item, item that will be in the opposite slot after the swap). For a single-coordinate move (equip to carry, carry to equip) that is the correct pair. For a simultaneous 6-and-7 exchange it is not: both items are validated against each other as they were before, and the post-swap combination is never re-validated. A request that swaps slot 6 and slot 7 in one packet therefore passes only if the pre-swap combination was already valid.

There is a second-order interaction with BR-28: after a successful swap the handler may relocate slot 7 to slot 6 if slot 6 is empty and slot 7's `EF_POS != 128`. That relocation is applied without re-running `BASE_CanEquip`, on the assumption that the item just validated for slot 7 is acceptable for slot 6. This assumption holds when the `EF_POS` bitmask permits both, and the `EF_POS` test in the consolidation branch is the only guard.

**Rule workflow**
```
equip-involved swap
  -> BASE_CanEquip(incoming, ..., other slot, ..., pre-swap Equip)
  -> 64/128 pair rules applied to pre-swap occupants
  -> accepted: commit (BR-20)
  -> then BR-28 may relocate 7 -> 6 without re-validation
```

---

### Business Rule: BR-20 Atomic all-or-nothing exchange with silent rejection

**Overview**
The exchange either happens completely or not at all; on failure the handler restores its snapshots and returns without informing the client.

**Detailed description**

The commit is two `memcpy` calls (lines 318-319) that write the destination snapshot into the source cell and the source snapshot into the destination cell, using the pointers resolved at lines 76-80. Everything that must be true before the exchange - domains, bounds, resolvability, crown exclusion, guild window, equipment requirements - has already been decided. This is a genuine all-or-nothing swap at the single-threaded server level: there is no yield point, no I/O and no exception path between the two `memcpy` calls, so no observer can see a half-swapped pair.

The failure path (lines 405-428) is a mirror of the snapshot code: it re-copies `save1` into `SrcItem` and `save2` into `DestItem`. At that point neither cell had been written, so the writes are a no-op restore. The important property is what is *absent*: no message panel, no `Log` line, no crack point, no `SendItem`, and no echo. The client that optimistically rendered the swap locally (which is how the drag-and-drop UI feels responsive) is left with a state the server does not share, and it learns of the divergence only when something else triggers a projection (`SendEquip`, `SendCarry`, a zone change or a relog).

The asymmetry is consistent with a "trusted client, silent server" posture: accepts are announced, rejects are not. It is nevertheless the difference between a recoverable UI glitch and a duplicated item on the client, because the client continues to believe it wears an item the server keeps in the inventory until the next authoritative projection. There is no sequence number, revision or negative acknowledgement in the protocol to shorten that window.

**Rule workflow**
```
snapshot both cells into save1/save2
... item-family side effects ...
Can && YouCan ?
  --yes--> memcpy(SrcItem, save2); memcpy(DestItem, save1)
           echo packet; projections; return
  --no---> memcpy(SrcItem, save1); memcpy(DestItem, save2)   [no-op]
           return silently
```

---

### Business Rule: BR-21 Stack-merge eligibility whitelist

**Overview**
Only five specific item families may merge on swap: 413, 412, 419, 420 and 2390..2419, and only while both stacks hold fewer than 120 units.

**Detailed description**

The eligibility test at lines 248-254 requires identical `sIndex` on both sides, membership in the family list, and `BASE_GetItemAmount() < 120` on both sides. `BASE_GetItemAmount` (`Code/Basedef.cpp:2287-2302`) returns the `cValue` of whichever effect slot carries `EF_AMOUNT` (61), or zero if none does. A zero amount is not treated as "not stackable" at this point; the arithmetic below normalises it to 1.

The whitelist is hard-coded and undocumented. What can be inferred: 413 is the item the combine recipes treat as a ten-unit material ("Pedra Misteriosa" in `GetMatchCombineEhre`, `Code/TMSrv/GetFunc.cpp:143`), and 2390..2419 is a 30-item range consistent with a gem/ore family. Items outside the list - including other obviously stackable consumables - are exchanged as whole cells, so two stacks of the same non-whitelisted item are swapped rather than merged. The whitelist therefore encodes a player-facing rule ("these categories stack in inventory, the others do not") that lives only in this comparison.

Because the test requires both amounts below 120, a swap involving an already-capped stack (120) falls through to the ordinary exchange, moving the full stack and leaving the other cell's stack untouched. That is the only path by which a 120-stack can move without arithmetic.

**Rule workflow**
```
save1.sIndex == save2.sIndex
&& sIndex in {413, 412, 419, 420} or in [2390, 2419]
&& amount(save1) < 120 && amount(save2) < 120 ?
  --yes--> BR-22 merge arithmetic
  --no---> plain exchange
```

---

### Business Rule: BR-22 Stack cap of 120 and split arithmetic

**Overview**
Merged stacks never exceed 120 units; surplus remains in the second cell, and an amount of zero or less is counted as one unit.

**Detailed description**

The total is computed as `tamount = (amount1 <= 0 ? 1 : amount1) + (amount2 <= 0 ? 1 : amount2)` (line 257), which normalises an effect-less stack to a single unit. If the total is at most 119, the merged value is written into `save1`'s amount effect and `save2` is cleared (lines 261-264). Otherwise `save1` is pinned to 120, the remainder is subtracted, and `save2` receives `min(remainder, 120)` or is cleared when nothing remains (lines 269-280). Since each input is < 120, the maximum total is 238 and the maximum remainder is 118, so the `tamount > 120 ? 120 : tamount` guard is defensive rather than reachable.

The two resulting cells are pushed to the client with explicit `SendItem` calls at the pre-swap coordinates (lines 266-267 and 282-283), and only afterwards does the echo at line 330 make the client exchange the two cells. The net client state therefore matches the server's `memcpy` result at lines 318-319. This ordering is essential and undocumented: reversing the two steps would leave the client holding the merged stack in the wrong cell.

The arithmetic is where the self-swap defect becomes an item-generation vector. If both coordinates name the same cell, `amount1 == amount2 == a`, so `tamount = 2a`; for `a = 100` the code writes 120 into the cell (a gain of 20 units over the 100 that existed), computes a remainder of 80 and then writes the cell twice (80 from `save2`, then 120 from `save1`), leaving 120. The duplicate 20 units exist only on the server; the client sees `SendItem(120)`, `SendItem(80)`, then an echo of a same-cell swap, which is a no-op visually and leaves the client showing 80. Subsequent resyncs make the 120 authoritative. No check anywhere in the function requires the two coordinates to differ.

**Rule workflow**
```
a1 = amount1 <= 0 ? 1 : amount1
a2 = amount2 <= 0 ? 1 : amount2
t  = a1 + a2
t <= 119 ? --yes--> save1.amount = t; clear(save2)
          \--no--> save1.amount = 120; r = t - 120
                  r >= 1 ? save2.amount = min(r, 120) : clear(save2)
SendItem(src, save1); SendItem(dest, save2)
```

---

### Business Rule: BR-23 Timed-item activation (+30 days)

**Overview**
Items in 4150..4188 that carry no effects at all receive a 30-day lifetime at the moment they first participate in a move.

**Detailed description**

Lines 287-296 test `sIndex in [4150, 4188]` together with all three effect slots being completely empty (`cEffect == 0` on each). When true, `BASE_SetItemDate(&item, 30)` writes a day/month/year triple into the three effect slots (`Code/Basedef.cpp:6732-6761`) and the item is pushed to the client at its pre-swap coordinate. The "all effects empty" precondition is what distinguishes a freshly-created item from one that has already been dated: the dating function uses exactly those three slots, so a second pass cannot re-date an already-stamped item.

The date computation is calendar-approximate, not exact: it adds 30 to the day-of-month, bumps the month when the day exceeds 29, bumps the year when the month exceeds 11, and then applies ad-hoc corrections for month length and February. The resulting expiry is therefore "roughly 30 days" rather than a precise instant, which is adequate for a rental/costume lifetime and consistent with the minute timer that purges equipped items later (`ProcessSecMinTimer.cpp:555`).

Placing this activation inside the move handler means an item's lifetime starts on first movement, not on acquisition. An item that is created directly into a carry slot and never moved has no date and is therefore never purged; one that is moved once acquires a deadline. That is a meaningful economic rule and it is expressed only by the position of this block inside this function.

**Rule workflow**
```
sIndex in [4150, 4188] && all effects zero ?
  --yes--> BASE_SetItemDate(item, 30)
           SendItem(conn, original place, original slot, item)
  --no---> BR-24
```

---

### Business Rule: BR-24 Expired-item purge during a move

**Overview**
Expired timed items (3980..3989 and 4150..4188) are destroyed as part of the move instead of being relocated.

**Detailed description**

Lines 297-307 evaluate `(3980..3989 && BASE_CheckItemDate()) || (4150..4188 && BASE_CheckItemDate())` for each side and, on a true result, call `BASE_ClearItem` and `SendItem` the now-empty cell. `BASE_CheckItemDate` (`Code/Basedef.cpp:6763-6780`) returns 1 when the current date is at or past the stored date, using three complementary comparisons (same month and past day, later month, later year) so that a month boundary does not require a day comparison. It is a destructive check: an item whose stored date equals today is already expired.

The two families correspond to the mount range (3980..3989, also treated as mounts by `BASE_VisualItemCode` at `Code/Basedef.cpp:6604`) and the timed-equipment range of BR-23. Purging them here rather than only in the minute timer (`ProcessSecMinTimer.cpp:546, 555`) closes a gap: an expired item sitting in the inventory would otherwise be movable, tradable and wearable indefinitely as long as it was never equipped.

Note the ordering with BR-23: a 4150..4188 item with empty effects is dated *first* (lines 287-296) and then expiry-tested (297-307) in the same pass. Because the freshly written date is 30 days ahead, the test cannot fire for that item, so the two rules compose safely. If the dating step were ever moved after the purge, a freshly created item would be destroyed on its first move.

**Rule workflow**
```
for each side:
  (sIndex in [3980, 3989] || sIndex in [4150, 4188]) && BASE_CheckItemDate(item) ?
    --yes--> BASE_ClearItem(item); SendItem(conn, place, slot, item)
    --no---> keep item
```

---

### Business Rule: BR-25 Audit trail for every accepted swap

**Overview**
Each accepted exchange writes one item-log record naming both coordinates and both item codes.

**Detailed description**

Lines 309-316 build two 2048-byte buffers, serialise each snapshot with `BASE_GetItemCode`, and format `tradingitem, SrcType:%d SrcSlot:%d Item:%s DstType:%d DstSlot:%d Item:%s` into the process-global `temp[4096]` (`Code/TMSrv/Server.h:382`), then hand it to `ItemLog` with the account name and IP (`Code/TMSrv/Server.cpp:9264-9284`), which prefixes a timestamp (and the dotted IP) and appends to the daily `item_log\TM_*.txt` file opened by `StartItemLog` (lines 9247-9262).

The record is written **before** the commit, using the snapshot values, and it includes the post-merge/post-purge content because the snapshots have already been mutated by BR-21..BR-24. It is therefore an accurate record of what the cells contained at the moment of exchange, which is the property an item-duplication investigation needs. What it does not record is the outcome: a rejected swap (BR-20 failure path) produces no record, so the log contains only successful transactions.

Structural note: the two 2048-byte buffers plus the global `temp` give this function a large stack and global footprint, and the `sprintf` into `temp` has no length bound. `BASE_GetItemCode` writes an encoded item string whose size depends on the item's effects; with two such strings inside a 4096-byte buffer the margin is adequate but not enforced.

**Rule workflow**
```
BASE_GetItemCode(save1, itemLog); BASE_GetItemCode(save2, itemLog2)
sprintf(temp, "tradingitem, SrcType:%d SrcSlot:%d ... DstType:%d DstSlot:%d ...")
ItemLog(temp, AccountName, IP)                     [timestamped file append]
```

---

### Business Rule: BR-26 Derived score recomputation

**Overview**
Every accepted exchange forces a full recomputation of the character's effective score.

**Detailed description**

`pMob[conn].GetCurrentScore(conn)` is called at line 328, after the commit. The method rebuilds `MOB.CurrentScore` from `BaseScore`, the equipment vector, the affect list and the extra/master-class data; it is the same entry point used after equipment changes elsewhere (`Server.cpp:8183, 8205, 8311, 8344`, `_MSG_UseItem.cpp` paths).

The rule matters because equipment bonuses can move `Hp`, `Mp`, `Damage`, `Ac`, the four primary stats, `Critical`, resistances and carry-related derived values. A swap that removes a stat-granting item must clamp current HP/MP down; one that adds such an item must raise the maxima. Without this call the character would keep stale maxima until some unrelated event triggered a recompute.

Because the recompute runs after the equipment validation (BR-18) but before any projection is sent (BR-29), the client receives a consistent view: score first, then equipment, then mount. The order also means `SendScore` at line 360 (called only when the local equip flag is set) transmits the already-recomputed values rather than forcing a second recompute.

**Rule workflow**
```
commit exchange (BR-20)
-> pMob[conn].GetCurrentScore(conn)   [recompute CurrentScore from BaseScore + Equip + effect]
-> echo packet (BR-27)
```

---

### Business Rule: BR-27 Client confirmation by packet echo

**Overview**
An accepted exchange is confirmed by returning the client's own request packet, unmodified in payload, to the client.

**Detailed description**

Line 330 calls `pUser[conn].cSock.AddMessage((char*)m, sizeof(MSG_TradingItem))` on the buffer that was received from the client. `AddMessage` (`Code/CPSock.cpp:513-...`) rewrites the header (`Size`, `KeyWord`, `CheckSum`, `ClientTick`) and applies the outbound key transform, but leaves the payload bytes (`DestPlace`, `DestSlot`, `SrcPlace`, `SrcSlot`, `WarpID`) exactly as the client sent them. The client therefore receives its own coordinates back and is expected to perform the corresponding local exchange. The protocol has no explicit "accepted" flag, no result code and no server-generated item payload for the common case: acceptance *is* the echo, absence of echo *is* rejection.

This design puts the whole burden of state convergence on ordering. Every `SendItem` that this handler emits (stack merge, dating, purge, cargo compaction) is written into the client's model *before* the echo arrives, so the client applies content updates to the pre-swap cells and then swaps them. Any reordering (for example by a future send-buffer optimisation) would corrupt the client's view, and there is no identifier in the payload that would let the client detect it.

The echo also reuses the receive buffer as a transmit buffer. `AddMessage` bounds-checks the send buffer (`nSendPosition + Size >= SEND_BUFFER_SIZE` returns `FALSE`) but the handler ignores the return value at both call sites (330 and 345), so a full send buffer silently drops the confirmation while the server state has already changed. `SendItem`, `SendEquip` and `SendScore` behave the same way for their own messages.

**Rule workflow**
```
commit -> pUser[conn].cSock.AddMessage(m, sizeof(MSG_TradingItem))
       -> header rewritten (Size=20, fresh KeyWord/CheckSum/ClientTick)
       -> payload echoed (DestPlace, DestSlot, SrcPlace, SrcSlot, WarpID)
       -> client applies the swap locally; no result code exists
```

---

### Business Rule: BR-28 Equipment slot 6 / slot 7 consolidation

**Overview**
After a successful exchange, if slot 6 is empty and slot 7 holds an item whose `EF_POS` is not 128, the item is moved from slot 7 to slot 6 and a synthetic swap packet is sent.

**Detailed description**

Lines 332-352 implement a repair step, not a player action. The conditions are: `MOB.Equip[6].sIndex == 0`, `MOB.Equip[7].sIndex != 0`, and `BASE_GetItemAbility(&Equip[7], EF_POS) != 128`. When all hold, the function rewrites the packet fields (`DestPlace = EQUIP`, `DestSlot = 6`, `SrcPlace = EQUIP`, `SrcSlot = 7`), queues a second `MSG_TradingItem` with those coordinates (line 345), copies `Equip[7]` into `Equip[6]`, and zeroes `Equip[7]`. The client, on receiving the synthetic packet, performs the same 7-to-6 move locally.

The purpose is to keep the paired slots canonical: slot 6 is the primary hand, slot 7 the off hand, and an off-hand item that is not the special 128-marked category belongs in slot 6 when slot 6 is free. The value 128 is the same `EF_POS` marker that `BASE_CanEquip` uses in its 64/128 exclusivity logic (`Code/Basedef.cpp:4321-4335`), so the consolidation deliberately leaves genuine off-hand-only items in slot 7.

Two properties are notable. First, the relocation is applied without re-running `BASE_CanEquip` for slot 6, relying on the item having just been validated for slot 7 (BR-19). Second, the synthetic packet reuses the same buffer as the original echo, so the client receives two `MSG_TradingItem` messages in sequence: the first applies the player's requested swap, the second applies the server-initiated repair. The ordering is guaranteed by the single send buffer.

**Rule workflow**
```
Equip[6].sIndex == 0 && Equip[7].sIndex != 0 ?
  --> EF_POS(Equip[7]) != 128 ?
        --yes--> m.DestPlace=EQUIP, m.DestSlot=6, m.SrcPlace=EQUIP, m.SrcSlot=7
                 AddMessage(m)                      [second echo]
                 Equip[6] = Equip[7]; clear(Equip[7])
        \--no--> leave slots as they are
```

---

### Business Rule: BR-29 Projection refresh policy

**Overview**
Equipment and score projections are emitted only when the exchange involved an equipment coordinate, and they are emitted for the actor only.

**Detailed description**

The control variables are the locals `DestPlace` and `SrcPlace` set to `1` in the equip arms (lines 114-115, 148-149) and left at `0` otherwise. Line 354 tests `DestPlace == ITEM_PLACE_CARRY` and calls `SendEquip(conn, conn)`; line 357 tests `SrcPlace == ITEM_PLACE_CARRY` and calls `GetGuild(conn)` followed by `SendScore(conn)`. Because `ITEM_PLACE_CARRY` is 1, the tests read as "the destination/source arm was an equipment arm", which is not what the constant name suggests. Any maintenance that "fixes" these comparisons to use `ITEM_PLACE_EQUIP` would invert the behaviour.

`SendEquip` (`Code/TMSrv/SendFunc.cpp:1084-1134`) rebuilds `MSG_UpdateEquip` from the whole equipment vector, computes visual codes and anct codes, handles the mount special-casing for index 14, and multicasts it on the player's grid position with `skip = conn` so the actor is included. `SendScore` (`SendFunc.cpp:1136-1193`) rebuilds `MSG_UpdateScore` from `CurrentScore`, critical, mana-save, guild, affects, resists and specials, then multicasts and emits an affect update.

The asymmetry (equip projection when the destination arm was an equip arm, score projection when the source arm was an equip arm) means a move whose *only* equip involvement is on the destination side sends equipment but not score, while the reverse sends both. Since BR-26 already recomputed the score before either branch, the missing `SendScore` leaves the client with stale stat numbers until another event, even though the server has the correct values. For a single-coordinate move exactly one of the two flags is set, so the two projections never both fire for the normal case.

**Rule workflow**
```
DestPlace(local) == 1 ? --yes--> SendEquip(conn, conn)
SrcPlace(local)  == 1 ? --yes--> GetGuild(conn) [no-op]; SendScore(conn)
```

---

### Business Rule: BR-30 Mount resynchronisation

**Overview**
If either post-swap item is in the 2330..2359 range, mount and summon state is reconciled for the character and their party.

**Detailed description**

Line 363 tests `DestItem->sIndex in [2330, 2360) || SrcItem->sIndex in [2330, 2360)` on the *post-swap* contents and calls `MountProcess(conn, Mount)`, where `Mount` is the pointer to `Equip[14]` captured at line 71. `MountProcess` (`Code/TMSrv/Server.cpp:4178-4221`) deletes party summons whose face is in 315..344 when they belong to this player, then derives `MBabyId = Equip[14].sIndex - 2320`, and if that lands in 10..40 with `EF_MOUNTHP > 0` calls `GenerateSummon`.

The captured pointer and the internal pointer are the same object, so the guard inside `MountProcess` (`memcmp(Mount, MItem) == 0` -> `IsEqual == 0` -> the early return is not taken) always allows execution. The guard was presumably meant to detect "the mount changed", but as wired it can never detect that, because both arguments alias live state. The call is therefore unconditional whenever a mount item is involved, which is the desired effect even though the guard is inert.

The range 2330..2359 overlaps but does not equal the ranges used elsewhere: `SendEquip` treats 2360..2389 as mounts with a level encoding (`SendFunc.cpp:1103-1127`) and `BASE_VisualItemCode` treats 3980..3994 specially. The consequence is that moving an item in 2360..2389 does not trigger this reconciliation, and moving an item in 2330..2359 triggers it whether or not the mount slot was touched.

**Rule workflow**
```
(post-swap DestItem or SrcItem) in [2330, 2360) ?
  --yes--> MountProcess(conn, &Equip[14])
             -> delete own summons with face 315..344
             -> Equip[14].sIndex - 2320 in [10, 40) && EF_MOUNTHP > 0 ? GenerateSummon
  --no---> BR-31
```

---

### Business Rule: BR-31 Warehouse staging compaction

**Overview**
After any exchange involving a cargo coordinate, the two staging cells 126 and 127 are drained into the first grid cell that can hold them.

**Detailed description**

Lines 366-403 take pointers to `pUser[conn].Cargo[MAX_CARGO-2]` and `[MAX_CARGO-1]` and, if either is occupied, iterate `i` from 0 to 125. For each candidate cell it computes the grid coordinates `(i % 9, i / 9)` and asks `BASE_CanCargo(item, Cargo, x, y)` whether the item's footprint fits. On success the item is copied into the cell, the staging cell is zeroed and `SendItem(conn, ITEM_PLACE_CARGO, i, ...)` announces the new location. The `Storage2` cell is preferred over `LastStorage`: the loop handles `Storage2` first and only falls through to `LastStorage` when `Storage2` is empty.

`BASE_CanCargo` (`Code/Basedef.cpp:4480-4537`) builds an occupancy bitmap of the warehouse by stamping every occupied cell's footprint (`EF_GRID` -> `g_pItemGrid[grid]`, 2x4 bits) into a flat 126-byte array, then tests the candidate footprint. It contains a stride defect at line 4531, where the destination test indexes `(yy + DestY) * CARGOGRIDY + xx + DestX` (stride 14) while the occupancy bitmap was built with stride `CARGOGRIDX` (9). The two indexings therefore disagree for every row after the first, so the compaction can place an item on top of another, or refuse a legal cell. The same function is used by the trade-space checks and by the DB loader, so the defect is not local to this handler.

The rule has no failure mode: if no cell fits, the staging item simply stays in 126/127 and is persisted there by `SaveUser` (BR-12). No message, log or crack point accompanies either outcome. Because the loop runs once per dropped `SendItem`, a physically full warehouse costs at most 126 `BASE_CanCargo` evaluations per swap.

**Rule workflow**
```
cargo coordinate involved && (Cargo[126] || Cargo[127]) ?
  --yes--> for i in 0..125:
             item = Cargo[126] ? Cargo[126] : (Cargo[127] ? Cargo[127] : null)
             item && BASE_CanCargo(item, Cargo, i%9, i/9) ?
               --yes--> Cargo[i] = item; clear(staging); SendItem(i)
  --no---> done
```

---

### 3.3 Implicit rules (absence-based)

These behaviours are defined by what the handler does *not* do. They are documented as constraints of the current implementation, with the confidence that follows from direct code reading.

| ID | Implicit rule | Evidence | Consequence |
|----|---------------|----------|-------------|
| IR-01 | The two coordinates need not differ | no equality test between `SrcPlace/SrcSlot` and `DestPlace/DestSlot` anywhere in lines 20-430 | aliasing cell -> merge arithmetic on itself (BR-22), see risk R-02 |
| IR-02 | `EF_NOTRADE` (127) is not consulted | the analogous check exists in `_MSG_Trade.cpp:179-189` but not here | items flagged non-tradable can be relocated freely; only trading is blocked |
| IR-03 | No rate limiting per connection | only two crack points exist in the whole function | log and CPU amplification possible at packet rate |
| IR-04 | Rejections are not acknowledged | BR-20 failure path, BR-15, BR-13 | client/server divergence without a repair trigger |
| IR-05 | Trade escrow is not consulted | no read of `Trade.Item`/`InvenPos` | any change during a trade is handled by tearing the trade down (BR-03), never by reconciliation |
| IR-06 | Persistence is not attempted | no `SaveUser` call in this file; the commit is in-memory only | a crash or disconnect before the next save rolls the swap back consistently (both cells revert together) |
| IR-07 | Neither participant of a trade is notified of a change beyond signal 900 | `RemoveTrade` sends `SendClientSignal(conn, conn, 900)` | no textual explanation reaches the player |

---

## 4. Component Structure

### 4.1 Component boundary

The component is a single translation unit containing exactly one function. There is no class, no namespace, no static helper, no header of its own and no state. Everything it needs is reached through `ProcessClientMessage.h`, which pulls in the whole server surface.

```
Code/TMSrv/
├── _MSG_TradingItem.cpp        # THE COMPONENT - 430 lines
│   ├── line 1-17              # GPL header
│   ├── line 18                 # #include "ProcessClientMessage.h"
│   └── line 20-430             # void Exec_MSG_TradingItem(int conn, char *pMsg)  (411 lines)
│       ├── 22                  # packet alias: MSG_TradingItem *m
│       ├── 24-29               # gate: liveness / play mode              [BR-01, BR-02]
│       ├── 31-36               # gate: open trade -> teardown            [BR-03]
│       ├── 38-42               # gate: auto-trade lock                   [BR-04]
│       ├── 44-70               # gate: warehouse privilege + NPC + range [BR-05..BR-07]
│       ├── 71                  # mount pointer snapshot
│       ├── 73-80               # pointer resolution (4 calls, 2 cells)   [BR-14]
│       ├── 83-150              # storage domain + slot bounds            [BR-08..BR-12]
│       ├── 152-153             # carry capacity                          [BR-13]
│       ├── 155-159             # resolvability                           [BR-14]
│       ├── 161                 # crown + cargo suppression               [BR-15]
│       ├── 163-179             # guild item weekday gate (dormant)       [BR-16]
│       ├── 181-182             # guild re-eval attempt (no-op)           [BR-17]
│       ├── 184-225             # equipment requirement checks            [BR-18, BR-19]
│       ├── 226-404             # accepted path
│       │   ├── 229-246         # snapshot save1/save2
│       │   ├── 248-285         # stack merge                             [BR-21, BR-22]
│       │   ├── 287-296         # timed item activation                   [BR-23]
│       │   ├── 297-307         # expiry purge                            [BR-24]
│       │   ├── 309-316         # item audit log                          [BR-25]
│       │   ├── 318-319         # COMMIT (memcpy swap)                    [BR-20]
│       │   ├── 322-326         # guild re-eval attempt (no-op)           [BR-17]
│       │   ├── 328             # score recompute                         [BR-26]
│       │   ├── 330             # client echo                             [BR-27]
│       │   ├── 332-352         # equip 7 -> 6 consolidation              [BR-28]
│       │   ├── 354-361         # projections                             [BR-29]
│       │   ├── 363-364         # mount resync                            [BR-30]
│       │   └── 366-403         # cargo staging compaction                [BR-31]
│       └── 405-428             # rejected path: silent revert            [BR-20]
│
├── ProcessClientMessage.h      # declaration (line 130) + 15 transitive includes
├── ProcessClientMessage.cpp    # dispatch: case _MSG_TradingItem (160-161)
├── TMSrv.vcxproj               # build inclusion (line 161)
└── TMSrv.vcxproj.filters       # filter placement (line 264)
```

### 4.2 Transitive include surface

`Code/TMSrv/ProcessClientMessage.h:21-43` includes `Windows.h`, `stdio.h`, `fcntl.h`, `time.h`, `math.h`, `io.h`, `errno.h`, and then `../Basedef.h`, `../CPSock.h`, `../ItemEffect.h`, `Language.h`, `CItem.h`, `Server.h`, `GetFunc.h`, `SendFunc.h`, `CMob.h`, `CUser.h`, `CNPCGene.h`, `CReadFiles.h`, `CCastleZakum.h`, `CWarTower.h`. The component therefore compiles against the full server object graph; any change to those headers rebuilds it.

### 4.3 File metrics

| Metric | Value | Note |
|---|---|---|
| Lines of file | 430 | including 17 lines of GPL header + 1 include |
| Functions | 1 | `Exec_MSG_TradingItem` |
| Lines in function | 411 | lines 20-430 |
| `if` statements | 59 | |
| `else` branches | 7 | 3 of them `else if`-style arms |
| Boolean operators | 42 `&&`, 25 `||` | |
| Estimated cyclomatic complexity | ~127 | 1 + 59 + 67 short-circuit operators |
| Maximum nesting depth | 8 | `place -> cargo -> merge -> amount -> send` chain |
| `memcpy` calls | 6 | 4 snapshot, 2 commit, plus 2 compaction copies |
| `SendItem` calls | 12 | merge, date, purge, compaction |
| `Log` calls | 9 | one per rejected structural class |
| Named constants used | 9 | `ITEM_PLACE_*`, `MAX_CARRY`, `MAX_CARGO`, `MAX_EQUIP`, `MAX_USER`, `MAX_MOB`, `VIEWGRID*` |
| Magic item/flag literals | ~31 | 747, 508, 522, 526..537, 413, 412, 419, 420, 2390..2419, 4150..4188, 3980..3989, 2330..2360, 0x2000, 128, 61, 120, 126, 119, 412, 30, 6, 7, 14, 40 |
| Stack locals | `STRUCT_ITEM save1`, `save2`, `char itemLog[2048]`, `char itemLog2[2048]` | ~4.1 KB of stack per call |

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains

```
ProcessClientMessage (Code/TMSrv/ProcessClientMessage.cpp:38)
  └─> Exec_MSG_TradingItem (Code/TMSrv/_MSG_TradingItem.cpp:20)
        ├─> SendHpMode                (Code/TMSrv/SendFunc.cpp:1622)
        ├─> AddCrackError             (Code/TMSrv/Server.cpp:684)
        │     ├─> SendClientMessage   (Code/TMSrv/SendFunc.cpp:27)
        │     ├─> CharLogOut          (Code/TMSrv/Server.cpp:7085)
        │     └─> Log                 (Code/TMSrv/Server.cpp:6701)
        ├─> RemoveTrade               (Code/TMSrv/Server.cpp:7319)
        │     ├─> SendClientSignal    (Code/TMSrv/SendFunc.cpp:197)
        │     └─> GetCreateMob / GridMulticast   (Code/TMSrv/GetFunc.cpp:947, SendFunc.cpp)
        ├─> SendClientMessage         (Code/TMSrv/SendFunc.cpp:27)
        ├─> SendClientSignal          (Code/TMSrv/SendFunc.cpp:197)   [ESCENE_FIELD / 412]
        ├─> GetItemPointer            (Code/Basedef.cpp:2340)
        │     └─> (bounds + sIndex validation only)
        ├─> Log                       (Code/TMSrv/Server.cpp:6701)
        ├─> BASE_CanEquip             (Code/Basedef.cpp:4293)
        │     ├─> BASE_GetItemAbility (Code/Basedef.cpp)
        │     └─> g_pItemList[]       (Code/Basedef.h:882)
        ├─> BASE_GetItemAmount        (Code/Basedef.cpp:2287)
        ├─> BASE_SetItemAmount        (Code/Basedef.cpp:2304)
        ├─> BASE_ClearItem            (Code/Basedef.cpp:4281)
        ├─> BASE_SetItemDate          (Code/Basedef.cpp:6732)
        ├─> BASE_CheckItemDate        (Code/Basedef.cpp:6763)
        ├─> BASE_GetItemCode          (Code/Basedef.cpp:1479)
        ├─> ItemLog                   (Code/TMSrv/Server.cpp:9264)
        │     └─> fItemLogFile        (opened by StartItemLog, Server.cpp:9247)
        ├─> GetGuild(STRUCT_ITEM*)    (Code/TMSrv/GetFunc.cpp:1869)   [result discarded]
        │     └─> BASE_GetGuild       (Code/Basedef.cpp)
        ├─> GetGuild(int)             (Code/TMSrv/GetFunc.cpp:1862)   [empty body]
        ├─> CMob::GetCurrentScore     (Code/TMSrv/CMob.cpp:598)
        ├─> CPSock::AddMessage        (Code/CPSock.cpp:513)           [echo x2]
        ├─> MountProcess              (Code/TMSrv/Server.cpp:4178)
        │     ├─> DeleteMob           (Code/TMSrv/Server.cpp)
        │     └─> GenerateSummon      (Code/TMSrv/Server.cpp)
        ├─> BASE_CanCargo             (Code/Basedef.cpp:4480)
        ├─> SendItem                  (Code/TMSrv/SendFunc.cpp:1055)
        ├─> SendEquip                 (Code/TMSrv/SendFunc.cpp:1084)
        └─> SendScore                 (Code/TMSrv/SendFunc.cpp:1136)
```

### 5.2 Internal dependencies (first-party, direct calls)

| Dependency | File:line | Kind | Purpose in this component |
|---|---|---|---|
| `SendHpMode` | `Code/TMSrv/SendFunc.cpp:1622` | project, `SendFunc` | resync HP/mode after liveness rejection |
| `AddCrackError` | `Code/TMSrv/Server.cpp:684` | project, `Server` | crack-point accounting |
| `RemoveTrade` | `Code/TMSrv/Server.cpp:7319` | project, `Server` | trade/auto-trade teardown |
| `SendClientMessage` | `Code/TMSrv/SendFunc.cpp:27` | project, `SendFunc` | player-facing refusal text |
| `SendClientSignal` | `Code/TMSrv/SendFunc.cpp:197` | project, `SendFunc` | warehouse rejection signal 412 |
| `GetItemPointer` | `Code/Basedef.cpp:2340` | project, `Basedef` | cell resolution + bounds |
| `Log` | `Code/TMSrv/Server.cpp:6701` | project, `Server` | text log + window title |
| `BASE_CanEquip` | `Code/Basedef.cpp:4293` | project, `Basedef` | equipment requirement engine |
| `BASE_GetItemAmount` | `Code/Basedef.cpp:2287` | project, `Basedef` | stack amount read |
| `BASE_SetItemAmount` | `Code/Basedef.cpp:2304` | project, `Basedef` | stack remainder write |
| `BASE_ClearItem` | `Code/Basedef.cpp:4281` | project, `Basedef` | cell zeroing (purge) |
| `BASE_SetItemDate` | `Code/Basedef.cpp:6732` | project, `Basedef` | 30-day activation |
| `BASE_CheckItemDate` | `Code/Basedef.cpp:6763` | project, `Basedef` | expiry test |
| `BASE_GetItemCode` | `Code/Basedef.cpp:1479` | project, `Basedef` | audit string encoding |
| `ItemLog` | `Code/TMSrv/Server.cpp:9264` | project, `Server` | item audit file |
| `GetGuild(STRUCT_ITEM*)` | `Code/TMSrv/GetFunc.cpp:1869` | project, `GetFunc` | item guild lookup (discarded) |
| `GetGuild(int)` | `Code/TMSrv/GetFunc.cpp:1862` | project, `GetFunc` | empty stub |
| `CMob::GetCurrentScore` | `Code/TMSrv/CMob.cpp:598` | project, entity | derived stat recompute |
| `CPSock::AddMessage` | `Code/CPSock.cpp:513` | project, transport | echo + send buffering |
| `MountProcess` | `Code/TMSrv/Server.cpp:4178` | project, `Server` | mount/summon reconciliation |
| `BASE_CanCargo` | `Code/Basedef.cpp:4480` | project, `Basedef` | warehouse footprint fit |
| `SendItem` | `Code/TMSrv/SendFunc.cpp:1055` | project, `SendFunc` | single-cell item projection |
| `SendEquip` | `Code/TMSrv/SendFunc.cpp:1084` | project, `SendFunc` | equipment projection |
| `SendScore` | `Code/TMSrv/SendFunc.cpp:1136` | project, `SendFunc` | score projection |

### 5.3 External / platform dependencies

| Dependency | Version / form | Purpose | Coupling |
|---|---|---|---|
| `Windows.h` / Win32 Sockets | MSVC 2015 (v140), `send`/`recv` | transport | indirect, through `CPSock` |
| C runtime (`sprintf`, `memcpy`, `memset`, `strncpy`) | MSVC CRT | formatting and copies | direct |
| `time.h` (`time`, `localtime`) | CRT | item dating and expiry | indirect, through `Basedef` |
| `g_pItemList[]`, `g_pItemGrid[]`, `pItemGrid`, `pHeightGrid` | global arrays | item definitions and footprints | indirect, through `Basedef` |
| File system (`item_log\TM_*.txt`, `Log` file) | local disk | audit and text logs | indirect, through `Server` |
| No third-party library | - | - | The component links nothing beyond the CRT and Win32 |

### 5.4 Dependency direction and stability

The component depends on four lower layers (`CRT` -> `Basedef` primitives -> `SendFunc`/`GetFunc`/`Server` services -> `CPSock` transport) and on the entity layer (`CMob`, `CUser`). It is depended on by exactly one caller. All dependencies point downward except two entity reads (`pUser[conn]`, `pMob[conn]`) which are global-singleton accesses rather than injected dependencies; there is no seam for substitution, which is why the component cannot be tested without the whole server object graph.

---

## 6. Afferent and Efferent Coupling

Measured at function granularity (the only granularity available: one free function, no types are declared in this component).

| Component | Afferent (Ce-in) | Efferent (Ce-out) | Instability (Ce/(Ca+Ce)) | Criticality |
|---|---|---|---|---|
| `Exec_MSG_TradingItem` | 1 | 23 first-party + 4 CRT = 27 | 0.96 | High (mutates persistent item state) |
| `GetItemPointer` | 6 callers project-wide | 0 | 0.00 | High (shared primitive) |
| `BASE_CanEquip` | 1 caller project-wide | 2 | 0.33 | High (sole equipment validator on this path) |
| `BASE_CanCargo` | 3 callers project-wide | 2 | 0.40 | Medium (defect propagates to 3 call sites) |
| `BASE_SetItemDate` | 3 callers project-wide | 2 | 0.40 | Medium |
| `BASE_GetItemAmount` | 7 callers project-wide | 0 | 0.00 | Medium |
| `MountProcess` | 6 callers project-wide | 5 | 0.45 | Medium |
| `RemoveTrade` | many callers (all trade handlers) | 4 | 0.80 | High (shared trade-state owner) |
| `GetGuild` overloads | 5 + several call sites | 1 / 0 | 0.20 | Low (inert) |

Notes:

- The afferent value of 1 is exact and matches the architecture report: `ProcessClientMessage.cpp:161` is the sole caller.
- The architecture report's efferent value of 14 is a corpus estimate. This analysis enumerates 23 distinct first-party symbols (`SendHpMode`, `AddCrackError`, `RemoveTrade`, `SendClientMessage`, `SendClientSignal`, `GetItemPointer`, `Log`, `BASE_CanEquip`, `SendItem`, `BASE_GetItemAmount`, `BASE_ClearItem`, `BASE_SetItemAmount`, `BASE_SetItemDate`, `BASE_CheckItemDate`, `BASE_GetItemCode`, `ItemLog`, `GetGuild` x2 overloads, `CMob::GetCurrentScore`, `CPSock::AddMessage`, `MountProcess`, `BASE_CanCargo`, `SendEquip`, `SendScore`) plus 4 CRT primitives. Counting `GetGuild` once gives 23; the architecture report's 14 corresponds to the number of distinct *calls* in a flattened form and undercounts overloads and repeated emitters.
- The composed fan-in of the transitive closure is far larger: through `SendItem`, `SendEquip` and `SendScore` the component reaches `GridMulticast`, the visual-code encoders and the affect encoder, each of which is shared with most of the server.

### 6.1 Cohesion assessment

Functional cohesion is low-to-moderate. The function performs at least six distinct responsibilities that are only loosely related by a common data flow: (1) access control (liveness, trade state, auto-trade, warehouse privileges), (2) structural request validation, (3) equipment-rule enforcement, (4) item-family side effects (merge, dating, expiry), (5) client projection and confirmation, and (6) a warehouse repair algorithm. The only thing binding them is the shared pair of cells and the single client connection. This is the classic "transaction script" shape produced by decompilation of a single server-side packet handler, and it explains the ~127 cyclomatic complexity.

### 6.2 Architectural boundary assessment

The file crosses three boundaries it does not own:
- It reads and writes `pUser[conn].Cargo`, whose authoritative lifecycle belongs to the account/DB layer (`SaveUser`, `MSG_DBSaveMob`).
- It reads and writes `pMob[conn].MOB.Equip/Carry`, whose invariants are also enforced by `_MSG_Trade`, `_MSG_UseItem`, `_MSG_CombineItem*`, `_MSG_DropItem`, `_MSG_GetItem` and the timers.
- It tears down `Trade` state owned by `_MSG_Trade`/`_MSG_QuitTrade`.

All three crossings are done through raw globals; there is no accessor, no invariant check and no lock. The design is safe only because the server is single-threaded with respect to these globals.

---

## 7. Endpoints

The component exposes **no HTTP, REST, GraphQL, gRPC or named-pipe endpoint**. Its endpoint is a binary game-protocol message handled over the server's Win32 TCP socket layer. It is documented here in protocol terms because it is the component's entire external contract.

| Message | Type code | Direction | Nominal size | Transport | Handler |
|---|---|---|---|---|---|
| `_MSG_TradingItem` | `118` (`0x76`) `\| 0x0200 \| 0x0100` | client to game (request); game to client (echo, same type) | `sizeof(MSG_TradingItem)` = 20 bytes (natural alignment; no `#pragma pack` in scope at `Code/Basedef.h:1729`) | TCP, `CPSock` framed (12-byte header, per-frame key transform, byte checksum) | `Exec_MSG_TradingItem` |

Request/echo payload (`Code/Basedef.h:1729-1739`):

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `Size` | `short` | frame length, set by `AddMessage` on the echo |
| 2 | `KeyWord` | `char` | frame key index |
| 3 | `CheckSum` | `char` | frame checksum |
| 4 | `Type` | `short` | `118` |
| 6 | `ID` | `short` | connection id |
| 8 | `ClientTick` | `unsigned int` | anti-cheat tick; `SKIPCHECKTICK` is rejected by the dispatcher |
| 12 | `DestPlace` | `unsigned char` | 0 = Equip, 1 = Carry, 2 = Cargo |
| 13 | `DestSlot` | `unsigned char` | slot index inside the destination domain |
| 14 | `SrcPlace` | `unsigned char` | 0 = Equip, 1 = Carry, 2 = Cargo |
| 15 | `SrcSlot` | `unsigned char` | slot index inside the source domain |
| 16 | `WarpID` | `int` | warehouse NPC index, only meaningful for cargo coordinates |

Messages emitted by the handler (all through `CPSock::AddMessage`, all best-effort):

| Message | Type | Direction | Trigger | Payload |
|---|---|---|---|---|
| `_MSG_SetHpMode` | 146 | srv to cli | liveness gate | current HP + mode |
| `_MSG_MessagePanel` | 1 | srv to cli | auto-trade lock, guild window | text from `g_pMessageStringTable` |
| `MSG_STANDARD` pseudo-signal | 30000 / 412 | srv to cli | warehouse rejection | `Type = 412`, `ID = ESCENE_FIELD` |
| `MSG_STANDARD` pseudo-signal | 30000 / 900 | srv to cli | `RemoveTrade` | close trade window |
| `_MSG_SendItem` | 130 | srv to cli | merge, dating, purge, compaction | `invType`, `Slot`, `STRUCT_ITEM` |
| `_MSG_TradingItem` | 118 | srv to cli | accepted swap; slot 6/7 consolidation | echo of the request / synthetic coordinates |
| `_MSG_UpdateEquip` | 107 | srv to cli (grid multicast) | equip involvement | 16 visual codes + anct codes |
| `_MSG_UpdateScore` | 54 | srv to cli (grid multicast) | equip involvement | full score, guild, affects, resists |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling | Evidence |
|---|---|---|---|---|---|---|
| `CPSock` frame layer | Internal transport | receives the request, queues the echo and all projections | TCP, 12-byte header + per-byte key transform + checksum | binary, little-endian, `MAX_MESSAGE_SIZE` 8192 | `AddMessage` returns `FALSE` on buffer-full or invalid socket; **return value ignored** at lines 330 and 345; checksum failure aborts the socket loop (`Server.cpp:3990-3999`) | `Code/CPSock.cpp:386-466, 513-541`, `Code/CPSock.h:42-50` |
| Client game binary (W2 v7662) | External client | performs the optimistic UI move and the local swap on echo | same binary protocol | `MSG_TradingItem`, `MSG_SendItem` | none; a missing echo leaves the client divergent | `Code/Basedef.h:1728`, `Code/CPSock.cpp:535-541` |
| Account warehouse store | Internal persistence | `pUser[conn].Cargo[128]` is the live warehouse, persisted on save | in-process array + DB message | `STRUCT_ITEM[128]` inside `MSG_DBSaveMob` | no save in this handler; persistence is deferred to `SaveUser` | `Code/TMSrv/CUser.h:50`, `Code/TMSrv/Server.cpp:7056-7083`, `Code/Basedef.h:983-997` |
| Character item state | Internal entity | `pMob[conn].MOB.Equip[16]`, `Carry[64]`, `CurrentScore` | in-process globals | `STRUCT_ITEM`, `STRUCT_SCORE` | invariants enforced per handler | `Code/Basedef.h:455-461`, `Code/TMSrv/CMob.h:122` |
| Trade subsystem | Internal state machine | aborted by BR-03 | in-process | `MSG_Trade` | `RemoveTrade` bound-checks the connection id | `Code/TMSrv/Server.cpp:7319-7352`, `Code/TMSrv/_MSG_Trade.cpp` |
| Auto-trade (private shop) subsystem | Internal state machine | locked by BR-04; torn down by `RemoveTrade` | in-process | `MSG_SendAutoTrade` | none needed | `Code/TMSrv/_MSG_SendAutoTrade.cpp:105`, `Server.cpp:7329-7351` |
| Warehouse NPC graph | Internal world state | identity + proximity validation | in-process | `CMob` fields (`Mode`, `MOB.Merchant`, `TargetX/Y`) | signal 412 on rejection; **no range guard before indexing** | lines 46-69, `Code/TMSrv/CMob.h:30, 122` |
| Item definition tables | Internal data | `EF_POS`, `EF_CLASS`, `EF_LEVEL`, `EF_REQ_*`, `EF_GRID`, `EF_MOUNTHP` lookups | in-process | `g_pItemList[MAX_ITEMLIST]`, `g_pItemGrid[]` | `BASE_GetItemAbility` returns 0 for unknown indexes | `Code/Basedef.cpp:4293-4411, 4480-4537` |
| Item audit file | External file | one record per accepted swap | `fprintf` on `item_log\TM_<date>.txt` | text line with timestamp, account, IP, both item codes | file handle null-checked; `sprintf` unbounded | `Code/TMSrv/Server.cpp:9247-9284` |
| Text log file + window title | External file / UI | rejection diagnostics | `fprintf` + `SetWindowText` | text | none | `Code/TMSrv/Server.cpp:6701-6727` |
| Mount / summon subsystem | Internal | resync after mount-range items move | in-process | `CMob` summon list | `GenerateSummon` internal checks | `Code/TMSrv/Server.cpp:4178-4221` |
| Party system | Internal | summon deletion by face range | in-process | `PartyList[MAX_PARTY]` | bounds checks on each party entry | `Code/TMSrv/Server.cpp:4195-4209` |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose | Assessment |
|---|---|---|---|---|
| Command Handler / packet dispatch | one free function per message type, selected by a `switch` | `Code/TMSrv/ProcessClientMessage.cpp:66-...`, `:160-161` | decouples wire type from logic | consistent across ~48 handlers; no registry, so the mapping is compile-time only |
| Guard-clause ladder (fail fast) | 59 `if` guards, each with an early `return` | lines 24-161 | rejects invalid requests before any mutation | effective for safety, but the guards are not ordered by cost or risk (the dangerous `pMob[WarpID]` deref precedes the cheap proximity test) |
| Server-authoritative transaction + client echo | commit then echo the request packet; acceptance is the echo | lines 318-330 | keeps client and server convergent without a result code | fragile: correctness depends on send ordering; a full send buffer silently drops the confirmation |
| Snapshot / restore (Memento-like) | `save1`/`save2` copies, `memcpy` back on failure | lines 229-246, 405-428 | all-or-nothing exchange | the restore is a no-op (nothing was written) and is performed with no notification |
| Two-phase style staging | warehouse cells 126/127 used as staging, drained after the operation | lines 366-403 | avoids in-place reshuffling of the grid | staging cells are client-addressable and persisted (risk R-07) |
| Projection / view refresh | `SendItem`, `SendEquip`, `SendScore` after a state change | lines 266-330, 354-361 | propagates authoritative state | projections are conditional on a confusing local flag (BR-29) |
| Derived-state recompute | `CMob::GetCurrentScore` after equipment change | line 328 | rebuilds effective stats | correct placement, but the score projection is not always emitted afterwards |
| Audit trail / event log | `ItemLog` with a structured `tradingitem, ...` record | lines 309-316 | forensic reconstruction of item flow | the strongest reliability feature in the component |
| Sentinel/semantic overloading | `DestPlace`/`SrcPlace` locals reuse `ITEM_PLACE_CARRY` as a boolean | lines 114-115, 148-149, 354-361 | cheapest encoding of "equip was involved" | primary readability defect; invites an inverted fix |
| Copy-paste duplication | identical destination/source blocks; duplicate pointer resolution; duplicate log strings | lines 83-116 vs 117-150, 76-80 | decompilation artefact | divergence risk between the two halves |
| Hard-coded item-family dispatch | inline comparisons against ~31 numeric item ids and flags | throughout | game-specific policy | no configuration surface; every tuning requires a rebuild |

### 9.1 Architectural decisions observed

1. **Movement invalidates trade rather than participating in it** (BR-03). The trade subsystem owns its escrow; this handler refuses to be a second writer. This is the cleanest architectural decision in the file.
2. **Persistence is deferred.** No `SaveUser` in the handler; durability is a property of the periodic/exit save path. The swap is therefore atomic in memory and atomic in persistence (both cells travel together in the next save), which is the right trade-off.
3. **The client is trusted for intent, not for state.** The client proposes coordinates; the server decides. No client-supplied item contents are ever accepted (the packet has no item payload at all), which is why the handler is immune to the classic "send me the item I want" injection.
4. **Policy is compiled in.** Item families, stack cap, timed ranges, guild windows, privilege bits and the 128 marker are all literals. There is no configuration, no item-list-driven rule, and no feature flag.
5. **New obligations were appended, not integrated.** The warehouse compaction, mount resync and slot 6/7 consolidation read as later additions appended to the end of the accepted path, after the projections; they run in a fixed order that is not documented and that matters for causality.

---

## 10. Technical Debt & Risks

| ID | Risk | Level | Area | Issue | Impact | Evidence |
|---|---|---|---|---|---|---|
| R-01 | Out-of-bounds read | High | Warehouse validation | `if (WarpID <= MAX_USER && WarpID >= MAX_MOB)` is unsatisfiable, so a negative or oversized client `WarpID` reaches `pMob[WarpID]` | undefined behaviour in a 25,000-element array of large objects; realistic crash of the world server, potential memory disclosure | lines 46-58, `Code/TMSrv/CMob.h:122` |
| R-02 | Item duplication | High | Stack merge | no check that source and destination differ; identical coordinates feed the merge arithmetic with `amount1 == amount2` and can inflate a stack (100 -> 120) | economic exploit on the whitelisted families (413, 412, 419, 420, 2390..2419); server-side item creation | lines 76-80, 248-285 |
| R-03 | Client/server divergence | High | Rejected path | equipment-check failure reverts silently: no message, no log, no crack point, no echo | the client can display an equipped item the server keeps in the inventory until the next projection or relog | lines 405-428 |
| R-04 | Dead policy | Medium | Guild item window | `g_Week` is defined as `0` and never assigned | the promised Sunday/level restriction for guild artefacts never applies | line 175, `Code/TMSrv/Server.cpp:287` |
| R-05 | Dead policy | Medium | Guild binding | `GetGuild(int)` is an empty stub; `GetGuild(STRUCT_ITEM*)` results are discarded | guild binding is never recomputed after a move; three call sites express an intent the code does not implement | lines 181-182, 322-326, 359, `Code/TMSrv/GetFunc.cpp:1862-1874` |
| R-06 | Shared-primitive defect | Medium | Warehouse fit test | `BASE_CanCargo` uses `CARGOGRIDY` as the row stride when testing the destination footprint | compaction can overwrite an occupied cell or refuse a legal one; the same helper is used by trade-space checks and the DB loader | `Code/Basedef.cpp:4531` |
| R-07 | Hidden capacity | Medium | Cargo staging cells | cells 126/127 are client-addressable, outside the renderable 9x14 grid, and persisted by `SaveUser` | up to two items can be invisible to the player and to grid-based audits; they survive logout | lines 366-403, `Code/TMSrv/Server.cpp:7065` |
| R-08 | Disabled validation | Medium | Packet shape | `BASE_CheckPacket` body is commented out and always returns `FALSE`, so the `_MSG_TradingItem` size check at `Basedef.cpp:6537` never runs | the handler relies on the transport's 4..8192 bound and on `sizeof`-based reads of a 20-byte struct; any future field addition becomes a read-past-end risk | `Code/Basedef.cpp:6475-6596` |
| R-09 | Dead validation | Medium | Anti-dupe intent | `int error = -2` is never assigned, and the enclosing blocks are only reachable when `Can == 1`, so `if (!Can)` never fires | lines 194-204 and 214-224 read as live duplication guards but cannot execute; a reviewer could wrongly believe the path is covered | lines 187-225 |
| R-10 | Silent no-op | Medium | Crown suppression | the crown + cargo case returns with no message, log or crack point | unattended client retry loops; no forensic trace of a deliberate policy refusal | line 161 |
| R-11 | Log/notification asymmetry | Low | Diagnostics | destination-arm rejections log `err,...` and source-arm rejections log `DEBUG:...`; two early exits log nothing (`DestSlot >= MaxCarry`, crown case) | log-mining rules must match both prefixes and will still miss two classes | lines 89-147, 152-153, 161 |
| R-12 | Ignored return values | Low | Transport | `AddMessage` returns `FALSE` on a full send buffer; both call sites ignore it | a full buffer loses the confirmation after the state change, producing divergence with no server-side signal | lines 330, 345, `Code/CPSock.cpp:518-533` |
| R-13 | Unbounded formatting | Low | Audit log | `sprintf` into the global `temp[4096]` with two 2048-byte item-code buffers | a pathological item encoding could overflow; the margin is unenforced | lines 309-316, `Code/TMSrv/Server.h:382` |
| R-14 | No rate limiting | Low | Abuse resistance | one crack point per rejected liveness check, none elsewhere; every accepted swap writes a disk record | packet-rate floods can amplify CPU work (126 `BASE_CanCargo` calls) and disk writes | lines 24-42, 309-316, 366-403 |
| R-15 | Semantic overloading | Low | Readability | locals `DestPlace`/`SrcPlace` are booleans but compared against `ITEM_PLACE_CARRY` | any "cleanup" of lines 354-361 inverts the projection policy | lines 73-74, 114-115, 148-149, 354-361 |
| R-16 | Redundant resolution | Low | Performance/clarity | four `GetItemPointer` calls for two cells (`DestItem`/`dItem`, `SrcItem`/`sItem`) | double work and divergent-edit risk | lines 76-80 |
| R-17 | Magic literals | Low | Maintainability | ~31 hard-coded item ids, ranges and flags with no named constants | item-policy changes require code edits in several files with no compiler help | lines 161-403 |
| R-18 | Complexity | Low | Maintainability | ~127 cyclomatic complexity, depth 8, 411-line function | high regression risk for any modification; no tests to catch it (section 11) | whole file |
| R-19 | Inert internal guard | Low | Mount path | `MountProcess(conn, Mount)` passes the same object it compares against, so its `memcmp` guard can never trigger | the "mount changed" optimisation is dead; reconciliation runs on every mount-range move | lines 71, 363-364, `Code/TMSrv/Server.cpp:4180-4188` |
| R-20 | Asymmetric capacity rule | Low | Inventory | capacity is enforced on the destination only | items legally exist above `MaxCarry`, so every `Carry` consumer needs its own bound and the codebase uses at least three different ones | line 152, `Server.cpp:719-725`, `_MSG_Trade.cpp:60` |

---

## 11. Test Coverage Analysis

### 11.1 Evidence of absence

The repository contains **no automated tests of any kind**, and specifically none for this component. Evidence, each item verified by exhaustive search over the workspace excluding `.agents`, `.codegraph`, `.opencode` and `openspec`:

| Check | Command / method | Result |
|---|---|---|
| Test projects in the solution | `W2PP Code Project.sln` project entries | 2 projects only: `DBSrv`, `TMSrv`. No test project. |
| Test build files | search for `*.vcxproj`, `CMakeLists.txt` under `Code/` | 2 `.vcxproj` files, no `CMakeLists.txt` |
| Test source files by name | recursive filename match for `test`, `spec`, `mock`, `fixture` | 0 matches |
| Test frameworks | content search for `gtest`, `catch2`, `CppUnit`, `BOOST_TEST`, `assert(` across `Code/**/*.cpp` and `Code/**/*.h` (104 files) | 0 matches |
| Test directories | recursive directory match for `test`/`spec` outside ignored folders | 0 matches (the only match was `openspec`, which is excluded by parameter) |
| CI configuration | repository root listing | no `.github`, no `azure-pipelines.yml`, no `.gitlab-ci.yml` |
| Documentation of testing | `README.md` | build/feature inventory only; the "Things to do" list still has "Fix all bugs", "Create an documentation", "Re-create the project" unchecked |
| Fixtures or golden data | search for `fixture`, `golden`, `expected` | 0 matches |

### 11.2 Coverage table

| Component / rule group | Unit tests | Integration tests | Coverage | Test quality |
|---|---|---|---|---|
| `Exec_MSG_TradingItem` (whole) | 0 | 0 | 0% | none |
| BR-01 / BR-02 liveness and crack accounting | 0 | 0 | 0% | no test exists for the HP==0 boundary, the negative-HP case, or the 2e9 ceiling |
| BR-03 / BR-04 trade and auto-trade gates | 0 | 0 | 0% | no test for the both-sides teardown or for the stale `OpponentID` case |
| BR-05..BR-07 warehouse privilege, NPC identity, proximity | 0 | 0 | 0% | no test for the negative `WarpID` case (R-01), no boundary test at exactly 33 cells |
| BR-08..BR-14 slot domains and resolvability | 0 | 0 | 0% | no test for slot 0 as source, slot 15, slots 60/61, 126/127, or the `MaxCarry` asymmetry |
| BR-15..BR-17 crown, guild window, guild binding | 0 | 0 | 0% | no test that would reveal that BR-16 and BR-17 have no effect |
| BR-18 / BR-19 equipment requirements | 0 | 0 | 0% | `BASE_CanEquip` has one caller project-wide and no test; the 6/7 pair and the 130%/150% surcharge are unexercised |
| BR-20 atomic swap and silent rejection | 0 | 0 | 0% | no test for the failure path (R-03) or for the self-swap aliasing case (R-02) |
| BR-21 / BR-22 stack merge and 120 cap | 0 | 0 | 0% | no boundary test at 119/120, no test for `amount <= 0` normalisation, no self-swap test |
| BR-23 / BR-24 dating and expiry | 0 | 0 | 0% | no test for month/year rollover in `BASE_SetItemDate`, no test for the same-day expiry boundary |
| BR-25..BR-27 audit, recompute, echo | 0 | 0 | 0% | no protocol-level test of the echo ordering property described in section 2.4 |
| BR-28..BR-31 consolidation, projections, mount, compaction | 0 | 0 | 0% | `BASE_CanCargo` (with its stride defect) is untested; no test for a full warehouse during compaction |

### 11.3 Testability assessment

The component is currently untestable as a unit: it is a free function with no injection seam, it reads and writes six global arrays (`pUser`, `pMob`, `g_pItemList`, `g_pItemGrid`, `g_pMessageStringTable`, `fItemLogFile`), it writes to the filesystem on every accepted request, and its only observable outputs are socket buffers and log files. There is no abstraction over time, over the transport, or over item definitions, so even `BASE_SetItemDate`'s calendar behaviour cannot be pinned without controlling `time()`.

The rule clusters that would be cheapest to pin, if a harness existed, are the pure ones: `BASE_CanEquip` (pure function of item, score, slot, class, equip vector, extra), `BASE_GetItemAmount`/`BASE_SetItemAmount` (pure), `GetItemPointer` (pure bounds logic over arrays), and `BASE_CanCargo` (pure given its inputs, and the natural place to demonstrate R-06). Everything above them requires the global session graph.

---

## 12. Error Handling & Resilience

| Mechanism | Where | Behaviour |
|---|---|---|
| Guard clauses with early return | 24, 31, 38, 44, 48, 54, 65, 87-153, 155, 161, 175, 405 | fail closed; no partial mutation is possible because all guards precede the commit |
| Client-visible refusal | 40, 177 | `_NN_CantWhenAutoTrade`, `_NN_Only_Sunday` |
| Client signal refusal | 50, 56, 67 | `ESCENE_FIELD` / 412 for warehouse problems |
| Silent refusal | 89-147 (log only), 152-153, 161, 405-428 | no client feedback |
| Server log | 89, 97, 102, 111, 123, 131, 136, 145, 157 | text log with account and IP |
| Penalty accounting | 27 | crack points accumulate; 2e9 forces logout |
| State repair | 332-352 (equip 7->6), 366-403 (cargo staging drain) | self-healing for two known inconsistency classes |
| Projection on failure | 26 (`SendHpMode`) | resyncs the client after a liveness rejection |
| Transport-level rejection | `Code/CPSock.cpp:397-411` | oversized/short frames reset the receive buffer and abort the socket |
| Checksum rejection | `Code/CPSock.cpp:455-464`, `Server.cpp:3990-3999` | mismatch is logged and the read loop breaks (packet not dispatched) |
| Tick replay guard | `Code/TMSrv/ProcessClientMessage.cpp:63-64` | `SKIPCHECKTICK` frames from clients are dropped |
| Exception handling | none in this component | no C++ exceptions; failures are expressed as return codes that are frequently ignored |
| Circuit breaker / timeout / retry | none | not applicable at this layer; no upstream call is made |

Resilience verdict: the state machine is fail-closed (a rejected request never half-applies), but it is not fail-*visible*. Three of the four rejection classes produce no client signal, and one of them (R-01) can terminate the process. There is no dead-letter path, no per-player rate limit, and no metrics surface other than text logs.

---

## 13. Security Assessment

| Area | Assessment |
|---|---|
| Input trust | The packet is fully client-controlled and is read with no size validation (`BASE_CheckPacket` disabled). The transport bounds it to 4..8192 bytes, which is smaller than the 20-byte struct's requirements, so the fields are readable; the risk is future field growth, not the present layout. |
| Memory safety | One confirmed out-of-bounds read (R-01, `pMob[WarpID]` with a negative `WarpID`). All other array accesses in this file are bounds-checked either locally or by `GetItemPointer`. |
| Authorization | Two independent gates: session mode (BR-01) and warehouse privilege/NPC (BR-05..BR-07). Equipment requirements are enforced server-side via `BASE_CanEquip`. No admin/GM bypass exists in this handler. |
| Duplication / economy | The self-swap merge path (R-02) is a genuine value-creation vector for the whitelisted item families. The off-grid warehouse cells (R-07) are a hidden-capacity vector. No other duplication path was found: the swap is symmetric, the snapshots are exact, and no path copies an item without clearing the source (the merge path is the only one that writes both cells from the same source). |
| State desync exploitation | R-03 lets a client believe it wears an item the server keeps in inventory. Because the server state is authoritative, this is a UI/economy nuisance rather than a privilege escalation. |
| Escrow protection | An open trade is destroyed on any movement attempt (BR-03) and the escrow is re-validated by `memcmp` before the trade commits (`_MSG_Trade.cpp:56-85`). No path in this component can mutate `Trade.Item` or the money fields. |
| Logging integrity | Accepted swaps are logged before commit with both item codes, account and IP (BR-25). Rejections are mostly unlogged, so an attacker's probing is only partially visible (R-11, R-14). |
| Denial of service | No per-connection rate limit; the worst case per request is 126 footprint tests plus two 2 KB stack buffers plus a disk append. A single connection could saturate the item log. |
| Privilege of the warehouse bypass | BR-05 (`Class == 3 && LearnedSkill & 0x2000`) is a server-side state check, not a client-supplied one, so it cannot be forged through this packet. Its exact gameplay meaning is undocumented (confidence: medium). |

---

## 14. Configuration & Environment Handling

The component reads **no configuration** and has **no environment-dependent behaviour of its own**. Every threshold is a compile-time literal.

| Aspect | Status | Evidence |
|---|---|---|
| Feature flags | none in this component | no reads of any `g_` config variable except `g_Week` (line 175), `g_pItemList`, `g_pItemGrid`, `g_pMessageStringTable` |
| Environment variables | none | not referenced |
| Config files | none read here (item lists and message tables are loaded by `CReadFiles` at startup) | `Code/TMSrv/CReadFiles.h` included transitively, not called |
| Build variants | the only conditional compilation around this code path is `_PACKET_DEBUG`, which wraps the (disabled) `BASE_CheckPacket` call sites, not this file | `Code/TMSrv/Server.cpp:3980-3988`, `Code/CPSock.cpp:544-552` |
| Time dependency | indirect: item dating/expiry via `time()`/`localtime()` in `Basedef`; the guild-day rule would depend on `g_Week`, which is never set | `Code/Basedef.cpp:6732-6780`, `Server.cpp:287` |
| Ordering / scheduling dependency | none; the handler is synchronous and single-threaded | - |
| Persistence dependency | the warehouse lives in the session (`CUser.Cargo`) and reaches disk only through `SaveUser` | `Code/TMSrv/Server.cpp:7056-7083` |
| Locale dependency | `sprintf("%d")` on ints only; no locale-sensitive formatting in the audit path | lines 315 |

Operational consequence: changing any of the 31 item-family literals, the 120 stack cap, the 60-slot carry bound, the 128 warehouse bound, the +/-33 warehouse envelope or the 128 `EF_POS` marker requires a source change and a rebuild. There is no way to tune the rules without redeploying the server binary.

---

## 15. Assumptions, Ambiguities and Confidence Register

| # | Item | Statement | Confidence | Basis |
|---|---|---|---|---|
| A-01 | Role classification | The component is an item relocation/swap transaction, not the trade-window handshake | High | The packet carries only two coordinates and no item payload; the escrow lives entirely in `_MSG_Trade` |
| A-02 | Trade coupling | The only trade behaviour is cancellation (BR-03) | High | lines 31-36 are the only `Trade` accesses |
| A-03 | Item 747 | "Crown", a guild-master artefact that must not leave the character | Medium-High | the only comment is `// 747 = Crown on itemlist` (`Server.cpp:646`); the exclusion appears in trade, drop and cargo paths |
| A-04 | Item families 508/522/526..537 | Guild artefacts subject to the (dormant) weekday rule | Medium | inferred from the shared family list in `_MSG_Trade.cpp:160` and `_MSG_DropItem.cpp:110` |
| A-05 | Item families 413/412/419/420/2390..2419 | Stackable consumables/materials with a 120 cap | Medium | whitelist in the merge branch; 413 is used as a consumable material in `GetMatchCombineEhre` |
| A-06 | Ranges 4150..4188 / 3980..3989 | Timed equipment / mounts with expiry dates | Medium-High | dated here, purged by the minute timer, special-cased as mounts in `BASE_VisualItemCode` |
| A-07 | `EF_POS == 128` in BR-28 | Off-hand-only items that must stay in slot 7 | Medium | the same value is used in `BASE_CanEquip`'s 64/128 pair logic; the exact item category is not named anywhere |
| A-08 | BR-05 privilege meaning | A skill-granted remote warehouse privilege | Medium | boolean composition is unambiguous; the gameplay meaning of class 3 + bit 0x2000 is undocumented |
| A-09 | `MaxCarry` semantics | 30 base + 15 per expansion charm | High | four independent call sites agree (`ProcessDBMessage.cpp:751-757`, `_MSG_UseItem.cpp:5688-5694`, `ProcessSecMinTimer.cpp:571-596`) |
| A-10 | `g_Week` dormancy | The guild weekday gate cannot fire | High | exhaustive search: one definition initialised to 0, no assignment anywhere |
| A-11 | Dead branches (R-09) | The `error`-guarded `SendItem` calls never execute | High | `error` is initialised to -2 and never assigned; the guards require `!Can` while `Can` is 1 in the enclosing arm |
| A-12 | R-02 duplication | A same-coordinate merge inflates a stack | High (arithmetic) / Medium (reachability from a real client) | arithmetic follows from the code; the real client is assumed not to emit identical coordinates, so the vector requires a crafted packet |
| A-13 | Echo ordering | SendItem-before-echo is intentional and load-bearing | Medium-High | the alternative (echo first) would leave the client with post-swap content in pre-swap cells; no comment documents it |
| A-14 | Persistence timing | An unsaved swap is lost on crash, reverting both cells consistently | High | `SaveUser` is the only persistence path and is not called here |
| A-15 | Behaviour of `GetGuild` | Both overloads are effectively inert | High | `Code/TMSrv/GetFunc.cpp:1862-1874` |

### 15.1 Ambiguities that could not be resolved from the codebase

1. Whether the real client ever emits a packet with `Src == Dest`. The server does not defend against it; whether the vector is remotely reachable depends on client behaviour and on whether modified clients are in scope.
2. The meaning of `MOB.Merchant == 2` beyond "warehouse keeper". The value 2 is not named anywhere; `_MSG_Deposit`/`_MSG_Withdraw` were not analysed here.
3. Whether the two staging cells 126/127 are known to the client UI. The protocol allows addressing them; whether the client renders them is outside this repository (the client is not in source form; the `ClientPatch_v7662` artefact is inventory-only).
4. Whether `g_Week` was ever intended to be driven by `WeekMode` (`Server.cpp:332`) or by an external admin command. The admin interpreter (`imple.cpp`) exposes `weekmode` but does not touch `g_Week`.
5. The intended interaction between the crown suppression (BR-15) and the warehouse privilege (BR-05): a player with the remote-warehouse privilege can still never move a crown, which may or may not be intended.

---

*End of report. This analysis was performed read-only; no project file was created, modified or deleted other than this report.*
