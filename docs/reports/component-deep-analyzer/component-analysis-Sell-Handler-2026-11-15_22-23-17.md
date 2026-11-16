# Component Deep Analysis Report - Sell Handler

**Component name:** Sell Handler (`Exec_MSG_Sell`)
**Primary location:** `Code/TMSrv/_MSG_Sell.cpp` (5,916 bytes, 236 lines)
**Project:** W2PP - decompiled C/C++ (Win32, MSVC / v143) MMORPG server emulator for W2
**Host process:** `TMSrv` (game / world server)
**Analysis date:** 2026-11-15 22:23:17
**Analysis scope:** entire project root (`E:\wyd\test-setup\w2pp`), excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
**Analysis mode:** read-only static analysis. No project file was modified.
**Architectural reference:** `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` (component #15, "Command Handler (economy)", Ca = 1, Ce = 14, 6 KB)

---

## 1. Executive Summary

### 1.1 Purpose

`Exec_MSG_Sell` is the server-side terminal for the `_MSG_Sell` client packet: a player standing next to an NPC shop-keeper asks the world server to convert one item from their equipment, inventory (`Carry`) or shop storage (`Cargo`) into gold. The handler is a single 217-line function body (`Code/TMSrv/_MSG_Sell.cpp:20-236`) that performs six distinct jobs in strict sequence: session gating, container/slot authorisation, item resolution and blacklist filtering, merchant-relationship validation, valuation with two independent modifier systems (tiered markdown plus a Celestial class bonus), and finally settlement (city sales tax, guild treasury routing, coin credit with a hard 2 G ceiling, item destruction, audit log and client projection).

It owns exactly one write primitive that matters economically: `pMob[conn].MOB.Coin += sPrice` (`:216`). Everything else is subtraction - the sold item is zeroed (`:226`) and, when a city tax applies, a fraction of the seller's proceeds is destroyed rather than conserved. The component therefore functions simultaneously as a **gold faucet** (sale proceeds), a **gold sink** (75% of every collected tax) and an **item sink** (destruction of the sold item).

### 1.2 Role in the system

The handler sits at the top of the TMSrv call stack. It is reached from exactly one call site - `case _MSG_Sell:` in the flat dispatcher at `Code/TMSrv/ProcessClientMessage.cpp:230-232`, declared at `Code/TMSrv/ProcessClientMessage.h:110` - and it depends downward on fourteen modules: the `Basedef` domain contract (`MSG_Sell`, `MAX_*` constants, `g_pItemList`, `g_pGuildZone`), the `Basedef` rules engine (`GetItemPointer`, `BASE_GetItemAbility`, `BASE_GetVillage`, `BASE_GetItemCode`, `BASE_ClearItem`), `Server.cpp` (`Log`, `AddCrackError`, `RemoveTrade`, `GuildImpostoID`, `pMob`, `temp`), `SendFunc.cpp` (five outbound projections), `GetFunc.cpp` (`GetInView`), `Language.h` (three message ids), `CUser.h`, `CMob.h`, `ItemEffect.h`, `CPSock` and the NT/CRT runtime.

Economically it is the mirror image of the **Buy Handler** (`Code/TMSrv/_MSG_Buy.cpp`) and the **AutoTrade purchase path** (`Code/TMSrv/_MSG_ReqBuy.cpp`). All three read the same `g_pItemList[].Price` base value and the same `g_pGuildZone[].CityTax`, but they apply them with different formulas, different rounding, different treasury ownership checks and different audit channels. The Sell Handler is the only one of the three that **destroys** part of the tax instead of crediting it, and the only one that pays a guild from a *player-to-NPC* transaction.

### 1.3 Key findings

1. **A Celestial skill bit disables the entire merchant-validation block and the entity-range check** (`:96-97` jumps to `LABEL_S1` at `:115`). With `Class == 3` and `LearnedSkill & 0x1000`, `TargetID` is never range-checked before `pMob[sTargetID].MOB.MobName` is read at `:119` and `pMob[sTargetID].TargetX/TargetY` at `:177`. `TargetID` is an `unsigned short` (`Code/Basedef.h:1764`) and `pMob` is `CMob pMob[MAX_MOB]` with `MAX_MOB = 25000` (`Code/TMSrv/Server.cpp:270`, `Code/Basedef.h:107`), so a crafted packet can drive an out-of-bounds read of up to index 65535 - a remote denial-of-service and an information-disclosure into the log file. This is the single most severe finding in the component.
2. **The sale price curve is not monotonic.** The tier breakpoints at `:146-152` create two cliffs where a more expensive item sells for *less*: list price 20,000 yields 5,000 but 20,004 yields 3,334; 40,000 yields 6,666 but 40,004 yields 5,000. The three tiers are 25%, 16.67% and 12.5% of list price.
3. **The `sPrice < 100000` branch at `:188-192` is a semantic no-op.** Both arms compute `(100 - CityTax) * sPrice / 100`; only operand order differs, which is irrelevant under integer arithmetic. The branch is either a decompilation artefact or dead authorial intent, and the multiplication-first ordering it preserves is the *dangerous* one (see finding 8).
4. **Settlement is nested inside the tax-success block.** The label `LABEL_S2` is *inside* `if (CityTax >= 0 && CityTax <= 30)` and inside `if (sPrice >= 0)` (`:184-235`). If `CityTax` were ever outside `[0,30]` the sale is silently aborted with no response, no log and no item destruction. Reachable in principle because the loader clamps to `[0,20]` (`Code/TMSrv/CReadFiles.cpp:811-812`) while the guild command accepts `[0,30]` (`Code/TMSrv/_MSG_MessageChat.cpp:76`), so the two validators disagree on 21-30.
5. **The intended "wilderness merchants are tax-exempt" path is unreachable, and the fall-through indexes one element past the city array.** `:178` guards with `sVillage < 0 || sVillage > 5`, but `BASE_GetVillage` can only return `0..5`, so the guard never fires; a vendor standing outside every city rectangle therefore reaches `g_pGuildZone[5]` at `:182` even though the array has `MAX_GUILDZONE = 5` elements (indices 0-4). `CityTax` is read from 72 bytes past the end of the array, so the sale is either taxed at a rate derived from unrelated static memory or silently discarded by the `[0,30]` window at `:184`.
6. **Guild treasury credit is weaker than in every sibling handler.** `:199` and `:207` check only that the treasury NPC exists (`Mode != MOB_EMPTY`) and is alive in the array - they do **not** check `pMob[GuildImpostoID[v]].MOB.Guild == g_pGuildZone[v].ChargeGuild`, which the Buy Handler (`_MSG_Buy.cpp:260`, `:267`) and the AutoTrade buyer (`_MSG_ReqBuy.cpp:168`, `:174`) both do. A stale `GuildImpostoID` entry therefore pays the wrong entity.
7. **The two treasury caps differ by two orders of magnitude.** `GuildImpostoID[sVillage]` is capped at 200,000,000,000 (`:201`) while `GuildImpostoID[4]` (Noatum) is capped at 20,000,000,000,000 (`:209`). In every sibling handler both caps are 200,000,000,000.
8. **Signed 32-bit overflow is reachable in three arithmetic sites** - the tax multiply (`:189`/`:192`, overflows when the post-tier price exceeds ~21.4 M, i.e. list price above ~171.8 M), the guild-tax delta (`:196`) and the coin-ceiling comparison (`:215`, overflows when `Coin + sPrice > 2,147,483,647`). The sibling AutoTrade buyer uses `unsigned int` for exactly this comparison (`_MSG_ReqBuy.cpp:96`); the Sell Handler does not.
9. **The 2,000,000,000 coin ceiling destroys value silently.** `:215-218` does not reject the sale when the wallet is full; it clamps to 2 G and still consumes the item. A player at 1,999,999,000 selling a 5,000 gold item receives 1,000 and loses the item, with no message.
10. **Audit asymmetry.** The Success path writes to the system log via `Log()` (`:223-224`) with the prefix `sel_npc,`, whereas the buy, autotrade-buy and manual trade paths write to the dedicated item-economy log via `ItemLog()` (`_MSG_Buy.cpp:291`, `_MSG_ReqBuy.cpp:135`, `_MSG_DeleteItem.cpp:46`). The item-economy log file therefore has a gap covering every NPC sale in the server.
11. **No persistence is forced.** The handler never calls `SaveUser`, unlike `_MSG_ReqBuy.cpp:163` and `_MSG_Trade.cpp:379-380`. Settlement relies on the rolling per-user save (`Code/TMSrv/ProcessSecMinTimer.cpp:169-199`, one user per 500 ms tick in a rotation over `MAX_USER`) and on logout. A crash between a sale and the rotation re-creates the sold item on the next login while keeping the gold.
12. **Zero tests exist.** No test project, harness, fixture, mock, assertion or CI pipeline exists anywhere in the repository (Section 11). The component's entire behaviour specification is the 236 lines of source plus the 2018 git history, and `git log` shows the file body has not changed since the initial import (`e47bf4f` touched only the licence header and a trailing newline).

---

## 2. Data Flow Analysis

### 2.1 Macro flow (network packet to world state)

```
1.  Client sends _MSG_Sell (opcode 122 | FLAG_GAME2CLIENT 0x0100 | FLAG_CLIENT2GAME 0x0200 = 890)
2.  CPSock::ReadMessage frames the packet, validates 12 <= Size <= MAX_MESSAGE_SIZE, XOR-decodes
3.  ProcessClientMessage (ProcessClientMessage.cpp:38) validates std->ID in [0, MAX_USER),
    ServerDown < 120, isServer/ClientTick SKIPCHECKTICK gate
4.  switch(std->Type) -> case _MSG_Sell (ProcessClientMessage.cpp:230)
5.  Exec_MSG_Sell(conn, pMsg)                   _MSG_Sell.cpp:20
5a.   Gate: HP != 0 and Mode == USER_PLAY      :24
5b.   Trade conflict: Trade.OpponentID == 0    :31
5c.   Shop-mode conflict: !(TradeMode && CARGO) :38
5d.   Slot authorisation per container         :49-82
5e.   Item resolution (GetItemPointer)         :84
5f.   Blacklist {3193, 3194, 747}              :92
5g.   Merchant relationship (unless Celestial skill 0x1000) :96-113
5h.   Empty-slot / invalid-index repair        :121-131
5i.   Volatile-class filter (potions)          :133-139
6.  Valuation                                 :141-168
6a.   base = g_pItemList[sIndex].Price / 4     :141-144
6b.   tier markdown (>10000 -> /2, >5000 -> *2/3) :146-152
6c.   Celestial haggling (Class 3, skill 0x200) :154-168
6d.   negative guard                          :170-174
7.  Geography and tax                         :177-214
7a.   village = BASE_GetVillage(merchant x, y)  -> 0..4, or 5 for "no city" :177
7b.   if (village < 0 || village > 5) -> jump to settlement (unreachable) :178-179
7c.   CityTax = g_pGuildZone[village].CityTax  (reads element 5 OOB if village == 5) :182
7d.   if CityTax outside [0,30] -> function ends, nothing happens :184
7e.   price = price - price*CityTax/100       :188-192
7f.   guildShare = ((pre-tax - post-tax) / 2) / 2 :196-197
7g.   village treasury NPC  EXP += guildShare (cap 2e11)  :199-205
7h.   Noatum (index 4)      EXP += guildShare (cap 2e13)  :207-211
8.  Settlement (LABEL_S2)                      :213
8a.   Coin += price, clamped at 2,000,000,000  :215-218
8b.   audit line "sel_npc,npc:%s price:%d item:%s" -> Log() :220-224
8c.   BASE_ClearItem(Item)                     :226
8d.   echo the original MSG_Sell back to the client :228
8e.   if equip slot < 8 -> SendEquip(conn, conn) to grid :230-231
8f.   SendEtc(conn) -> coin/exp/skill counters       :233
9.  Outbound bytes flushed by ProcessSecMinTimer's per-user SendMessageA() sweep (500 ms)
```

The pipeline is *fail-stop*: the first violated condition returns. There is no partial completion state and no transaction rollback because there is only one state transition (item zeroed + coin added) and it is written in a single burst at step 8.

### 2.2 Data mutation map

| State | Exact mutation | Site | Mutated when | Reversible |
|-------|----------------|------|--------------|------------|
| `pMob[conn].MOB.Coin` | `+= sPrice`, else `= 2000000000` | `:215-218` | settlement only | No (clamped excess is lost) |
| `STRUCT_ITEM` at `Carry[]` / `Equip[]` / `Cargo[]` position | all 7 fields zeroed via `BASE_ClearItem` | `:226` -> `Basedef.cpp:4281-4291` | settlement only | No |
| `pMob[GuildImpostoID[v]].MOB.Exp` | `+= tcoin` while `< 2e11` | `:201-202` | when tax > 0 and NPC alive | No |
| `pMob[GuildImpostoID[4]].MOB.Exp` | `+= tcoin` while `< 2e13` | `:209-210` | when tax > 0 and NPC alive | No |
| `pUser[conn].Trade` + `.AutoTrade` | `memset` + `Pos[] = -1` + optional grid re-create + `TradeMode = 0` | `:33-34` -> `Server.cpp:7319-7352` | only when a trade was open | No |
| `pUser[conn].NumError` | `+= 10` | `:27` -> `Server.cpp:692` | only on dead/not-playing session | No |
| `pUser[conn].cSock` send buffer | MSG_MessagePanel / MSG_Standard / MSG_UpdateEquip / MSG_UpdateEtc / MSG_Sell echo | `:26`, `:40`, `:106`, `:111`, `:228`, `:231`, `:233` | various | n/a |
| `temp[4096]` (global scratch) | `sprintf` audit line | `:123`, `:223` | error and success paths | Overwritten by any later Log caller |
| `fLogFile` / window title | `fprintf` + `SetWindowTextA` | `:224` -> `Server.cpp:6701-6737` | success/error paths | Append-only |
| Merchant entity | never written - only `Merchant`, `MobName`, `TargetX`, `TargetY` are read | whole body | - | The vendor is never notified that it bought something |

The absence of any merchant-side mutation is notable: unlike the Buy Handler (which decrements `pMobMerc[x].Stock`, `_MSG_Buy.cpp:255`) the Sell Handler never records that the vendor bought anything. NPC shops have unlimited gold and unlimited appetite. That is the design of the original game, but it means the sale path cannot be rate-limited by stock.

### 2.3 Exit map (every reachable return)

| # | Line | Condition | Client feedback | Server side effect |
|---|------|-----------|-----------------|--------------------|
| 1 | `:28` | dead or not `USER_PLAY` | `MSG_SetHpMode` | `NumError += 10`, log `cra point: 10  type: 23` |
| 2 | `:35` | an open trade exists | none | both trades torn down |
| 3 | `:41` | `TradeMode` and selling from `Cargo` | message 170 `_NN_CantWhenAutoTrade` | none |
| 4 | `:56` | carry slot outside `[0,60)` | none | `err,(Wrong dest position)Sell-Carry` |
| 5 | `:64` | `MyType` neither 1 nor 2 (in the truthy branch) | none | `err,sell mytype` |
| 6 | `:69` | cargo slot outside `[0,128)` | none | `err,(Wrong dest position)Sell-Cargo` |
| 7 | `:78` | equip slot outside `[1,16)` | none | `err,(Wrong dest position)Sell-Equip` |
| 8 | `:89` | `GetItemPointer` returned NULL | none | `err,sell target NULL` |
| 9 | `:93` | item is 3193, 3194 or 747 | **none** | none at all - fully silent |
| 10 | `:102` | `TargetID` outside `[1000,25000)` | none | `err,sell target of carrypos outof range` |
| 11 | `:107` | target is not `Merchant == 1` | message 116 `_NN_Only_Merchant_Buy_Yours` | none |
| 12 | `:112` | target outside +/-33 cells | `MSG_Standard` `_MSG_CloseShop` (406) | none |
| 13 | `:126` | item slot empty (sIndex == 0) | none | log `err,request sell ...` |
| 14 | `:128` | item slot empty, carry container | `MSG_SendItem` resync of that slot | none (repair path) |
| 15 | `:138` | `EF_VOLATILE == 1` | message 167 `_NN_Cant_Sell_Potion` | none |
| 16 | `:173` | computed price negative | none | `err,MSG_ReqSell ... MINUS ...` |
| 17 | `:184` | CityTax outside `[0,30]` | **none** | **none** - sale silently vanishes |
| 18 | `:194` | post-tax price negative | **none** | **none** - sale silently vanishes |
| 19 | `:236` | normal completion | 3 or 4 packets (echo, `SendEtc`, optional `SendEquip`) | coin + item + treasury + audit |

Seven of nineteen exits give the client no explanation (rows 2, 4, 8, 9, 13, 17, 18). Row 9 (`:92-93`) is the most consequential silent failure because it is the only *policy* rejection with no message: a player who acquires item 3193/3194/747 cannot learn why the shop refuses it.

### 2.4 Valuation pipeline with worked values

| Stage | Line | Expression | Value for P = 20,000 | P = 40,000 | P = 1,000,000 |
|-------|------|------------|----------------------|------------|---------------|
| raw list price P | `:141` | `g_pItemList[sIndex].Price` | 20,000 | 40,000 | 1,000,000 |
| base 25% | `:144` | `P / 4` | 5,000 | 10,000 | 250,000 |
| tier 1 | `:146-150` | if `> 5000`: `2*s/3` | not entered (s = 5000) | 6,666 | not entered |
| tier 2 | `:151-152` | if `> 10000`: `s/2` | not entered | not entered | 125,000 |
| Celestial +6% (Special[2]=0) | `:154-168` | `(3+50)*s/50` | 5,300 | 7,065 | 132,500 |
| CityTax 5% | `:188-192` | `s*(100-5)/100` | 5,035 | 6,711 | 125,875 |
| guildShare | `:196-197` | `((pre-post)/2)/2` | 66 | 88 | 1,656 |
| credited to seller | `:215-218` | `min(Coin+s, 2e9)` | 5,035 | 6,711 | 125,875 |

The worked values assume the default configuration `CityTax = 5` (`Code/Basedef.cpp:47-54`, nineteenth initialiser of each row) and a Celestial merchant-character with `LearnedSkill & 0x200`.

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Validation | Request requires a living character in `USER_PLAY` (HP != 0); otherwise resend HP/mode state and accrue 10 crack points (type 23) | `_MSG_Sell.cpp:24-29` |
| BR-02 | Validation | An open player trade is force-closed on both sides; the sale request is then dropped | `_MSG_Sell.cpp:31-36` |
| BR-03 | Validation | Selling from `Cargo` is refused while the character is in auto-trade (`TradeMode`) shop mode | `_MSG_Sell.cpp:38-42` |
| BR-04 | Validation | `MyType` must be exactly one of `ITEM_PLACE_EQUIP` (0), `ITEM_PLACE_CARRY` (1), `ITEM_PLACE_CARGO` (2) | `_MSG_Sell.cpp:49-82` |
| BR-05 | Constraint | Carry source slots are restricted to `[0, MAX_CARRY-4)` = `[0,60)`; the last four slots are reserved | `_MSG_Sell.cpp:51-58` |
| BR-06 | Constraint | Cargo source slots are restricted to `[0, MAX_CARGO)` = `[0,128)` | `_MSG_Sell.cpp:59-71` |
| BR-07 | Constraint | Equip source slots are restricted to `[1, MAX_EQUIP)` = `[1,16)`; slots `1..7` additionally count as visual equipment | `_MSG_Sell.cpp:73-82` |
| BR-08 | Validation | The item slot must resolve to a non-NULL pointer with `0 < sIndex < MAX_ITEMLIST`; an empty carry slot is repaired by resending it | `_MSG_Sell.cpp:84-90`, `:121-131`, `Basedef.cpp:2340-2372` |
| BR-09 | Business Logic | Three item indices are absolutely non-sellable: 3193, 3194 (clan mantle tiers) and 747 (crown) - silent rejection | `_MSG_Sell.cpp:92-93` |
| BR-10 | Business Logic | Celestial privilege: `Class == 3` with learned-skill bit `0x1000` bypasses merchant identification, view-range and target-range validation | `_MSG_Sell.cpp:96-97`, `:115` |
| BR-11 | Validation | The counterpart must be an NPC (`MAX_USER <= id < MAX_MOB`) whose `MOB.Merchant == 1` | `_MSG_Sell.cpp:99-108` |
| BR-12 | Validation | The counterpart must be within `VIEWGRIDX/Y` = 33 cells on both axes; otherwise the client shop window is closed | `_MSG_Sell.cpp:109-113`, `GetFunc.cpp:640-647` |
| BR-13 | Business Logic | Items whose `EF_VOLATILE` (effect 38) equals 1 are not sellable (HP/MP potions) | `_MSG_Sell.cpp:133-139` |
| BR-14 | Business Logic | Sale value starts at 25% of `g_pItemList[].Price`, integer-truncated | `_MSG_Sell.cpp:141-144` |
| BR-15 | Business Logic | Tiered markdown: base <= 5,000 unchanged; 5,001-10,000 multiplied by 2/3; > 10,000 halved | `_MSG_Sell.cpp:146-152` |
| BR-16 | Business Logic | Celestial haggling: `Class == 3` with skill bit `0x200`, post-tier price > 1,000, item not 693/694/695: `discount = min(Special[2]/10 + 6, 26)`, then `price = (discount/2 + 50) * price / 50` | `_MSG_Sell.cpp:154-168` |
| BR-17 | Validation | A negative computed price aborts the transaction (overflow sentinel) | `_MSG_Sell.cpp:170-174` |
| BR-18 | Business Logic | The city for tax purposes is where the **merchant** stands, resolved through `BASE_GetVillage` (`0..4`, or `5` for "no city"); the `5` case is not handled and indexes one element past `g_pGuildZone` | `_MSG_Sell.cpp:177-182`, `Basedef.cpp:4258-4267` |
| BR-19 | Business Logic | When `CityTax` is in `[0,30]`, the seller receives `price * (100 - CityTax) / 100` | `_MSG_Sell.cpp:182-192` |
| BR-20 | Business Logic | The collected tax is quartered (`(delta/2)/2`) and paid as EXP to the city treasury NPC **and** to the Noatum treasury NPC | `_MSG_Sell.cpp:196-211` |
| BR-21 | Constraint | Treasury accumulation ceilings: 200,000,000,000 for the village NPC, 20,000,000,000,000 for index 4 | `_MSG_Sell.cpp:201`, `:209` |
| BR-22 | Business Logic | Seller coin credit is clamped: if `Coin + price >= 2,000,000,000` the wallet is set to exactly 2,000,000,000 and the excess is destroyed | `_MSG_Sell.cpp:215-218` |
| BR-23 | Business Logic | The item is destroyed unconditionally on settlement, after an audit line `sel_npc,npc:<name> price:<n> item:<code>` | `_MSG_Sell.cpp:220-226` |
| BR-24 | Contract | The original `MSG_Sell` packet is echoed back to the client as the authoritative slot-clear notification | `_MSG_Sell.cpp:228` |
| BR-25 | Contract | Selling a visual equipment slot (`MyPos < 8`) rebroadcasts the character's appearance to the grid, excluding the seller | `_MSG_Sell.cpp:80-81`, `:230-231` |
| BR-26 | Contract | Coin, EXP, skill points and bonus points are re-projected to the client after settlement | `_MSG_Sell.cpp:233`, `SendFunc.cpp:1195-1231` |
| BR-27 | Business Logic | Valuation depends on the item **index only** - sanctification, bonus effects, stack amount and durability are all ignored | `_MSG_Sell.cpp:141` |
| BR-28 | Business Logic | There is no minimum sale price: a zero-price item is consumed for zero coin | `_MSG_Sell.cpp:141-144`, `:215-218` |

Rule count: 28. Rules 1-13 are authorisation and shape; 14-17 are valuation; 18-22 are fiscal; 23-26 are settlement and projection; 27-28 are valuation side effects that are easy to miss.

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 - Session and Liveness Gate

**Overview**:
No `_MSG_Sell` request is honoured unless the character is alive and the session is fully in-game. The gate also feeds the anti-cheat accumulator.

**Detailed description**:

The condition at `:24` is a disjunction of two independent session facts read directly from global arrays: `pMob[conn].MOB.CurrentScore.Hp == 0` (the character is dead) or `pUser[conn].Mode != USER_PLAY` (the session is in any of the pre-game, character-selection or saving modes). Because `Mode` is an `int` (`Code/TMSrv/CUser.h:45`) with `USER_PLAY` defined as 22 (`CUser.h:36`), any intermediate state - `USER_SELCHAR`, `USER_CHARWAIT`, `USER_WAITDB`, `USER_SAVING4QUIT` - is treated identically to a disconnect. Combined with the dispatcher-level check `std->ID in [0, MAX_USER)` (`ProcessClientMessage.cpp:42`), this is the component's only defence against being driven on behalf of a half-constructed session.

On rejection the handler calls `SendHpMode(conn)` (`SendFunc.cpp:1622-1645`), which re-projects `Msg_SetHpMode` with the current HP and `Mode`. That function has its own guard chain (`conn` in range, `Mode == USER_PLAY`, non-zero socket) which means the resynchronisation message is itself suppressed when `Mode != USER_PLAY` - the very case that triggered the rejection. In practice a non-playing session therefore receives nothing at all from this path, and a dead-but-playing session receives a correct HP update.

The anti-cheat contribution is `AddCrackError(conn, 10, 23)` (`Server.cpp:684-707`). Type 23 is not in the exemption list `{3, 8, 15}`, so the call writes `cra point: 10  type: 23` to the system log with the account name and IP, then adds 10 to `pUser[conn].NumError`. The disconnect threshold is `NumError >= 2000000000`, i.e. two hundred million rejected packets. As with every other handler in the project, the sanction is nominal: this is a logging device, not a rate limiter, and there is no per-second accounting.

**Rule workflow**:
```
receive MSG_Sell
  -> if (Hp == 0 || Mode != USER_PLAY)
       -> SendHpMode(conn)            [no-op when Mode != USER_PLAY]
       -> AddCrackError(conn, 10, 23) [log + NumError += 10]
       -> return
  -> continue to BR-02
```

---

### Business Rule: BR-02 - Open Trade Aborts the Sale

**Overview**:
A character cannot interact with a shop while a peer-to-peer trade session is open; the request both cancels the trade and drops the sale.

**Detailed description**:

`pUser[conn].Trade.OpponentID` is the non-zero partner index stored inside `MSG_Trade` (`Code/Basedef.h:2047-2057`, field at `:2056`). The check at `:31` is a plain truthiness test, so any non-zero opponent - including a stale one left behind by a partner who disconnected mid-trade - triggers the abort. The handler calls `RemoveTrade` twice: first for the *opponent* and then for the caller (`:33-34`), which is the correct ordering because `RemoveTrade(conn)` clears `pUser[conn].Trade` and would otherwise erase the opponent id needed for the first call.

`RemoveTrade` (`Server.cpp:7319-7352`) is a comparatively heavy function: it zeroes `MSG_Trade`, re-initialises all 15 `InvenPos[]` entries to `-1`, zeroes `MSG_SendAutoTrade`, re-initialises all 12 `CarryPos[]` entries to `-1`, sends a bare `900` signal to the client, and - if `TradeMode` was set - recreates the character's mob record on the surrounding grid and clears `TradeMode`. One malformed sell packet is therefore enough to forcibly evict a player from their auto-trade shop and re-broadcast their appearance to every observer.

The rule causes a small but real usability trap: the *sale* is abandoned rather than retried, and no message explains why. The client is expected to infer the cancellation from signal 900. A player who clicks "sell" during a trade loses both the trade and the sale in one action.

**Rule workflow**:
```
if (pUser[conn].Trade.OpponentID != 0)
  -> RemoveTrade(OpponentID)      [zeroes partner's Trade + AutoTrade, signal 900]
  -> RemoveTrade(conn)            [zeroes own Trade + AutoTrade, may re-broadcast mob]
  -> return                       [sale discarded]
```

---

### Business Rule: BR-03 - Auto-Trade Mode Blocks Cargo Sales

**Overview**:
While a character is operating a player shop (auto-trade), the `Cargo` container is frozen: items cannot be sold out of it.

**Detailed description**:

`pUser[conn].TradeMode` is set to 1 only by `Exec_MSG_SendAutoTrade` (`_MSG_SendAutoTrade.cpp:105`) after the shop has been validated and the items copied into `pUser[conn].AutoTrade`. `Cargo` is not a bank: it is the seller's *display shelf* used by the offline-shop feature, and `_MSG_ReqBuy` reads from it (`_MSG_ReqBuy.cpp:83`, `:128`, `:152`) and copies the sold entry into the buyer's `Carry`. Selling the same physical entry to an NPC shop while it is simultaneously advertised to other players would allow the item to be duplicated - the buyer's `ReqBuy` compares `memcmp(&pUser[targetID].Cargo[StorageSlot], &pUser[targetID].AutoTrade.Item[itemPos], ...)` (`_MSG_ReqBuy.cpp:83`) and would abort after the NPC had already paid for it, or worse, succeed if the timing inverted.

The guard is nevertheless narrow in two ways. First, it only fires for `MyType == ITEM_PLACE_CARGO`; a character in shop mode may still sell equipment and inventory items (`:38`), which is intentional because the shop offer lives exclusively in `Cargo`. Second, it fires *before* slot validation but *after* the trade-opponent check, so a shop owner who also has an open peer trade gets the trade torn down first and the "cannot sell while auto-trading" message never appears.

The message is index 170, `_NN_CantWhenAutoTrade` (`Code/TMSrv/Language.h:190`), resolved through `g_pMessageStringTable` loaded from the externally maintained `Language.txt`. Only the allocated index is in the source; the user-visible wording lives outside the repository.

**Rule workflow**:
```
if (TradeMode != 0 && MyType == ITEM_PLACE_CARGO)
  -> SendClientMessage(conn, str[170])
  -> return
else if (TradeMode != 0 && MyType in {0,1})
  -> allowed (shop shelf untouched)
```

---

### Business Rule: BR-04 - Container Discriminator

**Overview**:
`MyType` is a three-valued container selector; any other value is a protocol violation and is logged.

**Detailed description**:

The handler does not use a `switch`. It uses `if (sMyType)` to split "carry or cargo" from "equipment", then `if (sMyType == 1)` to split carry from cargo, and finally an explicit `if (sMyType != 2)` rejection (`:61-65`). The accepted set is therefore exactly `{0, 1, 2}` matching `ITEM_PLACE_EQUIP`, `ITEM_PLACE_CARRY`, `ITEM_PLACE_CARGO` (`Code/Basedef.h:86-88`). The structure is asymmetric: a value of 3 or 7 is caught by the inner rejection and logged as `err,sell mytype`, whereas a negative value is not reachable because `MyType` is `short` and any non-zero negative value enters the same `else` branch and is rejected by the same test. Zero is deliberately routed through the equipment branch, which is why the equipment lower-bound test must be `<= 0` rather than `< 0` (see BR-07).

The discriminator is also the value passed to `GetItemPointer(&pMob[conn].MOB, pUser[conn].Cargo, m->MyType, m->MyPos)` (`:84`), so the container decision is made twice: once in the validation block and once inside the resolver, which repeats the range checks (`Basedef.cpp:2340-2372`). This duplication is harmless but means a future inconsistency between the two would silently change which container is addressed.

Because the resolver receives both `pMob[conn].MOB` and `pUser[conn].Cargo` unconditionally, the *caller* determines which of the two objects a given `MyType` maps to. There is no way for a client to address another player's containers: `conn` comes from the socket, never from the packet.

**Rule workflow**:
```
MyType == 0 -> equip   (range [1,16), visual flag if <8)
MyType == 1 -> carry   (range [0,60))
MyType == 2 -> cargo   (range [0,128))
otherwise   -> Log("err,sell mytype") ; return
```

---

### Business Rule: BR-05 - Carry Slot Window and the Four Reserved Slots

**Overview**:
Inventory sales are restricted to slots 0-59. Slots 60-63 are excluded from the entire economy surface.

**Detailed description**:

`MAX_CARRY` is 64 (`Code/Basedef.h:76`) and the guard at `:53` is `sMyPos < 0 || sMyPos >= MAX_CARRY-4`, i.e. `[0,60)`. The reservation is not documented in `Basedef.h`, but the usage in the rest of the server makes the purpose clear: `Carry[60]` and `Carry[61]` hold time-limited items (index 3467) whose expiry is enforced by `ProcessSecMinTimer.cpp:571-595`, and `Carry[63]` is out-of-band storage for PK/guilt state (`KILL_MARK = 63` at `Basedef.h:166`, consumed by `GetPKPoint`/`GetGuilt`/`SetPKPoint` at `GetFunc.cpp:2105-2157`). `_MSG_ReqBuy.cpp:106` uses the identical bound when searching for a free slot, and `_MSG_DeleteItem.cpp:24`, `_MSG_SplitItem.cpp:24`, `_MSG_Trade.cpp:60` and `_MSG_TradingItem.cpp:87/121` all repeat it.

Consequently the sell handler cannot liquidate an expiring item - which is a deliberate protection, because selling a 30-day item would convert a timed asset into permanent gold and, if the item were later re-seeded by the renewal code, into a duplication vector. The cost is that a player holding an expired-but-not-yet-purged item in slot 60 must wait for the timer, not the shop.

Note the lower bound is `0` here (every ordinary inventory slot is sellable) whereas the equipment branch uses `<= 0`. Carry slot 0 is therefore a normal, sellable slot; only the equipment container treats index 0 as off-limits.

**Rule workflow**:
```
if (MyType == 1)
  if (MyPos < 0 || MyPos >= 60)
    -> Log("err,(Wrong dest position)Sell-Carry") ; return
  else -> resolve in pMob[conn].MOB.Carry[MyPos]
```

---

### Business Rule: BR-06 - Cargo Slot Window

**Overview**:
Shop-storage sales address `pUser[conn].Cargo[]` in the full `[0,128)` range.

**Detailed description**:

`MAX_CARGO` is 128 (`Code/Basedef.h:77`) and `CUser::Cargo[MAX_CARGO]` is a per-session array (`CUser.h:50`) that is persisted through `MSG_DBSaveMob.Cargo` (`Basedef.h:989`). The bound at `:66` is `sMyPos < 0 || sMyPos >= MAX_CARGO`, so all 128 slots are addressable - in contrast to the 60-slot carry restriction there is no reserved tail here, because the auto-trade shelf (`AutoTrade.Item[12]`, `MSG_SendAutoTrade`, `Basedef.h:1909-1923`) is a separate 12-entry projection rather than a slice of the array.

`Cargo` doubles as the offline-shop inventory: `_MSG_ReqBuy` delivers purchased items out of `Cargo[StorageSlot]` and clears the entry (`_MSG_ReqBuy.cpp:152-154`). Selling from `Cargo` at an NPC shop is therefore the "liquidation" route for a shop owner who does not want to buy their own stock back into `Carry` first. It is only available when `TradeMode == 0` (BR-03), so the two consumers of `Cargo` are mutually exclusive by construction.

There is an asymmetry worth recording: the handler validates the cargo window twice - once at `:66` and once inside `GetItemPointer` at `Basedef.cpp:2360-2361` - while the auto-trade buyer validates only the *destination* slot range and then trusts `pUser[targetID].AutoTrade.CarryPos[itemPos]` to be a valid `Cargo` index (`_MSG_ReqBuy.cpp:66-69`). The Sell Handler is the stricter of the two.

**Rule workflow**:
```
if (MyType == 2)
  if (MyPos < 0 || MyPos >= 128)
    -> Log("err,(Wrong dest position)Sell-Cargo") ; return
  else -> resolve in pUser[conn].Cargo[MyPos]
```

---

### Business Rule: BR-07 - Equip Slot Window and the Visual-Appearance Flag

**Overview**:
Equipment sales address slots 1-15; slots 1-7 additionally set a flag that triggers an appearance rebroadcast after the item is destroyed.

**Detailed description**:

The bound at `:75` is `sMyPos <= 0 || sMyPos >= MAX_EQUIP` with `MAX_EQUIP = 16` (`Basedef.h:75`), so the accepted set is `[1,16)` and **slot 0 can never be sold**. The same `<= 0` convention appears in `_MSG_TradingItem.cpp:143` for the source equip slot, so it is a project-wide convention rather than a local slip; the most likely explanation is that `Equip[0]` is reserved by the original client as a non-interactive slot. `_MSG_TradingItem` goes further and excludes slot 15 as well (`>= MAX_EQUIP-1`), while the Sell Handler permits 15 (the mantle slot, used for clan capes 3191-3199) - which is exactly why BR-09 has to blacklist 3193/3194.

The `isEquip` local (`:47`, set at `:80-81`) is a proxy for "this slot contributes to the character's rendered appearance". Slots 1-7 are the visible armour/weapon set; 8-13 are accessories (rings, amulets, circles), 14 the mount and 15 the mantle. After settlement, `if (isEquip) SendEquip(conn, conn)` (`:230-231`) rebroadcasts `MSG_UpdateEquip` through `GridMulticast(x, y, msg, skip=conn)`, so every nearby player refreshes the seller's appearance while the seller themself is skipped.

The flag is only set inside the equipment branch, so it is `0` for carry and cargo sales, and it is never set for slot 0 because slot 0 cannot reach that code. A second-order consequence: because `SendEquip` recomputes visual codes through `BASE_VisualItemCode` (`SendFunc.cpp:1099`), selling a mount (slot 14) or mantle (slot 15) does **not** trigger the refresh unless those slots are `< 8` - they are not. Selling a mantle produces no appearance update at all, which is observable as a stale cape on nearby clients until any other event refreshes them.

**Rule workflow**:
```
if (MyType == 0)
  if (MyPos <= 0 || MyPos >= 16)  -> Log("err,(Wrong dest position)Sell-Equip") ; return
  if (MyPos < 8) isEquip = 1
...
after settlement: if (isEquip) SendEquip(conn, conn)  // grid-wide, self skipped
```

---

### Business Rule: BR-08 - Item Resolution and Empty-Slot Repair

**Overview**:
The addressed slot must resolve to a real item; an empty carry slot is repaired by resending it, all other degenerate cases are logged.

**Detailed description**:

`GetItemPointer` (`Basedef.cpp:2340-2372`) validates `pos` per container and then rejects `sour->sIndex < 0 || sour->sIndex >= MAX_ITEMLIST`. It does **not** reject `sIndex == 0`, because zero is a legal array value; it therefore returns a valid pointer to an *empty* slot whenever the player clicks on one. The handler compensates with a second test at `:121`, `Item->sIndex <= 0 || Item->sIndex >= MAX_ITEMLIST`. Given the resolver's guarantees, `>= MAX_ITEMLIST` is unreachable and the effective predicate is `sIndex == 0`.

The repair branch is asymmetric and is the only place in the handler that re-synchronises client state on failure. For a carry source it emits `SendItem(conn, ITEM_PLACE_CARRY, sMyPos, &pMob[conn].MOB.Carry[sMyPos])` (`:128`), which forces the client to redraw the slot as empty; for cargo and equipment sources it only writes `err,request sell %d %d %d %d` with the index, target, type and position (`:123`). The logic is written as `if (sMyType != 1 || sMyPos < 0 || sMyPos >= MAX_CARRY) Log(...) else SendItem(...)`, so a carry request whose position has somehow drifted outside `[0,64)` is logged instead of repaired.

Reading the two checks together gives the component's real item contract: `GetItemPointer != NULL` guarantees index-in-range, and `sIndex > 0` guarantees non-empty. Anything the client sends that addresses a slot outside the validated window, or a slot whose index was corrupted, is rejected earlier by BR-04..BR-07. The one case the checks do not cover is a *valid but semantically impossible* item (e.g. an equip-only item stored in `Carry`), which the handler happily sells - the Sell Handler performs no item-type/position cross-validation, unlike `BASE_CanEquip`.

**Rule workflow**:
```
Item = GetItemPointer(&pMob[conn].MOB, pUser[conn].Cargo, MyType, MyPos)
if (Item == NULL)                      -> Log("err,sell target NULL") ; return
if (Item->sIndex == 0)
   if (carry && pos in [0,64))         -> SendItem(resync empty slot)
   else                                -> Log("err,request sell ...")
   return
```

---

### Business Rule: BR-09 - Absolute Non-Sellable Item Blocklist

**Overview**:
Items 3193, 3194 and 747 can never be sold, and the refusal is completely silent.

**Detailed description**:

The test at `:92` is a plain three-way disjunction against `Item->sIndex` and the only action on match is `return`. There is no log line, no client message and no `AddCrackError` credit; from the player's point of view the shop simply does nothing. The indices are the only remaining trace of the policy, and their identity has to be reconstructed from the rest of the source: 747 is the Crown, explicitly labelled in `Server.cpp:528` (`// 747 = Crown on itemlist`), while 3193 and 3194 are clan mantle/cape states written into `Equip[15]` by the quest system (`_MSG_Quest.cpp:1053-1067`, `:1207-1211`) and by `_MSG_CombineItemLindy.cpp:103`, `_MSG_UseItem.cpp:3234`.

The neighbouring mantle indices 3191, 3192, 3195, 3196, 3197, 3198 and 3199 are **not** blocked. The quest data shows 3191/3192 upgrading to 3193 (`_MSG_Quest.cpp:1207-1208`) and 3194/3195 upgrading to 3196 (`:1210-1211`), so the blocklist covers the intermediate and entry tiers of one upgrade family but not the terminal tiers. Either the terminal tiers are unobtainable as inventory items (plausible: they may only ever exist in `Equip[15]`), or this is an incomplete blacklist. Nothing in the source resolves the ambiguity, so it is recorded as such.

The blocklist is also inconsistent with the project's own attribute mechanism: `EF_NOTRADE` exists (`Code/ItemEffect.h`, used at `_MSG_SendAutoTrade.cpp:84` with the message `_NN_Cant_MoveItem`) and would express "this item may not leave the character" declaratively. The Sell Handler does not consult `EF_NOTRADE` at all, so an item flagged no-trade can be sold to an NPC for gold and destroyed - the strongest possible violation of the intent behind the flag. Conversely, the autotrade/trade/drop paths block a *different* set (`508, 509, 446, 522, 526-531, 747, 3993, 3994`) and never block 3193/3194.

**Rule workflow**:
```
if (sIndex == 3193 || sIndex == 3194 || sIndex == 747)
  -> return            // no message, no log, no crack points
```

---

### Business Rule: BR-10 - Celestial Sell-Anywhere Privilege (validation bypass)

**Overview**:
A class-3 character holding learned-skill bit `0x1000` skips merchant identification, view-range verification and target-index validation entirely.

**Detailed description**:

`if (pMob[conn].MOB.Class == 3 && pMob[conn].MOB.LearnedSkill & 0x1000) goto LABEL_S1;` (`:96-97`). `Class` is `unsigned char` (`Basedef.h:444`) and class 3 corresponds to `CELESTIAL` (`Basedef.h:180`); `LearnedSkill` is a `long` bitmask (`Basedef.h:461`) whose bits are class-scoped and undocumented - `CMob.cpp:728` uses bit 10 (`0x400`) for a Celestial weapon-mastery perk, so bit 12 (`0x1000`) is plausibly the Celestial trade perk, but the name is recovered by inference only. The effect is unequivocal: the jump lands at `:115`, i.e. *after* `:99-113`, so the three guards in between never execute.

Those three guards are the ones that make `sTargetID` safe. `:99` rejects ids below `MAX_USER` (1000) and at or above `MAX_MOB` (25000); `:104` requires `MOB.Merchant == 1`; `:109` requires the entity to be within 33 cells. The bypass path immediately dereferences the unvalidated index at `:119` (`strncpy(SellName, pMob[sTargetID].MOB.MobName, 16)`) and again at `:177` (`pMob[sTargetID].TargetX`, `.TargetY`). `TargetID` is `unsigned short` (`Basedef.h:1764`), so `sTargetID` can be 0..65535 while `pMob` has 25000 elements; indices 25000-65535 read past the end of the array. Because `CMob` embeds `STRUCT_MOB` first (roughly 1.5 KB per element), index 65535 lands hundreds of megabytes beyond the array base - a reliable access violation on a 32-bit process, i.e. a one-packet remote crash for any Celestial character with the skill.

Even without a crash the bypass changes policy in three ways: it allows selling with no merchant nearby (the intended convenience), it allows selling while the "merchant" is a player or an unrelated mob, and it makes the tax jurisdiction depend on the *target's* position rather than the seller's (`:177`). Since a legitimate client in this mode most likely sends `TargetID = 0`, `BASE_GetVillage(pMob[0].TargetX, pMob[0].TargetY)` evaluates slot 0 of the player array - typically an empty session or the id-0 player - and returns `5`, which then feeds the out-of-bounds `g_pGuildZone[5]` read of BR-18. Privileged sales therefore land on an indeterminate tax rate rather than on the intended exemption. This is an inference from the code path, stated as such.

**Rule workflow**:
```
if (Class == 3 && (LearnedSkill & 0x1000))
   -> goto LABEL_S1            // skips :99-113
   -> SellName = pMob[TargetID].MOB.MobName          [UNVALIDATED READ]
   -> village  = BASE_GetVillage(pMob[TargetID].TargetX, pMob[TargetID].TargetY)  [UNVALIDATED READ]
else
   -> BR-11 then BR-12 then LABEL_S1
```

---

### Business Rule: BR-11 - Merchant Identification

**Overview**:
For unprivileged sellers the counterpart must be a mob-class entity flagged as a merchant.

**Detailed description**:

`:99` implements the entity-class test as a *range* rather than a type lookup: `sTargetID < MAX_USER || sTargetID >= MAX_MOB` rejects anything in the player range `[0,1000)` and anything beyond the mob array. The rationale is the shared entity array: `pMob[0..999]` are player sessions and `pMob[1000..24999]` are NPCs/mobs, so "is a mob" is equivalent to "index >= MAX_USER" and the additional upper bound protects the array. This is the same idiom used by `_MSG_Buy.cpp:49` and by the guild-treasury lookups (`GuildImpostoID[v] >= MAX_USER && < MAX_MOB`).

`:104` then requires `pMob[sTargetID].MOB.Merchant == 1`. The `Merchant` field is `unsigned char` (`Basedef.h:442`) and is a *class code*, not a boolean: `Server.cpp:3190` tests `MOB.Merchant == 6` to identify the guild treasury NPC, and `_MSG_ReqTeleport.cpp:46` relies on the same convention. Requiring exactly 1 therefore excludes treasury NPCs (6) and every special vendor class from buying player items. The rejection message is index 116, `_NN_Only_Merchant_Buy_Yours` (`Language.h:136`), which is the only place in the component that names the restriction to the player.

The rule does **not** require the merchant to be alive: `Hp == 0` on the NPC is not checked, and neither is `Mode != MOB_EMPTY` (which `:199`/`:207` do check for treasuries). A dead-but-resident shop NPC will still buy. This matches `_MSG_Buy`'s behaviour (it also omits the check) but differs from the care taken in the treasury branch.

**Rule workflow**:
```
if (TargetID < 1000 || TargetID >= 25000)      -> Log(...) ; return
if (pMob[TargetID].MOB.Merchant != 1)          -> SendClientMessage(116) ; return
if (!GetInView(conn, TargetID))                -> SendClientSignal(_MSG_CloseShop) ; return
```

---

### Business Rule: BR-12 - Shop Proximity Envelope

**Overview**:
The vendor must be within +/-33 cells horizontally and vertically, otherwise the client's shop window is closed.

**Detailed description**:

`GetInView(a,b)` (`GetFunc.cpp:640-647`) compares `pMob[a].TargetX/Y` against `pMob[b].TargetX/Y` with `VIEWGRIDX = VIEWGRIDY = 33` (`Basedef.h:95-96`) and returns `FALSE` on any axis violation. Despite its name the function computes a *rectangular* envelope, not a client viewport, and it uses `TargetX/TargetY` (the authoritative tile position) rather than `LastX/LastY`.

The failure branch is different from its siblings: instead of a chat message it sends `SendClientSignal(conn, ESCENE_FIELD, _MSG_CloseShop)` (`:111`), i.e. `MSG_STANDARD` with `Type = 150 | FLAG_GAME2CLIENT = 406` and `ID = 30000` (`Basedef.h:170`, `:1819`). The effect is that the shop UI closes rather than displaying an error - which is the correct UX for "you walked away", but is also what a client sees when it is out of sync with the server about the merchant's position.

Because the check runs *after* the merchant-identity test, an out-of-range non-merchant produces message 116 rather than a shop close; and because the privileged path (BR-10) skips both, distance is never enforced for Celestials. There is no separate "facing" or "line of sight" requirement; walls do not matter.

**Rule workflow**:
```
dx = |conn.TargetX - merchant.TargetX| ; dy = |conn.TargetY - merchant.TargetY|
if (dx > 33 || dy > 33) -> SendClientSignal(conn, 30000, _MSG_CloseShop) ; return
```

---

### Business Rule: BR-13 - Volatile Item Exclusion (Potions)

**Overview**:
Items whose `EF_VOLATILE` attribute resolves to 1 are refused with message `_NN_Cant_Sell_Potion`.

**Detailed description**:

`BASE_GetItemAbility(Item, EF_VOLATILE)` (`Basedef.cpp:1647-1842`) sums the `EF_VOLATILE` (effect id 38, `Code/ItemEffect.h:77`) value from the item *template* (`g_pItemList[idx].stEffect[0..11]`) plus any matching per-instance effect in `item->stEffect[0..2]` (`:1777-1788`). For effect 38 the sanctification multiplier is deliberately not applied: `EF_VOLATILE` appears in the exclusion list at `:1813`, so a +9 potion does not become a ten-times-volatile potion. The value is therefore a small class code; the comment block at `ItemEffect.h:77-78` documents the intended vocabulary (`0` none, `1` potion, `2` coin bag, `3` ... `4-6` further classes) albeit in mangled Korean text.

Only `== 1` is blocked. Volatile 2 is the *coin bag*, whose value is decoded at `_MSG_GetItem.cpp:182-197` into `EF_HWORDCOIN << 8 | EF_LWORDCOIN` and converted into gold on pickup - and it is **sellable here** for 25% of its list price, which is harmless only because the bag's list price is presumably nominal. Volatiles 3-6 are also sellable. So the rule is best read as "potions are not a tradeable commodity at NPC shops", not as "volatile items are protected".

The rule executes after the merchant checks and after the empty-slot repair but before valuation, so a potion sale never touches the price formula and never reaches the treasury. It also confirms an important asymmetry with `_MSG_UseItem`, which uses the same `EF_VOLATILE == 1` test to select the potion-consumption path (`_MSG_UseItem.cpp:94-98`): the two components agree on the meaning of the code.

**Rule workflow**:
```
Vol = BASE_GetItemAbility(Item, 38)   // template + instance effects, no sanc scaling
if (Vol == 1) -> SendClientMessage(conn, str[167]) ; return
if (Vol == 2) -> allowed (coin bag sells for 25% of list price)
```

---

### Business Rule: BR-14 - Base Valuation at 25% of List Price

**Overview**:
The starting sale value is one quarter of the item's catalogue price, truncated toward zero.

**Detailed description**:

`int sPrice = g_pItemList[Item->sIndex].Price;` (`:141`) then `sPrice /= 4;` (`:144`). `STRUCT_ITEMLIST.Price` is an `int` (`Basedef.h:901`) populated at load time from the `nprice` field of `../../Common/itemlist.csv` (`Basedef.cpp:4971`, parsed at `:5062`, stored at `:5113`). The file is not present in the repository, so the actual numeric distribution of prices is unknown; every quantitative statement in this report is therefore a formula, with concrete numbers only where the formula is evaluated at a chosen input.

Three properties follow directly. First, the divisor is applied before any other modifier, so all later percentages compound on the truncated quarter (e.g. list 10,003 -> 2,500 -> 1,666 at the 2/3 tier); this ordering loses up to 3 gold per item relative to computing the full expression in one step. Second, valuation reads **only** `sIndex` (BR-27): sanctification, rarity grade, bonus effects and stack quantity (which for volatile items can encode an amount, `BASE_GetItemAmount`) are ignored, which makes every instance of a given item index worth exactly the same. Third, a negative or zero `Price` in the CSV propagates: zero yields a zero-coin sale that still destroys the item (BR-28), negative is caught later by BR-17.

The 25% factor is the economic spread between the vendor's buying and selling price: `_MSG_Buy` charges `g_pItemList[itemIndex].Price` plus city tax (`_MSG_Buy.cpp:104-116`). A player who buys and immediately sells the same item therefore loses at least 75% of the price, which is the intended anti-arbitrage property of the shop system.

**Rule workflow**:
```
sPrice = g_pItemList[sIndex].Price
sPrice = sPrice / 4            // integer truncation toward zero
```

---

### Business Rule: BR-15 - Tiered Markdown (and the non-monotonic price curve)

**Overview**:
Larger denominations are discounted further: 25% up to list 20,000, 16.67% up to list 40,000, 12.5% above.

**Detailed description**:

The code at `:146-152` is written against the *already quartered* value, which is why the literal thresholds are 5,000 and 10,000:

```
sPrice /= 4;                                   // 25 %
if (sPrice <= 10000) { if (sPrice > 5000) sPrice = 2 * sPrice / 3; }  // -> 16.67 %
else sPrice /= 2;                              // -> 12.5 %
```

Translated to list price P: for P/4 <= 5,000 (P <= 20,000) the seller receives P/4; for 5,000 < P/4 <= 10,000 (20,000 < P <= 40,000) the seller receives 2*(P/4)/3 ~ 16.67%; above that, P/8 = 12.5%. The intent is a progressive "bulk value" curve that discourages converting high-value gear into currency.

The implementation produces **two discontinuities that invert monotonicity**, which is almost certainly unintended:

| List price P | Computed | Effective ratio |
|--------------|----------|-----------------|
| 20,000 | 5,000 | 25.000% |
| 20,004 | 3,334 | 16.667% |
| 40,000 | 6,666 | 16.665% |
| 40,004 | 5,000 | 12.499% |

An item priced 20,004 pays less than one priced 20,000; an item priced 40,004 pays less than one priced 40,000. Players who can observe both prices (shop UI versus sale quote) see an incentive to split or re-price items at the cliff. The discontinuity is a direct consequence of applying the tiers to the truncated quarter instead of to P, and of using `>` rather than `>=` at the boundaries.

A related detail: the `else` arm at `:151-152` (`sPrice /= 2`) applies to *all* values above 10,000, so the maximum achievable sale price from the base formula is unbounded but grows at only 12.5% of list price - reaching the 2 G wallet ceiling at a list price of 16,000,000,000, which overflows 32-bit `int` long before that (see BR-17 and the risk register).

**Rule workflow**:
```
s = P / 4
if (s <= 10000)
   if (s > 5000) s = 2*s/3
   // else s unchanged
else
   s = s / 2
```

---

### Business Rule: BR-16 - Celestial Haggling Bonus

**Overview**:
A Celestial with learned-skill bit `0x200` receives a percentage bonus derived from `CurrentScore.Special[2]`, applying only to post-tier prices above 1,000 and excluding quest circle items 693-695.

**Detailed description**:

The guard at `:154` has five conjuncts: `sPrice > 1000`, `sIndex` not one of 693/694/695, `Class == 3`, and `LearnedSkill & 0x200`. The threshold is applied to the *post-tier* price, so with the default curve the bonus only engages above a list price of roughly 4,004 gold; below that the privilege is invisible. The three excluded indices are the Celestial "circle" accessories equipped in `Equip[13]` (`_MSG_Quest.cpp:537` assigns `rand()%3 + 693`, `:611-625` treats 693/694/695 as a quest class) - the exclusion prevents the bonus from being farmed by buying and reselling the quest rings, whose list price is presumably above the threshold.

The formula is a two-stage clamp followed by a proportional uplift. `special = CurrentScore.Special[2] / 10` (a `short`, `Basedef.h:435`), `discount = special + 6` clamped to 26, then `discount /= 2`, then `sPrice = (discount + 50) * sPrice / 50`. Because `discount` is halved, the maximum uplift is `(13+50)/50 = 1.26`, i.e. +26%, and the minimum (Special[2] = 0) is `(3+50)/50 = 1.06`, i.e. +6%. The `discount >= 5 && discount <= 26` guard at `:163` is defensive: given the previous clamp, values below 5 occur only when `Special[2]` is negative enough to drag `special + 6` under 5.

Note the arithmetic is multiply-then-divide (`(discount + 50) * sPrice / 50`), so the truncation happens once at the end and the ordering is the safe one for the magnitudes involved. The bonus is applied *before* the city tax, so the seller's uplift is itself taxed. The `_MSG_Buy` counterpart uses the identical derivation for the buy-side discount (`_MSG_Buy.cpp:135-145`) - same divisor 10, same +6, same 26 cap, same halving - which strongly suggests both are transcriptions of a single original `Special[2]`-based charisma formula; the Buy Handler computes `Desconto` but never applies it to `Price`, so only the Sell side actually works.

**Rule workflow**:
```
if (sPrice > 1000 && sIndex not in {693,694,695} && Class == 3 && (LearnedSkill & 0x200))
   special  = Special[2] / 10
   discount = special + 6
   if (discount >= 26) discount = 26
   if (discount >= 5 && discount <= 26)
      discount /= 2
      sPrice = (discount + 50) * sPrice / 50
```

---

### Business Rule: BR-17 - Non-Negative Price Invariant

**Overview**:
A negative computed price aborts the transaction and logs a distinctive sentinel message.

**Detailed description**:

`:170-174` tests `sPrice < 0`, writes `err,MSG_ReqSell  <><><><><><> MINUS <><><><><><>`, and returns. The message names `MSG_ReqSell`, a struct that does not exist in the current protocol (`Basedef.h` defines `MSG_Sell`), which is evidence that this code and its log string descend from an older message vocabulary and were never revisited. The same sentinel with the same padding appears in `_MSG_Buy.cpp:312` for `MSG_ReqBuy`, so the pattern is project-wide.

The check is positioned after the haggling bonus and before geography, i.e. it covers the base division, the tier multipliers and the bonus multiplier, but **not** the tax multiplication at `:188-192` (which is protected by its own `sPrice >= 0` test at `:194` inside the tax block). It therefore functions as an overflow sentinel for the `2*sPrice/3` and `(discount+50)*sPrice` expressions: `2*sPrice` can overflow when the quartered price exceeds ~1.07 G, and the haggling multiply when the quartered price exceeds ~1.6 G. Both require pathological `itemlist.csv` values.

What the check does not catch is the tax-site overflow, where `(100 - CityTax) * sPrice` can wrap *positive* as well as negative (signed overflow is undefined behaviour in C++, and MSVC's practical behaviour is wraparound that can land on any sign). The subsequent `sPrice >= 0` gate catches only the negative half. A configuration with a base price above ~171.8 M could therefore mint a large positive payout from a wrapped multiply, which is why the risk register rates this Medium rather than Low even though it is not reachable with sane data.

**Rule workflow**:
```
... valuation complete ...
if (sPrice < 0) -> Log("err,MSG_ReqSell ... MINUS ...") ; return
```

---

### Business Rule: BR-18 - Jurisdiction Resolution (and the unhandled `5` sentinel)

**Overview**:
The tax jurisdiction is where the **merchant** stands. `BASE_GetVillage` returns `0..4` or `5` for "no city"; the `5` case is not handled and produces an out-of-bounds read of `g_pGuildZone[5]`.

**Detailed description**:

`BASE_GetVillage(x, y)` (`Basedef.cpp:4258-4267`) linearly scans the five `g_pGuildZone` records, returning the first index whose `CityLimitX1 <= x <= CityLimitX2` and `CityLimitY1 <= y <= CityLimitY2`, and `5` when no rectangle matches. The rectangles are compile-time constants (`Basedef.cpp:47-54`): Armia `x 2052-2171, y 2052-2163`; Azran `2432-2675 / 1672-1767`; Erion `2448-2476 / 1966-2024`; Nippleheim `3605-3690 / 3090-3260`; Noatum `1036-1072 / 1700-1760`. The sentinel `5` is deliberately *not* a valid index - `g_pGuildZone` is declared with `MAX_GUILDZONE = 5`, i.e. elements `0..4` (`Basedef.h:148`, `:2441`) - so any consumer that uses the raw return value as an index must exclude `5` first.

The handler attempts exactly that at `:178`: `if (sVillage < 0 || sVillage > 5) goto LABEL_S2;`. The test is wrong in both directions. It cannot fire, because `BASE_GetVillage` returns only `0..5`; and it would not exclude the one problematic value, `5`, if it did fire, because `5 > 5` is false. The intended construct was almost certainly `if (sVillage < 0 || sVillage >= MAX_GUILDZONE) goto LABEL_S2;`, which would have jumped to settlement for a wilderness merchant and produced the tax-free sale the naming suggests. As written, execution continues to `:182` and evaluates `g_pGuildZone[5].CityTax`, a read of 72 bytes past the end of the array (`CityTax` is the nineteenth of twenty-one `int` fields, `Basedef.h:741`).

The consequence is that a vendor placed outside every city rectangle produces one of two outcomes, both wrong: if the adjacent static memory happens to yield a value in `[0,30]` the sale is taxed at that arbitrary rate; otherwise the `[0,30]` window at `:184` fails and the sale is silently abandoned (R-03's failure mode). Because the adjacent memory is other static objects, the value can differ between builds and between link orders, so the behaviour is not reproducible. This is the second-most severe defect in the component after the BR-10 bypass, and it is trivially reachable: any merchant NPC in an open field, or any merchant standing two cells outside its own city rectangle, takes this path.

The rule's *intent* - "the city taxes the shop, not the customer" - is sound and is shared with the buy path, which uses the correct comparison (`Village < 0 || Village >= 5`, `_MSG_Buy.cpp:109`) but evaluates it one line *after* dereferencing `g_pGuildZone[Village]` (`:107`), so the same out-of-bounds read exists there as well. The Sell Handler differs only in that its guard (`:178`) is also comparing against 5 instead of `>= MAX_GUILDZONE`, so it has no correct guard to fall back on at any point in the flow.

**Rule workflow**:
```
v = BASE_GetVillage(merchant.TargetX, merchant.TargetY)   // 0..4, or 5 = none
if (v < 0 || v > 5) goto LABEL_S2        // unreachable: v is always 0..5
CityTax = g_pGuildZone[v].CityTax        // v == 5 -> OUT-OF-BOUNDS READ
if (CityTax in [0,30]) { tax block ... } // else: sale silently dropped
```

---

### Business Rule: BR-19 - City Sales Tax Application

**Overview**:
When the vendor sits in a taxed city, the seller receives the price net of `CityTax` percent, truncated.

**Detailed description**:

`:184` opens the tax window with `CityTax >= 0 && CityTax <= 30`, and `:188-192` applies it *after* saving the pre-tax value in `fPrice`. The two arms are textually different but arithmetically identical - `sPrice * (100 - CityTax) / 100` versus `(100 - CityTax) * sPrice / 100` - so the `if (sPrice < 100000)` branch has no effect whatsoever. Both truncate once, after the multiplication, which is the numerically better ordering; the branch is either a decompiler artefact or a remnant of a fix that was never completed (the sibling `_MSG_Buy.cpp:112-116` contains the same vestigial structure, where both arms are literally identical).

The valid range deserves scrutiny because the two configuration paths disagree: `CReadFiles::ReadGuildFile` clamps a loaded value to `[0,20]`, substituting 10 when outside (`CReadFiles.cpp:809-813`), while the in-game `guildtax` command accepts `[0,30]` (`_MSG_MessageChat.cpp:74-80`) and writes it straight into `g_pGuildZone[i].CityTax` (`:88`). A guild that sets 25% therefore has the setting silently reset to 10% on the next restart. Conversely the handler's own window is `[0,30]`, so a value of 21-30 is honoured until restart.

The truncation direction always favours the house: `sPrice * 95 / 100` floors, so the seller loses the remainder. For small prices the loss is proportionally significant (a 30-gold sale at 5% yields 28, a 7% effective tax). The tax is never skipped by design: a merchant outside every city does not bypass this block, it merely supplies an out-of-bounds `CityTax` (BR-18); and the privileged path of BR-10 changes the *target* whose coordinates are used, not the decision to tax.

**Rule workflow**:
```
if (CityTax >= 0 && CityTax <= 30)
   pre = sPrice
   sPrice = (100 - CityTax) * sPrice / 100      // floor
   if (sPrice >= 0) { guild share ... ; settlement }
// no else: the sale is abandoned with no feedback
```

---

### Business Rule: BR-20 - Guild Treasury Split and Dual Routing

**Overview**:
A quarter of the collected tax is paid as EXP to the city's treasury NPC, and the same amount again to the Noatum (index 4) treasury NPC.

**Detailed description**:

`int tcoin = (fPrice - sPrice) / 2; tcoin /= 2;` (`:196-197`). The intermediate is the total tax removed from the seller; halving twice yields `floor(floor(tax/2)/2)` which equals `floor(tax/4)` for non-negative values. Three quarters of the tax is therefore **destroyed** - a deliberate gold sink - and one quarter is credited to guild treasuries. The `GuildImpostoID[]` array (`Server.cpp:387`, size `MAX_GUILDZONE` = 5) holds the mob index of each city's tax NPC, populated at spawn time for any NPC whose `MOB.Merchant == 6` by `GuildImpostoID[level] = tmob` where `level` is the NPC's `CurrentScore.Level` (`Server.cpp:3190-3195`); the treasury itself is the NPC's `MOB.Exp`, persisted to `./npc/<MobName>` every minute (`ProcessSecMinTimer.cpp:2074-2104`) and later converted to challenge currency by `_MSG_Challange.cpp:52-102` at a rate of 1e9 EXP per unit.

The two credits are issued independently and unconditionally of each other: `:199` pays `GuildImpostoID[sVillage]` (the city where the merchant stands) and `:207` pays `GuildImpostoID[4]` (Noatum, the capital) the *same* `tcoin`. When `sVillage == 4` the Noatum treasury is therefore paid twice, once by each block, and may reach its (much larger) ceiling faster. The checks are `>= MAX_USER && < MAX_MOB && tcoin > 0 && pMob[...].Mode != MOB_EMPTY`, i.e. existence and liveness only - there is **no** guild-ownership verification, unlike `_MSG_Buy.cpp:260/267` and `_MSG_ReqBuy.cpp:168/174` which additionally require `MOB.Guild == g_pGuildZone[v].ChargeGuild`. Selling items is thus a way to enrich a *displaced* guild master.

Two more asymmetries: the Sell path tests `MOB_EMPTY` (0) while the Buy path tests `USER_EMPTY` (0) - identical values, different symbols; and the Sell path splits the tax before the cap test, whereas the Buy path adds the full `GuildTax` to `[4]` and half to the village (`_MSG_Buy.cpp:263`, `:270`).

**Rule workflow**:
```
tax   = pre - sPrice
tcoin = tax / 4
if (villageNPC valid and alive and tcoin > 0 and Exp < 2e11)  Exp += tcoin
if (NoatumNPC valid and alive and tcoin > 0 and Exp < 2e13)   Exp += tcoin
// 3/4 of tax destroyed
```

---

### Business Rule: BR-21 - Treasury Accumulation Ceilings

**Overview**:
Treasury EXP is capped at 200,000,000,000 for a city and 20,000,000,000,000 for Noatum.

**Detailed description**:

The two constants are embedded as literals rather than named symbols, and they differ by a factor of one hundred: `:201` compares against `200000000000` (2e11) while `:209` compares against `20000000000000` (2e13). `MOB.Exp` is `long long` (`Basedef.h:450`), so neither cap risks overflow at the type level; the caps are policy, not safety.

Every other handler that touches the same treasuries uses 2e11 for both: `_MSG_Buy.cpp:262` and `:269`, `_MSG_ReqBuy.cpp:170` and `:176`. The 2e13 figure on line `:209` is therefore either an intentional privilege for the capital (allowing it to accumulate a hundred times more challenge currency) or a typographical duplication of a zero. Nothing in the component or its siblings documents the intent. Functionally the difference only matters once a treasury has accumulated 2e11 - which at the documented conversion rate of 1e9 EXP per currency unit (`_MSG_Challange.cpp:52`) is 200 units - after which Noatum keeps growing while cities stop.

The condition is written as `if (pMob[...].MOB.Exp < cap) pMob[...].MOB.Exp += tcoin;` with no partial credit: a treasury at `cap - 1` receiving a large `tcoin` overshoots the cap and is allowed to (the assignment happens once). So the cap is a *gate*, not a clamp, and the true ceiling is `cap + maxSingleCredit - 1`.

**Rule workflow**:
```
if (Exp < 200000000000)   Exp += tcoin      // village treasury
if (Exp < 20000000000000) Exp += tcoin      // Noatum treasury
```

---

### Business Rule: BR-22 - Seller Coin Credit and the 2 G Ceiling

**Overview**:
The net price is added to the character's coin purse, clamped at 2,000,000,000, and the sale completes regardless.

**Detailed description**:

`:215-218` is `if ((pMob[conn].MOB.Coin + sPrice) < 2000000000) Coin += sPrice; else Coin = 2000000000;`. `Coin` is `int` (`Basedef.h:448`), the ceiling 2e9 sits below `INT_MAX` (2,147,483,647), and the intent is clearly the game's long-standing "2 G maximum". The formulation has three consequences.

First, **the ceiling does not prevent the sale**: unlike `_MSG_GetItem`'s coin-bag pickup, which refuses with message 273 `_NN_Cant_get_more_than_2G` (`_MSG_GetItem.cpp:190-196`) and unlike `_MSG_ReqBuy`'s `_NN_Cant_get_more_than_2G` guard (`_MSG_ReqBuy.cpp:98-102`), the Sell Handler destroys the item and truncates the payment. A player at 1,999,999,000 selling a 5,000-gold item receives 1,000 and loses the item, with no notification. That is a silent economic loss caused by a UI-level condition the player cannot see.

Second, the comparison itself can overflow. `Coin` is capped at 2e9 by invariant, and `sPrice` can reach into the hundreds of millions for pathological item prices, so `Coin + sPrice` can exceed `INT_MAX` and invoke undefined behaviour; a wrapped negative result takes the `< 2000000000` branch and then performs a wrapping `+=`, producing a *negative* purse. The sibling AutoTrade buyer avoids exactly this by computing into an `unsigned int` (`_MSG_ReqBuy.cpp:96`).

Third, the boundary is `<` not `<=`, so an exact landing on 2e9 takes the clamp branch and assigns the same value - harmless, but it means the clamp path is exercised in normal play at exactly the ceiling.

**Rule workflow**:
```
if (Coin + sPrice < 2000000000)  Coin += sPrice      // overflow-prone comparison
else                             Coin  = 2000000000  // excess destroyed
```

---

### Business Rule: BR-23 - Item Destruction and the Sale Audit Line

**Overview**:
On settlement the source slot is zeroed and one audit line is written to the system log.

**Detailed description**:

`:220-226` builds `sel_npc,npc:%s price:%d item:%s` into the 4096-byte global `temp` (`Server.h:382`), using `SellName` (copied from the merchant's `MOB.MobName` at `:119`) and `BASE_GetItemCode(Item, tmplog)` (`Basedef.cpp:1479-1495`, which renders `" <Name> : <index>.<e0>.<v0>.<e1>.<v1>.<e2>.<v2>"` into a 2048-byte stack buffer). It then calls `Log(temp, pUser[conn].AccountName, pUser[conn].IP)` and finally `BASE_ClearItem(Item)` (`Basedef.cpp:4281-4291`) which zeroes `sIndex` and all six effect bytes.

The item is destroyed **after** the log is written and **after** the coin has been credited, so a crash between `:218` and `:226` leaves the player richer and still holding the item. Because settlement is not idempotent (no transaction id, no slot version, no persisted "sold" marker), a client that replays the same packet after such a crash gets paid twice for one item. In normal operation the window is microseconds and the server is single-threaded, so the practical exposure is limited to abnormal termination.

The audit line goes to `fLogFile` via `Log()` (`Server.cpp:6701-6737`) rather than to `fItemLogFile` via `ItemLog()` (`Server.cpp:9264-9284`). Every other economy mutation - NPC purchase (`_MSG_Buy.cpp:291`), autotrade purchase (`_MSG_ReqBuy.cpp:135`), autotrade listing (`_MSG_SendAutoTrade`), manual trade (`_MSG_Trade`), item deletion (`_MSG_DeleteItem.cpp:46`) - writes to the item log. The `sel_npc,` prefix is greppable and parseable, so the data is recoverable, but any reconciliation tool that reads only the item-economy log will show unexplained gold creation. `SellName` also carries a latent defect: `strncpy(SellName, MobName, NAME_LENGTH)` copies exactly 16 bytes and does not terminate when the source fills all 16, leaving `SellName[16]` uninitialised stack (declared at `:117` without initialisation) for `%s` to walk past.

**Rule workflow**:
```
sprintf(temp, "sel_npc,npc:%s price:%d item:%s", SellName, sPrice, itemCode)
Log(temp, account, ip)        -> fLogFile + window title
BASE_ClearItem(Item)          -> sIndex = 0, effects = 0
```

---

### Business Rule: BR-24 - Client Slot-Clear Echo Protocol

**Overview**:
The server returns the client's own `MSG_Sell` packet verbatim as the authoritative instruction to clear the sold slot.

**Detailed description**:

`:228` is `pUser[conn].cSock.AddMessage((char*)m, sizeof(MSG_Sell))`, i.e. the inbound buffer itself is re-queued to the same socket. `CPSock::AddMessage` (`CPSock.cpp:513-560`) overwrites `Size`, `KeyWord`, `CheckSum` and `ClientTick` with server-authoritative values before copying, so the echo is a properly framed server message carrying the client's original `ID`, `TargetID`, `MyType` and `MyPos`. The type constant `_MSG_Sell = 122 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME` (`Basedef.h:1760`) is bidirectional precisely to make this legal.

This echo is load-bearing: the handler never sends `SendItem` for the sold slot on the success path. The client is expected to interpret the returned packet as "this slot is gone". That is consistent with the repair path at `:128`, which does send `SendItem` when the *request* is invalid - the server explicitly re-asserts state only when it disagrees with the client. No other handler in the economy family uses this pattern (the Buy Handler instead mutates `m->Coin` and re-queues with `m->Size`, `_MSG_Buy.cpp:276-280`), so the echo is a component-specific contract that any client reimplementation must honour.

If the echo is lost (send-buffer full, `AddMessage` returns FALSE and the failure is ignored here), the server has already destroyed the item and credited the gold while the client still displays the item. The next inventory-full check or trade would then desynchronise. `AddMessage` failures are logged by `CPSock` itself (`:520-523`) but the caller cannot react.

**Rule workflow**:
```
pUser[conn].cSock.AddMessage((char*)m, sizeof(MSG_Sell))
   -> Size/KeyWord/CheckSum/ClientTick rewritten by CPSock
   -> queued; flushed by the 500 ms SendMessageA sweep
   -> client clears MyType/MyPos locally
```

---

### Business Rule: BR-25 - Appearance Rebroadcast for Visual Slots

**Overview**:
Selling from equip slots 1-7 forces a grid-wide `MSG_UpdateEquip`.

**Detailed description**:

`if (isEquip) SendEquip(conn, conn);` (`:230-231`). `SendEquip` (`SendFunc.cpp:1084-1134`) builds `MSG_UpdateEquip` from all 16 slots using `BASE_VisualItemCode(item, i)` and `BASE_VisualAnctCode(item)`, applies the mount special case for slot 14 (`:1103-1127`), and calls `GridMulticast(pMob[conn].TargetX, pMob[conn].TargetY, msg, skip)`. Passing `conn` as `skip` suppresses the packet for the seller, who already knows.

The threshold `MyPos < 8` is the component's own definition of "visual": slots 1-7 render on the character model. Slots 8-13 (accessories), 14 (mount) and 15 (mantle) are excluded, so selling a mount or a clan cape leaves every nearby client rendering the old appearance until some other event refreshes it. That is observable: a sold cape visually persists. `_MSG_CreateMob`-based refreshes (e.g. the one inside `RemoveTrade` at `Server.cpp:7344-7348`) will eventually correct it, and re-entering the view of other players always will.

The call is issued only on full settlement, after `BASE_ClearItem`, so observers never see a transient state in which the item is both worn and sold. There is exactly one `SendEquip` call and it happens before `SendEtc`, so the seller's own coin update is queued after the broadcast to others - an ordering that matters only for packet-log readability.

**Rule workflow**:
```
if (isEquip)                       // MyType == 0 && MyPos in [1,8)
   SendEquip(conn, conn)           // MSG_UpdateEquip -> GridMulticast(skip self)
SendEtc(conn)                      // MSG_UpdateEtc -> self (coin, exp, bonuses)
```

---

### Business Rule: BR-26 - Post-Settlement Client Projection

**Overview**:
`SendEtc` re-projects purse, experience and bonus counters after the sale.

**Detailed description**:

`SendEtc` (`SendFunc.cpp:1195-1231`) has its own guard chain (`conn` range, `Mode == USER_PLAY`, live socket) and then emits `MSG_UpdateEtc` carrying `MOB.Exp`, `SpecialBonus`, `LearnedSkill`, `ScoreBonus`, `SkillBonus`, `MOB.Coin`, `extra.Hold` and `MOB.Magic`. The only field this component changed is `Coin`, so the other seven are redundant re-transmissions - harmless, but they confirm that the projection layer is coarse-grained (one "update everything" message) rather than field-selective.

Only the seller receives the update; the merchant is never notified, and no observer learns about the gold. Combined with BR-24 (echo) and BR-25 (equip refresh) this completes the response contract: **three** packets on a normal sale (echo, optional equip, etc.), plus a fourth only in the rejection paths (`MSG_MessagePanel`, `MSG_Standard CloseShop`, `MSG_SetHpMode`, `MSG_SendItem` resync).

If `SendEtc` fails (buffer full, socket dead) it calls `CloseUser`, so a sale can terminate the session. That is the only path in the component that can disconnect a legitimate player.

**Rule workflow**:
```
SendEtc(conn) -> MSG_UpdateEtc{Exp, Coin, SpecialBonus, ScoreBonus, SkillBonus, Hold, Magic, Learn}
              -> on AddMessage failure: CloseUser(conn)
```

---

### Business Rule: BR-27 - Index-Only Valuation

**Overview**:
Price is a function of the item index alone; instance state is ignored.

**Detailed description**:

`:141` reads `g_pItemList[Item->sIndex].Price` and nothing else from `Item`. The six effect bytes (`STRUCT_ITEM.stEffect[0..2]`), which encode sanctification, bonus damage, elemental attributes and, for some items, the stack/charge amount, are used only for the audit string (`BASE_GetItemCode`) and for two binary filters (the volatile test in BR-13 and the index blacklist in BR-09). A +15 weapon with three perfect bonus rolls sells for the same amount as a clean one.

This is consistent with the rest of the shop economy - `_MSG_Buy` also prices from `g_pItemList[itemIndex].Price` (`_MSG_Buy.cpp:104`) - and it is what makes shop prices predictable to players. It also creates a permanent value gap: the entire enhancement economy (sanctification, bonus rolling, extraction) has no liquidation route to an NPC, so enhanced items can only be monetised through player-to-player trade or auto-trade, at whatever price the market bears.

The rule interacts with BR-15's tiering in an economically interesting way: because instance quality is free of charge at the vendor, the *optimal* strategy for a shop-bound player is to sell the cheapest possible instance of an expensive index. Since only the index affects the payout, an item whose list price sits just below a tier cliff is worth more per unit of market value than one above it, reinforcing the cliff incentive described in BR-15.

**Rule workflow**:
```
price = f(g_pItemList[sIndex].Price) only
effects, sanc, amount, grade -> unused for valuation
```

---

### Business Rule: BR-28 - No Minimum Sale Price

**Overview**:
A zero-price item is sold for zero coin and destroyed.

**Detailed description**:

There is no lower bound on `sPrice` other than the negative test at `:170`. If `g_pItemList[sIndex].Price` is 0 (a permitted value: `nprice` is parsed with `sscanf` and stored without validation, `Basedef.cpp:5062`/`:5113`), the whole pipeline degenerates gracefully: `/4` is 0, no tier fires, the haggling guard `sPrice > 1000` fails, the negative test passes, the tax multiplies zero, `tcoin` is 0 so both treasury blocks are skipped by `tcoin > 0`, the coin ceiling test succeeds, and `Coin += 0` is a no-op before the item is zeroed. The player receives a confirmation echo and an `MSG_UpdateEtc` with no change in balance.

This matters because `itemlist.csv` is external, hand-edited data with no schema validation in the loader (the only validations are `Index != -1`, `Index < MAX_ITEMLIST` and a name-length clamp, `Basedef.cpp:5065-5083`). A typo that zeroes a price converts a valuable item into a free deletion - the worst possible failure mode for a sell command, because the destroy action is irreversible and silent (the log records `price:0`).

The correct guard would be a floor check (reject when `sPrice <= 0`), which the code does not have; instead the design tolerates zero as "worthless but sellable". Compare the coin-bag pickup path, which refuses at a ceiling but has no floor either (`_MSG_GetItem.cpp:190-196`).

**Rule workflow**:
```
Price = 0 -> sPrice = 0 -> no modifier applies -> tax = 0 -> tcoin = 0
          -> Coin += 0 -> log "sel_npc ... price:0" -> item cleared
```

---

### 3.3 Cross-component consistency matrix

The Sell Handler is one of six components that move items or gold. Their policies should agree; where they do not, the divergence is a defect in at least one of them.

| Policy | Sell (`_MSG_Sell`) | Buy (`_MSG_Buy`) | AutoTrade buy (`_MSG_ReqBuy`) | Trade (`_MSG_Trade`) | Autotrade list (`_MSG_SendAutoTrade`) | Drop (`_MSG_DropItem`) |
|--------|--------------------|------------------|-------------------------------|----------------------|----------------------------------------|------------------------|
| Blocks crown 747 | yes (`:92`) | n/a | yes (`:80`) | yes (`:147`) | yes (`:80`) | yes (`:110`) |
| Blocks 3993/3994 | no | n/a | yes (`:80`) | yes (`:147`) | yes (`:80`) | yes (`:110`) |
| Blocks 3193/3194 mantles | yes (`:92`) | n/a | no | no | no | no |
| Blocks 508/509/522/446/526-531 | no | n/a | yes (`:80-81`) | no | yes (`:80-81`) | yes (`:110`) |
| Consults `EF_NOTRADE` | no | no | no | no | yes (`:84`) | no |
| Consults `EF_VOLATILE` | yes, `== 1` (`:133`) | no | no | no | no | no |
| Treasury guild-ownership check | no (`:199`) | yes (`:260`, `:267`) | yes (`:168`, `:174`) | n/a | n/a | n/a |
| Treasury cap | 2e11 / 2e13 (`:201`, `:209`) | 2e11 / 2e11 | 2e11 / 2e11 | n/a | n/a | n/a |
| Tax split | tax/4 to guild, 3/4 destroyed | tax/2 village, tax to capital | tax/2 village, tax to capital | n/a | n/a | n/a |
| Coin ceiling 2e9 | clamps, sale proceeds (`:215`) | not checked | rejects with message (`:98`) | n/a | n/a | n/a |
| Forced persistence | no | no | yes (`:163`) | yes (`:379`) | no | no |
| Audit channel | `Log` (`:224`) | `ItemLog` (`:291`) | `ItemLog` (`:135`) | `ItemLog` | `Log`/`ItemLog` | `ItemLog` |
| Trade-conflict handling | tears down both | tears down both | returns | n/a | tear down | tears down both |

---

## 4. Component Structure

### 4.1 Physical structure

```
Code/TMSrv/
├── _MSG_Sell.cpp                 236 lines / 5,916 bytes - THE COMPONENT (single free function)
│   ├──  1-17   GPL header (2015, VK / Charles TheHouse)
│   ├── 18      #include "ProcessClientMessage.h"     <- the only include
│   ├── 20      void Exec_MSG_Sell(int conn, char *pMsg)
│   ├──  22     MSG_Sell *m = (MSG_Sell*)pMsg;        <- trusted in-place cast, no size check
│   ├──  24-42  Stage 1: session, trade and shop-mode gates
│   ├──  44-82  Stage 2: container/slot authorisation (+ isEquip flag)
│   ├──  84-93  Stage 3: item resolution and blacklist
│   ├──  96-113 Stage 4: merchant relationship (skippable via LABEL_S1)
│   ├── 115     LABEL_S1: post-validation join point
│   ├── 117-139 Stage 5: name capture, empty-slot repair, volatile filter
│   ├── 141-174 Stage 6: valuation (base, tiers, haggling, sign check)
│   ├── 177-214 Stage 7: geography, tax, treasury distribution
│   ├── 213     LABEL_S2: settlement join point (inside the tax block)
│   ├── 215-224 Stage 8: coin credit (clamped) and audit log
│   └── 226-235 Stage 9: item destruction, echo, equip refresh, etc update
├── ProcessClientMessage.cpp      :230-232  sole call site (case _MSG_Sell)
├── ProcessClientMessage.h        :110      declaration; :29-43 aggregate include surface
├── ProcessClientMessage.h        :64       extern int GuildImpostoID[MAX_GUILDZONE]
├── ProcessClientMessage.h        :68       extern CUser pUser[MAX_USER]
└── (sibling handlers with identical shape: _MSG_Buy.cpp, _MSG_ReqBuy.cpp, _MSG_Trade.cpp, ...)

Code/
├── Basedef.h   :1760-1767  _MSG_Sell constant + MSG_Sell struct (18 bytes, naturally aligned)
├── Basedef.h   :175-181    MORTAL=2 / CELESTIAL=3
├── Basedef.h   :75-88      container constants and MAX_* limits
├── Basedef.h   :398-481   STRUCT_ITEM / STRUCT_MOB (Class, LearnedSkill, Coin, Exp, Merchant)
├── Basedef.h   :714-744   STRUCT_GUILDZONE (CityLimit*, CityTax)
├── Basedef.h   :882-906   STRUCT_ITEMLIST (Price, nUnique, nPos, Grade)
├── Basedef.cpp :47-54     g_pGuildZone[5] initialiser (default CityTax = 5)
├── Basedef.cpp :753       STRUCT_ITEMLIST g_pItemList[MAX_ITEMLIST]
├── Basedef.cpp :1479-1495 BASE_GetItemCode
├── Basedef.cpp :1647-1842 BASE_GetItemAbility   (EF_VOLATILE = 38)
├── Basedef.cpp :2340-2372 GetItemPointer
├── Basedef.cpp :4258-4267 BASE_GetVillage
├── Basedef.cpp :4281-4291 BASE_ClearItem
├── Basedef.cpp :4963-5134 itemlist.csv loader (Price source)
└── TMSrv/
    ├── Server.cpp :270      CMob pMob[MAX_MOB]
    ├── Server.cpp :277      char temp[4096]
    ├── Server.cpp :387      int GuildImpostoID[MAX_GUILDZONE]
    ├── Server.cpp :684-707  AddCrackError
    ├── Server.cpp :6701-6737 Log
    ├── Server.cpp :7319-7352 RemoveTrade
    ├── Server.cpp :9264-9284 ItemLog  (NOT used by this component)
    ├── SendFunc.cpp :27-45   SendClientMessage
    ├── SendFunc.cpp :197-206 SendClientSignal
    ├── SendFunc.cpp :1055-1082 SendItem
    ├── SendFunc.cpp :1084-1134 SendEquip
    ├── SendFunc.cpp :1195-1231 SendEtc
    ├── SendFunc.cpp :1622-1645 SendHpMode
    ├── GetFunc.cpp :640-647  GetInView
    ├── Language.h :136/167/170/190 message index definitions
    └── ProcessSecMinTimer.cpp :169-220 rolling save + socket flush (settlement consequence)
```

### 4.2 Symbol inventory inside the component

| Kind | Symbol | Storage | Scope |
|------|--------|---------|-------|
| Function | `Exec_MSG_Sell` | external linkage | declared in `ProcessClientMessage.h:110`, defined here |
| Local | `m` | stack, alias into the receive buffer | `MSG_Sell*`; the buffer is re-used at `:228` |
| Local | `sTargetID`, `sMyType`, `sMyPos` | stack, but `unsigned short`/`short` widened to `int` by the client's struct | `:44-46` |
| Local | `isEquip` | stack | `:47`, set at `:80-81` |
| Local | `Item` | stack pointer into a global array | `:84`; never re-seated |
| Local | `SellName[256]` | stack, 256 bytes, **not initialised** | `:117`; 16 bytes copied, terminator not guaranteed |
| Local | `Vol` | stack | `:133` |
| Local | `sPrice` | stack | `:141`, mutated six times |
| Local | `sIndex` | stack, copy of `Item->sIndex` | `:142`, used only by the 693-695 test |
| Local | `special`, `discount` | stack, inner scope of the haggling block | `:156-166` |
| Local | `sVillage` | stack | `:177` |
| Local | `CityTax` | stack | `:182` |
| Local | `fPrice` | stack, inner scope of the tax block | `:186` |
| Local | `tcoin` | stack, inner scope | `:196` |
| Local | `tmplog[2048]` | stack | `:220` |
| Labels | `LABEL_S1`, `LABEL_S2` | function scope | `:115`, `:213`; both targeted by `goto` |

The function has **no parameters other than `conn` and the raw packet**, **no return value**, and **no error channel**. Every failure is expressed as `return` (silent) or as a client message. There is no way for the caller - or for test code - to determine whether a sale occurred.

### 4.3 Include surface

`_MSG_Sell.cpp:18` includes only `ProcessClientMessage.h`, which transitively pulls in `Windows.h`, five CRT headers, `Basedef.h`, `CPSock.h`, `ItemEffect.h`, `Language.h` and eight TMSrv headers (`ProcessClientMessage.h:29-43`). The component therefore compiles against the **entire** server domain model: `STRUCT_MOB`, `STRUCT_ITEMLIST`, `CMob`, `CUser`, `CNPCGenerator`, `CCastleZakum`, `CWarTower`. This is the documented architectural risk "aggregate header couples all 60 handlers to 14 subsystems" and it is why a change to any struct in the project forces recompilation of this file.

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains

```
ProcessClientMessage (dispatcher)
└── Exec_MSG_Sell                                   _MSG_Sell.cpp:20
    ├── SendHpMode                                  SendFunc.cpp:1622   [BR-01]
    ├── AddCrackError ──> Log ──> fprintf/SetWindowTextA   Server.cpp:684 / :6701
    ├── RemoveTrade ──> SendClientSignal, GetCreateMob, GridMulticast   Server.cpp:7319
    ├── SendClientMessage ──> CPSock::AddMessage     SendFunc.cpp:27
    ├── GetItemPointer                               Basedef.cpp:2340   [BR-04/08]
    ├── GetInView                                    GetFunc.cpp:640    [BR-12]
    ├── SendClientSignal                             SendFunc.cpp:197   [BR-12]
    ├── BASE_GetItemAbility ──> BASE_GetItemSanc, BASE_GetItemAbilityNosanc
    │                                                    Basedef.cpp:1647
    ├── BASE_GetVillage                              Basedef.cpp:4258   [BR-18]
    ├── BASE_GetItemCode ──> g_pItemList[].Name      Basedef.cpp:1479   [BR-23]
    ├── BASE_ClearItem                               Basedef.cpp:4281   [BR-23]
    ├── SendEquip ──> BASE_VisualItemCode, BASE_VisualAnctCode, GridMulticast
    │                                                    SendFunc.cpp:1084
    └── SendEtc ──> CPSock::AddMessage, CloseUser    SendFunc.cpp:1195   [BR-26]
```

Data dependencies (globals read or written, not functions):

```
Exec_MSG_Sell
├── reads  pUser[conn].{Mode, Trade.OpponentID, TradeMode, Cargo[], AccountName, IP, cSock}
├── writes pUser[conn].{cSock, NumError(indirect), Trade/AutoTrade(indirect)}
├── reads  pMob[conn].MOB.{CurrentScore.Hp, Class, LearnedSkill, CurrentScore.Special[2], Coin}
├── reads  pMob[conn].MOB.{Equip[], Carry[]}  (through GetItemPointer)
├── writes pMob[conn].MOB.{Coin, Equip[] | Carry[]}
├── reads  pMob[TargetID].{MOB.Merchant, MOB.MobName, TargetX, TargetY}
├── reads  g_pItemList[sIndex].Price
├── reads  g_pGuildZone[v].CityTax
├── writes pMob[GuildImpostoID[0..4]].MOB.Exp
├── reads  g_pMessageStringTable[116, 167, 170]
└── writes temp[4096]
```

### 5.2 External dependencies

| Dependency | Kind | Used for | Where |
|------------|------|----------|-------|
| `../../Common/itemlist.csv` | Local data file (absent from repo) | Every price in the component | `Basedef.cpp:4971`, consumed at `_MSG_Sell.cpp:141` |
| `../../Common/extraitem.csv` | Local data file (absent) | Overrides item rows (`Build = 0`) | `Basedef.cpp:4982-4984` |
| `../../Common/Language.txt` | Local data file (absent) | Text behind message indices 116/167/170 | `Basedef.cpp:7160` + `g_pMessageStringTable` |
| `Guild.txt` (in `TMSRV/Run`) | Local data file (absent) | `g_pGuildZone[].ChargeGuild` and `CityTax` | `CReadFiles.cpp:785-821` |
| `./npc/<MobName>` | Local binary files (absent) | Treasury EXP persistence | `ProcessSecMinTimer.cpp:2082-2104` |
| `pKeyWord[512]` | In-memory obfuscation table | Packet framing for every outbound message | `CPSock.cpp:535-541` |
| Win32 winsock (`recv`/`send`) | OS | Transport | `CPSock.cpp:337-345`, `:617-700` |
| Win32 user32 (`SetWindowTextA`) | OS | Log echo to the server console window | `Server.cpp:6735` |
| MSVC CRT (`sprintf`, `strncpy`, `memcmp`) | Runtime | Formatting and copies | throughout |
| `DBSrv` (via `MSG_DBSaveMob`) | Internal service, **not** called here | Persistence of the resulting coin/item state | indirect via `SaveUser` |

### 5.3 Dependency quality observations

* **No abstraction layer between the handler and the domain.** Every rule is expressed as direct global-array access; there is no repository, no service interface, no value object. This is the single biggest factor in the component's untestability (§11.3).
* **The handler depends on data it never validates.** `g_pItemList[].Price` comes from a hand-edited CSV with no schema check, and the component applies no floor, no ceiling and no unit sanity. `g_pGuildZone[].CityTax` is validated by two *different* ranges in two other components and read here without re-validation beyond a broad window.
* **The handler trusts a `reinterpret_cast` of the receive buffer without a length check.** `MSG_Sell *m = (MSG_Sell*)pMsg` (`:22`) is followed by reads at offsets 12, 14 and 16. The transport guarantees only `12 <= Size <= MAX_MESSAGE_SIZE` (`CPSock.cpp:397`), and `BASE_CheckPacket` - the per-message size validator - is disabled in this build; a 12-byte frame therefore causes a 6-byte over-read of the receive buffer. In practice the buffer is a large heap allocation so the read stays mapped, but the values are stale bytes from a previous packet, i.e. attacker-influenced in a multi-packet sequence.
* **Dependency direction is strictly downward** (dispatcher → handler → domain → transport), with the single exception of `RemoveTrade`, which reaches sideways into session lifecycle and grid state, and of the direct `pUser[conn].cSock.AddMessage` call at `:228` which bypasses the `SendFunc` projection layer entirely.

---

## 6. Afferent and Efferent Coupling

Coupling is reported at two granularities. The architectural report's figures (Ca = 1, Ce = 14) are **module** granularity; the tables below add **function/symbol** granularity.

### 6.1 Component-level (module granularity)

| Component | Afferent (Ca) | Efferent (Ce) | Instability Ce/(Ca+Ce) | Criticality |
|-----------|---------------|---------------|------------------------|-------------|
| Sell Handler (`_MSG_Sell.cpp`) | 1 (`ProcessClientMessage.cpp:231`) | 14 modules | 0.93 | High (economic integrity, remote crash surface) |

Instability 0.93 places the component at the extreme "unstable" end: nothing depends on it, it depends on almost everything. That is normal for a command handler, but combined with a `void` return and silent failure paths it means a defect here is invisible to its only caller and manifests only as corrupted wallets or lost items.

Cross-check: a whole-project grep for `Exec_MSG_Sell` returns exactly two hits - the definition (`_MSG_Sell.cpp:20`) and the dispatcher call (`ProcessClientMessage.cpp:231`) - confirming Ca = 1 with no indirect re-entry (unlike the Attack Handler, which is re-entered by mob AI).

### 6.2 Symbol-level coupling

| Symbol | Kind | Afferent (distinct callers) | Efferent | Criticality | Notes |
|--------|------|-----------------------------|----------|-------------|-------|
| `Exec_MSG_Sell` | function | **1** | 22 symbols / 10 files | Critical | The component; no return value |
| `GetItemPointer` | function | 6 (Sell, Buy-side, DropItem, PutoutSeal, UseItem, GetItem) | `STRUCT_MOB`, `STRUCT_ITEM` | High | Does not reject empty slots |
| `BASE_GetItemAbility` | function | very high (~112 call sites) | `g_pItemList`, `g_pMount*` | Critical | Effect 38 path has the sanc exemption at `:1813` |
| `BASE_GetVillage` | function | 15 | `g_pGuildZone` | High | Returns 5 for "outside", which is also `MAX_GUILDZONE` |
| `BASE_GetItemCode` | function | 22 | `g_pItemList`, 2048-byte caller buffer | Medium | `sprintf` into caller-provided storage; no length parameter |
| `BASE_ClearItem` | function | 17 | the item | High | Total, unconditional zeroing |
| `GetInView` | function | ~5 | `pMob[].TargetX/Y` | Medium | Rectangular envelope, no LOS |
| `SendClientMessage` | function | 64 | `CPSock`, `MSG_MessagePanel` | Medium | Copies 96 bytes, forces terminators |
| `SendClientSignal` | function | 22 | `CPSock`, `MSG_STANDARD` | Medium | 8-byte frame |
| `SendItem` | function | 36 | `CPSock`, `MSG_SendItem` | Medium | Guard chain: conn, mode, socket |
| `SendEquip` | function | 11 | `GridMulticast`, `BASE_Visual*` | Medium | O(observers) fan-out |
| `SendEtc` | function | 24 | `CPSock`, may `CloseUser` | Medium | Only disconnecting sender in the component |
| `SendHpMode` | function | 28 | `CPSock`, `MSG_SetHpMode` | Low | Self-suppressing when `Mode != USER_PLAY` |
| `RemoveTrade` | function | 91 | `SendClientSignal`, `GetCreateMob`, `GridMulticast` | High | Side effects far beyond "remove trade" |
| `AddCrackError` | function | 44 | `Log`, `CharLogOut` | Medium | Threshold 2e9 makes it a logger in practice |
| `Log` | function | ~332 | `fLogFile`, `SetWindowTextA` | Low | Fixed 1024-byte internal buffer, overlapping `sprintf` |
| `ItemLog` | function | ~200 call sites | `fItemLogFile` | n/a | **Not used by this component** (BR-23) |
| `CPSock::AddMessage` | method | everywhere | send ring buffer | High | Can fail silently at this call site (`:228`) |
| `STRUCT_MOB` | struct | ~15 handlers | - | Critical | Component reads 7 fields, writes 2 |
| `STEP_ITEMLIST` (`STRUCT_ITEMLIST`) | struct | ~20 | - | Critical | Only `.Price` and `.Name` are consumed here |
| `STRUCT_GUILDZONE` | struct | ~8 | - | High | Only `.CityTax` and the four `.CityLimit*` are consumed |

### 6.3 Cohesion assessment

Measured as a unit, `Exec_MSG_Sell` has **low cohesion**: it is a single function serving at least seven separable responsibilities (session gating, container authorisation, merchant relationship, item policy, pricing, fiscal policy, settlement/projection). Measured per region, cohesion is high - §4.1's stage boundaries are clean and the ordering between them is deliberate.

The critical structural property is that the **regions cannot be exercised independently**. The pricing function (a pure `int -> int` mapping of `Price`, `Class`, `LearnedSkill`, `Special[2]`, `CityTax`) is buried between index blacklists and coin clamps, so it cannot be evaluated without a live `pMob[conn]`. The fiscal function (a pure mapping of `price`, `CityTax`, treasury state) is likewise inline and additionally entangled with `goto`. This is why the price cliffs of BR-15 and the treasury asymmetries of BR-20/21 survived unnoticed: there is no seam at which to state them.

The `goto LABEL_S2` construct deserves explicit comment under cohesion: it makes the settlement block reachable from *outside* the block that lexically contains it, so the tax region and the settlement region are coupled in the control-flow graph in a way that is invisible from either region's code. This is the mechanism behind finding 4 (silent void sale) and behind the undefined `g_pGuildZone[5]` read described in BR-18.

---

## 7. Endpoints

The component exposes no HTTP, GraphQL, gRPC or named-pipe surface. It is a **binary packet handler** in the custom TMSrv TCP protocol. There is exactly one inbound message type and five outbound message types.

### 7.1 Inbound endpoint

| Endpoint (message) | Opcode (decimal) | Direction | Wire struct | Size | Handler |
|--------------------|------------------|-----------|-------------|------|---------|
| `_MSG_Sell` | 890 (`122 \| 0x0100 \| 0x0200`) | client → game | `MSG_Sell` (`Basedef.h:1761-1767`) | 18 bytes (naturally aligned, no `#pragma pack`) | `Exec_MSG_Sell` (`_MSG_Sell.cpp:20`) |

### 7.2 Inbound field contract

| Field | Type | Offset | Consumed as | Validated by the handler |
|-------|------|--------|-------------|--------------------------|
| `Size` | `short` | 0 | Not read (transport only) | Not cross-checked against 18 |
| `KeyWord` | `char` | 2 | Not read | Obfuscation key, transport-level only |
| `CheckSum` | `char` | 3 | Not read | Transport-level only; `BASE_CheckPacket` disabled |
| `Type` | `short` | 4 | Dispatch key | Exact match in the dispatcher (`ProcessClientMessage.cpp:230`) |
| `ID` | `short` | 6 | Not read; echoed back unchanged at `:228` | Caller bounds-checks `[0, MAX_USER)`; not revalidated here, not rewritten by `AddMessage` |
| `ClientTick` | `unsigned int` | 8 | **Never read** | Dispatcher rejects `SKIPCHECKTICK` for client-originated packets; otherwise unvalidated |
| `TargetID` | `unsigned short` | 12 | Merchant entity index, or arbitrary id under BR-10 | `[1000, 25000)` **only when BR-10 is false** |
| `MyType` | `short` | 14 | Container selector | Exactly `{0,1,2}` (`:61`) |
| `MyPos` | `short` | 16 | Slot index within the container | `[0,60)` / `[0,128)` / `[1,16)` per container |

There is no anti-replay, no nonce, no sequence number and no signature. The `ClientTick` field, which every other handler uses for rate or desync detection, is ignored entirely here - the Sell Handler is one of the few that perform no timing validation at all.

### 7.3 Outbound endpoints (emitted by this component)

| Message | Opcode | Direction | Trigger | Recipients | Fields |
|---------|--------|-----------|---------|------------|--------|
| `_MSG_Sell` (echo) | 890 | game → client | settlement (`:228`) | seller | The client's own `ID`, `TargetID`, `MyType`, `MyPos`, with server-generated `Size`/`KeyWord`/`CheckSum`/`ClientTick` |
| `_MSG_UpdateEtc` | `_MSG_UpdateEtc` constant, `SendFunc.cpp:1210` | game → client | settlement (`:233`) | seller | `Exp`, `Coin`, `SpecialBonus`, `ScoreBonus`, `SkillBonus`, `Learn`, `Hold`, `Magic` |
| `_MSG_UpdateEquip` | `SendFunc.cpp:1089` | game → grid | settlement of a visual equip slot (`:231`) | every observer within the grid cell, seller skipped | 16 visual codes + 16 ancient codes |
| `_MSG_MessagePanel` | `SendFunc.cpp:36` | game → client | rejection: `_NN_CantWhenAutoTrade` `:40`, `_NN_Only_Merchant_Buy_Yours` `:106`, `_NN_Cant_Sell_Potion` `:137` | seller | 96-byte string plus two forced terminators |
| `_MSG_Standard` (`_MSG_CloseShop` = 406) | `SendFunc.cpp:199-205` | game → client | rejection: target out of view `:111` | seller | `Type = 406`, `ID = 30000` |
| `_MSG_SetHpMode` | `SendFunc.cpp:1636` | game → client | rejection: dead or not playing `:26` | seller | `Hp`, `Mode` |
| `_MSG_SendItem` | `SendFunc.cpp:1069` | game → client | repair: empty carry slot `:128` | seller | `invType`, `Slot`, `STRUCT_ITEM` |
| signal `900` (via `MSG_Standard`) | `Server.cpp:7337` | game → client | indirect, when a trade is torn down `:33-34` | seller and partner | bare standard frame |

### 7.4 Protocol observations

* **No response on 7 of 19 exits** (§2.3), so the endpoint has no uniform acknowledgement contract. A client cannot distinguish "silently refused" from "still processing".
* **The outbound confirmation is the inbound packet.** Clients must treat `_MSG_Sell` in the game→client direction as a state change instruction (BR-24). Any client that only expects `_MSG_SendItem`/`_MSG_UpdateEtc` will display a phantom item.
* **Observers are notified only for visual equipment.** No broadcast occurs for carry/cargo sales, so nearby clients see no evidence that another player just sold something - there is no "sold item" animation packet in this path (contrast the autotrade path's `_MSG_ItemSold` broadcast at `_MSG_ReqBuy.cpp:182-191`).
* **The opcode is bidirectional by design** (`Basedef.h:1760`), which is unusual: `_MSG_Buy` is also bidirectional but the buy handler *mutates* the struct before echoing (adding `Coin`), whereas the sell handler echoes without mutation.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol / mechanism | Data format | Error handling |
|-------------|------|---------|----------------------|-------------|----------------|
| Client socket (player → TMSrv) | Inbound network | Sale requests | Custom TCP, `CPSock::ReadMessage` | Binary 18-byte packet | Dispatcher envelope checks; handler returns `void`; no length validation |
| Client socket (TMSrv → seller) | Outbound network | Slot clear, coin/exp projection, rejections | `CPSock::AddMessage` + 500 ms flush sweep | Binary packets | `AddMessage` failure ignored at `:228`; `SendEtc` failure calls `CloseUser` |
| Client sockets (TMSrv → grid observers) | Outbound network | Appearance refresh | `SendEquip` → `GridMulticast` | `MSG_UpdateEquip` | None; no delivery confirmation |
| `itemlist.csv` / `extraitem.csv` | Local data file | `Price` and `Name` of every item | `fopen`/`fgets`/`sscanf` at boot (`Basedef.cpp:4963-5134`) | CSV, comma-translated to spaces, 12 effect pairs | `MessageBox` + return on missing file; **no numeric validation of `Price`** |
| `Language.txt` | Local data file | Text for indices 116, 167, 170 | `BASE_InitializeMessage` into `g_pMessageStringTable` (128 bytes/entry) | Newline-delimited UTF-8/CP949 | Missing file → empty strings; no bounds check on user-visible text |
| `Guild.txt` | Local data file | `CityTax` and `ChargeGuild` per city | `CReadFiles::ReadGuildFile` (`CReadFiles.cpp:785-821`) | Whitespace-separated integers | Clamps `CityTax` to `[0,20]`, default 10 |
| Treasury NPC record (`./npc/<name>`) | Local binary file | Persistent guild treasury (EXP) | `_open`/`_write` of `STRUCT_MOB` every minute (`ProcessSecMinTimer.cpp:2096-2103`) | Raw struct image | Log on open failure, then `return` |
| `GuildImpostoID[5]` | In-memory registry | Maps city index to treasury NPC | Populated at NPC spawn when `Merchant == 6` (`Server.cpp:3190-3195`) | `int` mob index | Bounds-tested by the consumer (`>= MAX_USER`, `< MAX_MOB`) but **not** ownership-tested |
| `pMob[]` shared entity array | In-memory | Merchant identity, position, treasury state | Direct array access | `CMob` (≈1.5 KB each) | Range-checked only on the non-privileged path |
| `pUser[]` session array | In-memory | Session mode, containers, trade state | Direct array access | `CUser` | `conn` comes from the socket, never the packet |
| `DBSrv` (persistence) | Internal service | Save the resulting wallet/container state | Indirect: rolling `SaveUser` → `MSG_DBSaveMob` (`ProcessSecMinTimer.cpp:189`) | Binary message | No acknowledgement inside this component; **no forced save** |
| `_MSG_Challange` | Internal service (consumer of treasury) | Converts `GuildImpostoID[].MOB.Exp` into challenge currency at 1e9 per unit (`_MSG_Challange.cpp:52-102`) | Direct array access | `long long` | Treasury balance zeroed on win (`:71`) |
| File log (`fLogFile`) | Filesystem | Audit of every sale (`sel_npc,`) and every rejection | `Log()` → `fprintf` + `SetWindowTextA` | Plain text, 1024-byte internal buffer | `fLogFile` null-checked; shared global `temp` reused |
| Item log (`fItemLogFile`) | Filesystem | **Not used** - see BR-23 | `ItemLog()` | Plain text | n/a |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose | Assessment |
|---------|----------------|----------|---------|------------|
| **Command Handler / Message Endpoint** | One `Exec_MSG_*` per opcode, dispatched by a flat `switch` | `ProcessClientMessage.cpp:230-232` | Decouple socket from game logic | Consistent with the other 59 handlers; zero abstraction, `void` return |
| **Transaction Script** | All sale logic in one sequential procedure with no domain objects | whole function | Keep the economy path in one readable place | Readable, but not testable or reusable; the "script" is 217 lines with 7 responsibilities |
| **Guard Clause (fail-fast)** | 19 early `return`s | `:24-174` | Flatten control flow | Mostly consistent, but 7 guards give the client no feedback |
| **`goto` Join Point** | `LABEL_S1` skips merchant policy; `LABEL_S2` skips tax | `:97`, `:115`, `:179`, `:213` | Share the post-validation and settlement tails | `LABEL_S1` is the enabler of the OOB read (BR-10); `LABEL_S2` jumps *into* a block, making settlement depend on a block it is not lexically part of |
| **Policy Magic Numbers** | 3193/3194/747 blacklist; 693-695 exclusion; 1000/5000/10000/100000 thresholds; 2000000000 coin cap; 2e11/2e13 treasury caps | `:92`, `:146-154`, `:215`, `:201`, `:209` | Encode game balance directly in code | No named constants, no data table; three of these numbers disagree with sibling components (§3.3) |
| **Tiered Pricing Table (implicit)** | Nested `if` chain on the quartered price | `:146-152` | Progressive bulk discount | The tiers are correct in intent, non-monotonic at the boundaries, and one branch is a no-op |
| **Proportional Uplift (charisma)** | `(discount + 50) * price / 50` | `:166` | Stat-driven price bonus without division loss | Multiply-before-divide is the correct ordering; the same derivation is duplicated (unused) in `_MSG_Buy.cpp:135-145` |
| **Attribute-Driven Filtering** | `BASE_GetItemAbility(item, EF_VOLATILE)` | `:133` | Classify items by template attribute | The **one** place the component uses the domain's attribute system - and it does not use `EF_NOTRADE`, which is the attribute designed for this purpose |
| **Sentinel Protocol (negative price)** | `sPrice < 0` after arithmetic | `:170`, `:194` | Detect overflow without `__builtin_*_overflow` | Adequate for the two sites it guards; misses the positive-wrap case |
| **Clamp-on-Range (coin ceiling)** | `Coin + price < 2e9 ? add : set 2e9` | `:215-218` | Enforce the 2 G purse limit | Clamping instead of rejecting destroys value; the comparison itself can overflow |
| **Money Sink (tax destruction)** | `tcoin = tax/4`, remainder never credited | `:196-211` | Combat inflation | Deliberate and effective: 75% of every city sale-tax leaves the economy permanently |
| **Fiscal Federation (dual treasury)** | Village treasury plus capital treasury both paid | `:199-211` | Share tax between city guild and capital | Implemented without the ownership check the buy-side applies; double-pays when `sVillage == 4` |
| **Client Projection / Shadow State** | `SendEtc`, echo, `SendEquip` | `:228-233` | Keep client view in sync with server truth | Coarse-grained "send everything" plus a packet echo that doubles as a slot-clear instruction |
| **Singleton Global State** | `pMob`, `pUser`, `g_pItemList`, `g_pGuildZone`, `GuildImpostoID`, `temp` | throughout | Avoid parameter plumbing in a single-process server | The root cause of the untestability recorded in §11.3 |
| **Centralised Transport Obfuscation** | `pKeyWord` XOR table applied by `AddMessage` | `CPSock.cpp:535-560` | Light anti-tamper | Symmetric and uniform; no per-message authentication |

### 9.1 Architectural decisions embedded in the component

1. **Vendor shops have infinite liquidity and unlimited stock appetite.** No merchant-side state is read or written except identity and position. This is the original game's design and it is what makes NPC sale a reliable gold faucet.
2. **The seller is trusted for the container/slot, the server decides everything else.** Item identity, price and tax all come from server state; the client only names a slot. This is the correct trust split, and the one place it fails (`TargetID` under BR-10) is the one place the server forgot to apply it.
3. **Appearance is derived, not stored per-viewer.** Because `SendEquip` recomputes from `MOB.Equip`, an incorrect `isEquip` decision produces stale rendering rather than corrupt state - a forgiving failure mode.
4. **Taxes are geographic, not political.** The jurisdiction is the merchant's tile, resolved by rectangle, not by guild ownership or character nationality. This makes the tax sensitive to vendor placement - and, because the `5` sentinel is mishandled (BR-18), a vendor outside every rectangle produces an undefined rate rather than a defined exemption.

---

## 10. Technical Debt & Risks

### 10.1 Risk register

| # | Risk level | Component area | Issue | Impact | Evidence |
|---|------------|----------------|-------|--------|----------|
| R-01 | **Critical** | BR-10 (Celestial bypass) | `goto LABEL_S1` skips the `TargetID` range check, `Merchant == 1` test and view check; the id is then dereferenced twice | Remote OOB read of `pMob[0..65535]` against a 25,000-element array → access violation (remote DoS) and log injection of adjacent memory | `:96-97`, `:115`, `:119`, `:177`; `Basedef.h:1764`; `Server.cpp:270` |
| R-02 | **High** | BR-18 | `g_pGuildZone[5]` is read when the merchant is outside all city rectangles (`MAX_GUILDZONE == 5`) | Undefined read; sale is either taxed from unrelated memory or silently dropped | `:182`; `Basedef.h:148`; `Basedef.cpp:4258-4267` |
| R-03 | **High** | BR-19 / settlement | Settlement is nested inside `if (CityTax >= 0 && CityTax <= 30)`; an out-of-window tax makes the sale vanish with no message, no log and no item clear | Player-visible dead shop; undiagnosable support case | `:184-235` |
| R-04 | **High** | BR-20 | Treasury credit omits `MOB.Guild == g_pGuildZone[v].ChargeGuild`; sibling handlers include it | Tax revenue routed to a displaced/wrong guild; economy leak | `:199`, `:207` vs `_MSG_Buy.cpp:260/267`, `_MSG_ReqBuy.cpp:168/174` |
| R-05 | **High** | BR-22 | `Coin + sPrice < 2000000000` uses signed `int`; overflow is UB and can produce a negative purse | Wallet corruption / duplication of value under pathological prices | `:215` vs `_MSG_ReqBuy.cpp:96` (uses `unsigned int`) |
| R-06 | **High** | BR-14/15/28 | Pricing has no floor and no ceiling validation; `itemlist.csv` `Price` is unvalidated at load; tier boundaries are non-monotonic | Zero-price items destroyed for nothing; a more expensive item can pay less | `:141-152`, `:170`; `Basedef.cpp:5062`, `:5113` |
| R-07 | **Medium** | BR-09 | Item blacklist is a hardcoded 3-index literal and does not use `EF_NOTRADE`; inconsistent with the blocklists in trade/drop/autotrade | Policy drift on every item addition; no-trade items can be monetised and destroyed | `:92`; §3.3 |
| R-08 | **Medium** | BR-23 | No `SaveUser` after settlement; relies on a rolling one-user-per-tick save and logout | Crash window re-creates the sold item while keeping the gold | `:226`; `ProcessSecMinTimer.cpp:169-199`; contrast `_MSG_ReqBuy.cpp:163` |
| R-09 | **Medium** | BR-23 | Audit goes to `Log()` (`fLogFile`) instead of `ItemLog()` (`fItemLogFile`) | Item-economy ledger has a hole for all NPC sales; reconciliation shows unexplained gold | `:224` vs `_MSG_Buy.cpp:291` |
| R-10 | **Medium** | BR-19 | Tax multiply `(100 - CityTax) * sPrice` overflows for post-tier prices above ~21.4 M (list price ~171.8 M) | Positive-wrap payout (gold creation) or negative abort | `:188-192`, `:194` |
| R-11 | **Medium** | BR-21 | Treasury caps differ by 100x between `[v]` (2e11) and `[4]` (2e13); siblings use 2e11 for both | Noatum treasury can accumulate 100x more challenge currency; unexplained economic asymmetry | `:201` vs `:209` |
| R-12 | **Medium** | BR-20 | When `sVillage == 4` both treasury blocks pay the Noatum NPC (double credit) | Unbalanced revenue between capital and cities | `:199-211` |
| R-13 | **Medium** | BR-15 | Two price cliffs invert monotonicity (20,000 vs 20,004; 40,000 vs 40,004) | Players can be paid less for a more valuable item; exploitable re-pricing incentive | `:146-152`, §3.2 table |
| R-14 | **Medium** | BR-16 | 693/694/695 exclusion is a hardcoded index triple; the bonus reads `Special[2]` with no upper clamp before `/10` | Unbounded stat growth (e.g. via buffs) saturates at 26% anyway, but the exclusion will silently go stale if the circle indices ever change | `:154-158` |
| R-15 | **Medium** | Input contract | `MSG_Sell*` cast without size validation; `BASE_CheckPacket` disabled | 6-byte over-read of the receive buffer for short frames; stale attacker-influenced values | `:22`; `CPSock.cpp:397`; §5.3 |
| R-16 | **Low** | BR-19 | `if (sPrice < 100000)` branch is arithmetically identical in both arms | Dead code that misleads readers into thinking precision differs | `:188-192` |
| R-17 | **Low** | BR-23 | `strncpy(SellName, MobName, 16)` into an uninitialised 256-byte buffer without an explicit terminator | `%s` may read past the copied region into stack garbage | `:117-119` |
| R-18 | **Low** | BR-23 | `Log()` does `sprintf(LogTemp, "%s %s %s \n", LogTemp, ...)` into a 1024-byte buffer with a 2048-byte source | Overlapping `sprintf` (UB) and potential stack overrun with long item names | `Server.cpp:6720` |
| R-19 | **Low** | BR-01 | `NumError >= 2000000000` sanction is unreachable in practice; low-value rejections are only logged | Anti-cheat is a logger, not a limiter | `Server.cpp:694-704` |
| R-20 | **Low** | BR-06/25 | `MyPos < 8` visual threshold and `MAX_CARRY-4` reserve are unexplained constants; mantle/mount sales never refresh appearance | Stale rendered cape/mount for observers | `:80-81`, `:230` |
| R-21 | **Low** | BR-19 | Loader clamps `CityTax` to `[0,20]`, guild command accepts `[0,30]` | A 21-30% tax is silently reset to 10% at next restart | `CReadFiles.cpp:811`; `_MSG_MessageChat.cpp:76` |
| R-22 | **Low** | BR-03 | `TradeMode` blocks only cargo sales; a shop owner may still liquidate carry/equip | Inconsistent shop state, but no duplication (the shelf is untouched) | `:38` |
| R-23 | **Low** | BR-11 | Merchant liveness (`Hp`, `Mode`) is not checked | A dead-but-resident NPC still buys | `:104` |
| R-24 | **Low** | Local `SellName` | `pMob[sTargetID].MOB.MobName` is 16 bytes with no guaranteed terminator source check | Log corruption when a name fills the field | `:119`; `Basedef.h:440` |

### 10.2 Debt summary by area

| Area | Count | Highest severity |
|------|-------|------------------|
| Validation / authorisation | 8 | Critical (R-01) |
| Pricing / valuation | 4 | High (R-06) |
| Fiscal (tax, treasury) | 6 | High (R-02, R-04) |
| Settlement / persistence / audit | 4 | Medium (R-08, R-09) |
| Input contract / memory safety | 4 | Medium (R-15) |
| Projection / UX | 3 | Low (R-20) |

### 10.3 Code smells observed

* **Magic numbers without names** - 15 distinct literals carrying business meaning (3193, 3194, 747, 693, 694, 695, 1000, 5000, 10000, 100000, 26, 6, 50, 2000000000, 200000000000, 20000000000000).
* **Long method** - 217 lines, cyclomatic complexity conservatively 34 (19 guarded exits, 15 boolean conjuncts), nesting depth 6.
* **Duplicated logic across siblings** - the haggling derivation appears in two handlers with different use; the blocklist appears in six with six different sets.
* **`goto` for controlled flow** - two labels, four jumps, one of which jumps into a block past variable declarations.
* **Dead branch** - `:188-192`.
* **Unreachable condition** - `sVillage < 0 || sVillage > 5` at `:178` cannot fire against `BASE_GetVillage`'s contract.
* **Asymmetric guard typing** - `MOB_EMPTY` here, `USER_EMPTY` in `_MSG_Buy` for the same constant 0.
* **Comment-free implementation** - the file contains no explanatory comments beyond the licence header; every constraint in this report was recovered from code or from sibling components.
* **Inconsistent error signalling** - four mechanisms for "no" (silent `return`, message panel, standard signal, resync `SendItem`) applied without a stated rule.
* **Primitive obsession** - containers, slots, cities, guilds and items are all bare `int`s with ranges enforced ad hoc at every use site.

---

## 11. Test Coverage Analysis

### 11.1 Direct answer: there are no tests

**No test file, test project, test harness, assertion, mock, fixture or CI pipeline exists anywhere in the repository for this component or for any other.** Evidence:

| Evidence | Observation |
|----------|-------------|
| `W2PP Code Project.sln:5-10` | Contains exactly two projects, `DBSrv` and `TMSrv`, plus one empty "Solution Items" folder. No test project, no `ProjectReference`. |
| Glob `**/*{test,Test,TEST,spec,Spec}*` over the whole workspace (ignoring `.agents`, `.codegraph`, `.opencode`, `openspec`) | **0 files found.** |
| Glob `**/*Sell*` over the whole workspace | Exactly 1 file: `Code/TMSrv/_MSG_Sell.cpp`. No `_test`, no `Test_Sell`, no fixture. |
| Directory census under `Code/` | 15 directories, all of them build-output artifacts (`Debug`, `Release`, `.tlog`, `TMSrv`, `DBSrv`, `run`). No `tests/`, `test/`, `spec/`, `unittest/`. |
| `Select-String` for `gtest` / `catch2` / `CppUnit` / `TestAdapter` / `MSTest` across `*.vcxproj` and `*.vcxproj.filters` | **0 matches.** |
| `Select-String` for `\bassert\s*\(` across `Code/TMSrv/*.cpp`, `Code/*.cpp`, `Code/TMSrv/*.h` | **0 matches.** No `_ASSERT`, no `static_assert`, no `verify()`. |
| Build/CI configuration (`*.yml`, `*.yaml`, `CMakeLists.txt`, `Makefile`) | **0 files.** No `.github/workflows`, no `azure-pipelines.yml`, no `.gitlab-ci.yml`. |
| `docs/` contents | Only agent reports: `MANIFEST.md`, one dependency audit, one architectural analysis, six component analyses. No test plan, no test data, no traceability matrix. |
| Commit history for this file | 3 commits total: `489c4e0 Add project files.`, `b030892 License update`, `e47bf4f Update _MSG_Sell.cpp`. The last one changed only the copyright line and the missing trailing newline (`git show e47bf4f`: 2 insertions, 3 deletions, all in the header/EOF). **The logic has never been modified since the 2018 decompilation import.** |

### 11.2 Coverage table

| Component area | Unit tests | Integration tests | Coverage | Test quality |
|----------------|------------|-------------------|----------|--------------|
| Session/liveness gate (BR-01) | 0 | 0 | 0% | None. Requires `pMob`/`pUser` seeding; no seam. |
| Trade-conflict handling (BR-02, BR-03) | 0 | 0 | 0% | None. `RemoveTrade` touches grid state and socket buffers. |
| Container/slot authorisation (BR-04..BR-07) | 0 | 0 | 0% | None. Would be the cheapest unit to test (pure integer ranges) if extracted. |
| Item resolution and repair (BR-08) | 0 | 0 | 0% | None. `GetItemPointer` is testable in isolation but has no test. |
| Blacklist (BR-09) | 0 | 0 | 0% | None. |
| Celestial bypass (BR-10) | 0 | 0 | 0% | None. **Highest-risk untested branch in the component.** |
| Merchant identification and proximity (BR-11, BR-12) | 0 | 0 | 0% | None. |
| Volatile filter (BR-13) | 0 | 0 | 0% | None. Requires `g_pItemList` from an absent CSV. |
| Base valuation and tiers (BR-14, BR-15) | 0 | 0 | 0% | None. Pure `int -> int`; would be trivial to table-test, and would have caught the cliffs. |
| Haggling bonus (BR-16) | 0 | 0 | 0% | None. Pure function of `Price`, `Special[2]`, `sIndex`. |
| Sign/overflow guards (BR-17) | 0 | 0 | 0% | None. |
| Village resolution and tax exemption (BR-18) | 0 | 0 | 0% | None. `BASE_GetVillage` is pure and untested; the `[5]` read is untested. |
| Tax application and windows (BR-19) | 0 | 0 | 0% | None. |
| Treasury distribution and caps (BR-20, BR-21) | 0 | 0 | 0% | None. `GuildImpostoID` state must be fabricated by hand. |
| Coin credit and ceiling (BR-22) | 0 | 0 | 0% | None. Boundary behaviour (exactly 2e9, overflow) entirely unverified. |
| Audit logging (BR-23) | 0 | 0 | 0% | None. No log-format test, no golden file. |
| Echo/projection protocol (BR-24..BR-26) | 0 | 0 | 0% | None. No packet-capture harness, no fake socket. |
| Index-only valuation, zero price (BR-27, BR-28) | 0 | 0 | 0% | None. |
| **Component total** | **0** | **0** | **0%** | **None** |

### 11.3 Why the component is currently untestable

1. **No seam.** `Exec_MSG_Sell` has no injectable collaborators: it reads and writes `pMob[]`, `pUser[]`, `g_pItemList[]`, `g_pGuildZone[]`, `GuildImpostoID[]`, `temp[]` and a live `CPSock` directly. There is no way to supply a fixture merchant, a price table or a recording socket without linking the entire `TMSrv` executable.
2. **No return value.** Success and 19 distinct failure modes are indistinguishable to a caller. Even a linked integration test cannot assert "the sale was refused" without inspecting global state after the call.
3. **Irreversible global mutation.** A successful call zeroes the item and adds gold. There is no transaction, no snapshot and no reset hook, so tests would have to re-initialise the world between cases.
4. **Data assets absent from the repository.** `itemlist.csv`, `extraitem.csv`, `Language.txt` and `Guild.txt` are all loaded from `../../Common/` and `./Run/` and none exists here. In a test run all prices would be 0 and all strings empty, which is exactly the degenerate path described in BR-28.
5. **Single-threaded Win32 event loop with no headless entry point.** `ProcessClientMessage` is driven by `WM_TIMER`/`WSA_ASYNC_SELECT` messages (`Server.cpp:3718-3729`), so no test can call the handler without either a message pump or a direct call that bypasses the dispatcher's envelope checks.
6. **Socket coupling inside the verdict path.** `SendClientMessage`, `SendEtc` and the direct `AddMessage` at `:228` will each log errors or close the session if the socket is a stub, so a naive test harness changes the behaviour under test.

### 11.4 Closest available substitute for tests

* **Git history** - three commits, none altering logic (see §11.1). There is no regression knowledge encoded anywhere; every rule in §3.2 was reconstructed from the current source.
* **Sibling-handler cross-checks** - the Buy, AutoTrade-buy, Trade, Drop and Autotrade-list handlers form an implicit specification of the item/economy policy. Where they disagree with the Sell Handler (§3.3), at least one is wrong; this is the most productive verification technique available without executing code.
* **In-code log strings** - `"err,(Wrong dest position)Sell-Carry"`, `"err,sell mytype"`, `"err,sell target NULL"`, `"err,sell target of carrypos outof range"`, `"err,request sell %d %d %d %d"`, `"err,MSG_ReqSell  <><><><><><> MINUS <><><><><><>"`, `"sel_npc,npc:%s price:%d item:%s"` are the component's de facto specification, and they are the only place its intent is written down in words.

### 11.5 Highest-value verification targets (analysis only, no implementation)

Ordered by the risk register: (1) `TargetID` visibility on the BR-10 bypass path - the single rule whose absence is a memory-safety defect; (2) `g_pGuildZone[5]` reachability from `:182`; (3) the fiscal settlement nesting at `:184-235`; (4) `Coin + sPrice` boundary values `{2e9-1, 2e9, 2e9+1, INT_MAX}`; (5) the four tier boundaries `{20000, 20001, 20004, 40000, 40004}` evaluated against the haggling and tax stages in both orders; (6) treasury credit with a displaced guild to confirm R-04; (7) `EF_NOTRADE` items sold through this path to confirm the attribute is ignored. All seven are deterministic, single-site checks that a table-driven harness over `{Price, sIndex, Class, LearnedSkill, Special[2], CityTax, Coin}` would cover exhaustively.

---

## 12. Analysis Limitations

| Limitation | Effect on this report |
|------------|-----------------------|
| `itemlist.csv` and `extraitem.csv` are absent from the repository | No actual item price, name or effect can be quoted. All numbers in §2.4 and §3.2 are the formula evaluated at illustrative inputs. The claim that 3193/3194 are clan mantles and 747 the crown is *inferred* from `_MSG_Quest.cpp`, `_MSG_CombineItemLindy.cpp`, `_MSG_UseItem.cpp` and one explicit comment at `Server.cpp:528`. |
| `Language.txt` is absent | Message indices 116, 167, 170 are known; their user-visible wording is not. |
| `Guild.txt` is absent | The shipped `CityTax` per city is unknown; the compiled default of 5 (`Basedef.cpp:47-54`) is quoted as *the initialiser*, not as the running value. |
| Learned-skill bit semantics are not declared anywhere | `0x1000` (BR-10) and `0x200` (BR-16) are identified by class-scoped usage only. `0x400` is documented in `CMob.cpp:728` as a Celestial weapon perk; the two bits used here have no such comment. The *effects* are certain; the intended *names* are not. |
| Source is a decompilation | Some constructs (the identical branches at `:188-192`, the unreachable `sVillage > 5` test, the `MSG_ReqSell` log string for a `MSG_Sell` handler) may be reconstruction artefacts rather than authorial choices. Each such case is flagged as either a probable artefact or an ambiguity, never as a certain intent. |
| No runtime access | No packet capture, no debugger, no live server. Whether a legitimate Celestial client sends `TargetID = 0` under BR-10 (and therefore lands on the out-of-bounds jurisdiction read) is an inference, marked as such. |
| Cross-component logic out of scope | `_MSG_Buy`, `_MSG_ReqBuy`, `_MSG_Trade`, `_MSG_SendAutoTrade`, `_MSG_Challange`, `ProcessSecMinTimer` and `Server.cpp` are referenced only at their boundaries. Their internal correctness is not asserted; where they are used as evidence of intended policy, only the specific comparison is cited. |
| `pMob` layout assumption | The 32-bit out-of-bounds severity of R-01 assumes `CMob` embeds `STRUCT_MOB` first (confirmed at `CMob.h:40`) and that `MAX_MOB = 25000` (`Basedef.h:107`). The exact address accessed depends on the compiler's struct layout; the *fact* of the out-of-bounds read does not. |
| Build artefacts present but not analysed | `Code/release/TMSrv/run/TMSrv.exe` and its `.pdb`, plus 233 `.obj` files, are build outputs of this same source. No binary analysis was performed; source is treated as authoritative. |

---

## 13. Summary of Referenced Files

| File | Lines referenced | Role |
|------|------------------|------|
| `Code/TMSrv/_MSG_Sell.cpp` | 1-236 (all) | The component |
| `Code/TMSrv/ProcessClientMessage.cpp` | 38-64, 230-232 | Dispatcher, envelope validation, sole call site |
| `Code/TMSrv/ProcessClientMessage.h` | 29-43, 64, 68, 110 | Include surface, globals, declaration |
| `Code/TMSrv/_MSG_Buy.cpp` | 21-44, 49-64, 104-145, 258-273, 290-312 | Sibling buy path (policy comparison, tax, treasury ownership check, audit channel) |
| `Code/TMSrv/_MSG_ReqBuy.cpp` | 44-102, 106, 120-192 | AutoTrade purchase path (unsigned coin arithmetic, treasury checks, `ItemLog`) |
| `Code/TMSrv/_MSG_SendAutoTrade.cpp` | 56-120 | `TradeMode` origin, autotrade blocklist, `EF_NOTRADE` usage |
| `Code/TMSrv/_MSG_DeleteItem.cpp` | 20-49 | Sibling inventory destruction (audit channel, slot window) |
| `Code/TMSrv/_MSG_TradingItem.cpp` | 87-161 | Equip slot conventions (`<= 0`, `MAX_EQUIP-1`) |
| `Code/TMSrv/_MSG_Quest.cpp` | 537, 600-644, 1045-1079, 1195-1234, 2530-2545 | Mantle indices 3191-3199 and circle indices 693-695 |
| `Code/TMSrv/_MSG_UseItem.cpp` | 86-119, 2458, 3223-3234 | `EF_VOLATILE == 1` potion semantics, mantle application |
| `Code/TMSrv/_MSG_MessageChat.cpp` | 62-99 | `guildtax` command; `CityTax` range 0-30 |
| `Code/TMSrv/_MSG_Challange.cpp` | 52-102 | Consumer of treasury EXP (1e9 per unit) |
| `Code/TMSrv/Server.cpp` | 46, 269-277, 387, 523-672, 684-707, 3189-3196, 3718-3729, 6701-6737, 7319-7352, 9264-9284 | `ServerDown`, globals, crown helpers, `AddCrackError`, `GuildImpostoID` assignment, message loop, `Log`, `RemoveTrade`, `ItemLog` |
| `Code/TMSrv/SendFunc.cpp` | 27-45, 197-206, 1055-1134, 1195-1231, 1622-1645 | Outbound projection layer |
| `Code/TMSrv/GetFunc.cpp` | 640-647 | `GetInView` |
| `Code/TMSrv/CPSock.cpp` | 337-345, 380-440, 513-560, 617-700 | Framing, size limits, `AddMessage` header rewrite, flush |
| `Code/TMSrv/CUser.h` | 25-37, 39-121 | Session modes, `Mode`/`TradeMode`/`Trade`/`Cargo` |
| `Code/TMSrv/CMob.h` | 26-35, 37-58 | `MOB_EMPTY`, `CMob` layout (`STRUCT_MOB` first) |
| `Code/TMSrv/Server.h` | 230, 382 | `extern CMob pMob[]`, `extern char temp[4096]` |
| `Code/TMSrv/Language.h` | 132-190, 293 | Message indices 112/116/167/170/273 |
| `Code/TMSrv/ProcessSecMinTimer.cpp` | 165-230, 2065-2104 | Rolling save, socket flush, treasury persistence |
| `Code/TMSrv/CReadFiles.cpp` | 785-821 | `Guild.txt` loading and `CityTax` clamp |
| `Code/Basedef.h` | 56-181, 398-481, 714-744, 882-906, 943-946, 1019-1024, 1492, 1700-1923, 2047-2057, 1758-1767, 2439-2464 | Constants, `STRUCT_MOB`/`SCORE`/`ITEM`/`ITEMLIST`/`GUILDZONE`, `_MSG` macro, `MSG_Sell`, `MSG_SendAutoTrade`, `MSG_Trade`, externs |
| `Code/Basedef.cpp` | 40-54, 753, 1479-1495, 1647-1842, 2340-2372, 4258-4267, 4281-4291, 4963-5134 | `g_pGuildZone` defaults, `g_pItemList`, `BASE_GetItemCode`, `BASE_GetItemAbility`, `GetItemPointer`, `BASE_GetVillage`, `BASE_ClearItem`, CSV loader |
| `Code/ItemEffect.h` | 76-78 | `EF_VOLATILE = 38` and its class comment |
| `W2PP Code Project.sln` | 5-10 | Evidence of a two-project solution (no tests) |
| `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` | 137-141, 284-288 | Component role, Ca/Ce figures, aggregate-header risk |
| `README.md` | 1-60 | Provenance (decompilation of a Polly's Server release), build toolchain, "Fix all bugs" backlog |
| Git history | `489c4e0`, `b030892`, `e47bf4f` | Proof the logic is unchanged since the 2018 import |

---

*End of report. This analysis was read-only; no project file was created, modified or deleted except this report.*
