# Component Deep Analysis Report — Trade Handler

| Field | Value |
|-------|-------|
| Component name | Trade Handler |
| Primary location | `Code/TMSrv/_MSG_Trade.cpp` (425 lines, ~12 KB) |
| Entry symbol | `Exec_MSG_Trade(int conn, char *pMsg)` — `Code/TMSrv/_MSG_Trade.cpp:20` |
| Message contract | `_MSG_Trade` = `(131 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME)` — `Code/Basedef.h:2046` |
| Router | `ProcessClientMessage.cpp:234-236` |
| Declaration | `ProcessClientMessage.h:111` |
| Domain | In-game player-to-player item and coin escrow (trade window lifecycle) |
| Afferent / Efferent coupling (architecture report) | 1 / 14 |
| Related components | TradingItem Handler (`_MSG_TradingItem.cpp`), QuitTrade Handler (`_MSG_QuitTrade.cpp`), SendAutoTrade Handler (`_MSG_SendAutoTrade.cpp`), ReqTradeList Handler (`_MSG_ReqTradeList.cpp`) |
| Project scope analyzed | `E:\wyd\test-setup\w2pp` (entire root) |
| Ignored folders | `.agents`, `.codegraph`, `.opencode`, `openspec` |
| Analysis date | 2026-11-15 22:21:31 |

---

## 1. Executive Summary

The Trade Handler is the server-authoritative escrow engine for direct player-to-player exchange in TMSrv. A single translation unit, `Code/TMSrv/_MSG_Trade.cpp`, implements one 405-line function that serves three distinct protocol roles behind a single message type (`_MSG_Trade`, id 131): (a) the trade invitation, (b) the rolling offer/negotiation update, and (c) the final two-sided confirmation that performs the atomic swap of `Carry[]` items and `MOB.Coin` between two `CUser` slots. The role is selected at runtime by the contents of the packet (`MyCheck`) and by the session state stored in `pUser[conn].Trade` (`CUser::Trade`, declared `Code/TMSrv/CUser.h:59`).

Its position in the architecture is that of a **Command Handler / inbound adapter** in the TMSrv "one file per client message" convention. The Client Message Dispatcher (`ProcessClientMessage.cpp`) owns the only inbound edge into the handler; the handler then talks to the global world model (`pUser[]`, `pMob[]`), the shared rules engine (`Basedef.cpp`), the outbound projection layer (`SendFunc.cpp`), the audit log (`ItemLog`), and the persistence path (`SaveUser`). It is the only component in the trade family that mutates persistent ownership of both parties, which makes it the trust boundary for item duplication and coin-inflation attacks.

Key findings:

1. **Dual-authority state machine.** The trade session lives entirely in two mirrored copies of `MSG_Trade` inside `CUser`, including the transport header fields (`KeyWord`, `CheckSum`, `ClientTick`). The handler deliberately copies transport fields into the session object before a whole-struct `memcmp` (`_MSG_Trade.cpp:228-238`) so that a "one packet must equal the stored session" invariant can be expressed as a single memory comparison. This is the core anti-tamper mechanism and also the reason domain state is coupled to transport state.

2. **Server re-derives the offer from inventory.** Client-supplied item payloads are never trusted for settlement. After validating each `Item[i]` byte-for-byte against `pMob[conn].MOB.Carry[InvenPos[i]]`, the handler overwrites `m->Item[i]` from the authoritative inventory (`_MSG_Trade.cpp:196-223`), rejects duplicate inventory positions, and requires `InvenPos >= 0 && < MAX_CARRY-4` for every non-empty entry.

3. **Escrow is deferred, not reserved.** Offered items stay in `Carry[]` until the second confirmation arrives, so every handler entry re-validates both parties' escrow against live inventory (`_MSG_Trade.cpp:56-85`). Settlement itself is a dry-run (`BASE_CanTrade`, `Code/Basedef.cpp:4588`) followed by a commit of two local destination arrays, two coin assignments, and two `SendCarry` resyncs — an all-or-nothing block with no intermediate client round trip.

4. **The settlement gate has asymmetric failure semantics.** A large group of rejections (`_MSG_Trade.cpp:287-326`) returns *without* calling `RemoveTrade` and *without* clearing `MyCheck`, leaving both sessions in the "both confirmed" state. Item/coin transfers are still safe (nothing is written), but the window can be re-submitted indefinitely, and a stale double-confirmed session can persist until another action, logout, or disconnect clears it.

5. **No automated tests exist anywhere in the repository.** There is no test project in `W2PP Code Project.sln` (only `DBSrv` and `TMSrv`), no test framework symbols (`gtest`, `CppUnit`, `BOOST_TEST`, `TEST_CASE`, `TEST_F`) in `Code/`, and no `*test*`/`*spec*` files outside the ignored folders. Verification of this component is manual, log-based (`ItemLog`, `Log`), and protocol-level.

6. **Per-message size validation is disabled.** `BASE_CheckPacket()` (`Code/Basedef.cpp:6475-6596`) has its entire body inside a `/* ... */` block and unconditionally `return FALSE`. The `_MSG_Trade` size assertion at `Code/Basedef.cpp:6535` is therefore dead code, and the handler parses a fixed 156-byte structure from whatever length the transport delivered (bounded by `MAX_MESSAGE_SIZE` = 8192, `Code/CPSock.h:38`).

---

## 2. Data Flow Analysis

### 2.1 Transport ingress (common precondition)

```
1. Client writes a framed message (2-byte Size, 1-byte KeyWord, 1-byte CheckSum, then payload)
2. CPSock::ReadMessage validates Size against MAX_MESSAGE_SIZE (8192) and buffer remainder
   (Code/CPSock.cpp:386-411), de-obfuscates bytes 4..Size-1 and verifies CheckSum (CPSock.cpp:424-458)
3. TMSrv WndProc / WSA_READ -> pUser[User].cSock.ReadMessage() -> ProcessClientMessage(User, Msg, FALSE)
   (Code/TMSrv/Server.cpp:3973-4001)
4. ProcessClientMessage rejects ID out of [0, MAX_USER), drops packets while ServerDown >= 120,
   drops SKIPCHECKTICK, then dispatches on std->Type (Code/TMSrv/ProcessClientMessage.cpp:38-66)
5. case _MSG_Trade -> Exec_MSG_Trade(conn, pMsg) (ProcessClientMessage.cpp:234-236)
```

Per-message `sizeof` validation is not performed: `BASE_CheckPacket()` returns `FALSE` for every input (`Code/Basedef.cpp:6475-6596`, body commented out at line 6476 and `return FALSE;` at line 6595).

### 2.2 Flow A — invitation (session does not yet exist)

```
1. Exec_MSG_Trade entry, session gate: HP > 0 && Mode == USER_PLAY     (_MSG_Trade.cpp:24-30)
2. OpponentID range validation 1..MAX_USER-1                          (_MSG_Trade.cpp:34-40)
3. Opponent liveness: pUser[OpponentID].Mode == USER_PLAY             (_MSG_Trade.cpp:42-47)
4. Money sanity: 0 <= m->TradeMoney <= pMob[conn].MOB.Coin            (_MSG_Trade.cpp:49-54)
5. Offered-item provenance: InvenPos bounds + memcmp vs Carry[]       (_MSG_Trade.cpp:56-85)
6. Partner pinning: pUser[conn].Trade.OpponentID == 0 (no prior session)
                                                                      (_MSG_Trade.cpp:87-118)
7. PK-mode exclusion (both parties)                                   (_MSG_Trade.cpp:120-128)
8. Whisper/DND exclusion (both parties)                               (_MSG_Trade.cpp:130-138)
9. Item-class gates: crown/royal seal, guild-bound, EF_NOTRADE        (_MSG_Trade.cpp:145-190)
10. Opponent busy check: pUser[OpponentID].Trade.OpponentID           (_MSG_Trade.cpp:192, 402-407)
11. INVITE BRANCH: memcpy(&pUser[conn].Trade, m) -> MyCheck=0, TradeMoney=0
                                                                      (_MSG_Trade.cpp:411-414)
12. Swap identity in the same buffer: m->ID = OpponentID, m->OpponentID = conn
                                                                      (_MSG_Trade.cpp:416-418)
13. pUser[OpponentID].cSock.AddMessage(m, sizeof(MSG_Trade))          (_MSG_Trade.cpp:420)
14. Opponent session pre-cleared: MyCheck=0, TradeMoney=0             (_MSG_Trade.cpp:422-423)
```

### 2.3 Flow B — rolling offer update (session exists, `MyCheck == 0`)

```
1-10. Same validation chain as Flow A (all eleven gates re-run on every packet)
11. pUser[OpponentID].Trade.OpponentID == conn (mutual pairing)       (_MSG_Trade.cpp:194)
12. Server re-derives m->Item[i] from Carry[InvenPos[i]]              (_MSG_Trade.cpp:196-223)
    - InvenPos == -1            -> memset(Item[i], 0)
    - InvenPos out of bounds    -> _NN_Wrong_Trade_Packet1 + RemoveTrade(conn) + break
    - duplicate InvenPos        -> _NN_Wrong_Trade_Packet1 + RemoveTrade(conn) + return
13. ELSE BRANCH (m->MyCheck != 1): memcpy(&pUser[conn].Trade, m)      (_MSG_Trade.cpp:389)
14. Both sides un-checked: MyCheck = 0 for conn and OpponentID        (_MSG_Trade.cpp:391-392)
15. Identity swap + forward to opponent                               (_MSG_Trade.cpp:394-397)
```

### 2.4 Flow C — confirmation (`MyCheck == 1`)

```
1-10. Same validation chain as Flow A
11. Mutual pairing check (as Flow B)
12. Server re-derivation of m->Item[i] (as Flow B)
13. Local session marked checked: MyCheck=1, then transport fields imported
                                                                      (_MSG_Trade.cpp:227-230)
14. Freeze check: memcmp(&pUser[conn].Trade, m, sizeof(MSG_Trade))    (_MSG_Trade.cpp:232)
    - mismatch -> RemoveTrade(conn) + RemoveTrade(OpponentID) + Log "Click check button but diffrent data"
15. If opponent MyCheck == 0:
      SendClientSignal(conn, conn, _MSG_CNFCheck)                     (_MSG_Trade.cpp:242)
      forward packet with swapped IDs to opponent                     (_MSG_Trade.cpp:244-247)
      return
16. Settlement (both checked):
      money range per side, 0..2,000,000,000                          (_MSG_Trade.cpp:251-265)
      resulting coin per side via unsigned arithmetic                 (_MSG_Trade.cpp:267-285)
      affordability per side                                          (_MSG_Trade.cpp:287-299)
      BASE_CanTrade dry-run per side (dest arrays on the stack)       (_MSG_Trade.cpp:301-326)
      commit: memcpy Carry x2, Coin x2                               (_MSG_Trade.cpp:328-332)
      resync: SendCarry(conn), SendCarry(OpponentID)                 (_MSG_Trade.cpp:334-335)
      audit: ItemLog x4 (start, side A payload, side B payload, end) (_MSG_Trade.cpp:343-377)
      persistence: SaveUser(conn,1), SaveUser(OpponentID,1)          (_MSG_Trade.cpp:379-380)
      teardown: RemoveTrade(conn), RemoveTrade(OpponentID)           (_MSG_Trade.cpp:382-383)
```

### 2.5 Flow D — teardown (`RemoveTrade`)

```
1. Guard: conn <= 0 || conn >= MAX_USER -> return                     (Server.cpp:7321-7322)
2. memset(&pUser[conn].Trade, 0, sizeof(MSG_Trade))                   (Server.cpp:7324)
3. Re-arm sentinel: Trade.InvenPos[0..14] = -1                         (Server.cpp:7326-7327)
4. memset(&pUser[conn].AutoTrade, 0, sizeof(MSG_SendAutoTrade))       (Server.cpp:7329)
5. Re-arm sentinel: AutoTrade.CarryPos[0..11] = -1                    (Server.cpp:7331-7332)
6. If Mode != USER_PLAY -> return (no client notification)            (Server.cpp:7334-7335)
7. SendClientSignal(conn, conn, 900)                                  (Server.cpp:7337)
8. If TradeMode: GetCreateMob + GridMulticast (restore player body), TradeMode = 0
                                                                      (Server.cpp:7339-7351)
```

### 2.6 Session exit points outside the handler

| Trigger | Location | Effect on trade pair |
|---|---|---|
| `_MSG_QuitTrade` | `_MSG_QuitTrade.cpp:29-30` | Clears both sides (partner first, then self) |
| SaveAndQuit / `CloseUser` (USER_PLAY) | `Server.cpp:6873-6878` | Clears partner if mutually paired, then `pUser[conn].Trade.OpponentID = 0` |
| `CharLogOut` | `Server.cpp:7104-7110` | Clears partner if mutually paired, then proceeds to save/delete mob |
| Any conflicting action packet | `_MSG_Action.cpp:40-43`, `_MSG_Buy.cpp:34-41`, `_MSG_Sell.cpp:31-34`, `_MSG_GetItem.cpp:31-34`, `_MSG_DropItem.cpp:31-34`, `_MSG_DeleteItem.cpp:36-39`, `_MSG_UseItem.cpp:51-53`, `_MSG_Quest.cpp:35-37`, `_MSG_ReqBuy.cpp:38`, `_MSG_SplitItem.cpp:36`, `_MSG_PutoutSeal.cpp:30`, `_MSG_PKMode.cpp:26-33`, 7 `_MSG_CombineItem*` handlers | Clears the window before the other action proceeds |
| Newbie-event server switch | `ProcessSecMinTimer.cpp:2047-2056` | `RemoveTrade(i)` for every session with `TradeMode == 1` |
| Auto-trade activation attempt | `_MSG_TradingItem.cpp:31-36`, `_MSG_SendAutoTrade.cpp:32-37` | Refuses to start while a session exists and clears it |

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule type | Rule description | Location |
|----|-----------|------------------|----------|
| BR-01 | Validation | Only a live, in-world player (`HP > 0`, `Mode == USER_PLAY`) may send `_MSG_Trade`; otherwise crack counter +5 and local session cleared | `_MSG_Trade.cpp:24-30` |
| BR-02 | Validation | `OpponentID` must be a legal user slot in `[1, MAX_USER-1]` | `_MSG_Trade.cpp:34-40` |
| BR-03 | Validation | The counterpart must also be `USER_PLAY` at the moment of contact | `_MSG_Trade.cpp:42-47` |
| BR-04 | Validation | Offered money must be `0 <= TradeMoney <= own MOB.Coin` on first contact | `_MSG_Trade.cpp:49-54` |
| BR-05 | Integrity | Every offered item must still exist, unchanged, at the declared `InvenPos` in the sender's `Carry[]` | `_MSG_Trade.cpp:56-70` |
| BR-06 | Integrity | The counterpart's stored escrow must still match their live `Carry[]` | `_MSG_Trade.cpp:72-84` |
| BR-07 | Integrity | While a session is pinned to one opponent, the offer may not change for already-populated item slots or a non-zero money value | `_MSG_Trade.cpp:87-109` |
| BR-08 | Business logic | A user may participate in at most one trade session at a time (self side) | `_MSG_Trade.cpp:110-118` |
| BR-09 | Business logic | PK mode on either side forbids trading | `_MSG_Trade.cpp:120-128` |
| BR-10 | Business logic | Whisper/DND state on either side forbids trading (both players must have whispers enabled) | `_MSG_Trade.cpp:130-138` |
| BR-11 | Business logic | Items 747, 3993 and 3994 (crown / royal seal family) can never be traded | `_MSG_Trade.cpp:147-158` |
| BR-12 | Business logic | Guild-bound items (508, 522, 526..537, 446) may only be traded when the item's encoded guild is owned by a guild master on one of the two sides | `_MSG_Trade.cpp:160-177` |
| BR-13 | Business logic | Items carrying `EF_NOTRADE` can never be traded | `_MSG_Trade.cpp:179-189` |
| BR-14 | Validation | Slot bounds: every `InvenPos` must satisfy `0 <= pos < MAX_CARRY-4` (60) | `_MSG_Trade.cpp:60-61`, `74-75`, `205-210` |
| BR-15 | Integrity | No two offer slots may reference the same inventory position | `_MSG_Trade.cpp:211-219` |
| BR-16 | Business logic | The offer is server-derived: `Item[i]` is overwritten from `Carry[InvenPos[i]]`; `InvenPos == -1` clears the slot | `_MSG_Trade.cpp:196-223` |
| BR-17 | State machine | A checked offer is frozen: a `MyCheck == 1` packet must deep-equal the stored session byte-for-byte | `_MSG_Trade.cpp:225-238` |
| BR-18 | State machine | Settlement requires both sides checked; the first confirmer only causes an echo (`_MSG_CNFCheck`) and a forward | `_MSG_Trade.cpp:240-249` |
| BR-19 | Validation | Each side's `TradeMoney` must be in `[0, 2,000,000,000]` at settlement | `_MSG_Trade.cpp:251-265` |
| BR-20 | Business logic | Resulting coin per side must stay within `[0, 2,000,000,000]` | `_MSG_Trade.cpp:267-285` |
| BR-21 | Validation | Each side must still be able to pay its offered money at settlement | `_MSG_Trade.cpp:287-299` |
| BR-22 | Business logic | Both sides must have inventory capacity for the incoming items; three distinct messages differentiate the failure cases | `_MSG_Trade.cpp:301-326` |
| BR-23 | Business logic | Commit is all-or-nothing: destination arrays are built on the stack and only then written back | `_MSG_Trade.cpp:301-335` |
| BR-24 | Audit | Four `ItemLog` records per completed trade (start marker, both payloads with coin, end marker) | `_MSG_Trade.cpp:340-377` |
| BR-25 | Persistence | Both characters are persisted immediately after a successful trade (`Export = 1`) | `_MSG_Trade.cpp:379-380` |
| BR-26 | State machine | Any update packet with `MyCheck == 0` resets the confirmation of both sides | `_MSG_Trade.cpp:391-392` |
| BR-27 | Business logic | An invitation forces `TradeMoney = 0` and `MyCheck = 0` on both sides; money can only be introduced in an update packet | `_MSG_Trade.cpp:413-414`, `422-423` |
| BR-28 | Lifecycle | `RemoveTrade` clears `Trade` and `AutoTrade`, restores `-1` sentinels, notifies the client with signal 900, and re-creates the player mob if auto-trade was active | `Server.cpp:7319-7352` |
| BR-29 | Lifecycle | Disconnect/logout must clear the partner's window only when the pairing is mutual | `Server.cpp:6873-6878`, `7104-7110` |
| BR-30 | Contract | Per-message size validation for `_MSG_Trade` is specified but inert (`BASE_CheckPacket` returns `FALSE`) | `Basedef.cpp:6535`, `6475-6596` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Live in-world player gate

**Overview**:
`Exec_MSG_Trade` refuses to process any packet unless the sender is a living, fully logged-in character. The check is `pMob[conn].MOB.CurrentScore.Hp == 0 || pUser[conn].Mode != USER_PLAY` (`_MSG_Trade.cpp:24`), and the failure path is `SendHpMode` → `AddCrackError(conn, 5, 18)` → `RemoveTrade(conn)` → `return`.

**Detailed description**:
The gate protects the entire handler from operating on a slot that is still negotiating (modes `USER_ACCEPT=1`, `USER_LOGIN=2`, `USER_SELCHAR=11`, `USER_CHARWAIT=12`, `USER_WAITDB=13`, defined at `CUser.h:26-37`) or on a corpse. In practice a dead player cannot start, advance or complete a trade; the trade window is torn down locally so the client is resynchronised rather than left with a phantom window. Because `SendHpMode` (`SendFunc.cpp:1622`) pushes the current HP/MP/mode triple, the client is forced back onto an authoritative state instead of trusting its local view.

The crack counter is the anti-automation mechanism of the server: each rejected packet adds 5 points to `pUser[conn].NumError` (`Server.cpp:684-707`), and reaching 2,000,000,000 triggers `_NN_Bad_Network_Packets`, `CharLogOut(conn)` and an audit line. Since a legitimately playing client never sends this packet while dead or out of world, repeated hits indicate a scripted or forged packet stream. Note that this is one of only two trade-family entry points that use `AddCrackError` with weight 5 (`_MSG_Trade.cpp:27`), while `_MSG_QuitTrade`, `_MSG_SendAutoTrade` and `_MSG_ReqTradeList` use weight 10 (`_MSG_QuitTrade.cpp:25`, `_MSG_SendAutoTrade.cpp:27`, `_MSG_ReqTradeList.cpp:29`).

The gate is evaluated on every packet, not only at invitation time, which matters because the trade window can survive across a death if some other path does not clear it: the state machine cannot advance on a corpse. There is one asymmetry worth recording: the gate clears only the sender's session (`RemoveTrade(conn)`), never the counterpart's, so a partner keeps a window pointing at a session that is about to be zeroed by the next teardown path. The partner's window is not left referencing a dangling trade, because the pairing always requires both sides (`_MSG_Trade.cpp:194`), but the partner's client is not explicitly notified at that instant.

**Rule workflow**:
```
receive _MSG_Trade
  -> if Hp == 0 OR Mode != USER_PLAY:
        SendHpMode(conn)
        AddCrackError(conn, 5, 18)   // +5 to NumError, logout at 2e9
        RemoveTrade(conn)            // clears Trade/AutoTrade, signal 900
        return
  -> continue to BR-02
```

---

### Business Rule: BR-02 — Opponent slot validity

**Overview**:
`OpponentID` is read from the packet (`_MSG_Trade.cpp:32`) and must satisfy `> 0 && < MAX_USER` (1000). Out-of-range targets are logged with account name and IP, answered with `_NN_Wrong_Trade_Packet1`, and the sender's session is cleared (`_MSG_Trade.cpp:34-40`).

**Detailed description**:
`MAX_USER` is 1000 (`Basedef.h:56`); indices `0` and `>= 1000` are not player slots. Index `0` is a reserved "no user" value used throughout TMSrv (`SendCarry` and `PutItem` both treat `conn <= 0` as invalid), and indices `>= 1000` belong to the mob space (`pMob` covers `MAX_MOB` = 25000 entries, `Basedef.h:107`). Accepting such a value would let a client address a mob slot through a player-session API, which is why the check is numeric and unconditional.

The rejection produces an audit trail in two channels: `Log("err,msg_trade - target out of bound.", account, ip)` for the server log, and the client-facing string 80 (`_NN_Wrong_Trade_Packet1`, `Language.h:100`). Compared with the other failures in this handler, this is one of the few that both logs and messages the client; several later failures (for example `_MSG_Trade.cpp:49-54`) only remove the session silently.

`OpponentID` is declared `unsigned short` in `MSG_Trade` (`Basedef.h:2056`), so the `if (OpponentID <= 0 ...)` guard actually catches only the value `0`; the upper bound is the meaningful test. Because the field is 16-bit, no negative value can arrive on the wire, and the handler's later comparisons (`pUser[OpponentID]`) are always inside the `pUser` array once this check passes.

**Rule workflow**:
```
OpponentID = m->OpponentID
  -> if OpponentID <= 0 OR OpponentID >= 1000:
        Log "err,msg_trade - target out of bound."
        SendClientMessage(conn, _NN_Wrong_Trade_Packet1)
        RemoveTrade(conn)
        return
  -> continue to BR-03
```

---

### Business Rule: BR-03 — Counterpart must be in play

**Overview**:
If `pUser[OpponentID].Mode != USER_PLAY`, both sessions are cleared and the call returns without any client message (`_MSG_Trade.cpp:42-47`).

**Detailed description**:
This rule is the counterpart of BR-01: the sender must be playing, and so must the target. The two teardown calls are ordered opponent-first (`RemoveTrade(OpponentID)` then `RemoveTrade(conn)`), a recurring convention in the trade family (`_MSG_QuitTrade.cpp:29-30`, `_MSG_TradingItem.cpp:33-34`) that keeps the counterpart's client consistent before the initiator's session object is wiped. Because `RemoveTrade` is a no-op for invalid slots (`Server.cpp:7321-7322`), calling it on a slot that is merely not `USER_PLAY` is safe.

There is no message on this path. From the initiator's point of view the trade window simply closes, which is consistent with the server's general behaviour of silently dropping actions aimed at non-existent or non-playing targets (compare `_MSG_ReqTradeList.cpp:33-37`). The absence of a `Log` line means that repeated attempts to trade with empty slots will not be visible in the log; only the transport-level and `ItemLog` channels would show anything.

Because the check runs on every packet, a disconnect that happens between offer and confirmation terminates the attempt on the survivor's next packet. There is no independent timer for this: the handler validates the partner's mode afresh each time, so the state cannot be advanced but can linger until the player sends something.

**Rule workflow**:
```
  -> if pUser[OpponentID].Mode != USER_PLAY:
        RemoveTrade(OpponentID)
        RemoveTrade(conn)
        return
  -> continue to BR-04
```

---

### Business Rule: BR-04 — Money must be provable at invitation

**Overview**:
On first contact, `m->TradeMoney` must satisfy `>= 0` and `<= pMob[conn].MOB.Coin` (`_MSG_Trade.cpp:49-54`). A violation removes both the initiator and the addressed opponent and returns silently.

**Detailed description**:
The handler validates the sender's funds before it even knows whether a session exists, so the rule behaves as an admission test for every packet rather than only for invitations. `MOB.Coin` is the character-level currency carried in `STRUCT_MOB` (`Basedef.h:448`), distinct from the account-level `CUser::Coin` (`CUser.h:51`) that `Deposit`/`Withdraw` and the persistence path use (`_MSG_Deposit.cpp:40`, `Server.cpp:7067`). Trading therefore never touches the account bank.

The failure path is notable for what it does *not* do: no `Log`, no `SendClientMessage`, and the opponent's session is cleared even though the opponent may be an innocent third party that merely happened to be named by the packet. In practice the opponent cannot have a session with `conn` at that moment (the mutual pairing is checked later at `_MSG_Trade.cpp:194`), so this is only cleaning pre-invite state, but the rule does allow one client to reset another client's pending trade window by sending a `_MSG_Trade` with an over-large money field.

The value remains provisional: BR-27 forces the stored money back to `0` when the session is created, so the amount that actually settles is always the one carried by a later update packet and re-validated by BR-19/BR-20/BR-21. This rule therefore only prevents an obviously impossible first packet from creating a session.

**Rule workflow**:
```
  -> if m->TradeMoney < 0 OR m->TradeMoney > pMob[conn].MOB.Coin:
        RemoveTrade(conn)
        RemoveTrade(OpponentID)
        return
  -> continue to BR-05
```

---

### Business Rule: BR-05 — Offered item provenance (sender side)

**Overview**:
For every `i` in `0..MAX_TRADE-1` where `m->Item[i].sIndex != 0`, the handler requires a valid `InvenPos[i]` and requires `memcmp(&pMob[conn].MOB.Carry[InvenPos[i]], &m->Item[i], sizeof(STRUCT_ITEM)) == 0` (`_MSG_Trade.cpp:56-70`).

**Detailed description**:
`STRUCT_ITEM` is 8 bytes (`Basedef.h:398-412`: `short sIndex` plus three `{unsigned char cEffect; unsigned char cValue;}` unions), so the comparison covers the item id and all three effect/value pairs. The item payload in the packet is therefore only a reference; the authoritative value is the inventory slot. This blocks the classic forgery where a client describes a high-value item while pointing at a low-value inventory slot, because both the id and every effect byte must match.

`MAX_TRADE` is 15 (`Basedef.h:79`), so at most 15 distinct stacks can be offered per trade — a hard economic limit of the protocol that mirrors the client's trade-window grid. Empty slots (`sIndex == 0`) are skipped entirely and their `InvenPos` is not validated here; they are handled later by BR-16, which treats `-1` as an explicit "empty this slot" instruction.

The bound check is `InvenPos[i] < 0 || InvenPos[i] >= MAX_CARRY-4` (`_MSG_Trade.cpp:60`), i.e. `[0, 59]`, because slots 60..63 of `Carry` are reserved for the two inventory-expansion tickets (`Carry[60]`, `Carry[61]`, both item 3467 at `ProcessSecMinTimer.cpp:571-597`) and two spare slots. `MAX_CARRY` is 64 (`Basedef.h:76`) and the effective `MaxCarry` is 30/45/60 depending on those tickets (`ProcessSecMinTimer.cpp:576-596`). The bound uses the constant, not `pMob[conn].MaxCarry` — see the risk entry TD-04.

On out-of-bounds `InvenPos`, the handler executes a bare `return` (`_MSG_Trade.cpp:60-61`) without `RemoveTrade`. This is the only place in the file where a rejection leaves the session fully intact, and it is inconsistent with the neighbouring provenance failure at `_MSG_Trade.cpp:63-69`, which logs and clears both sides.

**Rule workflow**:
```
for i in 0..14:
  if m->Item[i].sIndex == 0: continue
  if m->InvenPos[i] < 0 OR m->InvenPos[i] >= 60:
      return                       // no log, no teardown
  if memcmp(&pMob[conn].MOB.Carry[m->InvenPos[i]], &m->Item[i], 8) != 0:
      Log "err,msg_trade - item remove or changed."
      RemoveTrade(conn); RemoveTrade(OpponentID); return
  -> continue to BR-06
```

---

### Business Rule: BR-06 — Counterpart escrow still matches their inventory

**Overview**:
In the same `MAX_TRADE` loop, for each non-empty entry of the counterpart's stored session (`pUser[OpponentID].Trade.Item[i]`), the handler requires their `Trade.InvenPos[i]` to be in range and their live `Carry[InvenPos[i]]` to deep-equal the stored `Trade.Item[i]` (`_MSG_Trade.cpp:72-84`).

**Detailed description**:
Because escrow is deferred (items are not removed when offered), the counterpart's inventory can change between their offer and the current packet — they might have sold, dropped, equipped, consumed or deleted the offered item. This rule re-proves the counterpart's side on the sender's packet, so the settlement never relies on a stale snapshot. It is the reason `RemoveTrade` is invoked by so many unrelated handlers: any action that can mutate `Carry[]` is expected to close the trade window first (`_MSG_Buy.cpp:34-41`, `_MSG_DropItem.cpp:31-34`, `_MSG_UseItem.cpp:51-53`, ...). The memcmp is the second line of defence when such a handler forgets or when the mutation happens through a path that does not clear sessions.

The check is symmetric in structure but not in consequence: a failure clears both sessions and logs under the *sender's* account name (`_MSG_Trade.cpp:79`), which means the audit line can name an innocent party as the offender. Operationally this makes log-based root-cause analysis of a failed trade ambiguous, since the actor who invalidated the counterpart's escrow is not recorded.

Two guarded `return`s (`_MSG_Trade.cpp:74-75`) again skip teardown when the counterpart's stored `InvenPos` is out of range. A stored `InvenPos` outside `[0, 59]` can only arise from a corrupted session or from a build that wrote it differently; with `RemoveTrade` re-arming every slot to `-1` (`Server.cpp:7326-7327`) this is a defensive branch rather than a reachable client attack.

**Rule workflow**:
```
for i in 0..14:
  if pUser[OpponentID].Trade.Item[i].sIndex == 0: continue
  if pUser[OpponentID].Trade.InvenPos[i] < 0 OR >= 60: return
  if memcmp(&pMob[OpponentID].MOB.Carry[pos], &pUser[OpponentID].Trade.Item[i], 8) != 0:
      Log "err,msg_trade - item remove or changed."
      RemoveTrade(conn); RemoveTrade(OpponentID); return
  -> continue to BR-07
```

---

### Business Rule: BR-07 — Offer stability and partner pinning

**Overview**:
After the provenance loops, the handler compares the incoming `OpponentID` with `pUser[conn].Trade.OpponentID` (`_MSG_Trade.cpp:87`). If they are equal, the incoming offer must be identical to the stored one for every populated slot and, when the stored money is non-zero, for the money value (`_MSG_Trade.cpp:89-109`). If they differ and a session exists, the request is rejected with `_NN_Already_Trading` and the local session is cleared (`_MSG_Trade.cpp:112-117`).

**Detailed description**:
This is the rule that makes the state machine monotonic within a pairing. Without it a client could keep the same opponent but silently swap the offered item between the two confirmations, which is the exact scenario the settlement would otherwise have to detect on its own. The comparison only inspects slots where the *stored* session has a non-zero `sIndex` (`_MSG_Trade.cpp:95`), so adding a new item to an empty slot is permitted here and is later caught by the whole-struct freeze check (BR-17) once a side has checked.

The money half of the rule is conditional in a way that matters: `if (pUser[conn].Trade.TradeMoney && m->TradeMoney != pUser[conn].Trade.TradeMoney) Check = 1;` (`_MSG_Trade.cpp:98`). A stored money of `0` therefore does not constrain the incoming money, which is required because the invitation branch forces money to `0` and the first update packet legitimately introduces an amount. Once a non-zero amount is stored, decreasing or increasing it through this path is treated as tampering and clears both sessions.

The partner-pinning half implements the "one trade at a time, per user" rule from the initiator's perspective: if the sender already has a session with somebody else, the packet is refused with string 82 (`_NN_Already_Trading`, `Language.h:102`) and only the sender's own session is destroyed — the previous partner is left with a window that references a session that no longer exists. Because the pairing check at `_MSG_Trade.cpp:194` requires mutuality, the leftover window cannot progress and is cleared by the next teardown trigger (BR-29 and the action-packet sweep).

**Rule workflow**:
```
lastOpponent = pUser[conn].Trade.OpponentID
if lastOpponent == OpponentID:
    for i in 0..14:
        Check = (stored Item[i] ? memcmp(stored Item[i], m->Item[i]) : 0)
        if (stored TradeMoney != 0 AND m->TradeMoney != stored TradeMoney) Check = 1
        if Check: Log "... item remove or changed."; RemoveTrade both; return
else:
    if lastOpponent != 0:
        SendClientMessage(conn, _NN_Already_Trading)
        RemoveTrade(conn); return
  -> continue to BR-09
```

---

### Business Rule: BR-08 — One session per user

**Overview**:
A user can have at most one active trade session. The self side is enforced at BR-07 (`_MSG_Trade.cpp:110-118`) and again at BR-28 through the single `Trade` member; the other side is enforced by the "opponent already trading" branch (`_MSG_Trade.cpp:192-194`, `402-407`): if the opponent's session points at somebody else, the request is refused with `_NN_Already_Trading` and the sender's session is cleared.

**Detailed description**:
The session is a single embedded `MSG_Trade` in `CUser` (`CUser.h:59`), so there is exactly one slot per user by construction. The two branches together make the relation between sessions a strict mutual pairing: `pUser[a].Trade.OpponentID == b` and `pUser[b].Trade.OpponentID == a`, or no trade at all. Every subsequent branch tests mutuality before acting on both sides (`_MSG_Trade.cpp:154`, `171`, `185`, `194`), and the teardown paths do the same (`Server.cpp:6875`, `7108`).

A consequence of the strict pairing is that there is no "one-sided invitation registry": an invitation is not stored on the recipient until the recipient's own `_MSG_Trade` arrives. The forward at `_MSG_Trade.cpp:420` only transmits the packet; `pUser[OpponentID].Trade` is not populated with the initiator's offer. This keeps the state symmetric but means the recipient has no server-side record of the pending invitation, and therefore no server-side expiry either.

A second consequence is that self-trading (`OpponentID == conn`) is not explicitly rejected. With `m->OpponentID == conn`, the invite branch stores the session, sends the packet to the same socket (`m->ID = conn`, `m->OpponentID = conn`) and, on confirmation, `pUser[OpponentID]` and `pUser[conn]` are the same object, so both `BASE_CanTrade` dry-runs start from the same inventory and produce identical results. The arithmetic and the dry-runs cancel out, no items or coins change, and four `ItemLog` records are still written. The absence of a self-trade guard is therefore a validation gap without a demonstrated exploit.

**Rule workflow**:
```
own side:  session empty OR stored OpponentID == incoming OpponentID
other side: opponent has no session OR opponent's session points back at conn
violation (own):   _NN_Already_Trading + RemoveTrade(conn)
violation (other): _NN_Already_Trading + RemoveTrade(conn)
  -> continue to BR-09
```

---

### Business Rule: BR-09 — PK mode forbids trading

**Overview**:
If either `pUser[conn].PKMode` or `pUser[OpponentID].PKMode` is non-zero, both clients receive `_NN_Cant_trade_pkmode` and both sessions are cleared (`_MSG_Trade.cpp:120-128`).

**Detailed description**:
`CUser::PKMode` (`CUser.h:89`) is the player's "player-killer" toggle. The rule prevents a player from using the trade window as a laundering channel while flagged, and more practically prevents a hostile player from holding a victim's window open as a distraction during PvP. It is one of the few rules that clears both sides unconditionally, without first testing mutuality, which is safe because `RemoveTrade` zeroes only the addressed slot.

The mirrored enforcement on the toggle side is in `_MSG_PKMode.cpp:26-33`: enabling PK mode while a trade is open closes the trade and notifies the partner with the same string. Together the two enforce that the invariant "no trade session with PKMode set" holds regardless of the order of operations, which matters because the trade handler is not the only writer of `PKMode`.

`PKMode` is also used by `Exec_MSG_QuitTrade` when computing the PK-info broadcast state (`_MSG_QuitTrade.cpp:45`), so the two flags are read consistently across the trade family. The check here runs after the offer-stability rule but before the item-class gates, which means a PK-mode rejection takes precedence over any guild or `EF_NOTRADE` diagnostics.

**Rule workflow**:
```
if pUser[conn].PKMode OR pUser[OpponentID].PKMode:
    SendClientMessage(conn,       _NN_Cant_trade_pkmode)
    SendClientMessage(OpponentID, _NN_Cant_trade_pkmode)
    RemoveTrade(conn); RemoveTrade(OpponentID); return
  -> continue to BR-10
```

---

### Business Rule: BR-10 — Whisper/DND state forbids trading

**Overview**:
If either `pUser[conn].Whisper` or `pUser[OpponentID].Whisper` is non-zero, both clients receive `_NN_Deny_Whisper` (string 165, `Language.h:185`) and both sessions are cleared (`_MSG_Trade.cpp:130-138`).

**Detailed description**:
`CUser::Whisper` is the private-message privacy flag, toggled by the `/whisper` chat command (`_MSG_MessageChat.cpp:111-119`), where `Whisper == 1` is reported to the player as "Whisper : Off". The trade rule therefore requires both participants to have private messages enabled, an anti-harassment measure inherited from the original server: a player who has opted out of whispers is also opted out of unsolicited trade invitations.

The rule is deliberately duplicated into the trade window rather than being enforced only at invitation time, because invitations can be issued by a client that never opens a chat channel. It also means that toggling whispers off mid-trade does not immediately close an existing window: `_MSG_Trade` must be sent again for the rule to fire, and until then the session remains in whatever check state it had. The only other place where `Whisper` is reset is the login path (`ProcessDBMessage.cpp:387`).

The message sent to both sides is the whisper-denial string rather than a trade-specific one, which is a user-facing ambiguity: a player sees "whispers denied" while attempting a trade. There is no dedicated trade message for this condition in `Language.h`, which is consistent with the reuse of `_NN_Deny_Whisper` in the ranking handler (`_MSG_ReqRanking.cpp:32`).

**Rule workflow**:
```
if pUser[conn].Whisper OR pUser[OpponentID].Whisper:
    SendClientMessage(conn,       _NN_Deny_Whisper)
    SendClientMessage(OpponentID, _NN_Deny_Whisper)
    RemoveTrade(conn); RemoveTrade(OpponentID); return
  -> continue to BR-11
```

---

### Business Rule: BR-11 — Crown and royal seal items are non-tradable

**Overview**:
For each offered item, `sIndex == 747` (crown) or `sIndex == 3993` or `sIndex == 3994` (royal seal family) aborts the trade with `_NN_Only_With_Guild_Master` on both clients, clears the sender, and clears the opponent only if the opponent's session points back at the sender (`_MSG_Trade.cpp:147-158`).

**Detailed description**:
These three ids are the server's guild-sovereignty artefacts: 747 is the crown checked throughout the guild code (`Server.cpp:670-681` scans `Cargo[]` for 747/3993/3994 when a guild master changes), and 3993/3994 are its sealed variants. Allowing them through the direct trade window would bypass the guild-transfer procedures that the crown is meant to trigger, so the rule refuses them outright — even when both parties are guild masters, which is why the message text ("only with guild master") is misleading for this specific branch.

The cleanup here is conditional and different from every other item gate in the file: `RemoveTrade(conn)` is unconditional, but `RemoveTrade(OpponentID)` is guarded by `if (pUser[OpponentID].Trade.OpponentID == conn)` (`_MSG_Trade.cpp:154-155`). The same conditional pattern is repeated for the guild-bound and `EF_NOTRADE` branches (`_MSG_Trade.cpp:171-172`, `185-186`) because those gates can fire on a packet addressed to a player who is already trading with someone else, and destroying that unrelated session would be collateral damage.

Only the sender's 15 slots are inspected. The counterpart's crown items are inspected when the counterpart sends their own `_MSG_Trade`, since both sides must send packets for the state machine to progress; the settlement cannot occur on a single side's packet (BR-18).

**Rule workflow**:
```
for i in 0..14:
  if m->Item[i].sIndex in {747, 3993, 3994}:
      SendClientMessage(conn, _NN_Only_With_Guild_Master)
      SendClientMessage(OpponentID, _NN_Only_With_Guild_Master)
      RemoveTrade(conn)
      if pUser[OpponentID].Trade.OpponentID == conn: RemoveTrade(OpponentID)
      return
  -> continue to BR-12
```

---

### Business Rule: BR-12 — Guild-bound item transfer requires a guild master

**Overview**:
For `sIndex` in the set `{508, 522, 446} ∪ [526, 537]`, the handler decodes the item's guild owner with `GetGuild(&m->Item[i])` and permits the trade only if the item belongs to the sender's guild and the sender has a non-zero `GuildLevel`, or the item belongs to the opponent's guild and the opponent has a non-zero `GuildLevel` (`_MSG_Trade.cpp:160-177`).

**Detailed description**:
Guild ownership is not a field on `STRUCT_ITEM`; it is encoded in two effects, `EF_HWORDGUILD` (high byte) and `EF_LWORDGUILD` (low byte), combined as `hword * 256 + lword` by `BASE_GetGuild` (`Basedef.cpp:4207-4235`) and exposed through `GetGuild` (`GetFunc.cpp:1869-1874`). The ids covered here are the guild-plate/mark family (508, 522, 526..537) plus 446; the handler reads the values `MyGuild = pMob[conn].MOB.Guild`, `OpponentGuild = pMob[OpponentID].MOB.Guild`, `MyGuildLevel`, `OpponentGuildLevel` once at `_MSG_Trade.cpp:140-143` and then evaluates the nested condition for each item.

The logic is an OR of two authorisation paths: the sender may move it if they are a leader of the owning guild, or the receiver may be a leader of the owning guild. `GuildLevel == 0` means "not a guild leader" in this codebase (the same field gates guild-master-only operations elsewhere), so a plain member of the owning guild still fails the test. When neither path passes, both sides receive `_NN_Only_With_Guild_Master` and the same conditional teardown as BR-11 applies.

The rule is evaluated on the sender's offered items only, exactly like BR-11, and it does not distinguish "item belongs to a third guild" from "item belongs to no guild": both decode to a guild id that matches neither party, so both are refused. Note that the comparison uses `pMob[].MOB.Guild`, i.e. the current guild membership, not the guild recorded in the item, so a leader of guild A cannot trade an item stamped with guild B even if they used to belong to B.

**Rule workflow**:
```
MyGuild, OpponentGuild, MyGuildLevel, OpponentGuildLevel = snapshot of both players
for i in 0..14:
  if m->Item[i].sIndex in {508, 522, 446, 526..537}:
      g = GetGuild(&m->Item[i])
      if not (g == MyGuild AND MyGuildLevel):
          if not (g == OpponentGuild AND OpponentGuildLevel):
              SendClientMessage(both, _NN_Only_With_Guild_Master)
              RemoveTrade(conn); conditional RemoveTrade(OpponentID); return
  -> continue to BR-13
```

---

### Business Rule: BR-13 — `EF_NOTRADE` items are non-tradable

**Overview**:
For each offered item with a non-zero `sIndex`, `BASE_GetItemAbility(&m->Item[i], EF_NOTRADE) != 0` aborts the trade with `_NN_Cant_MoveItem` on both clients and the same conditional teardown (`_MSG_Trade.cpp:179-189`).

**Detailed description**:
`EF_NOTRADE` is the item-template ability that marks an item as bound or otherwise untransferable. The check is template-driven (`BASE_GetItemAbility` reads `g_pItemList` for the item index, `Basedef.cpp:1497-1518` for the same accessor family), so the set of forbidden items is data-defined and can be extended without touching this handler. It is the broadest of the three item gates in this file and the one most likely to fire in normal gameplay.

Because the gate is the last in the loop, an item that is both guild-bound and `EF_NOTRADE` produces the guild message first. Since the loop returns on the first failing index, the diagnostic names the first offending slot only; a client offering several forbidden items learns nothing about the others.

The `_NN_Cant_MoveItem` string is shared with other inventory-transfer rejections, so the client cannot distinguish "bound item" from "item in a slot that cannot be moved" from the message alone. On the server side, no `Log` line is emitted for this rule, so only the client-visible message records the attempt.

**Rule workflow**:
```
for i in 0..14:
  if m->Item[i].sIndex AND BASE_GetItemAbility(&m->Item[i], EF_NOTRADE) != 0:
      SendClientMessage(both, _NN_Cant_MoveItem)
      RemoveTrade(conn); conditional RemoveTrade(OpponentID); return
  -> continue to BR-08 opponent-side check (line 192)
```

---

### Business Rule: BR-14 — Slot bounds for offered positions

**Overview**:
Every `InvenPos` must satisfy `0 <= pos < MAX_CARRY - 4` (= 60). The rule is enforced at `_MSG_Trade.cpp:60-61` (sender, first loop), `74-75` (counterpart, first loop) and `205-210` (sender, re-derivation).

**Detailed description**:
`MAX_CARRY` is 64 (`Basedef.h:76`) and the last four slots are structurally reserved: `Carry[60]` and `Carry[61]` hold the inventory-expansion tickets (item 3467) that raise `MaxCarry` from 30 to 45 to 60 (`ProcessSecMinTimer.cpp:571-597`), while slots 62 and 63 are spare. Excluding them from the trade window prevents a player from trading away the tickets that define their own capacity.

The two enforcement sites behave differently on failure. In the first loop, an out-of-range `InvenPos` produces a bare `return` (`_MSG_Trade.cpp:60-61`, `74-75`) with no teardown and no message. In the re-derivation loop, an out-of-range position produces `_NN_Wrong_Trade_Packet1` plus `RemoveTrade(conn)` and a `break` (`_MSG_Trade.cpp:205-210`), which leaves the opponent's session untouched. The same input can therefore produce two different outcomes depending on which loop sees it first, and the first loop always sees it first — so the re-derivation branch is only reachable for a position that becomes invalid between the two loops, which cannot happen in a single-threaded pass.

The bound is expressed against the constant `MAX_CARRY-4`, not against `pMob[conn].MaxCarry`. See TD-04 for the consequence: a slot in `[MaxCarry, 60)` is inside the accepted range but outside the player's active inventory.

**Rule workflow**:
```
if m->Item[i].sIndex != 0:
    if pos < 0 OR pos >= 60: return           // silent
if opponent stored Item[i].sIndex != 0:
    if pos < 0 OR pos >= 60: return           // silent
in re-derivation:
    if pos < -1 OR pos >= 60:
        _NN_Wrong_Trade_Packet1; RemoveTrade(conn); break
  -> continue to BR-15
```

---

### Business Rule: BR-15 — No duplicate inventory positions

**Overview**:
Within the re-derivation loop, each non-`-1` position is compared against all other entries of the same packet; a duplicate produces `_NN_Wrong_Trade_Packet1`, `RemoveTrade(conn)` and `return` (`_MSG_Trade.cpp:211-219`).

**Detailed description**:
The duplicate check is the only guard that keeps the offer a set rather than a multiset. Without it, a client could list the same inventory slot twice; at settlement `BASE_CanTrade` would clear that slot twice (`Basedef.cpp:4596-4605`) while the sender's `Trade.Item[]` would contain the item twice, giving the receiver two copies of one item — a direct duplication exploit. The check runs *after* provenance (BR-05), so both duplicates already matched the same real inventory item.

The comparison is `if (j != i && iPos == m->InvenPos[j])`, i.e. quadratic in `MAX_TRADE` (225 comparisons worst case) and performed on the raw `char` positions. It only inspects the sender's own array; the counterpart's duplicates are validated when the counterpart's packet is processed, which is guaranteed to happen because settlement requires both sides to send a `MyCheck == 1` packet.

The failure path clears only the sender (`RemoveTrade(conn)`) and returns immediately, unlike the provenance failures which clear both. Since a duplicate can only be produced by a malformed or forged packet, leaving the counterpart's window open is a deliberate leniency: the innocent party is not punished for the other side's malformed offer.

**Rule workflow**:
```
for i in 0..14:
  iPos = m->InvenPos[i]
  if iPos == -1: m->Item[i] = 0
  else:
      if iPos < -1 OR iPos >= 60: _NN_Wrong_Trade_Packet1; RemoveTrade(conn); break
      for j in 0..14: if j != i AND iPos == m->InvenPos[j]: error -> return
      m->Item[i] = pMob[conn].MOB.Carry[iPos]
  -> continue to BR-17
```

---

### Business Rule: BR-16 — Server-derived offer

**Overview**:
The handler replaces each packet item with the authoritative inventory content: `memcpy(&m->Item[i], &pMob[conn].MOB.Carry[iPos], sizeof(STRUCT_ITEM))` for `iPos != -1`, and `memset(&m->Item[i], 0, sizeof(STRUCT_ITEM))` for `iPos == -1` (`_MSG_Trade.cpp:196-223`).

**Detailed description**:
This is the pivotal anti-forgery rule: after BR-05 has proved the packet is honest, the packet's item bytes are discarded and rebuilt from the server's own inventory. Any subsequent consumer (the stored session, the forwarded packet, `BASE_CanTrade`, the audit log) therefore operates on server data only. The rule also gives `InvenPos == -1` a precise meaning — "this offer slot is empty" — which the client uses to clear a slot it previously filled.

The re-derivation happens *before* the `MyCheck` test and before the branch that stores or forwards the packet, so both the update path and the confirmation path operate on re-derived items. On the update path the re-derived `m` is `memcpy`-ed wholesale into `pUser[conn].Trade` (`_MSG_Trade.cpp:389`), which means the stored session's items are always exactly the inventory content at the time of the packet; that is what makes the whole-struct freeze check (BR-17) meaningful.

The rule is applied to every packet from every sender, so the counterpart's items are re-derived when the counterpart's own packet is processed. The handler never re-derives the counterpart's items from the sender's packet, which is why BR-06 exists as a separate live-inventory comparison.

**Rule workflow**:
```
for i in 0..14:
  iPos = m->InvenPos[i]
  if iPos == -1: clear m->Item[i]
  else: validate bounds and duplicates, then m->Item[i] = Carry[iPos]
  -> stored session / forwarded packet now carries server data
```

---

### Business Rule: BR-17 — A checked offer is frozen

**Overview**:
When `m->MyCheck == 1`, the handler sets `pUser[conn].Trade.MyCheck = 1`, copies the packet's transport fields (`KeyWord`, `CheckSum`, `ClientTick`) into the stored session, then requires `memcmp(&pUser[conn].Trade, m, sizeof(MSG_Trade)) == 0`; any difference clears both sessions and logs "err,msg_trade - Click check button but diffrent data" (`_MSG_Trade.cpp:225-238`).

**Detailed description**:
The comparison is a whole-struct byte comparison of a 156-byte `MSG_Trade` (12-byte `_MSG` header + 15 x 8-byte items + 15 position bytes + 4-byte money + 1-byte `MyCheck` + 2-byte `OpponentID`, default MSVC alignment; no `#pragma pack` applies at `Basedef.h:2047`). It therefore asserts that the confirmation packet is byte-identical to the session the server last stored — same items, same positions, same money, same check flag, same header — which is the strongest available proof that the client is confirming the offer it actually made rather than a substituted one.

The three transport fields are copied in first (`_MSG_Trade.cpp:228-230`) precisely because the transport layer rewrites them on every send and receive: `CPSock::Send` assigns a random `KeyWord`, a `CheckSum` and the current tick before obfuscating (`CPSock.cpp:535-541`), and the receive side validates the checksum against the obfuscated payload (`CPSock.cpp:424-458`). Without the copy, every confirmation would fail the `memcmp` because of the `KeyWord`/`CheckSum`/`ClientTick` bytes alone. Storing transport state in the domain object is the price of expressing the invariant as one comparison.

The rule also fixes the semantics of the check flag: `MyCheck` is a per-side latch that can only be set while the offer is unchanged, and it can be cleared by the other side's update packet (BR-26). If the comparison fails, both sides are torn down and the event is logged under the sender's identity, since only the sender can have sent a divergent confirmation.

**Rule workflow**:
```
if m->MyCheck == 1:
    pUser[conn].Trade.MyCheck   = 1
    pUser[conn].Trade.KeyWord   = m->KeyWord
    pUser[conn].Trade.CheckSum  = m->CheckSum
    pUser[conn].Trade.ClientTick= m->ClientTick
    if memcmp(&pUser[conn].Trade, m, sizeof(MSG_Trade)) != 0:
        RemoveTrade(conn); RemoveTrade(OpponentID)
        Log "err,msg_trade - Click check button but diffrent data"
        return
  -> continue to BR-18
```

---

### Business Rule: BR-18 — Two-sided confirmation protocol

**Overview**:
After a successful freeze check, if `pUser[OpponentID].Trade.MyCheck == 0` the handler sends `_MSG_CNFCheck` to the sender (`SendClientSignal(conn, conn, _MSG_CNFCheck)`, type 134), forwards the packet to the opponent with the identity fields swapped (`m->ID = OpponentID; m->OpponentID = conn`), and returns without settling (`_MSG_Trade.cpp:240-249`). Settlement only executes on the packet that arrives when the opponent is already checked.

**Detailed description**:
The protocol is a classic two-phase "ready/ready" handshake collapsed into a single message type. Phase one is the set of `MyCheck == 0` updates that move the offer; phase two is one `MyCheck == 1` per side. The *second* confirmer's packet is the one that executes the swap, because at that moment both latches are set: the sender's was set at `_MSG_Trade.cpp:227` and the opponent's was persisted earlier. There is no third message and no commit token; the settlement is triggered purely by latch state.

`_MSG_CNFCheck` (id 134, `Basedef.h:2060`) is emitted only to the *already-checked* sender, addressed to itself (`SendClientSignal(conn, conn, ...)` builds a `MSG_STANDARD` with `Type = 134`, `ID = conn`, `SendFunc.cpp:197-206`). Its role is to tell that client "your check was recorded, waiting for the other side". The other side learns about the check by receiving the forwarded `_MSG_Trade` itself, whose `MyCheck` byte is 1.

Two consequences follow. First, the settlement is executed inside the same function invocation that processed the second confirmation, so both inventories and both coin fields are read and written within one non-preemptible Win32 message-loop callback — there is no window for a third packet to interleave, which is why the handler needs no locks (TMSrv is single-threaded; see TD-08 for the caveat about DB persistence). Second, because nothing is committed on the first confirmation, a player who confirms and then changes the offer (BR-26) un-checks both sides without needing a cancel message: the counterpart's latch is zeroed by the update packet.

**Rule workflow**:
```
if pUser[OpponentID].Trade.MyCheck == 0:
    SendClientSignal(conn, conn, _MSG_CNFCheck)
    m->ID = OpponentID; m->OpponentID = conn
    pUser[OpponentID].cSock.AddMessage(m, sizeof(MSG_Trade))
    return
// both latches set -> BR-19 settlement
```

---

### Business Rule: BR-19 — 2G ceiling on offered money

**Overview**:
At settlement, each side's stored `TradeMoney` must be in `[0, 2,000,000,000]`; violations clear both sessions silently (`_MSG_Trade.cpp:251-265`).

**Detailed description**:
The ceiling is the server-wide coin cap of 2,000,000,000 (below `INT_MAX` by 147,483,647, leaving headroom for additions). `_MSG_Sell.cpp:218` and `_MSG_UseItem.cpp:2275` clamp coin to the same value, and `_MSG_SendAutoTrade.cpp:75` rejects auto-trade prices above 1999999999, so 2G is the project-wide economic ceiling. The rule reads the *stored* session values (`pUser[conn].Trade.TradeMoney`), not the packet, because BR-17 already proved the packet equals the stored session when `MyCheck == 1`.

The check is duplicated for both sides rather than factored into a loop, and both failures execute the same silent teardown. "Silent" here means no client message and no `Log`: the players see the window close with no explanation. Given that BR-04 already bounds the money to the sender's own coin at every packet, a negative or over-2G stored value can only arise from a corrupted session or an arithmetic path outside this handler.

The order matters: this check precedes the resulting-balance arithmetic (BR-20) and the affordability check (BR-21), so an out-of-range input is rejected before it can influence `fGold`/`opfGold`. It also precedes `BASE_CanTrade`, so no inventory work is done for an invalid amount.

**Rule workflow**:
```
if pUser[conn].Trade.TradeMoney < 0 OR > 2e9: RemoveTrade both; return
if pUser[OpponentID].Trade.TradeMoney < 0 OR > 2e9: RemoveTrade both; return
  -> continue to BR-20
```

---

### Business Rule: BR-20 — Resulting balance must stay within bounds

**Overview**:
The handler computes `fGold = OpponentMoney + pMob[conn].MOB.Coin - MyMoney` and `opfGold = MyMoney + pMob[OpponentID].MOB.Coin - OpponentMoney` as `unsigned int`, and requires each to be `<= 2,000,000,000` and not "negative" (`_MSG_Trade.cpp:267-285`).

**Detailed description**:
This is the net-transfer computation: each player's new coin is "my current coin, minus what I give, plus what I receive". Both expressions are evaluated in `unsigned int` because the destination variables are declared unsigned, so the `fGold < 0` half of the test is always false on this compiler and the real guard is the `> 2,000,000,000` comparison, which also catches underflow (a negative mathematical result wraps to a value near `2^32`, far above 2G). The two-part test is therefore correct in effect but expressed redundantly.

On failure the handler sends `_NN_Cant_get_more_than_2G` to the *sender only*, clears both sessions, and returns (`_MSG_Trade.cpp:269-275`, `277-285`). The counterpart is not told why, and no `Log` line is written.

The arithmetic is read from `pMob[].MOB.Coin` at settlement time, not from the values used when the offers were made, so any coin change between offer and confirmation is reflected in the result. Because coin can only change through handlers that close the trade window first (BR-06 and the action sweep), this is normally a no-op re-read; it becomes meaningful if a coin mutation path exists that does not clear the session.

**Rule workflow**:
```
fGold   = OppMoney   + conn.Coin - MyMoney            // unsigned
if fGold > 2e9: _NN_Cant_get_more_than_2G (conn only); RemoveTrade both; return
opfGold = MyMoney    + opp.Coin  - OppMoney           // unsigned
if opfGold > 2e9: _NN_Cant_get_more_than_2G (conn only); RemoveTrade both; return
  -> continue to BR-21
```

---

### Business Rule: BR-21 — Affordability re-verification

**Overview**:
Each side's stored `TradeMoney` must not exceed their current `MOB.Coin` (`_MSG_Trade.cpp:287-299`). The failure messages are asymmetric: the sender gets `_NN_Havent_Money_So_Much` and the counterpart gets `_NN_Opponent_Havent_Money` (or the reverse when the counterpart is short).

**Detailed description**:
This rule is the settlement-time counterpart of BR-04. The two messages encode perspective: the short party reads "you do not have that much money", the other party reads "your opponent does not have that much money". Both messages are pushed before the `return`, and — unlike most other settlement failures — this branch does **not** call `RemoveTrade` and does **not** clear `MyCheck`, so both sessions remain in the "both confirmed" state.

That omission is the most significant behavioural asymmetry in the handler. A confirmed trade that fails affordability can be re-submitted by either side with the same packet and will fail again identically, producing one pair of chat messages per attempt. Because nothing is written, no economic damage results; the window simply becomes a stuck dialog that only the client, a cancel (`_MSG_QuitTrade`), a conflicting action, or a disconnect can clear.

The check reads live `pMob[].MOB.Coin` while the earlier BR-20 arithmetic also read it, so the two are consistent within the same callback. A player cannot spend money between BR-20 and BR-21 because both run inside one function body in a single-threaded loop.

**Rule workflow**:
```
if MyMoney > conn.Coin:
    SendClientMessage(conn, _NN_Havent_Money_So_Much)
    SendClientMessage(OpponentID, _NN_Opponent_Havent_Money)
    return                                  // session kept, latches kept
if OppMoney > opp.Coin:
    SendClientMessage(OpponentID, _NN_Havent_Money_So_Much)
    SendClientMessage(conn, _NN_Opponent_Havent_Money)
    return                                  // session kept, latches kept
  -> continue to BR-22
```

---

### Business Rule: BR-22 — Both sides need capacity for incoming items

**Overview**:
`BASE_CanTrade` is run twice — once per side — into stack arrays `mydest[MAX_CARRY]` and `oppdest[MAX_CARRY]`; the return values are combined into three distinct outcomes (`_MSG_Trade.cpp:301-326`): both fail → `_NN_Each_Of_You_Havent_Space` to both; sender fails → `_NN_You_Have_No_Space_To_Trade` + `_NN_Opponent_Have_No_Space_To`; opponent fails → the mirrored pair.

**Detailed description**:
`BASE_CanTrade(Dest, Carry, MyTrade, OpponentTrade, MaxCarry)` (`Basedef.cpp:4588-4635`) is a dry-run: it copies the player's `Carry[]` into `Dest`, clears every slot named in `MyTrade` (the positions the player is giving away, treating both `-1` and `255` as "empty"), then attempts to place every non-empty `OpponentTrade` item into the first free slot below `MaxCarry`, returning `FALSE` if any item cannot be placed. It also sorts the incoming items by ability 33 via `BASE_SortTradeItem` (`Basedef.cpp:4539`) so that placement order is deterministic.

The three-way message split (`mytrade == 0 && opptrade == 0`, `mytrade == 0`, `opptrade == 0`) exists because the client UI can only render a generic "not enough space" message; the server chooses the wording that matches the actual failing side, in each player's perspective. All three branches `return` without teardown and without clearing `MyCheck`, so a space failure also leaves the session latched — the same stuck-dialog condition described in BR-21.

The capacity test uses `pMob[].MaxCarry` (30/45/60) while the accepted `InvenPos` range is `[0, 59]` (BR-14). An item in a slot at or above `MaxCarry` can therefore be given away but the resulting layout is computed only over `[0, MaxCarry)`. This is the coupling behind TD-04.

**Rule workflow**:
```
mytrade  = BASE_CanTrade(mydest,  conn.Carry, connTrade.InvenPos, oppTrade.Item,  conn.MaxCarry)
opptrade = BASE_CanTrade(oppdest, opp.Carry,  oppTrade.InvenPos,  connTrade.Item, opp.MaxCarry)
if !mytrade AND !opptrade: _NN_Each_Of_You_Havent_Space x2; return
if !mytrade:               _NN_You_Have_No_Space_To_Trade / _NN_Opponent_Have_No_Space_To; return
if !opptrade:              mirrored; return
  -> continue to BR-23
```

---

### Business Rule: BR-23 — All-or-nothing commit

**Overview**:
Only after both dry-runs succeed does the handler commit: `memcpy(pMob[conn].MOB.Carry, mydest)` and `memcpy(pMob[OpponentID].MOB.Carry, oppdest)`, then `pMob[conn].MOB.Coin = fGold` and `pMob[OpponentID].MOB.Coin = opfGold`, then `SendCarry` to both (`_MSG_Trade.cpp:328-335`).

**Detailed description**:
The commit is four writes plus two notifications, executed with no intervening client interaction. `mydest`/`oppdest` are locals that were built from copies of `Carry[]`, so until the commit succeeds the authoritative inventories are untouched; a failure in BR-22 leaves both inventories byte-identical to their pre-trade state. `SendCarry` (`SendFunc.cpp:1534-1558`) pushes `MSG_UpdateCarry` containing the full 64-slot `Carry[]` and `MOB.Coin`, and closes the user if the send buffer rejects the message, which is the mechanism that resolves the client's optimistic view.

The two `memcpy`s use `sizeof(mydest)` = `sizeof(oppdest)` = `MAX_CARRY * sizeof(STRUCT_ITEM)` = 512 bytes each, matching the `Carry` array size, so the copy is exact. The coin assignments take `unsigned int` sources into `int` fields; values are guaranteed `<= 2e9` by BR-20, which is representable in `int`.

There is no rollback path and no journal: if `SaveUser` later fails (BR-25), the in-memory exchange has already happened and the clients have already been told. The complete absence of intermediate state is what makes the operation atomic from the game's point of view, and it is also why the same function must re-validate everything on entry rather than relying on state recorded at offer time.

**Rule workflow**:
```
memcpy(conn.Carry,  mydest,  512)
memcpy(opp.Carry,   oppdest, 512)
conn.Coin = fGold
opp.Coin  = opfGold
SendCarry(conn); SendCarry(OpponentID)
  -> continue to BR-24
```

---

### Business Rule: BR-24 — Four-record audit trail

**Overview**:
A completed trade writes four `ItemLog` entries: "Troca entre A e B foi iniciada." (system), a payload line for the initiator (`trade,conta:<opponent> coin:<myMoney>` plus each item code), a payload line for the opponent, and "Troca entre A e B foi finalizada." (system) (`_MSG_Trade.cpp:340-377`).

**Detailed description**:
Each payload line is built in the global `char temp[4096]` (`Server.h:382`) with `sprintf` for the header and `strcat` for each item code produced by `BASE_GetItemCode` (`Basedef.cpp:1479-1495`), which formats `" <name> : <index>.<e0>.<v0>.<e1>.<v1>.<e2>.<v2>"`. The per-item scratch buffer is `char tmplog[2024]` declared inside the loop, reused for each iteration. Up to 15 items are concatenated, so the worst-case line is bounded by the item-name length times 15 plus fixed text; with `temp` at 4096 bytes this is safe for realistic item names but not proven for adversarial data (see TD-06).

The two payload lines are attributed to different accounts: the first uses `pUser[conn].AccountName`/`IP` but describes `pUser[conn].Trade.Item[]` with the *opponent's* account name in the text (`trade,conta:%s` uses `pUser[OpponentID].AccountName` with `MyTradeMoney`). This mirroring makes the pair of records read as "what each player gave and received", at the cost of the account attribution in the log line being the counterpart's while the `ItemLog` owner is the sender's.

The start and end markers are written under the pseudo-account `-system`, which allows a transaction to be reconstructed from the log by bracketing. All four entries are written *before* `SaveUser` (BR-25) and *before* `RemoveTrade` (BR-28), so a crash between logging and teardown still leaves a complete audit record of an exchange that did happen in memory.

**Rule workflow**:
```
ItemLog("Troca entre <opp> e <me> foi iniciada.", "-system", 0)
sprintf(temp, "trade,conta:<opp> coin:<myMoney>") + item codes of conn  -> ItemLog(conn)
sprintf(temp, "trade,conta:<me>  coin:<oppMoney>") + item codes of opp  -> ItemLog(opp)
ItemLog("Troca entre <opp> e <me> foi finalizada.", "-system", 0)
  -> continue to BR-25
```

---

### Business Rule: BR-25 — Immediate persistence of both characters

**Overview**:
`SaveUser(conn, 1)` and `SaveUser(OpponentID, 1)` are called immediately after the audit block (`_MSG_Trade.cpp:379-380`), before teardown. `SaveUser` (`Server.cpp:7056-7083`) sends a `MSG_DBSaveMob` to DBSrv containing the full `STRUCT_MOB` (which includes `Coin` and `Carry[]`), the `Cargo[]` array, the account coin, slot, donate flag, account name, short-skill table, affects and extra score.

**Detailed description**:
The `Export = 1` argument marks the save as an export/immediate write, in contrast to the periodic save path. Because the exchange is a two-party operation, two independent messages are queued to DBSrv; there is no atomic multi-character transaction, no transaction id and no acknowledgement wait. If DBSrv persists only one of them (crash, disconnect, disk error), the surviving record is a post-trade state for one character and a pre-trade state for the other, which on the next login materialises as duplicated items or vanished items depending on which side persisted.

The save is issued after the items and coins have already been written to memory and after `SendCarry` has told both clients the new state, so the client's view is never rolled back by a persistence failure. The handler does not inspect any return value (the send is fire-and-forget), and `SaveUser` itself returns `void`.

Persistence ordering also matters for the logging story: the audit lines (BR-24) are written before the save messages are queued, so the log is the primary evidence of intent and the DB is the evidence of effect. On restart, the log can show a completed trade whose effect was only partially persisted.

**Rule workflow**:
```
SaveUser(conn, 1)          // MSG_DBSaveMob queued to DBSrv
SaveUser(OpponentID, 1)    // independent message, no atomicity
  -> continue to BR-28 teardown
```

---

### Business Rule: BR-26 — Update packets un-check both sides

**Overview**:
On the update path (`m->MyCheck != 1`), after storing the re-derived packet the handler forces `pUser[conn].Trade.MyCheck = 0` and `pUser[OpponentID].Trade.MyCheck = 0` (`_MSG_Trade.cpp:389-392`).

**Detailed description**:
This rule implements "editing the offer cancels both confirmations". The sender's latch is cleared trivially (the packet carried 0), but the counterpart's latch is cleared *without the counterpart's involvement*, which is the server's way of preventing a player from being committed to an older offer while the other side keeps editing. The counterpart's client is informed because the re-derived packet is forwarded with `MyCheck = 0` (`_MSG_Trade.cpp:394-397`), which the client interprets as "the offer changed, un-check".

Because the forward happens after the copy, the counterpart receives the re-derived items (BR-16), not the raw packet. The forward uses `pUser[OpponentID].cSock.AddMessage(...)` whose boolean result is not checked here, unlike `SendCarry` which closes the user on failure (`SendFunc.cpp:1557-1558`). A send-buffer failure therefore silently desynchronises the two clients: the server believes the offer is updated, the counterpart never sees it.

The rule is what allows a legal negotiation loop: fill items → check → other side edits → both un-checked → fill again. It is also the mechanism by which an attacker can indefinitely reset the other side's confirmation by sending trivial updates, a nuisance-level griefing vector (TD-07).

**Rule workflow**:
```
memcpy(&pUser[conn].Trade, m, sizeof(MSG_Trade))
pUser[conn].Trade.MyCheck = 0
pUser[OpponentID].Trade.MyCheck = 0
m->ID = OpponentID; m->OpponentID = conn
pUser[OpponentID].cSock.AddMessage(m, sizeof(MSG_Trade))    // result ignored
  -> end of handler
```

---

### Business Rule: BR-27 — Invitations start empty

**Overview**:
When neither side has a session, the handler stores the packet, forces `MyCheck = 0` and `TradeMoney = 0` locally, swaps identities, forwards, and then forces the opponent's `MyCheck = 0` and `TradeMoney = 0` (`_MSG_Trade.cpp:409-424`).

**Detailed description**:
The invitation is deliberately money-free. Any coin amount present in the invitation packet is discarded on both sides, so the first real amount must arrive in a subsequent update packet, where it is subject to BR-04, BR-07 and the settlement checks. This removes a class of "invite with an amount the other player never saw" scams and makes the offer UI symmetric: both players start at zero coin and add explicitly.

Note that the initiator's stored session *is* populated (`memcpy(&pUser[conn].Trade, m, ...)` at line 411) while the opponent's session is *not* populated — only its `MyCheck` and `TradeMoney` fields are written (`_MSG_Trade.cpp:422-423`). The opponent's `OpponentID` and `Item[]` remain zero until the opponent's own packet arrives (BR-08). This asymmetry is why the pairing test at line 194 (`pUser[OpponentID].Trade.OpponentID == conn`) fails until the opponent answers, and why the invite path is only taken when `pUser[OpponentID].Trade.OpponentID == 0`.

The forwarded packet has already had its identity fields swapped (`m->ID = OpponentID`, `m->OpponentID = conn`), which is the convention every forward in this handler uses. It means the `ID` field arriving at a client always names that client, and `OpponentID` always names the peer, so the client can validate the packet against its own session without extra state.

**Rule workflow**:
```
memcpy(&pUser[conn].Trade, m, sizeof(MSG_Trade))
pUser[conn].Trade.MyCheck = 0
pUser[conn].Trade.TradeMoney = 0
m->ID = OpponentID; m->OpponentID = conn
pUser[OpponentID].cSock.AddMessage(m, sizeof(MSG_Trade))
pUser[OpponentID].Trade.MyCheck = 0
pUser[OpponentID].Trade.TradeMoney = 0
```

---

### Business Rule: BR-28 — Teardown contract of `RemoveTrade`

**Overview**:
`RemoveTrade(conn)` (`Server.cpp:7319-7352`) is the single canonical reset. It zeroes the whole `MSG_Trade`, re-arms every `InvenPos` to `-1`, zeroes `AutoTrade` and re-arms every `CarryPos` to `-1`; if the user is in play it also sends signal 900 to the client and, when `TradeMode` is set, restores the player body with `GetCreateMob` + `GridMulticast` and clears `TradeMode`.

**Detailed description**:
The two `-1` re-arming loops are not cosmetic: `BASE_CanTrade` treats `-1` and `255` as "empty slot" (`Basedef.cpp:4600`), and the handler's re-derivation treats `-1` as "clear this offer slot" (`_MSG_Trade.cpp:200-201`). Without re-arming, a zeroed session would read as fifteen offers of inventory slot 0. `memset(..., 0, sizeof(MSG_Trade))` alone would therefore be a bug; the loops are the contract.

The function is slot-scoped and re-entrant-safe by construction: it guards `conn <= 0 || conn >= MAX_USER`, never dereferences the partner, and never follows `Trade.OpponentID`. That is what allows the 91 call sites across 27 TMSrv files to clear a partner without recursion, and what makes the paired calls (`RemoveTrade(opp)` then `RemoveTrade(conn)`) safe even when the pairing is one-sided.

Signal 900 is sent for any in-play user, whether or not a session existed; it is the client's "close trade window" instruction. The `TradeMode` block handles the auto-trade shop: an open player-run shop is represented by a mob body created for display, so tearing down the shop requires re-broadcasting the real player mob (`GetCreateMob`, `GridMulticast`) and clearing the flag that gates auto-trade (`_MSG_SendAutoTrade.cpp:105`). `Mode != USER_PLAY` short-circuits before both, so a disconnecting user is reset silently — which is exactly what `CloseUser` needs.

**Rule workflow**:
```
RemoveTrade(conn):
  if conn invalid: return
  Trade = 0; Trade.InvenPos[*] = -1
  AutoTrade = 0; AutoTrade.CarryPos[*] = -1
  if Mode != USER_PLAY: return
  SendClientSignal(conn, conn, 900)
  if TradeMode:
      GetCreateMob(conn, &sm); GridMulticast(x, y, sm)
      TradeMode = 0
```

---

### Business Rule: BR-29 — Disconnect and logout must clear the partner

**Overview**:
Both the SaveAndQuit path (`Server.cpp:6871-6878`) and `CharLogOut` (`Server.cpp:7104-7110`) read `Trade.OpponentID`, and clear the partner's session only when the partner is `USER_PLAY` **and** the partner's session points back at the disconnecting user. `CloseUser` then sets `pUser[conn].Trade.OpponentID = 0`.

**Detailed description**:
The mutuality test is the important part: `pUser[trade].Mode == USER_PLAY && pUser[trade].Trade.OpponentID == conn`. Without it, following a stale `OpponentID` could destroy a session that the named player has since opened with somebody else. The trade handler's own branches repeat this test before touching a partner (`_MSG_Trade.cpp:154`, `171`, `185`, `194`), so the invariant "only clear a partner that points back" is consistent across the whole family.

The disconnecting user's own session is not fully reset by these paths — `CloseUser` only zeroes `OpponentID` after `RemoveParty`; the item array, money and `MyCheck` remain until the slot is reused. That is acceptable because the slot's `Mode` becomes `USER_EMPTY`/`USER_SAVING4QUIT` and every handler gates on `USER_PLAY` before touching `Trade` (BR-01), and because `pUser[conn].CloseUser()` clears the account name and socket.

A hard socket drop reaches the same code through `CloseUser(User)` (`Server.cpp:3934`, `3952`, `3965`, `4027`, `4036`). The `WSA_READ`/`FD_CLOSE` path calls `CloseUser` directly, so an abrupt disconnect is handled identically to a graceful logout as far as trade cleanup is concerned.

**Rule workflow**:
```
tradeOpponentID = pUser[conn].Trade.OpponentID
if tradeOpponentID in (0, MAX_USER) AND pUser[tradeOpponentID].Mode == USER_PLAY
   AND pUser[tradeOpponentID].Trade.OpponentID == conn:
       RemoveTrade(tradeOpponentID)
pUser[conn].Trade.OpponentID = 0
```

---

### Business Rule: BR-30 — Declared packet-size contract is inert

**Overview**:
`Code/Basedef.cpp:6535` declares that `_MSG_Trade` must be exactly `sizeof(MSG_Trade)`, and `6536`/`6538` do the same for `_MSG_QuitTrade` and `_MSG_CNFCheck`. The whole validation body is commented out from line 6476 to line 6594 and the function ends with `return FALSE;` (line 6595), so no caller can ever observe a size violation.

**Detailed description**:
The only caller in the client path is inside `#ifdef _PACKET_DEBUG` in `CPSock::AddMessage` (`CPSock.cpp:544-552`), which logs a `**PACKET_DEBUG**` line and then obfuscates and sends the message anyway; the TMSrv callers at `Server.cpp:3898` and `3983` are likewise debug-only and, with `return FALSE`, always fall through. Consequently the handler's `(MSG_Trade*)pMsg` cast is applied to a buffer whose declared `Size` is unchecked at the message-type level; only the transport-level bounds (`Size <= MAX_MESSAGE_SIZE` = 8192, `Size >= sizeof(HEADER)`, `Size <= received bytes`, `CPSock.cpp:386-411`) apply.

Because `CPSock` reads into a fixed `RECV_BUFFER_SIZE` of 128 KB (`CPSock.h:35`), a short `_MSG_Trade` causes the handler to read stale bytes from earlier packets still present in that buffer rather than out-of-allocation memory. The values read are then subjected to every rule in this document, so a truncated packet fails provenance, bounds, or the freeze check long before anything is written.

The practical consequence is that the declared contract is documentation, not enforcement: readers of `Basedef.cpp:6535` will believe `_MSG_Trade` is size-checked, and the report records that it is not. The same is true for every other message type in that function, so this is a project-wide condition rather than a trade-specific one.

**Rule workflow**:
```
CPSock::ReadMessage -> transport bounds only
ProcessClientMessage -> no per-type size check (BASE_CheckPacket always FALSE)
Exec_MSG_Trade -> casts pMsg to MSG_Trade and trusts field layout
```

---

## 4. Component Structure

```
Code/TMSrv/
├── _MSG_Trade.cpp                 # THE COMPONENT: Exec_MSG_Trade (425 lines, single function)
│                                  #   lines  20-30   session gate (HP / Mode)
│                                  #   lines  32-54   target validation (range, mode, money)
│                                  #   lines  56-85   escrow provenance, both sides
│                                  #   lines  87-118  partner pinning / offer stability
│                                  #   lines 120-138  PK mode + whisper gates
│                                  #   lines 140-190  item-class gates (crown, guild, NOTRADE)
│                                  #   lines 192-223  pair check + server re-derivation
│                                  #   lines 225-238  check freeze (whole-struct memcmp)
│                                  #   lines 240-249  first-confirm echo
│                                  #   lines 251-326  settlement validation (money/space)
│                                  #   lines 328-335  commit + client resync
│                                  #   lines 337-377  audit logging
│                                  #   lines 379-384  persistence + teardown
│                                  #   lines 387-399  offer update path
│                                  #   lines 402-407  opponent busy
│                                  #   lines 409-424  invitation path
│
├── _MSG_TradingItem.cpp           # SIBLING: same-Carry item rearrangement (outside head)
├── _MSG_QuitTrade.cpp             # CANCEL: removes both sessions, then PK-info broadcast
├── _MSG_SendAutoTrade.cpp         # SIBLING: player shop creation (refuses while trading)
├── _MSG_ReqTradeList.cpp          # SIBLING: browse a player shop (outside this boundary)
├── ProcessClientMessage.h:111     # AFFERENT CONTRACT: void Exec_MSG_Trade(int, char*)
├── ProcessClientMessage.cpp:234   # AFFERENT ROUTE: case _MSG_Trade -> Exec_MSG_Trade
├── CUser.h:59                     # STATE HOME: MSG_Trade Trade; (per-session copy)
├── CUser.h:46                     # MSG_Trade/TradeMode: auto-trade flag
├── CUser.h:65                     # MSG_SendAutoTrade AutoTrade
├── Server.cpp:7319-7352           # RemoveTrade: canonical teardown (called 91x in 27 files)
├── Server.cpp:684-707             # AddCrackError: crack counter / force logout
├── Server.cpp:7056-7083           # SaveUser: persistence projection
│
Code/Basedef.h:2046-2057           # MSG_Trade layout (no pack pragma)
Code/Basedef.h:925-930             # _MSG macro (Size, KeyWord, CheckSum, Type, ID, ClientTick)
Code/Basedef.h:75-81               # MAX_EQUIP 16, MAX_CARRY 64, MAX_CARGO 128, MAX_TRADE 15,
                                   #   MAX_AUTOTRADE 12, MAX_AUTOTRADETITLE 24
Code/Basedef.cpp:4588-4635         # BASE_CanTrade (dry-run placement)
Code/Basedef.cpp:4539-4586         # BASE_SortTradeItem (ability-33 ordering of incoming items)
Code/Basedef.cpp:1479-1495         # BASE_GetItemCode (audit formatting)
Code/Basedef.cpp:6475-6596         # BASE_CheckPacket (disabled)
```

`MSG_Trade` layout (`Code/Basedef.h:2047-2057`), with `_MSG` expanded (`Basedef.h:925-930`):

| Offset | Size | Field | Notes |
|--------|------|-------|-------|
| 0 | 2 | `Size` | Transport frame length |
| 2 | 1 | `KeyWord` | Obfuscation key index, rewritten per send |
| 3 | 1 | `CheckSum` | Transport checksum, rewritten per send |
| 4 | 2 | `Type` | 131 (`_MSG_Trade`) |
| 6 | 2 | `ID` | Always the receiving client (swapped on forward) |
| 8 | 4 | `ClientTick` | Rewritten per send |
| 12 | 15 x 8 = 120 | `Item[MAX_TRADE]` | `STRUCT_ITEM` = sIndex + 3 x {effect, value} |
| 132 | 15 | `InvenPos[MAX_TRADE]` | `char`, `-1` = empty, 0..59 valid |
| 148 | 4 | `TradeMoney` | Character coin offered |
| 152 | 1 | `MyCheck` | Per-side confirmation latch |
| 154 | 2 | `OpponentID` | Peer session index (swapped on forward) |
| 156 | — | total | Default MSVC alignment; no `#pragma pack` active |

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (outbound)

```
Exec_MSG_Trade
  ├─ ProcessClientMessage.h            (declaration contract, header aggregation)
  ├─ pUser[conn] / pUser[OpponentID]   (CUser session state; Trade, AutoTrade, Mode, PKMode,
  │                                     Whisper, cSock, AccountName, IP, Cargo)
  ├─ pMob[conn] / pMob[OpponentID]     (CMob world state; MOB.CurrentScore.Hp, MOB.Coin,
  │                                     MOB.Carry, MOB.Guild, MOB.GuildLevel, MaxCarry, TargetX/Y)
  ├─ RemoveTrade()                     (Server.cpp:7319) — teardown, 17 call sites in this file
  ├─ AddCrackError()                   (Server.cpp:684)  — anti-automation counter
  ├─ SendHpMode()                      (SendFunc.cpp:1622)
  ├─ SendClientSignal()                (SendFunc.cpp:197) — signal 900 / _MSG_CNFCheck
  ├─ SendClientMessage()               (SendFunc family)  — message-string table lookups
  ├─ SendCarry()                       (SendFunc.cpp:1534) — full inventory + coin resync
  ├─ SaveUser()                        (Server.cpp:7056)  — DBSrv persistence projection
  ├─ GetGuild()                        (GetFunc.cpp:1869) -> BASE_GetGuild() (Basedef.cpp:4207)
  ├─ BASE_GetItemAbility()             (Basedef.cpp)      — EF_NOTRADE
  ├─ BASE_CanTrade()                   (Basedef.cpp:4588) -> BASE_SortTradeItem, BASE_ClearItem
  ├─ BASE_GetItemCode()                (Basedef.cpp:1479) — audit formatting
  ├─ ItemLog() / Log()                 (Server.cpp logging)
  └─ g_pMessageStringTable[]           (_NN_* indices, Language.h)
```

Compile-time includes: only `ProcessClientMessage.h` (`_MSG_Trade.cpp:18`), which pulls `Basedef.h`, `CPSock.h`, `Server.h`, `CUser.h`, `SendFunc.h`, `GetFunc.h` and the subsystem headers.

### 5.2 External dependencies

| Dependency | Type | Purpose in this component | Notes |
|---|---|---|---|
| Winsock TCP (`CPSock`) | Transport | Framing, obfuscation, checksum, buffering | `pUser[].cSock.AddMessage()` writes; `CPSock.cpp:535-552` rewrites `KeyWord`/`CheckSum`/`ClientTick` |
| DBSrv (`MSG_DBSaveMob`) | Process | Persist items + coin of both characters | Fire-and-forget, no ack, no two-phase commit (`Server.cpp:7056-7083`) |
| Item log files (`ItemLog`) | File I/O | Economic audit trail | 4 records per completed trade |
| Message string table (`g_pMessageStringTable`) | Data file | All client-facing rejections | Loaded via `BASE_InitializeMessage` (`Basedef.cpp:4637`) |
| Item template table (`g_pItemList`) | Data file | `EF_NOTRADE`, item names for logs | `BASE_GetItemAbility`, `BASE_GetItemCode` |

### 5.3 Dependency direction summary

- Inbound: exactly one edge — `ProcessClientMessage.cpp:234` (matching the architecture report's afferent coupling of 1).
- Outbound: the 14 symbols counted by the architecture report (`RemoveTrade`, `AddCrackError`, `SendHpMode`, `SendClientMessage`, `SendClientSignal`, `SendCarry`, `SaveUser`, `ItemLog`, `Log`, `GetGuild`, `BASE_GetItemAbility`, `BASE_CanTrade`, `BASE_GetItemCode`, plus the `CPSock::AddMessage` edge).
- Reverse edge: `RemoveTrade` is *owned* by Server.cpp but 17 of its 91 call sites live in this file, so the component is simultaneously a heavy consumer of Server Core and a heavy contributor to its call volume.

---

## 6. Afferent and Efferent Coupling

Units are C-style free functions and plain structs (no classes in this component except `CUser`/`CMob` which are consumed, not defined).

| Component / symbol | Afferent (Ca) | Efferent (Ce) | Instability Ce/(Ca+Ce) | Criticality |
|---|---|---|---|---|
| `Exec_MSG_Trade` (function) | 1 (`ProcessClientMessage.cpp:235`) | 14 | 0.93 | Critical (sole settlement authority for player trade) |
| `MSG_Trade` (struct, `Basedef.h:2047`) | 5 (`CUser.h:59`, `Server.cpp:7324`, `Basedef.cpp:6535`, `_MSG_Trade.cpp` x3 explicit uses) | 1 (`_MSG` macro) | 0.17 | Critical (binary protocol contract + session storage) |
| `Trade` field of `CUser` (`CUser.h:59`) | 27 files read/write `.Trade.*` | 0 | 0.00 | Critical (shared mutable session state) |
| `RemoveTrade` (`Server.cpp:7319`, consumed here) | 91 call sites / 27 files | 4 (`memset`, `SendClientSignal`, `GetCreateMob`, `GridMulticast`) | 0.04 | Critical (global teardown primitive) |
| `AddCrackError` (`Server.cpp:684`) | ~40 call sites | 3 (`Log`, `SendClientMessage`, `CharLogOut`) | 0.07 | High |
| `BASE_CanTrade` (`Basedef.cpp:4588`) | 2 (both in `_MSG_Trade.cpp`) | 3 (`BASE_ClearItem`, `BASE_SortTradeItem`, `memcpy`) | 0.60 | High (settlement feasibility oracle) |
| `SaveUser` (`Server.cpp:7056`) | ~15 call sites | 1 (`MSG_DBSaveMob` send) | 0.06 | High (durability boundary) |
| `SendCarry` (`SendFunc.cpp:1534`) | ~25 call sites | 2 (`AddMessage`, `CloseUser`) | 0.07 | High (client resync) |
| `_MSG_Trade` message id (`Basedef.h:2046`) | 4 (`Basedef.cpp`, `ProcessClientMessage.cpp`, `_MSG_Trade.cpp`, protocol flags) | 0 | 0.00 | Critical (wire contract) |

Interpretation: the handler function itself is maximally unstable (it depends on everything, nothing depends on it) — the correct shape for a Command Handler. The instability inverts for the state it touches: `CUser::Trade` and `RemoveTrade` are stable, heavily-depended-upon primitives, so any change to the teardown contract would ripple across 27 files; correspondingly, the component contributes 17 of `RemoveTrade`'s 91 call sites, making it the largest single consumer of that primitive.

Cohesion within the component is high by feature (one message, one lifecycle) but low by responsibility: the same function performs input validation, access control, escrow bookkeeping, economic arithmetic, capacity planning, logging and persistence. The 405-line, 16-branch function is the direct cause of the "asymmetric failure semantics" findings in section 10.

---

## 7. Endpoints

The component exposes no HTTP/REST/GraphQL/gRPC surface. Its entry points are protocol messages on the TMSrv binary TCP channel.

| Entry point | Direction | Message id | Frame size | Handler | Description |
|---|---|---|---|---|---|
| `_MSG_Trade` | Client -> Game, Game -> Client | 131 (`0x0083`) | 156 bytes (`sizeof(MSG_Trade)`) | `Exec_MSG_Trade` (`_MSG_Trade.cpp:20`) | Invite (`MyCheck=0`, no session), offer update (`MyCheck=0`, session), or confirm (`MyCheck=1`); echoed/forwarded back to the peer with swapped `ID`/`OpponentID` |
| `_MSG_CNFCheck` | Game -> Client | 134 (`0x0086`) | `sizeof(MSG_STANDARD)` = 12 | emitted at `_MSG_Trade.cpp:242` | "Your check was recorded"; sent by this component, not handled by it |
| `_MSG_QuitTrade` | Client -> Game | 132 (`0x0084`) | 12 | `_MSG_QuitTrade.cpp:20` | Cancel/close; clears both sessions then broadcasts PK info |
| `_MSG_TradingItem` | Client -> Game, Game -> Client | 118 (`0x0076`) | `sizeof(MSG_TradingItem)` | `_MSG_TradingItem.cpp:20` | Rearranges Carry/Equip/Cargo; refused while a trade session exists |

Wire-level invariants observable by the client:

- Every packet sent by this component has `Size` set by `CPSock` and `ID` naming the *receiving* client (`m->ID = OpponentID; m->OpponentID = conn`).
- On invitation, `TradeMoney` and `MyCheck` are forced to `0` on both sides (BR-27).
- On update, the peer receives `MyCheck = 0` (BR-26).
- On first confirm, the confirmer receives `_MSG_CNFCheck` and the peer receives the offer with `MyCheck = 1`.
- On success, both clients receive `MSG_UpdateCarry` (`SendCarry`) followed by signal 900 (`RemoveTrade`).

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data format | Error handling |
|---|---|---|---|---|---|
| Client (game client, external binary) | External peer | Offer negotiation and confirmation UI | TMSrv binary TCP frames (obfuscated, checksummed) | `MSG_Trade` (156 B), `MSG_STANDARD` signal 134/900 | Reject + message; `RemoveTrade` on most paths; silent return on four paths (BR-05/14, BR-18 freeze not reached) |
| `pUser[]` / `pMob[]` globals | In-process state | Session and world model | Direct memory | C structs | Caller-gated by `Mode == USER_PLAY`; `RemoveTrade` guards slot range |
| DBSrv | Internal process | Persist `Carry[]`, `MOB.Coin`, `Cargo[]` for both parties | Winsock `MSG_DBSaveMob` | Binary struct | Fire-and-forget; send failure not observed; no atomicity across the pair |
| `ItemLog` | File | Audit both payloads | Text append | `sprintf`-built lines | No return value checked |
| `Log` | File | Diagnostics ("err,msg_trade - ...") | Text append | `sprintf` into global `temp[4096]` | No return value checked |
| Item template table (`g_pItemList`) | Memory-mapped config | `EF_NOTRADE` decision, item names for audit | Direct array | `STRUCT_ITEMLIST` | None; invalid index yields 0 from `BASE_GetItemAbility` |
| Message string table | Config file -> memory | Client-facing rejections | Loaded by `BASE_InitializeMessage` | Indexed strings | Default 128-char filler on missing index |
| Grid broadcast (`GridMulticast`) | In-process | Re-broadcast player body after auto-trade teardown | Direct call | `MSG_CreateMob` | Not applicable |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---|---|---|---|
| Command Handler / Message Dispatcher | `case _MSG_Trade: Exec_MSG_Trade(conn, pMsg)` | `ProcessClientMessage.cpp:234-236` | One handler per wire message; keeps the router dumb |
| Session State Object (embedded, single slot) | `MSG_Trade Trade;` inside `CUser` | `CUser.h:59` | One trade per user by construction |
| Two-phase commit (application level) | Offer latches `MyCheck` + settle-on-second-confirm | `_MSG_Trade.cpp:225-249` | Both parties must accept the identical offer before any write |
| Optimistic concurrency with re-validation | Re-check provenance/pairing on every packet | `_MSG_Trade.cpp:56-85`, `192-194` | No reservation of items; correctness via re-read |
| Server-authoritative projection | Re-derive `Item[i]` from `Carry[InvenPos[i]]` | `_MSG_Trade.cpp:196-223` | Client payload treated as a reference, not a value |
| Dry-run then commit (staging) | `BASE_CanTrade` into stack arrays, then two `memcpy` | `_MSG_Trade.cpp:301-335`, `Basedef.cpp:4588` | All-or-nothing inventory transformation |
| Guard-clause / fail-fast validation chain | 20+ sequential `if (...) { cleanup; return; }` | whole file | Flat, readable rejection ladder |
| Canonical teardown primitive | `RemoveTrade(conn)` | `Server.cpp:7319` | Single reset path, slot-scoped, re-entrant safe |
| State-sync packet reuse | Same message type for invite, update, confirm, and echo | `_MSG_Trade.cpp:225`, `387`, `409` | Protocol economy; multiplexed by `MyCheck` + session state |
| Sentinel-value convention | `InvenPos = -1`, `CarryPos = -1`, `255` accepted as empty | `Server.cpp:7326-7332`, `Basedef.cpp:4600` | Distinguishes "empty" from "slot 0" |
| Transport-state mirroring | Copies `KeyWord`/`CheckSum`/`ClientTick` into the domain struct | `_MSG_Trade.cpp:228-230` | Enables whole-struct equality as a freeze test |
| Defensive anti-automation counter | `AddCrackError(conn, 5, 18)` | `_MSG_Trade.cpp:27`, `Server.cpp:684-707` | Penalises forged/replayed packets, forces logout at 2e9 |
| Audit-before-mutation ordering | Four `ItemLog` records precede `SaveUser`/`RemoveTrade` | `_MSG_Trade.cpp:340-384` | Log is complete even if persistence or teardown fails |

Architectural boundary assessment: the component sits correctly at the adapter edge and does not leak transport concerns upward, with one exception — the session object stores transport fields (see above), so `CUser::Trade` is a hybrid DTO. The component also reaches directly into two aggregates (`pUser`, `pMob`) and one global rules module (`Basedef`), which is the project-wide convention rather than a local choice; consequently there is no repository/abstraction layer between the handler and the world model, and the "unit" that must be isolated for testing is effectively the whole TMSrv global state.

---

## 10. Technical Debt & Risks

| ID | Risk level | Component area | Issue | Impact |
|----|------------|----------------|-------|--------|
| TD-01 | High | Settlement failure paths (`_MSG_Trade.cpp:287-326`) | Affordability and space failures `return` without `RemoveTrade` and without clearing `MyCheck`, leaving both sessions latched | Stuck trade window; unlimited retry with one chat message pair per attempt; session persists until another action, logout, or disconnect |
| TD-02 | Medium | Persistence (`_MSG_Trade.cpp:379-380`) | Two independent `SaveUser` messages with no transaction id, ordering guarantee, or acknowledgement | If DBSrv persists only one side, the next login materialises duplicated or vanished items/coin |
| TD-03 | Medium | Transactional integrity (whole handler) | No write-ahead journal, no rollback, and no post-commit verification; commit is pure in-memory mutation | A crash between the two memory writes and the DB save cannot be detected or repaired automatically |
| TD-04 | Medium | Slot bound (`_MSG_Trade.cpp:60`, `74`, `205`) | `InvenPos` validated against the constant `MAX_CARRY-4` (60) instead of `pMob[conn].MaxCarry` (30/45/60) | Items sitting in slots `[MaxCarry, 60)` (possible after inventory-ticket expiry, `ProcessSecMinTimer.cpp:571-597`) can be offered and given away while placement is only computed over `[0, MaxCarry)` |
| TD-05 | Medium | Error handling consistency (`_MSG_Trade.cpp:60-61`, `74-75`) | Two provenance failures perform a bare `return` with no log, no message, and no teardown, unlike the neighbouring branches | Session left in an indeterminate state; failures invisible in logs; inconsistent client behaviour for malformed packets |
| TD-06 | Low | Audit formatting (`_MSG_Trade.cpp:346-374`) | Up to 15 item codes are `strcat`-ed into the shared global `temp[4096]`; the per-item scratch buffer is `char tmplog[2024]` | Theoretical overflow if item names are long or corrupted; `temp` is also a shared global used concurrently by other handlers' formatting |
| TD-07 | Low | Update path (`_MSG_Trade.cpp:389-392`) | A `MyCheck == 0` packet un-conditions the peer's confirmation with no rate limit | Peer's confirmation can be reset repeatedly; griefing vector rather than an economic exploit |
| TD-08 | Low | Concurrency assumptions | Correctness relies on TMSrv's single-threaded Win32 message loop; no lock, version counter, or reentrancy guard exists | Any future threading (or a re-entrant DB callback that touches `pUser[].Trade`) would break the invariants silently |
| TD-09 | Low | Packet validation (`Basedef.cpp:6475-6596`) | `BASE_CheckPacket` is entirely commented out; the `_MSG_Trade` size assertion at line 6535 is dead | Handler reads a fixed 156-byte layout from a shorter message; bounded over-read inside the 128 KB receive buffer, but unvalidated data reaches the rules |
| TD-10 | Low | Message semantics | `_MSG_Trade` multiplexes invite/update/confirm/echo; the branch taken depends on mutable session state rather than an explicit opcode | Hard to reason about, hard to extend, and impossible to validate without full session state |
| TD-11 | Low | Send-result handling (`_MSG_Trade.cpp:247`, `397`, `420`) | `cSock.AddMessage(...)` return value ignored, unlike `SendCarry` which closes the user on failure (`SendFunc.cpp:1557-1558`) | A dropped forward silently desynchronises the pair: server state advances, peer never receives the offer |
| TD-12 | Low | Self-trade (`_MSG_Trade.cpp:192-408`) | `OpponentID == conn` is never rejected | Writes four audit records and performs a no-op swap on one inventory; no demonstrated economic effect, but the path is untested and unintentional |
| TD-13 | Low | Logging attribution (`_MSG_Trade.cpp:79`, `343-374`) | Counterpart-escrow failures are logged under the sender's account; payload lines mix the peer's account name with the sender's `ItemLog` owner | Log forensics cannot attribute a failed trade to the party that actually mutated their inventory |
| TD-14 | Informational | Dead/duplicate work (`_MSG_Trade.cpp:76-80`) | `DestItem`/`SrcItem` and `dItem`/`sItem` in the sibling handler are duplicated lookups; in this file the second provenance loop re-reads `pUser[OpponentID].Trade.InvenPos` three times per index | Negligible cost at 15 entries; noted for completeness |
| TD-15 | Informational | Domain/transport coupling (`CUser.h:59`) | The session object stores `KeyWord`, `CheckSum`, `ClientTick` because the freeze test is a whole-struct `memcmp` | Any transport-layer change (checksum algorithm, header layout) silently breaks trade confirmation |
| TD-16 | Informational | Zero test coverage | No unit, integration, or regression tests exist for any rule in this component | Every rule above is verified only by manual play and log inspection |

---

## 11. Test Coverage Analysis

### 11.1 Verdict

**No automated tests exist for this component, and none exist anywhere in the repository.**

### 11.2 Evidence

| Check | Command / method | Result |
|---|---|---|
| Test source files | Glob `**/*{test,Test,TEST,spec,Spec}*` over the project root (ignoring `.agents`, `.codegraph`, `.opencode`, `openspec`) | No files found |
| Test frameworks | Grep `gtest\|CppUnit\|catch2\|BOOST_TEST\|unittest\|TEST_F\|TEST_CASE` across `Code/` | No matches |
| Test projects | `W2PP Code Project.sln` project list | Only `DBSrv` (`Code\DBSrv\DBSrv.vcxproj`) and `TMSrv` (`Code\TMSrv\TMSrv.vcxproj`); no test project |
| Test harness artifacts | Glob `**/*.{sln,vcxproj,vcxproj.filters,yml,yaml,props}` | 7 files, all build configuration; no CI workflow, no test runner config |
| Trade-specific tests | Grep `Trade` under any `test`/`spec` path | None exist (no such paths) |
| Assertions as substitutes | Grep `assert(` in `Code/TMSrv` | No test-style assertions; only runtime guard `if` statements |
| Documentation | `README.md` | Explicitly lists "Create an documentation" as unchecked; no test strategy mentioned |

### 11.3 Coverage by rule (manual/observational only)

| Area | Rules | Automated coverage | Manual verifiability | Test quality assessment |
|---|---|---|---|---|
| Session gate | BR-01, BR-03 | 0% | Reproducible (die mid-trade, logout mid-trade) | No harness; `AddCrackError` counter observable only in logs |
| Target validation | BR-02, BR-08 | 0% | Reproducible with a modified client | Requires packet crafting; no fixture exists |
| Escrow integrity | BR-05, BR-06, BR-07, BR-14, BR-15, BR-16 | 0% | Difficult (requires racing an inventory mutation against a confirm) | The most security-relevant rules and the least testable today |
| Access control | BR-09, BR-10, BR-11, BR-12, BR-13 | 0% | Reproducible per item class | Guild-bound path (`GetGuild` + `GuildLevel`) needs crafted items with `EF_HWORDGUILD`/`EF_LWORDGUILD` |
| State machine | BR-17, BR-18, BR-26, BR-27 | 0% | Reproducible with two clients | Whole-struct `memcmp` is sensitive to header fields; no regression net |
| Economic arithmetic | BR-19, BR-20, BR-21 | 0% | Boundary cases (0, 2e9, 2e9+1, coin = 0) reproducible by hand | Unsigned underflow path (`fGold < 0` always false) is untested and undocumented before this report |
| Capacity | BR-22, BR-23 | 0% | Reproducible with a full inventory | Three-way message split has no assertions on which branch fired |
| Lifecycle | BR-28, BR-29 | 0% | Reproducible (disconnect/logout, auto-trade teardown) | 91 `RemoveTrade` call sites, zero tests |
| Audit/persistence | BR-24, BR-25 | 0% | Log inspection after a manual trade | No log-content assertions; no DB round-trip test |
| Protocol contract | BR-30 | 0% | Not verifiable at runtime (validation disabled) | Dead code path |

### 11.4 Consequence for this analysis

Because no tests exist, every behavioural statement in section 3 was derived from source reading and cross-file tracing rather than from observed execution. Statements about client-side interpretation (for example what `_MSG_CNFCheck` causes in the client UI, or what `MyCheck = 0` causes in the trade window) are inferences from the server contract and are marked as such; the client source is not in this repository (`Code/ClientPatch_v7662` is listed in `README.md` as a DLL to unlock the client, with no source present). Confidence levels: server-side state transitions and arithmetic (high); client UI semantics (medium); the intent behind `_MSG_CNFCheck` (medium — the name and the self-addressed send are the only evidence).

---

## 12. Appendix — Evidence Index

| # | Reference | Used for |
|---|---|---|
| 1 | `Code/TMSrv/_MSG_Trade.cpp:20-425` | Entire handler |
| 2 | `Code/Basedef.h:2046-2057` | `MSG_Trade` definition, message id 131 |
| 3 | `Code/Basedef.h:925-930` | `_MSG` header macro (12 bytes) |
| 4 | `Code/Basedef.h:75-81` | `MAX_EQUIP`/`MAX_CARRY`/`MAX_CARGO`/`MAX_TRADE`/`MAX_AUTOTRADE` |
| 5 | `Code/Basedef.h:56`, `107` | `MAX_USER` 1000, `MAX_MOB` 25000 |
| 6 | `Code/Basedef.h:398-412` | `STRUCT_ITEM` layout (8 bytes) |
| 7 | `Code/Basedef.h:2060` | `_MSG_CNFCheck` = 134 |
| 8 | `Code/Basedef.cpp:4588-4635` | `BASE_CanTrade` dry-run |
| 9 | `Code/Basedef.cpp:4539-4586` | `BASE_SortTradeItem` |
| 10 | `Code/Basedef.cpp:1479-1495` | `BASE_GetItemCode` audit format |
| 11 | `Code/Basedef.cpp:4207-4235` | Guild id decode (`EF_HWORDGUILD`/`EF_LWORDGUILD`) |
| 12 | `Code/Basedef.cpp:6475-6596` | `BASE_CheckPacket` disabled (6535 is the `_MSG_Trade` line) |
| 13 | `Code/TMSrv/ProcessClientMessage.cpp:38-66`, `234-236` | Router preconditions and trade route |
| 14 | `Code/TMSrv/ProcessClientMessage.h:111` | Handler declaration |
| 15 | `Code/TMSrv/CUser.h:46`, `59`, `65`, `84`, `89`, `115` | `TradeMode`, `Trade`, `AutoTrade`, `Whisper`, `PKMode`, `OnlyTrade` |
| 16 | `Code/TMSrv/Server.cpp:684-707` | `AddCrackError` semantics |
| 17 | `Code/TMSrv/Server.cpp:6871-6878`, `7104-7110` | Disconnect / logout partner cleanup |
| 18 | `Code/TMSrv/Server.cpp:7056-7083` | `SaveUser` projection |
| 19 | `Code/TMSrv/Server.cpp:7319-7352` | `RemoveTrade` contract |
| 20 | `Code/TMSrv/SendFunc.cpp:197-206`, `1534-1558`, `1622` | `SendClientSignal`, `SendCarry`, `SendHpMode` |
| 21 | `Code/TMSrv/_MSG_QuitTrade.cpp:20-54` | Cancel path and PK-info broadcast |
| 22 | `Code/TMSrv/_MSG_TradingItem.cpp:20-430` | Sibling rearrangement handler, `TradeMode` and `Trade.OpponentID` guards |
| 23 | `Code/TMSrv/_MSG_SendAutoTrade.cpp:20-120` | Auto-trade creation preconditions |
| 24 | `Code/TMSrv/_MSG_ReqTradeList.cpp:20-45` | Auto-trade browse preconditions |
| 25 | `Code/TMSrv/ProcessSecMinTimer.cpp:571-597`, `2047-2056` | `MaxCarry` ticket logic, newbie-switch teardown |
| 26 | `Code/TMSrv/GetFunc.cpp:1869-1874` | `GetGuild` |
| 27 | `Code/CPSock.cpp:386-458`, `535-552` | Transport framing, checksum, header rewrite |
| 28 | `Code/CPSock.h:35-38` | 128 KB buffers, `MAX_MESSAGE_SIZE` 8192 |
| 29 | `Code/TMSrv/Language.h:100`, `102`, `185` | `_NN_Wrong_Trade_Packet1`, `_NN_Already_Trading`, `_NN_Deny_Whisper` |
| 30 | `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:123-147` | Afferent/efferent coupling baseline (1 / 14) |
| 31 | `docs/reports/MANIFEST.md:46` | Component identity within the 48-component inventory |
| 32 | `README.md` | No test/CI infrastructure; decompiled project statement |
