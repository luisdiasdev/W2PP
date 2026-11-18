# Component Deep Analysis Report - Buy Handler

**Component name:** Buy Handler
**Primary location:** `Code/TMSrv/_MSG_Buy.cpp` (9,024 bytes, 313 lines, single free function)
**Project:** W2PP - decompiled C/C++ (Win32, MSVC / v143) MMORPG server emulator for W2
**Host process:** `TMSrv` (game / world server)
**Analysis date:** 2026-11-15 22:21:02
**Analysis scope:** entire project root (`E:\wyd\test-setup\w2pp`), excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
**Analysis mode:** read-only static analysis. No project file was modified, no code executed, no test run.

---

## 1. Executive Summary

### 1.1 Purpose

The Buy Handler is the server-authoritative resolver for **NPC shop purchases**. It is implemented as one free function, `void Exec_MSG_Buy(int conn, char *pMsg)`, declared at `Code/TMSrv/ProcessClientMessage.h:109` and defined at `Code/TMSrv/_MSG_Buy.cpp:21`. It is bound to exactly one packet type, `_MSG_Buy` (opcode `121 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME`, `Code/Basedef.h:1749`), in the dispatcher at `Code/TMSrv/ProcessClientMessage.cpp:226-228`:

```cpp
case _MSG_Buy:
    Exec_MSG_Buy(conn, pMsg);
    break;
```

The handler completes, in one linear pass with two `goto` labels:

1. Session/state gating (alive, `USER_PLAY`, no trade in progress).
2. Index validation of the three client-supplied selectors (`TargetID`, `TargetInvenPos`, `MyInvenPos`).
3. Trust-boundary resolution: the item is read out of the **merchant NPC's own carry array**, never from packet-supplied item data.
4. Donate-currency branch (`EF_DONATE`), which bypasses coin, tax, discount and stock logic entirely.
5. Price computation: base price from `g_pItemList`, city-tax surcharge from `g_pGuildZone[].CityTax`, guild-ruler discount (30%), class-3 artisan "discount" (label only).
6. Affordability check against `pMob[conn].MOB.Coin`.
7. Guild-bound item customization for six hard-coded item indices plus guild-medal serial issuance with persistent counter.
8. Special-merchant stock enforcement (128-entry `pMobMerc` table).
9. Guild tax revenue distribution to two impostor NPCs.
10. Coin debit, balance echo back to the client, inventory write, audit logging.
11. Merchant self-removal for potion sellers and special "explosive" merchants.

### 1.2 Role in the system

The architectural report classifies the Buy Handler as a **Command Handler (economy)** with **afferent coupling 1** and **efferent coupling 14** (`docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:138`). This analysis confirms **Ca = 1** exactly: the only caller is `ProcessClientMessage` (`Code/TMSrv/ProcessClientMessage.cpp:226`). The only other textual references in the codebase are the declaration (`ProcessClientMessage.h:109`), the disabled size check (`Basedef.cpp:6525`, inside the commented-out `BASE_CheckPacket`) and the two build-file entries (`TMSrv.vcxproj:114`, `TMSrv.vcxproj.filters:213`). At symbol granularity the handler calls **17 distinct project functions** (decomposing the 14 module-level edges) plus 4 CRT routines (`sprintf`, `memcpy`, `rand`, and the `CPSock::AddMessage` member). It is a pure consumer/leaf: nothing in the project depends on it except the dispatcher.

Economically, the handler is one half of the NPC shop transaction pair, the other half being `_MSG_Sell.cpp` (player → NPC). It is **not** the player-to-player trade path: `_MSG_ReqBuy.cpp` handles player autotrade stalls and has a completely separate validation model (client-supplied `Price`/`Tax`/`item` cross-checked against the seller's `AutoTrade` mirror). In the Buy Handler the price is never accepted from the client - it is derived from the server-side `g_pItemList[itemIndex].Price` and mutated by server-side zone tax data. This is the component's single strongest design property.

Unlike the Sell and ReqBuy handlers, the Buy Handler **does not persist anything**: there is no `SaveUser`, no DB packet, no write-through. Coin and inventory mutations live only in the in-memory `pMob` / `pUser` structures until the periodic save or logout path runs.

### 1.3 Key findings

| # | Finding | Severity |
|---|---------|----------|
| 1 | **Donate path jumps into the middle of the coin path** (`goto DonateBuy` at `_MSG_Buy.cpp:101` → label at `:283`, inside the `if (Price >= 0)` block at `:236`). This skips the initialization of `Price` (`:104`), `Price2` (`:105`), `Village` (`:106`), `CityTax` (`:107`), `GuildTax` (`:121`), `bPrice` (`:124`) and `Desconto` (`:125`) while still *reading* them at `:236` (`if (Price >= 0)`), `:290` (audit log `price:%d`) and `:297` (debug trace). The `Price >= 0` test decides whether the item is delivered at all, so a donate purchase is delivered on an indeterminate value. Undefined behaviour in the primary "buy with donate" flow. | High |
| 2 | **Out-of-bounds read of `g_pGuildZone[5]`** at `_MSG_Buy.cpp:107`: `BASE_GetVillage` returns `5` for any coordinate outside a city (`Code/Basedef.cpp:4258-4267`) and `g_pGuildZone` has exactly 5 elements (`MAX_GUILDZONE = 5`, `Code/Basedef.h:148`, `:2441`). The range check `Village >= 5` only happens afterwards at `:109`. Every purchase made from a merchant standing outside a city executes this 4-byte over-read. | High |
| 3 | **`GuildTax` is used uninitialized** when the tax block is skipped (`:109-110` jumps over the initializer at `:121`), yet `:263` and `:270` add `GuildTax / 2` and `GuildTax` to NPC experience whenever `CityTax` is in `(0,30)`. `GuildTax` and `CityTax` are read from *different* villages (`Village` at `:106` vs `TargetVillage` at `:126`), so the guard at `:258` does not imply the assignment at `:121` ran. | High |
| 4 | **Class-3 artisan discount changes no price.** `:135-145` computes `special = Special[2]/10 + 6` (capped 26), assigns it to `Desconto`, divides the local by 2 and discards it. `Price` is untouched. The audit/debug line at `:297` therefore reports a discount percentage that was never applied. | High |
| 5 | **`BASE_CheckPacket` is entirely commented out** (`Code/Basedef.cpp:6476-6594`, closing `*/` at `:6594`, function returns `FALSE` at `:6595`). The `_MSG_Buy` size assertion at `:6525` is dead code, and the call sites are additionally wrapped in `#ifdef _PACKET_DEBUG` (`Code/TMSrv/Server.cpp:3980-3988`, `Code/CPSock.cpp:544-552`). No packet-size validation exists for this handler. | High |
| 6 | **The outbound echo reuses the inbound buffer and its client-supplied length**: `pUser[conn].cSock.AddMessage((char*)m, m->Size)` at `:280`. The frame length of the server's response is whatever the client declared, not `sizeof(MSG_Buy)`. Bounded by `ReadMessage` (`Code/CPSock.cpp:397-411`, `MAX_MESSAGE_SIZE = 8192`), so not a memory-safety break, but it is a protocol-integrity defect in the only response the handler emits. | Medium |
| 7 | **Shop-list visibility is advisory only.** `SendShopList` transmits 27 of the merchant's 64 carry slots using the mapping `invpos = (i % 9) + (i / 9) * MAX_SHOPLIST` (`Code/TMSrv/SendFunc.cpp:1392-1399`), i.e. slots 0-8, 27-35, 54-62. The Buy Handler accepts **any** `TargetInvenPos` in `[0, 64)` (`:49`), so slots 9-26, 36-53 and 63 are purchasable although never displayed. Price is still server-derived, so this is an exposure of hidden stock, not a price exploit. | Medium |
| 8 | **Reserved inventory tail is writable.** `MyInvenPos` is validated against `MAX_CARRY` (64, `:49`) while the rest of the server treats slots 60-63 as reserved (`MAX_CARRY-4`: `_MSG_Sell.cpp:53`, `_MSG_DeleteItem.cpp:24`, `_MSG_SplitItem.cpp:24`, `_MSG_Trade.cpp:60`, `_MSG_TradingItem.cpp:87`, `_MSG_ReqBuy.cpp:106`). Slots 60 and 61 hold item `3467` for a timed feature (`_MSG_UseItem.cpp:5666-5693`, `ProcessSecMinTimer.cpp:571-595`); the Buy Handler can place purchased goods into them when empty. | Medium |
| 9 | **Merchant inventory is permanently mutated** for guild-bound items: the handler writes effects directly into `pMob[TargetID].MOB.Carry[TargetInvenPos]` (`:191-204`, `:210-217`) and, for item 508 with sub-guild 1-3, rewrites `sIndex` to `sub + 531` (`:203-204`). The shop slot is never restored, so the stamp of the first buyer persists for every subsequent buyer of that slot. | Medium |
| 10 | **Tax arithmetic is duplicated and can overflow**: `:112-116` are two identical branches, and `Price * CityTax` with `Price` near `INT_MAX` (`STRUCT_ITEMLIST.Price` is a plain `int`, `Code/Basedef.h:901`) is signed overflow before the `Price < 0` guard at `:118`. The later discounts are additionally guarded by `Price <= 1999999999` (`:129`, `:135`) but the surcharge itself is not. | Medium |
| 11 | **`CityTax == 30` silently disables the tax**: the guard is `CityTax >= 30` (`:109`), while the GM command that sets it accepts `0..30` (`_MSG_MessageChat.cpp:76`) and the file loader clamps to `0..20` (`CReadFiles.cpp:811-812`). A 30% city tax therefore results in **0%** charged, while `SendShopList` still advertises `Tax = 30` to the client (`SendFunc.cpp:1408`). | Medium |
| 12 | **Asymmetric trade teardown**: when `TradeMode` is set the handler tears down both sides (`:32-37`), but when only `Trade.OpponentID` is set it tears down only the buyer (`:39-43`), leaving the counterparty's trade window open until its own timeout/cleanup. | Low |
| 13 | **No rate limiting, no idempotency key, no persistence.** Purchase requests are processed as fast as the client can send them; the only anti-abuse accumulator is `AddCrackError(conn, 10, 21)` on a dead/invalid session (`:28`). Coin and item mutations reach disk only via the periodic/user-triggered save path. | Medium |
| 14 | **Zero automated tests exist anywhere in the repository** for this or any other component - no test project, no test file, no assertion (evidence in section 11). | High |

---

## 2. Data Flow Analysis

### 2.1 Macro flow (TCP frame → world state)

```
 1. TCP frame received
    -> Code/TMSrv/Server.cpp:3963        recv(...) into pUser[User].cSock.pRecvBuffer
 2. Frame framing + de-obfuscation + length validation
    -> Code/CPSock.cpp:353-411           CPSock::ReadMessage()
         * Size = *(unsigned short*)pMsg           (:390)
         * reject if Size > MAX_MESSAGE_SIZE (8192) or Size < sizeof(HEADER)   (:397)
         * reject if Size > bytes available        (:410)
         * in-place de-scramble from byte 4        (:430-450)
         * nProcPosition += Size                   (:416)
 3. Envelope validation
    -> Code/TMSrv/ProcessClientMessage.cpp:38-65
         * std->ID in [0, MAX_USER)                (:42)
         * ServerDown >= 120 short-circuit         (:53)
         * LastReceiveTime refresh                 (:57)
         * _MSG_Ping dropped                       (:59)
         * client packets carrying SKIPCHECKTICK dropped (:63)
 4. Opcode dispatch
    -> Code/TMSrv/ProcessClientMessage.cpp:226-228   case _MSG_Buy (121)
 5. Component entry
    -> Code/TMSrv/_MSG_Buy.cpp:21        Exec_MSG_Buy(conn, pMsg)
    -> :23                               MSG_Buy *m = (MSG_Buy*)pMsg;   (no copy, no bounds check)
 6. Extract client selectors
    -> :45-47    TargetID, TargetInvenPos, MyInvenPos  (m->Coin is ignored on input)
 7. Gate: session state
    -> :25-30    Hp == 0 || Mode != USER_PLAY  -> SendHpMode + AddCrackError(10,21) + return
    -> :32-43    TradeMode / Trade.OpponentID  -> RemoveTrade(...) + return
 8. Gate: index ranges
    -> :49-53    TargetID in [MAX_USER=1000, MAX_MOB=25000)
                 TargetInvenPos, MyInvenPos in [0, MAX_CARRY=64)
 9. Gate: merchant identity + reachability
    -> :55-59    pMob[TargetID].MOB.Merchant != 1  -> "Only merchant sells"
    -> :60-64    !GetInView(conn, TargetID)        -> _MSG_CloseShop signal
10. Resolve item from SERVER state (trust boundary)
    -> :66       itemIndex  = pMob[TargetID].MOB.Carry[TargetInvenPos].sIndex
    -> :68       ItemMob    = &pMob[TargetID].MOB.Carry[TargetInvenPos]   (raw pointer, later mutated)
    -> :70-71    reject unless 1 <= itemIndex < MAX_ITEMLIST (6500)
11. Donate branch (EF_DONATE = effect 91, Code/ItemEffect.h:134)
    -> :73       Donate = BASE_GetItemAbility(ItemMob, EF_DONATE)
    -> :77-82    Donate > pUser[conn].Donate      -> "need %d donate"
    -> :84-88    itemIndex == 0 (dead: already rejected at :70)
    -> :90-94    destination slot occupied        -> echo item back, return
    -> :96       pUser[conn].Donate -= Donate
    -> :98-99    ItemLog (Portuguese free-text, not from message table)
    -> :101      goto DonateBuy  ----+
                                     |
12. Coin branch                      |
    -> :104-105  Price = g_pItemList[itemIndex].Price ; Price2 = Price
    -> :106-107  Village = BASE_GetVillage(merchant x,y) ; CityTax = g_pGuildZone[Village].CityTax
    -> :109-110  if (Village <0 || >=5 || CityTax <=0 || >=30) goto LABEL_BUY1 (skip tax)
    -> :112-116  Price += Price * CityTax / 100   (both branches identical)
    -> :118-119  if (Price < 0) return
    -> :121      GuildTax = (Price - Price2) / 2
    -> :122      LABEL_BUY1:
    -> :124-127  bPrice = Price ; Desconto = 0 ; TargetVillage = BASE_GetVillage(...) ; Guild = buyer guild
    -> :129-133  guild ruler of that village -> Desconto = 30 ; Price = 7*Price/10
    -> :135-145  Class == 3 && LearnedSkill & 0x800 -> Desconto = min(Special[2]/10+6, 26)  (no price change)
    -> :147-151  Price > pMob[conn].MOB.Coin -> "Not enough money"
    -> :153-157  itemIndex == 0 (dead)
    -> :159-163  destination slot occupied -> echo item back, return
13. Guild-bound item customization
    -> :165      itemIndex in {446, 508, 509} or [526,531]
    -> :167-177  requires ServerIndex != -1 and GuildCounter != 0
    -> :179-206  requires GuildLevel != 0 (and >= 2 for 526-531); stamps effects 56/57 = buyer guild;
                 for 508: effect 85 = sub-guild from Equip[12], and sIndex -> 532..534
    -> :208-232  for 509: stamps effects 56/57/59, increments/persists GuildCounter, WriteGuild()
14. Stock check (special merchants only)
    -> :240-245  linear scan of pMobMerc[0..127] by GenerateIndex
    -> :250-255  Stock[TargetInvenPos] == 0 -> "No stock" ; else Stock--
15. Tax revenue distribution
    -> :258-273  if target village valid and 0 < CityTax < 30:
                   village impostor NPC Exp += GuildTax/2   (cap 200,000,000,000)
                   zone-4 (castle) impostor NPC Exp += GuildTax
16. Settlement
    -> :274      pMob[conn].MOB.Coin -= Price
    -> :276-278  m->Coin = new balance ; m->ID = ESCENE_FIELD (30000)
    -> :280      cSock.AddMessage((char*)m, m->Size)      <-- inbound buffer re-used as response
    -> :282      SendEtc(conn)                            <-- MSG_UpdateEtc with full economy state
17. Item delivery  (DonateBuy label at :283 is the donate join point)
    -> :285      memcpy(&pMob[conn].MOB.Carry[MyInvenPos], ItemMob, sizeof(STRUCT_ITEM))
    -> :287-291  BASE_GetItemCode + "buy_npc,npc:%s price:%d item:%s" -> ItemLog
    -> :293      SendItem(conn, ITEM_PLACE_CARRY=1, MyInvenPos, &pMob[conn].MOB.Carry[MyInvenPos])
18. Optional broadcast / lifecycle
    -> :295-299  if (FREEEXP == -2) SendSay(TargetID, "<name> base:<Price2> tax:<bPrice>(<CityTax>%) discount:<Price>(<Desconto>%)")
    -> :301-307  itemIndex 787 or 481 -> merchant PotionCount++ ; > POTIONCOUNT (default 10) -> DeleteMob(TargetID,3)
    -> :308-309  merchant Equip[0] is 259 or 230 -> DeleteMob(TargetID,3)
    -> :311-312  Price < 0 -> "err,MSG_ReqBuy <><><><><><> MINUS <><><><><><>" (mis-labelled: this file is _MSG_Buy)
```

### 2.2 Price computation pipeline

```
                    g_pItemList[idx].Price  (Code/Basedef.h:901, loaded by BASE_ReadItemListFile Basedef.cpp:4987)
                                  |
                                  v
   Price2 = Price  -------------------------------------------+
                                  |                            |
        +-------------------------+                        (audit/trace only)
        |  if Village in 0..4 and 0 < CityTax < 30
        v
   Price += Price * CityTax / 100        [surcharge, :112-116]
   if (Price < 0) return                 [overflow bail-out, :118]
   GuildTax = (Price - Price2) / 2       [guild revenue basis, :121]
                                  |
                                  v
        if (Price <= 1999999999 && buyer is ruler of the merchant's village)
            Price = 7 * Price / 10 ; Desconto = 30                  [:129-133]
                                  |
                                  v
        if (Price <= 1999999999 && !Desconto && Class == 3 && LearnedSkill & 0x800)
            Desconto = min(Special[2]/10 + 6, 26)                   [:135-145]  (price unchanged)
                                  |
                                  v
        if (Price > Coin) reject                                        [:147-151]
```

Worked example (all values verified against the code above): item list price `1,000`, `CityTax = 10`, buyer is the village's charging guild leader, `Special[2] = 100`.

```
Price  = 1000
tax    = 1000 + 1000*10/100 = 1100
GuildTax = (1100 - 1000)/2 = 50
Price  = 7 * 1100 / 10      = 770     (Desconto = 30)
village impostor Exp += 25 ; castle (zone 4) impostor Exp += 50
Coin  -= 770
```

### 2.3 State mutations

| Target | Field | Operation | Line | Condition |
|--------|-------|-----------|------|-----------|
| Buyer | `pUser[conn].Donate` | `-= Donate` | `:96` | donate branch, balance sufficient, slot empty |
| Buyer | `pMob[conn].MOB.Coin` | `-= Price` | `:274` | coin branch only |
| Buyer | `pMob[conn].MOB.Carry[MyInvenPos]` | `memcpy` from merchant slot | `:285` | donate branch always; coin branch only if `Price >= 0` |
| Merchant | `pMob[TargetID].MOB.Carry[TargetInvenPos]` | `stEffect[0..2]` writes, occasionally `sIndex` | `:191-217` | guild-bound item indices |
| Merchant | `PotionCount` | `++` | `:303` | item 787 or 481 |
| Merchant | mob lifetime | `DeleteMob(TargetID, 3)` | `:306`, `:309` | potion cap exceeded, or `Equip[0]` in {259, 230} |
| Merchant table | `pMobMerc[x].Stock[TargetInvenPos]` | `--` | `:255` | merchant is a `STRUCT_MERC`-managed mob |
| Guild counter | `GuildCounter` | `++`, wrap `4096 -> 2048` | `:224-230` | item 509 |
| Guild file | `Guild.txt` | full rewrite | `:231` | item 509 |
| Impostor NPC | `pMob[GuildImpostoID[z]].MOB.Exp` | `+=` (cap `2e11`) | `:263`, `:270` | valid zone, tax, matching guild NPC |
| Client | outbound `MSG_Buy` echo | `Coin`, `ID`, checksum | `:276-280` | coin branch only |
| Client | `MSG_UpdateEtc` | full economy snapshot | `:282` | coin branch only |
| Client | `MSG_SendItem` | slot + item | `:293` | always (when delivered) |
| Void mobs | `MSG_RemoveMob` grid broadcast | via `DeleteMob` | `:7020-7054` | merchant removal |

`Donate` is a per-session field of `CUser` (`Code/TMSrv/CUser.h:105`), not part of `MSG_Trade`; a donate purchase therefore never touches `Trade` state.

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | Validation / Security | Buyer must be alive (`CurrentScore.Hp != 0`) and in `USER_PLAY` mode; otherwise HP/Mode resync + crack counter +10 (type 21) and abort | `_MSG_Buy.cpp:25-30` |
| BR-02 | Validation / State | A purchase is illegal while any trade or autotrade state exists; the handler tears trade state down and aborts | `:32-43` |
| BR-03 | Validation | `TargetID` must be an NPC index in `[MAX_USER, MAX_MOB)`; both inventory positions must be in `[0, MAX_CARRY)` | `:49-53` |
| BR-04 | Validation | Only mobs whose `MOB.Merchant == 1` may sell through this handler | `:55-59` |
| BR-05 | Validation / Spatial | The merchant must be within `VIEWGRIDX/VIEWGRIDY` (33/33) of the buyer; failure closes the shop UI | `:60-64` |
| BR-06 | Validation | The selected shop slot must hold an item index in `[1, MAX_ITEMLIST)` | `:66-71` |
| BR-07 | Business Logic | Items carrying `EF_DONATE` (91) are priced in donate currency and bypass coin, tax, discount, stock and guild-tax logic | `:73-102` |
| BR-08 | Validation | Donate balance must be >= the item's `EF_DONATE` value | `:77-82` |
| BR-09 | Validation | The destination carry slot must be empty (`sIndex == 0`) in both currency branches | `:90-94`, `:159-163` |
| BR-10 | Business Logic | Base price is read exclusively from `g_pItemList[itemIndex].Price`; no client-supplied price is accepted | `:104-105` |
| BR-11 | Business Logic | City tax surcharge `Price += Price*CityTax/100`, applied only when the merchant stands in a valid city with `0 < CityTax < 30`; `GuildTax` = half the surcharge | `:106-122` |
| BR-12 | Business Logic | A buyer who is the charging guild of the merchant's village pays 70% (30% discount) | `:129-133` |
| BR-13 | Business Logic (defective) | Class 3 with learned-skill bit `0x800` computes an artisan discount label (`Special[2]/10+6`, capped 26) but never applies it to the price | `:135-145` |
| BR-14 | Validation | `Price` must not exceed the buyer's coin balance | `:147-151` |
| BR-15 | Business Logic | Guild-bound items (446, 508, 526-531) require a guild, guild level, valid `ServerIndex` and non-zero `GuildCounter`; the buyer's guild is stamped into effects 56/57, and 508 is re-indexed by sub-guild | `:165-206` |
| BR-16 | Business Logic | Guild medals (509) are issued as unique serials derived from `GuildCounter + (ServerIndex << 12)`, increment the counter, wrap at 4096→2048, and force a `Guild.txt` rewrite | `:208-232` |
| BR-17 | Business Logic | For merchants registered in `pMobMerc`, stock is per-slot, decremented after payment, and zero stock blocks the sale | `:240-256` |
| BR-18 | Business Logic | Half of the surcharge is credited to the village impostor NPC, and the full surcharge to the zone-4 (castle) impostor NPC, each capped at 200,000,000,000 exp | `:258-273` |
| BR-19 | Business Logic | Coin is debited, the new balance is echoed to the client, and a full economy snapshot (`MSG_UpdateEtc`) is pushed | `:274-282` |
| BR-20 | Business Logic / Audit | The item is copied from the merchant slot into the buyer slot, then both `BASE_GetItemCode` and a free-text `ItemLog` audit entry are produced | `:285-293` |
| BR-21 | Diagnostics | When the global `FREEEXP` equals `-2`, the merchant "speaks" the price breakdown in local chat | `:295-299` |
| BR-22 | Business Logic | Buying item 787 or 481 increments the merchant's `PotionCount`; exceeding `POTIONCOUNT` (default 10) destroys the merchant | `:301-307` |
| BR-23 | Business Logic | Merchants wearing item 259 or 230 in `Equip[0]` are destroyed after any successful purchase | `:308-309` |
| BR-24 | Defensive | A negative computed `Price` aborts the transaction and logs a "MINUS" error | `:311-312` |
| BR-25 | Implicit / Persistence | The handler performs no save; coin/donate/inventory mutations are volatile until the periodic or logout save path runs | whole function |
| BR-26 | Implicit / Trust boundary | The shop list is a display hint only; the server enforces price and ownership but not "was this slot displayed" | `:49` vs `SendFunc.cpp:1390-1400` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: Session and liveness gate (BR-01)

**Overview**:
Every purchase request must originate from an authenticated session whose character is alive and in the normal gameplay mode; otherwise the request is treated as a protocol/cheat signal rather than a business transaction.

**Detailed description**:

The gate is the very first statement of the handler, evaluated before any packet field is read. It rejects when `pMob[conn].MOB.CurrentScore.Hp == 0` (dead, or a slot that has been zeroed by `DeleteMob`) **or** when `pUser[conn].Mode != USER_PLAY`. On rejection the server sends a `MSG_SetHpMode` snapshot through `SendHpMode` (`Code/TMSrv/SendFunc.cpp:1622-1645`) so the client can resynchronise its local HP/mode display, then calls `AddCrackError(conn, 10, 21)`, which accumulates 10 points into `pUser[conn].NumError` and logs `cra point: 10 type: 21` on every occurrence (`Code/TMSrv/Server.cpp:684-707`).

The crack counter is a soft anti-abuse mechanism, not an immediate ban: only when `NumError >= 2,000,000,000` does `AddCrackError` force `CharLogOut`. Reaching that threshold requires roughly 200 million rejected packets, so in practice the gate is a logging/telemetry rule with a theoretical lockout at the extreme end. The rule's real value is that no other branch of the handler is reachable from a dead or non-playing session, which keeps the merchant/coin logic free of "is this a player" checks.

The handler does **not** re-validate that `conn` itself is in range; it relies on the dispatcher (`ProcessClientMessage.cpp:42-51`), which rejects packets whose `ID` is outside `[0, MAX_USER)`. Note the subtlety that the dispatcher validates `std->ID` (a field the client controls) rather than `conn` (the socket-derived index), so this rule does not add protection against a mismatched `ID`; it only adds the liveness/mode predicate.

**Rule workflow**:
1. `Hp == 0 || Mode != USER_PLAY`?
2. Yes → `SendHpMode(conn)` (HP + mode snapshot) → `AddCrackError(conn, 10, 21)` → `NumError += 10`; if `>= 2e9` message + `CharLogOut` → return.
3. No → continue to BR-02.

---

### Business Rule: Trade-state exclusivity (BR-02)

**Overview**:
Buying from an NPC is mutually exclusive with player trading and with autotrade-stall operation; the handler forcibly cancels trade state instead of servicing the purchase.

**Detailed description**:

Two distinct states are checked. `pUser[conn].TradeMode` indicates the buyer is currently displaying an autotrade stall (`TradeMode` is also set/cleared in `RemoveTrade`, `Code/TMSrv/Server.cpp:7339-7351`). In that case the handler calls `RemoveTrade(pUser[conn].Trade.OpponentID)` first and then `RemoveTrade(conn)`, closing both sides of the interaction. The second check, `pUser[conn].Trade.OpponentID`, catches a negotiation that is open but not yet in autotrade mode; here only `RemoveTrade(conn)` is invoked.

The asymmetry is intentional only in the first branch: an autotrade stall is a persistent public state that must be withdrawn from the world, so the opponent (or the stall viewer slot) is cleaned too. In the second branch, the counterparty is left untouched - its `Trade` struct still points at `conn` until its own quit/timeout path runs. Because `RemoveTrade` also zeroes `AutoTrade` and re-sends `_MSG_CreateMob` for the buyer (`Server.cpp:7319-7352`), the buyer leaves the trade cleanly, but the residual reference on the other side is a consistency gap that this handler neither detects nor repairs.

`RemoveTrade` additionally guards `conn` range and returns early for non-`USER_PLAY` sessions, and always sends signal `900` to the client. The handler returns `void` in both branches, so the client receives no explicit "purchase rejected because trading" message; it observes only the cumulative effect of `RemoveTrade` (trade window closure). This is why the rule reads as "cancel the trade" rather than "reject the purchase".

**Rule workflow**:
1. `TradeMode` set → `RemoveTrade(opponent)` → `RemoveTrade(conn)` → return.
2. Else `Trade.OpponentID` set → `RemoveTrade(conn)` → return (opponent left dangling).
3. Else continue.

---

### Business Rule: Entity and slot index bounds (BR-03)

**Overview**:
The three client-supplied selectors are validated against the world-array layout before any dereference: `TargetID` must be an NPC slot and both inventory positions must be inside a character's 64-slot carry array.

**Detailed description**:

`TargetID` is declared `unsigned short` in `MSG_Buy` (`Code/Basedef.h:1754`), so it cannot be negative; the test `TargetID < MAX_USER` (`:49`) therefore means "is this a player or empty slot", which is exactly the property that makes a merchant lookup meaningful. `MAX_USER = 1000` (`Code/Basedef.h:56`) is the boundary between `pUser` and NPC slots, and `MAX_MOB = 25000` (`:107`) bounds `pMob[]`. `TargetInvenPos` and `MyInvenPos` are `short` (`Basedef.h:1755-1756`) and are checked against `MAX_CARRY = 64` (`:76`) on both ends, since a negative value survives the cast if a client sends raw bytes.

Failure of any predicate logs `err,MSG_BUY, target of carrypos outof range` with account name and IP (`:51`) and returns silently - the client gets no error panel. That silence is consistent with the file's overall "log and drop" style for range violations, and it is the cheapest way to avoid giving an attacker a response oracle, but it also makes legitimate desync (e.g. a shop window left open after a teleport) indistinguishable from an invalid request.

The validation happens **after** the trade teardown of BR-02, so a malformed `TargetID` still costs the buyer their trade state. It happens **before** the merchant check, so the ordering guarantees all subsequent `pMob[TargetID]` accesses in this function - merchant flag, coordinates, `MOB.Carry`, guild fields, `GenerateIndex`, `Equip[0]` - are in-bounds. `MAX_CARRY` is shared with the shop-list sender, which is what makes `TargetInvenPos` interoperate with the 27-slot display mapping.

**Rule workflow**:
1. `TargetID < 1000 || TargetID >= 25000 ||` `TargetInvenPos < 0 || >= 64 ||` `MyInvenPos < 0 || >= 64`?
2. Yes → log range error with account/IP → return.
3. No → continue to BR-04.

---

### Business Rule: Merchant-only counter (BR-04)

**Overview**:
This handler is exclusively the NPC-shop purchase path: the target mob must advertise merchant type exactly `1`, which also means the alternate shop presentation (type `19`) is unreachable here.

**Detailed description**:

`pMob[TargetID].MOB.Merchant` is an `unsigned char` of `STRUCT_MOB` (`Code/Basedef.h:442`), populated from the NPC generator data. The strict equality `!= 1` (`:55`) is the component's identity check. `_MSG_REQShopList.cpp:39-55` shows the two supported presentations: `Merchant == 1` opens the standard shop (`SendShopList(conn, target, 1)`), while `Merchant == 19` opens a variant with `ShopType = 3`. Because the Buy Handler accepts only `1`, any purchase attempt against a `19` shop is refused with the localized `_NN_Only_Merchant_Sells` message - that variant must be served by a different client command (outside this component's boundary).

Failure sends `SendClientMessage` with the message-table string (`Code/TMSrv/SendFunc.cpp:27-45`), which copies at most `MESSAGE_LENGTH` bytes into `MSG_MessagePanel` and force-terminates the string twice. This is the only rejection in the handler that produces a visible, localized panel message before the affordability check; the rest are logs or item echoes.

Together with BR-03 this establishes the ownership boundary: an attacker can address any NPC in the world, but only ones flagged as shop type 1 will be considered, and only for the item physically present in their carry array. There is no "shop id" or dialog state to spoof.

**Rule workflow**:
1. `pMob[TargetID].MOB.Merchant != 1`?
2. Yes → `SendClientMessage(_NN_Only_Merchant_Sells)` → return.
3. No → continue to BR-05.

---

### Business Rule: Spatial reachability gate (BR-05)

**Overview**:
The buyer must be inside the merchant's 33x33 view grid; otherwise the shop is force-closed on the client.

**Detailed description**:

`GetInView(conn, TargetID)` (`Code/TMSrv/GetFunc.cpp:640-647`) compares `TargetX`/`TargetY` of both mobs against `VIEWGRIDX = 33` and `VIEWGRIDY = 33` (`Code/Basedef.h:95-96`). The function returns `FALSE` when the absolute coordinate delta exceeds the grid box, i.e. the rule is a box test, not a radius, and it is the same primitive used by the shop-list sender (`_MSG_REQShopList.cpp:45`), the sell handler and the attack handler.

On failure the server emits `SendClientSignal(conn, ESCENE_FIELD, _MSG_CloseShop)` (`:62`), where `_MSG_CloseShop` is opcode 150 (`Code/Basedef.h:1819`). This is a UI-level correction: the client is told to close the shop window, which is the natural state after the buyer walked or teleported away. No message text and no audit log entry are produced, so a burst of out-of-range buy requests is invisible in the item log and only visible in general logs if another rule fires.

The check runs **after** the merchant-identity check, so the "not a merchant" path does not leak position information and does not close a shop window that was never legitimately opened. It runs **before** the item index is read, so no shop inventory is touched by an out-of-range request. Notably, the check uses the *merchant's* current coordinates, not the player's recorded shop anchor, which means a mobile merchant (or a merchant moved by a GM command) immediately invalidates outstanding shop windows.

**Rule workflow**:
1. `GetInView(conn, TargetID) == FALSE`?
2. Yes → `SendClientSignal(conn, ESCENE_FIELD, _MSG_CloseShop)` → return.
3. No → continue to BR-06.

---

### Business Rule: Shop slot item validity (BR-06)

**Overview**:
The selected merchant slot must contain a real item: index in `[1, MAX_ITEMLIST)`. Empty slots and out-of-table indices are silently dropped.

**Detailed description**:

`itemIndex` is copied out of the merchant's `MOB.Carry[TargetInvenPos].sIndex` (`:66`) and immediately validated against the item-list domain `1..6499` (`MAX_ITEMLIST = 6500`, `Code/Basedef.h:139`). The same slot's address is retained as `ItemMob` (`:68`) and is used for the donate lookup and, later, for the `memcpy` into the buyer and for guild-stamping. The validation is therefore load-bearing: every later `g_pItemList[itemIndex]` index (`:104`, `:290`, `:297`) and every effect write through `ItemMob` is safe only because this check passed.

Two of the three explicit "null item" checks in the file are dead code: `:84-88` (donate branch) and `:153-157` (coin branch) both test `itemIndex == 0` after `:70` has already rejected `<= 0`. They are harmless but indicate the validation was historically spread across the function and then hoisted without removing the copies. The dead checks do confirm the original author was thinking about the "empty slot purchase" attack.

The failure mode is completely silent - no log, no message. From a protocol standpoint that is defensible (an empty slot is a stale UI, not an attack), but it removes any server-side trace when a client repeatedly probes empty slots of a merchant, which is exactly the pattern a stock-scanner would produce.

**Rule workflow**:
1. `itemIndex <= 0 || itemIndex >= 6500`?
2. Yes → return (no log, no message).
3. No → continue to BR-07 (donate probe).

---

### Business Rule: Donate-currency override (BR-07)

**Overview**:
Any shop item whose item-list/instance effect `EF_DONATE` (91) is non-zero is sold for donate currency instead of coin, and the entire coin/price/tax/discount/stock/guild-tax pipeline is bypassed.

**Detailed description**:

`BASE_GetItemAbility(ItemMob, EF_DONATE)` (`:73`, implementation `Code/Basedef.cpp:1647-1811`) returns the sum of the static item-list effect value and the instance effect values whose effect id equals 91. Because it is evaluated on the *merchant's* item instance, a shop can price a single slot in donate currency without any separate price table: the effect value **is** the price. When non-zero, the handler takes the donate branch and never executes the `Price` computation at `:104-145`.

The branch deducts `pUser[conn].Donate` (`CUser.h:105`), writes a free-text `ItemLog` entry in Portuguese (`"%s comprou o item %s por %d donate e restou %d de donate.."`, `:98`) and jumps to `DonateBuy`. Because the jump target is *inside* the `if (Price >= 0)` block (`:236`), the donate purchase inherits the item-delivery code while skipping the coin debit, the stock decrement, the guild-tax distribution and the tax/discount computation. The intent is clear; the implementation is the source of key finding #1, because `Price` is read at `:236` before it has ever been assigned on that path.

Consequences visible in the code: donate purchases ignore merchant stock (a `pMobMerc` slot priced in donate is effectively infinite), ignore city tax and the guild-ruler discount, and cannot be traced by the coin audit line (`:290`) with a meaningful price. Conversely, donate items are still subject to BR-09 (empty destination slot) and to BR-22/BR-23 (merchant lifecycle), since those live after the join point.

**Rule workflow**:
1. `Donate = BASE_GetItemAbility(ItemMob, 91)`.
2. `Donate == 0` → continue to coin branch (BR-10).
3. `Donate != 0` → BR-08 → deduct donate → `ItemLog` → `goto DonateBuy` → item delivery (BR-20) → lifecycle (BR-22/23).

---

### Business Rule: Donate balance sufficiency (BR-08)

**Overview**:
The buyer's donate balance must cover the item's donate price; otherwise a formatted "need N donate" panel message is sent and the sale aborts.

**Detailed description**:

The comparison is `Donate > pUser[conn].Donate` (`:77`) on two `int` values, so zero and negative balances are naturally handled: any positive price is unaffordable for a zero balance, and a negative stored balance can never satisfy a positive price. On failure the handler formats `g_pMessageStringTable[_DN_NeedDonate]` with `sprintf(temp, ..., Donate)` (`:79`) - the `_DN_` prefix denotes a message with one integer substitution - and sends it through `SendClientMessage`. The balance itself is not modified and no log entry is produced.

This is the donate analogue of BR-14 and is checked **before** the destination-slot test, which means a broke buyer with a full inventory is told "not enough donate" rather than being echoed an item. That ordering is the opposite of what a strict "cheapest check first" policy would choose, but it is consistent with the coin branch, where the affordability check at `:147` also precedes the slot check at `:159`.

`Donate` is loaded/saved as part of the account file (`STRUCT_ACCOUNTFILE.Donate`, `Code/Basedef.h:853`) and travels in `MSG_DBSaveMob`/`MSG_SavingQuit` (`:996`, `:1177`); it is a real premium currency, not a display counter, which makes this rule financially significant even though the code is three lines.

**Rule workflow**:
1. `Donate > pUser[conn].Donate`?
2. Yes → `sprintf(temp, _DN_NeedDonate, Donate)` → `SendClientMessage` → return.
3. No → BR-09 (donate variant).

---

### Business Rule: Empty destination slot (BR-09)

**Overview**:
The purchased item may only be placed into an empty carry slot; if the requested slot is occupied, the server echoes the occupying item back to the client and aborts the purchase.

**Detailed description**:

The rule appears twice - once in the donate path (`:90-94`) and once in the coin path (`:159-163`) - with identical logic and identical response: `SendItem(conn, ITEM_PLACE_CARRY, MyInvenPos, &pMob[conn].MOB.Carry[MyInvenPos])`. This "echo" is the protocol's way of resynchronising the client's view of the slot the client believed was empty; it does not modify server state. `ITEM_PLACE_CARRY` is `1` (`Code/Basedef.h:87`).

The check deliberately does **not** search for the first free slot; the client chooses the destination. That choice is validated only for range (BR-03) and emptiness, never for "is this a slot the client is allowed to use" - see key finding #8, where `MyInvenPos` up to 63 is accepted while most other handlers cap at `MAX_CARRY-4 = 60`, reserving 60-63.

Because the check happens after the affordability test in the coin branch, a player with insufficient coin never reaches the echo. Because it happens **before** the merchant's stock decrement, a full inventory does not consume stock. In the donate branch the check is after the balance test but before the debit at `:96`, so a full inventory does not consume donate either. The ordering is therefore economically correct: no currency is taken unless the item can be delivered.

**Rule workflow**:
1. `pMob[conn].MOB.Carry[MyInvenPos].sIndex != 0`?
2. Yes → `SendItem(conn, 1, MyInvenPos, current item)` → return (no currency charged).
3. No → proceed (donate debit at `:96`, or coin checks at `:147`).

---

### Business Rule: Authoritative base price (BR-10)

**Overview**:
The item's base price comes only from the server-side item table `g_pItemList[itemIndex].Price`; the packet carries no price field that the server would trust, and `m->Coin` is an output-only field.

**Detailed description**:

`MSG_Buy` (`Code/Basedef.h:1750-1758`) contains `TargetID`, `TargetInvenPos`, `MyInvenPos` and `Coin`. The handler reads the first three and overwrites `Coin` with the post-purchase balance before echoing the struct back (`:276`). There is therefore **no client-controlled monetary input** in this component: the price is `g_pItemList[itemIndex].Price` (`:104`), an `int` field of `STRUCT_ITEMLIST` (`Code/Basedef.h:901`) loaded at boot from the item-list file by `BASE_ReadItemListFile` (`Code/Basedef.cpp:4987-5066`).

This is the component's central security property and the main reason its efferent coupling is dominated by reads rather than by trusting writes. Compare with the sibling `_MSG_ReqBuy.cpp` (player stalls), where the client *does* supply `Price`/`Tax` and the server must cross-check them against the seller's `AutoTrade` mirror (`_MSG_ReqBuy.cpp:71-87`) - that handler needs three `memcmp`/equality checks that this one does not.

The trust boundary descends one level further: because `itemIndex` itself is read from the merchant's carry array rather than from the packet (`:66`), the only way to influence which item is bought is to choose a slot index. A malicious client can pick a hidden slot (key finding #7) but cannot pick an arbitrary item index, cannot invent an item, and cannot influence the price beyond choosing which real item to buy.

**Rule workflow**:
1. `Price = g_pItemList[itemIndex].Price` → `Price2 = Price` (snapshot for tax/audit).
2. Price is then transformed only by server-side data (BR-11, BR-12, BR-13).
3. Continue to BR-11.

---

### Business Rule: City tax surcharge and guild tax basis (BR-11)

**Overview**:
When the merchant stands inside one of the five city zones and that zone's `CityTax` is strictly between 0 and 30, the price is increased by `Price * CityTax / 100`. The surcharge amount, halved, becomes `GuildTax`, the revenue that BR-18 later distributes.

**Detailed description**:

Two server-side lookups feed the rule: `BASE_GetVillage(merchant x, y)` (`Code/Basedef.cpp:4258-4267`) maps coordinates to a zone index by testing the five `STRUCT_GUILDZONE` city rectangles, returning `5` when no rectangle matches; and `g_pGuildZone[Village].CityTax` (`Code/Basedef.h:714-744`, field at `:741`) supplies the tax percentage. The guard at `:109` rejects `Village < 0`, `Village >= 5`, `CityTax <= 0` and `CityTax >= 30`. The comparison `>= 30` makes a legitimate 30% setting behave as **no tax** (key finding #11), even though the GM command `guildtax` accepts exactly that value (`_MSG_MessageChat.cpp:76`).

The surcharge itself is written twice, at `:112-113` and `:115-116`, under `if (Price < 100000)` / `else` with **identical bodies** - the decompilation artifact of a removed price-tier distinction. It uses integer arithmetic (`Price * CityTax / 100`), truncating toward zero, and is followed by the only overflow defence in the function: `if (Price < 0) return;` (`:118`). Because the multiplication is performed in `int`, a base price above roughly `71,000,000` with a 30% tax overflows before the guard can see it.

`GuildTax = (Price - Price2) / 2` (`:121`) is computed immediately after the surcharge and is the *nominal* guild revenue; half is credited to the village impostor and the full amount to the castle impostor (BR-18), so the code intentionally pays out 1.5x the nominal half-share. If the guard at `:109` fires, control jumps to `LABEL_BUY1` and skips the assignment - the variable is then read later at `:263`/`:270` (key finding #3), and the two lookups can disagree because `Village` and `TargetVillage` are computed from the same coordinates but only `TargetVillage` is used by the later guard.

**Rule workflow**:
1. `Village = BASE_GetVillage(x, y)`; `CityTax = g_pGuildZone[Village].CityTax` (OOB read when `Village == 5`).
2. If `Village` invalid or `CityTax` outside `(0,30)` → `goto LABEL_BUY1` (no surcharge, `GuildTax` unassigned).
3. Else `Price += Price * CityTax / 100` → if `Price < 0` return → `GuildTax = (Price - Price2) / 2`.
4. Continue to BR-12.

---

### Business Rule: Guild-ruler discount (BR-12)

**Overview**:
If the buyer belongs to the guild that currently charges the merchant's city, the price is reduced to 70% of the surcharged price (a nominal 30% discount).

**Detailed description**:

The predicate is `Price <= 1999999999 && TargetVillage in [0,5) && Guild > 0 && g_pGuildZone[TargetVillage].ChargeGuild == Guild` (`:129`). `ChargeGuild` is the guild that owns the zone (`STRUCT_GUILDZONE.ChargeGuild`, `Basedef.h:716`), loaded from `Guild.txt` (`CReadFiles.cpp:799-805`) and reassigned at runtime by castle-war logic. The upper-bound `Price <= 1999999999` prevents the multiplication `7 * Price / 10` from overflowing after the surcharge - but it does not prevent the surcharge itself from overflowing (BR-11).

The discount is applied as `Price = 7 * Price / 10` (`:132`), which computes `7*Price` first (bounded because `Price <= ~2e9` would still overflow `7*Price` at `Price > 306,783,564`; the actual safe range is therefore much narrower than the guard implies, an inconsistency worth noting under BR-11/risks). `Desconto = 30` records the percentage for the diagnostic line at `:297`.

The rule is mutually exclusive with BR-13 by the `!Desconto` test in the latter, so a guild ruler who is also a class-3 artisan gets 30%, not a stacked discount. Authority is checked against the *merchant's* village, not the buyer's position: a guild ruler can therefore buy at the discount from a merchant in their city without standing in it, as long as they are within view (BR-05). Because `ChargeGuild` is zero when a zone is unowned, unowned cities grant the discount to nobody.

**Rule workflow**:
1. `Price <= 1999999999` and valid `TargetVillage` and `Guild > 0` and `ChargeGuild == Guild`?
2. Yes → `Desconto = 30` ; `Price = 7 * Price / 10`.
3. No → BR-13.
4. Continue to BR-14.

---

### Business Rule: Class-3 artisan discount label (BR-13)

**Overview**:
A class-3 character that has learned the artisan skill bit `0x800` is treated as entitled to a Special-based "discount"; the handler computes and reports a percentage but never changes the price.

**Detailed description**:

The predicate is `Price <= 1999999999 && !Desconto && MOB.Class == 3 && MOB.LearnedSkill & 0x800` (`:135`). Inside, `special = (CurrentScore.Special[2] / 10) + 6`, clamped to `26` (`:137-140`), assigned to `Desconto` (`:142`), and then halved into the same local (`special /= 2`, `:143`) where the halved value is never used again. The computed `Desconto` therefore has no effect whatsoever on `Price`, on the amount debited, or on any client-visible field - it is only interpolated into the developer diagnostic at `:297` (`discount:%d(%d%%)`, which passes `Price` and `Desconto`, further conflating the two).

Reading the surrounding code, the intended behaviour was almost certainly `Price = Price - Price * (special/2) / 100` or an equivalent subtraction of the halved value; the subtraction statement is missing, and the halving that survived suggests the original formula used the halved percentage. As written, the rule is a no-op that *looks* like a discount in logs and in `SendSay`, which is dangerous for reconciliation: a player at `Special[2] = 100` is logged as receiving a 16% discount while paying full price.

Operationally the rule still matters because it consumes the `!Desconto` slot: a guild ruler never enters it (BR-12 already set `Desconto = 30`), so the two "discount" rules are alternatives even though only one of them has an economic effect. Any future fix must preserve that exclusivity or discounts will stack.

**Rule workflow**:
1. `Price <= 1999999999` and `Desconto == 0` and `Class == 3` and `LearnedSkill & 0x800`?
2. Yes → `special = Special[2]/10 + 6`; if `>= 26` clamp to 26; `Desconto = special`; `special /= 2` (value discarded).
3. `Price` unchanged → BR-14.

---

### Business Rule: Coin sufficiency (BR-14)

**Overview**:
The final price (after surcharge and guild discount) must not exceed the buyer's on-hand coin; otherwise the localized "not enough money" panel is shown and nothing is charged or delivered.

**Detailed description**:

`if (Price > pMob[conn].MOB.Coin)` (`:147`) is a strict greater-than comparison, so a purchase that exactly consumes the balance is allowed and leaves `Coin == 0`. `MOB.Coin` is the character's carried money (`STRUCT_MOB.Coin`, `Code/Basedef.h:448`), which is distinct from the account-level `pUser[conn].Coin` used by cargo/warehouse features and from `Donate`.

Because this check sits *after* BR-11/BR-12/BR-13, the compared value is the fully transformed price, and because it sits *before* BR-09's destination-slot check in the coin branch, a poor player with a full inventory receives only the money message - never the slot echo. The message is `g_pMessageStringTable[_NN_Not_Enough_Money]`, the same string used by the player-to-player path (`_MSG_ReqBuy.cpp:92`), which keeps the client-facing vocabulary consistent across purchase surfaces.

No log entry is emitted on failure. That means coin-based purchase failures are invisible server-side, whereas donate failures are also silent except for the panel message, and index/range failures are logged. For economic forensics (e.g. detecting a client that repeatedly tries to buy items it cannot afford), this rule produces no evidence.

**Rule workflow**:
1. `Price > pMob[conn].MOB.Coin`?
2. Yes → `SendClientMessage(_NN_Not_Enough_Money)` → return.
3. No → BR-15 (guild item probe).

---

### Business Rule: Guild-bound item customization and eligibility (BR-15)

**Overview**:
Six item indices (446, 508, 526-531) plus the medal (509) are "guild items". Before sale they require a defined server index, a non-zero guild counter, and a guild (plus guild level 2 for 526-531); the buyer's guild is stamped into the item's effects, and item 508 is re-indexed by the buyer's sub-guild.

**Detailed description**:

The outer predicate is `itemIndex == 508 || 509 || 446 || (itemIndex >= 526 && <= 531)` (`:165`). Two global preconditions are checked for all of them: `ServerIndex != -1` (`:167`) - otherwise `err,Buy Guild item, but, Server index undefined` - and `GuildCounter != 0` (`:173`) - otherwise `err,Buy Guild item, but, Guild counter zero`. Both are recorded with account/IP. `ServerIndex` is the configured server slot used to namespace guild-medal serials; `GuildCounter` is loaded from `Guild.txt` (`CReadFiles.cpp:799`) and incremented by this handler for medals.

For 446, 508 and 526-531, `MOB.GuildLevel` must be non-zero (`:181`) and for 526-531 at least `2` (`:186`), both rejected with the raw table index `g_pMessageStringTable[114]` (not a named constant - a maintainability smell, since index 114 also happens to be the `_MSG_DropItem` opcode value in `Basedef.h:1638`). Passing these, the handler **writes into the merchant's slot**: `stEffect[0] = {56, Guild}` and `stEffect[1] = {57, Guild}` (`:191-194`). For 508 it additionally reads `BASE_GetSubGuild(pMob[conn].MOB.Equip[12].sIndex)` (`:198`, implementation `Basedef.cpp:4237-4245`, mapping 526-537 to sub-guild 1-3), stamps `{85, sub}` into `stEffect[2]` (`:200-201`), and if `sub` is 1-3 rewrites `sIndex = sub + 531` (`:203-204`) i.e. 532/533/534.

The write-through-merchant pattern is the rule's main defect (key finding #9): the buyer receives the item by `memcpy` from `ItemMob` at `:285`, but the merchant's own slot keeps the stamped item, so the *next* buyer of that slot receives a guild item pre-stamped with the previous buyer's guild, and 508 loses its identity once re-indexed. Because these effect writes overwrite `stEffect[0..2]` unconditionally, any pre-existing bonuses on the shop instance are destroyed. There is no cost multiplier for guild items - they are sold at list price with all normal tax/discount rules applying - so the eligibility rules are pure access control.

**Rule workflow**:
1. `itemIndex` in {446, 508, 509, 526..531}?
2. `ServerIndex == -1` → log → return. `GuildCounter == 0` → log → return.
3. If (446 | 508 | 526..531): `GuildLevel == 0` → message 114 → return; if 526..531 and `GuildLevel < 2` → message 114 → return; stamp effects 56/57 = buyer guild; for 508 additionally effect 85 = sub-guild and possibly re-index to 532..534.
4. If 509 → BR-16.
5. Continue to BR-17.

---

### Business Rule: Guild medal serial issuance (BR-16)

**Overview**:
Buying item 509 (guild medal) mints a unique serial built from `GuildCounter + (ServerIndex << 12)`, stores it in effect 56, stores the raw counter in effect 57, a random value in effect 59, increments the counter, wraps it at 4096 back to 2048, and persists the guild file.

**Detailed description**:

`stEffect[0].cValue = (GuildCounter + (ServerIndex << 12)) >> 8` and `stEffect[1].cValue = GuildCounter` (`:211-214`) split the serial across two effect values so that each fits in the 8-bit `cValue` channel: effect 56 receives the high byte of the combined 24-bit value, effect 57 the low byte. Effect 59 receives `rand()` (`:216-217`) as an anti-duplication token. A diagnostic line `sys,guild medal value:%d count:%d` is logged with the combined value (`:219-222`).

`GuildCounter++` (`:224`) then wraps: at `>= 4096` it is reset to `2048` with `err,GuildCounter Restarted.` (`:226-230`). This is a deliberate address-space reuse policy - serials are only unique within a 2048-4096 window - so two medals minted far apart in time can carry the same serial. `CReadFiles::WriteGuild()` (`:231`, implementation `CReadFiles.cpp:827-866`) rewrites `Guild.txt` with the counter, the five charging guilds, challengers, city taxes, clans and victory flags. That is a synchronous file rewrite on a per-purchase path, so medal purchases are the only buy operation with disk I/O.

`WriteGuild` is a static method invoked without an instance and without error handling; if the file cannot be opened it falls back to `Guild.txt` in the working directory and logs. Because the write happens **before** the coin debit at `:274`, a crash between `:231` and `:274` can advance the persisted counter without charging the buyer (and, symmetrically, a crash after the debit but before the next save loses the coin but keeps the counter).

**Rule workflow**:
1. `itemIndex == 509` (and BR-15 preconditions passed).
2. Stamp effects 56/57/59 with serial halves and random.
3. Log `sys,guild medal value:...`.
4. `GuildCounter++`; if `>= 4096` → `GuildCounter = 2048` + error log.
5. `CReadFiles::WriteGuild()`.
6. Continue to BR-17 (coin path).

---

### Business Rule: Special merchant stock control (BR-17)

**Overview**:
For merchants registered in the 128-entry `pMobMerc` table, each carry slot has a finite stock; buying decrements it and an empty slot blocks the sale with a "no stock" message.

**Detailed description**:

The merchant is matched by identity, not by index: the loop at `:240-245` scans `pMobMerc[0..MAX_MOB_MERC)` (`MAX_MOB_MERC = 128`, `Basedef.h:158`, array at `Server.h:400` / `Server.cpp:449`) and stops at the first entry whose `GenerateIndex` equals `pMob[TargetID].GenerateIndex`. If no entry matches, `x == MAX_MOB_MERC` and the merchant is treated as unlimited stock (the common case for static city merchants). When matched, `pMobMerc[x].Stock[TargetInvenPos] == 0` sends `_NN_NOSTOCK` and aborts (`:250-254`); otherwise `Stock[TargetInvenPos]--` (`:255`).

Stock is configured from the mob-merchant file (`Server.cpp:9341-9370`, `MaxStock[i]` and a live `Stock[i]` copy) and refilled by the periodic timer: `memcpy(pMobMerc[k].Stock, pMobMerc[k].MaxStock, ...)` every `RenewTime` seconds, with mob regeneration every `RebornTime` (`ProcessSecMinTimer.cpp:1341-1348`). Stock is indexed by the *same* `TargetInvenPos` used to pick the item, so the finite-stock graph is aligned with the shop layout.

Three structural weaknesses follow from the implementation. First, the check and the decrement sit **inside** `if (Price >= 0)` (`:236`) and **after** the donate branch's jump, so donate-priced slots never consume stock. Second, the check occurs after the coin sufficiency test but before the debit, so a failed debit cannot leak stock - correct ordering - yet it occurs before the guild-tax payout, so the merchant is charged down even if the impostor bookkeeping later misbehaves. Third, because the scan matches only `GenerateIndex`, two `pMobMerc` entries sharing one generator would have only the first considered.

**Rule workflow**:
1. Scan `x` in `[0,128)` for `pMobMerc[x].GenerateIndex == pMob[TargetID].GenerateIndex`.
2. If found: `Stock[TargetInvenPos] == 0` → `_NN_NOSTOCK` → return; else `Stock--`.
3. If not found → unlimited stock, continue.
4. Continue to BR-18.

---

### Business Rule: Guild tax revenue distribution (BR-18)

**Overview**:
When the merchant is in a taxed city, half of the surcharge is credited as experience to the village's guild impostor NPC and the full surcharge to the zone-4 (castle) impostor NPC, each capped at 200,000,000,000 exp.

**Detailed description**:

The guard at `:258` repeats the tax-validity test using `TargetVillage` and `CityTax`: `TargetVillage in [0,5) && CityTax in (0,30)`. Two independent credits follow. For the merchant's own village, `GuildImpostoID[TargetVillage]` (`Server.h:60`, array `Server.cpp`) must be a live NPC slot `>= MAX_USER && < MAX_MOB`, with `Mode != USER_EMPTY` and `MOB.Guild == g_pGuildZone[TargetVillage].ChargeGuild`; the NPC gains `GuildTax / 2` (`:263`). For zone 4 (the castle), the same checks apply with `GuildImpostoID[4]` and the NPC gains the full `GuildTax` (`:270`), additionally requiring `GuildTax > 0`.

Each credit is individually capped: if the NPC's `Exp` is already `>= 200000000000`, the credit is skipped entirely rather than clamped to the cap (`:262`, `:269`). The cap exists because `Exp` is a `long long` (`Basedef.h:450`) used for guild-level progression, and the cap prevents a high-traffic city from pushing an NPC past the level table's range. `USER_EMPTY` is used for NPCs while the player-to-player path uses `MOB_EMPTY` in the equivalent check (`_MSG_ReqBuy.cpp:168`); both constants are used interchangeably in this area of the codebase.

The economic consequence is that the server pays out 1.5x the nominal half-share: 50% to the village guild and 100% to the castle guild, funded effectively out of thin air rather than deducted from the buyer (the buyer already paid the surcharge, so the buyer's loss equals 100% while the recipients gain 150%). Also note the rule is evaluated even when `LABEL_BUY1` was taken (no surcharge), in which case `CityTax` may be valid while `GuildTax` is uninitialized (key finding #3), making the credit amount indeterminate.

**Rule workflow**:
1. `TargetVillage in [0,5) && CityTax in (0,30)`?
2. Village NPC valid (index, mode, owning guild)? → `Exp += GuildTax / 2` if `Exp < 2e11`.
3. Zone-4 NPC valid and `GuildTax > 0`? → `Exp += GuildTax` if `Exp < 2e11`.
4. Continue to BR-19.

---

### Business Rule: Coin debit and balance synchronisation (BR-19)

**Overview**:
The price is deducted from the buyer's carried coin, and the server pushes the new balance twice: by echoing the mutated `MSG_Buy` request back, and by sending a full `MSG_UpdateEtc` economy snapshot.

**Detailed description**:

`pMob[conn].MOB.Coin -= Price` (`:274`) is the only monetary mutation on the coin path and has no lower-bound guard beyond BR-14, so the balance cannot go negative through this handler (an already-negative balance, however, would remain negative). The new balance is written into the inbound packet's `Coin` field (`:276`) and the packet's `ID` is overwritten with `ESCENE_FIELD` (30000, `:278`) before being queued with `cSock.AddMessage((char*)m, m->Size)` (`:280`). Reusing the request struct as the response is a memory optimisation used across this codebase; here it has the side effect that the response's length is client-controlled (key finding #6).

`SendEtc(conn)` (`:282`, implementation `SendFunc.cpp:1195-1231`) then sends `MSG_UpdateEtc` carrying `Exp`, `SpecialBonus`, `Learn`, `ScoreBonus`, `SkillBonus`, `Coin`, `Hold` and `Magic`. If the socket write fails, `SendEtc` calls `CloseUser(conn)` - a behaviour the caller does not check, so a closed connection mid-purchase still proceeds to deliver the item into a session being torn down. The item copy and `SendItem` at `:285-293` happen after that, on the same `conn` index, which is safe only because `CloseUser` does not zero the `pMob` carry array immediately.

Note the sequencing asymmetry: coin is debited **before** the item is copied into the inventory (`:274` vs `:285`), and the audit log at `:290` is written after the copy. There is no rollback path anywhere in the function - the code is not transactional - so an exception or early return between `:274` and `:285` would leave the buyer charged with no item. In the actual control flow no such early return exists, but the ordering means correctness depends on that absence rather than on any invariant.

**Rule workflow**:
1. `Coin -= Price`.
2. `m->Coin = Coin`; `m->ID = ESCENE_FIELD`.
3. `cSock.AddMessage(m, m->Size)`.
4. `SendEtc(conn)` (may `CloseUser` on write failure).
5. Continue to BR-20.

---

### Business Rule: Item delivery and audit trail (BR-20)

**Overview**:
The purchased item is copied verbatim from the merchant's slot to the buyer's chosen slot, a compact item code is generated, a free-text audit line is logged, and the slot is pushed to the client.

**Detailed description**:

`memcpy(&pMob[conn].MOB.Carry[MyInvenPos], ItemMob, sizeof(STRUCT_ITEM))` (`:285`) copies 8 bytes (`STRUCT_ITEM` = `short sIndex` + three 2-byte effect unions, `Basedef.h:398-412`), including any guild stamping done earlier (BR-15). It is a plain overwrite; the destination was proven empty by BR-09, so no data is lost. `BASE_GetItemCode(ItemMob, tmplog)` (`:288`, implementation `Basedef.cpp:1479`) serialises the item into a human-readable code including index and effects, which is then embedded in the audit string `buy_npc,npc:%s price:%d item:%s` with the merchant's `MobName` and `Price` (`:290`).

`ItemLog` (`Server.cpp:9264-9290`) writes to the item log file with a timestamped, IP-decorated prefix. The audit line is the primary forensic record of NPC purchases and is emitted on **both** currency branches (including donate), but on the donate path `Price` is uninitialized (key finding #1), so the recorded price for donate purchases is arbitrary - a direct forensic-integrity problem for premium-currency sales. Contrast the donate branch's own earlier log at `:98-99`, which correctly records the donate amount and the remaining balance.

Finally `SendItem(conn, ITEM_PLACE_CARRY, MyInvenPos, &pMob[conn].MOB.Carry[MyInvenPos])` (`:293`) sends `MSG_SendItem` with the slot and the item (guarded by `conn` range, `USER_PLAY`, non-zero socket in `SendFunc.cpp:1055-1082`). No `GridMulticast` or world-visible item creation occurs: other players do not see the item appear, and the merchant's own visual inventory is not updated for regular merchants.

**Rule workflow**:
1. `memcpy` merchant slot → buyer slot.
2. `BASE_GetItemCode` → `sprintf` audit string → `ItemLog`.
3. `SendItem(conn, 1, MyInvenPos, item)`.
4. Continue to BR-21 / BR-22 / BR-23.

---

### Business Rule: Developer price trace (BR-21)

**Overview**:
When the global `FREEEXP` is set to the magic value `-2`, the merchant speaks the price breakdown into local chat, exposing base price, tax-inclusive price, tax percentage, final price and discount percentage to nearby players.

**Detailed description**:

`if (FREEEXP == -2)` (`:295`) gates a single `sprintf`/`SendSay` pair: `"%s base:%d tax:%d(%d%%) discount:%d(%d%%)"` filled with the item name, `Price2` (base), `bPrice` (tax-inclusive), `CityTax`, `Price` (final) and `Desconto` (`:297`). `SendSay` (`SendFunc.cpp:1647-1660`) builds `MSG_MessageChat` with the merchant's ID and `GridMulticast`s it, i.e. every player within view sees the merchant "say" it. `FREEEXP` is an otherwise unrelated newbie-level threshold (`Server.cpp:43`, default 35, configurable) whose `-2` value is repurposed here as a debug switch - an arbitrary overload with no constant or comment.

The trace is doubly misleading given the other rules: `Desconto` may be a label that was never applied (BR-13), and the field printed as `discount:%d` is `Price`, not the discount amount, while `(%d%%)` is `Desconto`. So the line reads as `discount:<final price>(<percentage>%)`. On the donate path all six operands except the item name are indeterminate.

Because the switch is global and production-visible (local chat to all nearby players), leaving `FREEEXP = -2` in a live configuration would broadcast internal pricing of every purchase to players. It is a diagnostic aid, not a business rule, but it is observable game behaviour and is therefore documented here.

**Rule workflow**:
1. `FREEEXP == -2`?
2. Yes → `sprintf` breakdown → `SendSay(TargetID, temp)` → multicast in view.
3. Continue to BR-22.

---

### Business Rule: Potion merchant depletion (BR-22)

**Overview**:
Buying item 787 or 481 increments the merchant's `PotionCount`; when the count exceeds the configurable `POTIONCOUNT` (default 10), the merchant mob is deleted from the world.

**Detailed description**:

`if (itemIndex == 787 || itemIndex == 481)` (`:301`) covers two specific consumable indices. `pMob[TargetID].PotionCount` is a per-mob counter (`Code/TMSrv/CMob.h:74`, zeroed in the `CMob` constructor `CMob.cpp:35`), and `POTIONCOUNT` is a global `int` (`Server.h:278`) defaulting to `10` and settable at runtime through the GM command path (`imple.cpp:621`) and the config line (`Server.cpp:322`, `:920`). When `PotionCount > POTIONCOUNT`, `DeleteMob(TargetID, 3)` removes the merchant (`:306`).

`DeleteMob` (`Server.cpp:7020-7054`) broadcasts `MSG_RemoveMob` with `RemoveType = 3` to the grid, decrements the generator's live count, zeroes `Hp`/`Mode`, clears the mob-grid cell and calls `RemoveParty`. Because the removal is generator-tracked, the mob can be respawned by the normal regeneration cycle (`ProcessSecMinTimer.cpp:1347-1348` for `pMobMerc` entries). The deletion happens **after** the item has been delivered and paid for, so the buyer is never charged for a vanished merchant.

The rule models a "limited stock potion vendor" without using the `pMobMerc` stock table, i.e. two independent finite-stock mechanisms coexist. Because the check is `>` rather than `>=`, a merchant with `POTIONCOUNT = 10` survives exactly 11 sales and is removed on the 11th. Note the counter is never reset except by respawn (new mob instance) - `DeleteMob` is what ends the merchant's life, so the rule is effectively "every N+1 potion sales, the vendor leaves".

**Rule workflow**:
1. `itemIndex == 787 || 481`?
2. Yes → `PotionCount++`.
3. `PotionCount > POTIONCOUNT`? → `DeleteMob(TargetID, 3)`.
4. Else continue.

---

### Business Rule: Self-destruct merchants (BR-23)

**Overview**:
A merchant whose equipped item 0 is index 259 or 230 is destroyed after any successful purchase, regardless of what was bought.

**Detailed description**:

`if (pMob[TargetID].MOB.Equip[0].sIndex == 259 || == 230)` (`:308`) then `DeleteMob(TargetID, 3)` (`:309`). Unlike BR-22 this rule is not gated on the purchased item; it is a property of the vendor. The equipped head slot (`Equip[0]`) is used here as a behaviour flag - a "disposable shop" marker - which is a data-driven scripting idiom common in this codebase (compare merchants wearing specific items elsewhere).

The check runs after item delivery, so the sale always completes first. It is not conditioned on `Price` or on the donate branch inside the block, but it *is* inside `if (Price >= 0)`, meaning a donate purchase executes it only if the indeterminate `Price` happens to be non-negative (key finding #1 again). Because `DeleteMob` also decrements the generator count and clears the grid, a `259`/`230` merchant disappears from the world permanently after the first buyer, and every other player's shop window for it becomes stale.

The rule interacts with BR-22: if a merchant both wears 259/230 and sells potions, the `PotionCount` branch runs first, then the equipment branch runs on an already-deleted mob (a second `DeleteMob` on a dead slot, which re-broadcasts removal and re-decrements the generator count, clamped at zero in `DeleteMob`).

**Rule workflow**:
1. After delivery: `pMob[TargetID].MOB.Equip[0].sIndex in {259, 230}`?
2. Yes → `DeleteMob(TargetID, 3)`.
3. Continue.

---

### Business Rule: Negative price abort (BR-24)

**Overview**:
A computed negative price is treated as corruption: the transaction is refused and a "MINUS" error is logged.

**Detailed description**:

There are two negative-price guards. The early one at `:118-119` returns silently if the surcharge produced a negative value - this is the practical overflow guard, though it only fires *after* the overflowing multiplication. The late one at `:236` / `:311-312` is the `if (Price >= 0)` envelope around the entire settlement block; when `Price < 0` the else-branch logs `err,MSG_ReqBuy  <><><><><><> MINUS <><><><><><>` with account/IP. The message text names `_MSG_ReqBuy`, a copy-paste artefact from the sibling handler.

Because the settlement block does nothing when `Price < 0`, no coin is debited, no stock is consumed, no tax is distributed and no item is delivered - the transaction is fully abandoned. However, the same `Price >= 0` test is what a donate purchase evaluates against an *uninitialized* `Price`, so the intended "safety envelope" is exactly the mechanism that makes donate sales non-deterministic.

The guard also documents an architectural limitation: prices, taxes and discounts are all `int` arithmetic with ad-hoc bounds checks scattered between the operations rather than a single validated money type or 64-bit accumulator. The `Price <= 1999999999` guards on the two discounts are attempts to keep the result inside `int` but appear after the surcharge that can already overflow.

**Rule workflow**:
1. After tax: `Price < 0` → silent return.
2. After all pricing: `Price >= 0` → full settlement; else → `Log("err,MSG_ReqBuy ... MINUS ...")`.
3. Function ends.

---

### Business Rule: No persistence in the handler (BR-25)

**Overview**:
Purchases mutate only in-memory structures; durability relies entirely on the periodic save or the logout/quit save path.

**Detailed description**:

There is no `SaveUser`, no `MSG_DBSaveMob`, and no DB socket write anywhere in `_MSG_Buy.cpp`. The only disk interaction reachable from the handler is `CReadFiles::WriteGuild()` for medal purchases (`:231`). Coin, donate, inventory and merchant stock all live in `pMob`/`pUser`/`pMobMerc` until some external path persists them, and `pMobMerc.Stock` is not persisted at all (rebuilt from `MaxStock` by the renew timer, `ProcessSecMinTimer.cpp:1345`).

This is a deliberate architectural choice consistent with the rest of `TMSrv`: the game server batches writes, and the DB server owns durability. Its consequences for the Buy Handler are threefold. First, a server crash loses all purchases since the last save, including guild-tax credits to impostor NPC experience. Second, the handler cannot report failure of persistence, so the client's "purchase succeeded" and the server's durable state can diverge. Third, there is no transaction boundary spanning the coin debit, the item grant and the guild-file rewrite, so the ordering of side effects (BR-16 writes to disk *before* the coin debit at `:274`) can leave the persistent guild counter ahead of the persistent economy state.

For contrast, the player-to-player handler explicitly calls `SaveUser(targetID, 1)` and `SaveCargoCoin` (`_MSG_ReqBuy.cpp:162-163`) because it settles between two characters whose sessions may end independently; the NPC handler has no such requirement and takes the simpler path.

**Rule workflow**:
1. Mutations applied in memory only.
2. Authorization to persist deferred to the periodic save loop / `CharLogOut`.
3. Guild medal purchase additionally rewrites `Guild.txt` synchronously.

---

### Business Rule: Shop list is advisory, not enforced (BR-26)

**Overview**:
The server enforces price, ownership and stock, but it does not enforce that the requested slot was part of the shop list the client was shown; any of the merchant's 64 carry slots is purchasable.

**Detailed description**:

`SendShopList` (`SendFunc.cpp:1369-1414`) transmits 27 entries (`MAX_SHOPLIST = 27`, `Basedef.h:82`) using the index remap `invpos = (i % 9) + (i / 9) * MAX_SHOPLIST` (`:1393-1395`), which yields carry slots 0-8, 27-35 and 54-62 - a 3-row window into a 64-slot array. The Buy Handler, by contrast, validates `TargetInvenPos` only against `MAX_CARRY` (`:49`), so slots 9-26, 36-53 and 63 are reachable even though no client UI ever displays them.

The impact is bounded by BR-10: the price of a hidden slot is still the authoritative item-list price, so this is not a price-tampering vector. It is an inventory-exposure vector (a modified client can enumerate the merchant's entire carry array, including stock reserved for quests or events) and a rule-consistency defect (the shop window is meant to define the offer). A tightening would require remembering the displayed window per session, which the server does not track anywhere.

The same "advisory" property applies to the merchant selection itself: the handler does not verify that the buyer ever opened *this* merchant's shop window, only that the merchant is a type-1 merchant in view (BR-04, BR-05). Combined with the ability to script requests, a client can buy from any nearby shop without UI interaction; the server-side gate is physical proximity, not session state.

**Rule workflow**:
1. Client sends `TargetInvenPos` in `[0,64)`.
2. Server validates range (BR-03) and item validity (BR-06) only.
3. No comparison against the 27-slot displayed window → hidden slots purchasable at list price.

---

## 4. Component Structure

### 4.1 File and directory context

```
W2PP Code Project.sln                     # single solution, 2 projects, no test project
Code/
├── Basedef.h                             # world constants + all message structs (MSG_Buy at 1750)
├── Basedef.cpp                           # BASE_* primitives (GetItemAbility, GetVillage, GetSubGuild,
│                                         #   GetItemCode, ReadItemListFile, CheckPacket[disabled])
├── ItemEffect.h                          # EF_* effect ids (EF_DONATE = 91 at :134)
├── CPSock.cpp/.h                         # framing, de-obfuscation, AddMessage (513-591), ReadMessage (353-...)
├── TMSrv/
│   ├── _MSG_Buy.cpp                      # <-- THIS COMPONENT (313 lines, 9,024 bytes, 1 function)
│   ├── ProcessClientMessage.h            # declaration at :109 ; also declares GuildImpostoID at :64
│   ├── ProcessClientMessage.cpp          # dispatch: case _MSG_Buy at :226-228
│   ├── Server.h                          # pUser, pMob, pMobMerc, FREEEXP, POTIONCOUNT, GuildImpostoID
│   ├── Server.cpp                        # AddCrackError :684, DeleteMob :7020, RemoveTrade :7319,
│   │                                     #   Log :6701, ItemLog :9264, merc file loader :9341-9370
│   ├── SendFunc.cpp                      # SendClientMessage :27, SendClientSignal :197, SendItem :1055,
│   │                                     #   SendEtc :1195, SendShopList :1369, SendHpMode :1622, SendSay :1647
│   ├── GetFunc.cpp                       # GetInView :640
│   ├── CReadFiles.cpp                    # ReadGuild :772, WriteGuild :827
│   ├── ProcessSecMinTimer.cpp            # pMobMerc stock renew :1341-1348
│   ├── CUser.h                           # CUser: TradeMode :46, Trade :59, AutoTrade :65, Donate :105
│   └── CMob.h/.cpp                       # CMob: PotionCount :74, init :35
└── DBSrv/                                # separate process; not reachable from this handler
```

### 4.2 Internal anatomy of `_MSG_Buy.cpp`

| Lines | Block | Purpose |
|-------|-------|---------|
| 1-18 | License header | GPLv3 boilerplate + contact stub |
| 19 | `#include "ProcessClientMessage.h"` | single include; pulls `Server.h`, `Basedef.h`, `SendFunc.h`, `GetFunc.h`, `Language.h` transitively |
| 21-23 | Function signature + cast | `MSG_Buy *m = (MSG_Buy*)pMsg` - no copy, no length check |
| 25-30 | BR-01 | liveness/mode gate |
| 32-43 | BR-02 | trade-state teardown |
| 45-53 | BR-03 + extraction | three selectors, range validation, log |
| 55-59 | BR-04 | merchant type 1 |
| 60-64 | BR-05 | view-gate + CloseShop |
| 66-71 | BR-06 | item index resolution + validity |
| 73-102 | BR-07/08/09 | donate branch and `goto DonateBuy` |
| 104-122 | BR-10/11/24 | price, tax, GuildTax, `LABEL_BUY1` |
| 124-145 | BR-12/13 | guild discount, artisan label |
| 147-163 | BR-14/09 | coin sufficiency, destination slot |
| 165-233 | BR-15/16 | guild items, medal serials, `WriteGuild` |
| 236-256 | BR-17 | `if (Price >= 0)` envelope + merc stock |
| 258-273 | BR-18 | guild-tax distribution |
| 274-282 | BR-19 | coin debit + echo + `SendEtc` |
| 283-293 | BR-20 | `DonateBuy` label, item copy, audit log, `SendItem` |
| 295-299 | BR-21 | `FREEEXP == -2` trace |
| 301-309 | BR-22/23 | merchant lifecycle |
| 311-312 | BR-24 | negative-price log |

Structural observations: the function is 313 lines with 26 rules, 2 labels and 4 `return`-heavy gates; cyclomatic complexity is dominated by the gate ladder plus the guild-item block. All state is global (`pMob`, `pUser`, `g_pItemList`, `g_pGuildZone`, `pMobMerc`, `GuildImpostoID`, `GuildCounter`, `ServerIndex`, `FREEEXP`, `POTIONCOUNT`); there is no encapsulation, no parameter object beyond the raw packet, and no error type - every failure is a bare `return`.

### 4.3 Message contract (the component exposes no endpoints)

No HTTP, GraphQL, gRPC, REST or RPC surface exists. The component is a binary packet handler inside a custom encrypted TCP protocol. Its full wire contract:

| Direction | Message | Opcode | Struct | Size (Win32, default alignment) |
|-----------|---------|--------|--------|----------------------------------|
| client → server | `_MSG_Buy` | `121 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME` (`Basedef.h:1749`) | `MSG_Buy` (`Basedef.h:1750-1758`) | 24 bytes nominal (12-byte `_MSG` header `Basedef.h:925-930` + `ushort TargetID` + `short TargetInvenPos` + `short MyInvenPos` + 2 bytes padding + `int Coin`) |
| server → client | `_MSG_Buy` (echo of the same struct) | 121 | `MSG_Buy` | **whatever `m->Size` the client sent** (`:280`) |
| server → client | `_MSG_SendItem` | `_MSG_SendItem` | `MSG_SendItem` | `sizeof(MSG_SendItem)` (`SendFunc.cpp:1070`) |
| server → client | `_MSG_UpdateEtc` | `_MSG_UpdateEtc` | `MSG_UpdateEtc` | `sizeof(MSG_UpdateEtc)` (`SendFunc.cpp:1211`) |
| server → client | `_MSG_MessagePanel` | `_MSG_MessagePanel` | `MSG_MessagePanel` | `sizeof(MSG_MessagePanel)` (`SendFunc.cpp:35`) |
| server → client | `_MSG_CloseShop` | `150` (`Basedef.h:1819`) | `MSG_STANDARD` | `sizeof(MSG_STANDARD)` (`SendFunc.cpp:197-206`) |
| server → grid | `_MSG_MessageChat` | from `SendSay` | `MSG_MessageChat` | `sizeof(MSG_MessageChat)` (`SendFunc.cpp:1652-1659`) |
| server → client/merchant | `_MSG_RemoveMob` | from `DeleteMob` | `MSG_RemoveMob` | `sizeof(MSG_RemoveMob)` (`Server.cpp:7022-7030`) |

| Inbound field | Type | Consumed as | Validated |
|---------------|------|-------------|-----------|
| `Size` | `short` | Outbound echo length only (`:280`) | Only by `ReadMessage` (`CPSock.cpp:397-411`); the per-type check at `Basedef.cpp:6525` is inside the commented-out `BASE_CheckPacket` |
| `KeyWord`, `CheckSum` | `char` | Not read | De-scrambled/checked by `ReadMessage` |
| `Type` | `short` | Dispatch key | Exact match in dispatcher |
| `ID` | `short` | **Overwritten** with `ESCENE_FIELD` (`:278`) | Dispatcher validates `std->ID`, not `conn` |
| `ClientTick` | `unsigned int` | Not read | Not validated here |
| `TargetID` | `unsigned short` | NPC index (`:45`) | `[1000, 25000)` (`:49`) |
| `TargetInvenPos` | `short` | Merchant carry slot (`:46`) | `[0, 64)` (`:49`) |
| `MyInvenPos` | `short` | Buyer carry slot (`:47`) | `[0, 64)` (`:49`) - **not** the `MAX_CARRY-4` used elsewhere |
| `Coin` | `int` | **Ignored inbound**, set to post-purchase balance (`:276`) | n/a |

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains

```
ProcessClientMessage (Code/TMSrv/ProcessClientMessage.cpp:38)
  └─ case _MSG_Buy (:226)
      └─ Exec_MSG_Buy (Code/TMSrv/_MSG_Buy.cpp:21)
          ├─ SendHpMode ────────────────────────────► SendFunc.cpp:1622  (► MSG_SetHpMode, CloseUser)
          ├─ AddCrackError ─────────────────────────► Server.cpp:684     (► Log, SendClientMessage, CharLogOut)
          ├─ RemoveTrade ───────────────────────────► Server.cpp:7319    (► SendClientSignal, GetCreateMob, GridMulticast)
          ├─ Log ───────────────────────────────────► Server.cpp:6701
          ├─ SendClientMessage ─────────────────────► SendFunc.cpp:27
          ├─ GetInView ─────────────────────────────► GetFunc.cpp:640
          ├─ SendClientSignal ──────────────────────► SendFunc.cpp:197
          ├─ BASE_GetItemAbility ───────────────────► Basedef.cpp:1647   (► BASE_GetItemSanc, BASE_GetBonusItemAbility)
          ├─ BASE_GetVillage ───────────────────────► Basedef.cpp:4258   (reads g_pGuildZone[0..4])
          ├─ BASE_GetSubGuild ──────────────────────► Basedef.cpp:4237
          ├─ BASE_GetItemCode ──────────────────────► Basedef.cpp:1479
          ├─ ItemLog ───────────────────────────────► Server.cpp:9264
          ├─ SendItem ──────────────────────────────► SendFunc.cpp:1055
          ├─ DeleteMob ─────────────────────────────► Server.cpp:7020    (► GridMulticast, RemoveParty)
          ├─ CReadFiles::WriteGuild ────────────────► CReadFiles.cpp:827
          ├─ SendEtc ───────────────────────────────► SendFunc.cpp:1195   (► CloseUser on write failure)
          ├─ SendSay ───────────────────────────────► SendFunc.cpp:1647   (► GridMulticast)
          └─ CPSock::AddMessage ────────────────────► CPSock.cpp:513
```

### 5.2 Global state read/written

| Symbol | Declared | Access in handler | Semantics |
|--------|----------|-------------------|-----------|
| `pMob[MAX_MOB]` | `Server.h` | read + **write** (`Coin`, `Carry`, `PotionCount`, mob lifetime) | unified mob/player array; `[0,1000)` are players |
| `pUser[MAX_USER]` | `Server.h:64/229` | read + write (`Donate`, `Trade`, `TradeMode`) | session/account state (`CUser`) |
| `g_pItemList[MAX_ITEMLIST]` | `Basedef.h:2467` | read (`Price`, `Name`) | item table loaded at boot |
| `g_pGuildZone[MAX_GUILDZONE]` | `Basedef.h:2441` | read (`CityTax`, `ChargeGuild`) | 5 zone descriptors from `Guild.txt` |
| `pMobMerc[MAX_MOB_MERC]` | `Server.h:400`, `Server.cpp:449` | read + **write** (`Stock`) | limited-stock merchant table |
| `GuildImpostoID[MAX_GUILDZONE]` | `Server.h:60` | read (`pMob[...]` via index) | collector NPC slots per zone |
| `GuildCounter` | `Server.h:205` | read + **write** | medal serial counter, persisted via `WriteGuild` |
| `ServerIndex` | `Server.h:202` | read | server slot id, namespaces medal serials |
| `FREEEXP` | `Server.h:196` (`Server.cpp:43` = 35) | read (`== -2` debug switch) | newbie threshold repurposed as debug flag |
| `POTIONCOUNT` | `Server.h:278` (`Server.cpp:322` = 10) | read | potion-vendor sales cap |
| `temp`, `fLogFile`, `hWndMain` | `Server.cpp` / `Server.h` | **write via sprintf** | shared global formatting buffer used by `Log` |

### 5.3 External / platform dependencies

| Dependency | Kind | Use | Risk note |
|-----------|------|-----|-----------|
| `sprintf` (CRT) | libc | messages, audit strings, debug trace | several `%s` with fixed-size destinations (`temp`, `tmplog[2048]`); item names capped by `ITEMNAME_LENGTH` |
| `memcpy` (CRT) | libc | `STRUCT_ITEM` copy (8 bytes) | destination proven empty |
| `rand` (CRT) | libc | medal effect-59 token (`:217`) | not cryptographically unique; duplicates possible |
| `Win32` / MSVC v143 | platform | build only (`TMSrv.vcxproj`, `<Platform>Win32</Platform>`) | x86 `int` = 32-bit is assumed by all price arithmetic |
| `Guild.txt` (filesystem) | data file | rewritten synchronously on medal purchase | non-atomic `fopen("wt")` + `fprintf` |
| DB server (`DBSrv`) | process | **not used by this handler** | persistence is deferred, not delegated here |

No HTTP client, no external service, no database call, no crypto API is used by this component.

---

## 6. Afferent and Efferent Coupling

Measured at **module edge** granularity the handler reproduces the architecture report exactly: **Ca = 1**, **Ce = 14** (module edges: `ProcessClientMessage`, `Server`, `SendFunc`, `GetFunc`, `Basedef`, `CReadFiles`, `CPSock`, `Language`/message table, plus CRT and Win32 groups). At **symbol** granularity the fan-out decomposes into 17 project functions. Both views are shown because they answer different questions: the module view says "this handler is a leaf in the call graph"; the symbol view says "this handler is a hub of global-state reads".

### 6.1 Component-level coupling

| Component | Afferent (known callers) | Efferent (callees) | Instability `Ce/(Ca+Ce)` | Criticality |
|-----------|--------------------------|--------------------|--------------------------|-------------|
| `Exec_MSG_Buy` (function) | 1 (`ProcessClientMessage.cpp:226`) | 17 project + 4 CRT/1 method | 0.94 | High |
| `MSG_Buy` (struct) | 1 live use (handler) + 1 dead (`Basedef.cpp:6525`) | 0 | 0.00 | Medium |
| `_MSG_Buy` (opcode const) | 2 (`ProcessClientMessage.cpp:226`, `Basedef.cpp:6525` dead) | 0 | 0.00 | Low |
| `CPSock::AddMessage` (method, shared) | 46 callers | 4 | 0.08 | High (shared) |
| `pMob` / `pUser` global arrays | > 100 (project-wide) | 0 | 0.00 | Critical (shared) |

### 6.2 Callee fan-out (efferent details)

| Callee | Defined at | Afferent (known callers) | Role in this component | Failure behaviour |
|--------|------------|--------------------------|------------------------|-------------------|
| `SendClientMessage` | `SendFunc.cpp:27` | not enumerated (project-wide) | user-visible rejection | silently drops for invalid `conn` |
| `SendClientSignal` | `SendFunc.cpp:197` | 10 | `_MSG_CloseShop` | none |
| `SendItem` | `SendFunc.cpp:1055` | 36 | give item, echo occupied slot | none |
| `SendEtc` | `SendFunc.cpp:1195` | 24 | economy snapshot | `CloseUser(conn)` on write failure |
| `SendHpMode` | `SendFunc.cpp:1622` | 23 | HP/mode resync | `CloseUser` on failure |
| `SendSay` | `SendFunc.cpp:1647` | 7 | debug price trace | none |
| `ItemLog` | `Server.cpp:9264` | 19 | audit trail | none |
| `Log` | `Server.cpp:6701` | project-wide | error/telemetry | none |
| `AddCrackError` | `Server.cpp:684` | 21 | anti-abuse counter | `CharLogOut` at 2e9 |
| `RemoveTrade` | `Server.cpp:7319` | 28 | trade teardown | none |
| `DeleteMob` | `Server.cpp:7020` | 21 | merchant removal | none |
| `GetInView` | `GetFunc.cpp:640` | 5 | proximity gate | returns `FALSE` |
| `BASE_GetItemAbility` | `Basedef.cpp:1647` | project-wide | donate price | returns 0 |
| `BASE_GetVillage` | `Basedef.cpp:4258` | 15 | zone resolution | returns 5 (caller mishandles) |
| `BASE_GetSubGuild` | `Basedef.cpp:4237` | 2 | item 508 sub-guild | returns 0 |
| `BASE_GetItemCode` | `Basedef.cpp:1479` | 22 | audit serialisation | none observed |
| `CReadFiles::WriteGuild` | `CReadFiles.cpp:827` | 6 | persist medal counter | logs on open failure |

### 6.3 Coupling assessment

Efferent coupling is high and *data* coupling dominates: of the 17 callees, 9 are pure state mutators (`SendItem`, `SendEtc`, `DeleteMob`, `RemoveTrade`, `AddCrackError`, `WriteGuild`, `SendClientMessage`, `SendClientSignal`, `SendHpMode`), 6 are read-only helpers (`BASE_Get*`, `GetInView`, `ItemLog`), and 2 are mixed (`Log`, `SendSay`). There is no abstraction layer between the handler and the world model - it indexes `pMob`, `pUser`, `pMobMerc` and `g_pGuildZone` directly, so any change to those structures' semantics silently changes this component.

Afferent coupling of 1 is the component's main structural virtue: it is trivially isolated for testing-by-substitution in principle, and no other code can regress when the handler changes. The price of that isolation is that the handler is also unreachable for reuse - there is no way to perform an NPC purchase programmatically (no internal "buy" function), so any future feature (auction house, GM grant, quest vendor) must duplicate this 313-line decision tree.

Cohesion is functional at the file level (one function, one job) but low at the statement level: 26 rules share 5 mutable locals (`Price`, `Price2`, `bPrice`, `Desconto`, `GuildTax`) whose initialization is control-flow dependent, which is precisely how key findings #1 and #3 arise.

---

## 7. Endpoints

The component exposes **no HTTP, REST, GraphQL, gRPC or RPC endpoints**. It is a binary packet handler in a custom encrypted TCP protocol; its only inbound message is `_MSG_Buy` (opcode 121), documented field by field in section 4.3, and it has no re-usable entry point besides `Exec_MSG_Buy` itself.

For completeness, the adjacent message that opens the purchase flow (and establishes the slot indices this handler consumes) is `_MSG_REQShopList` (opcode 123, `Basedef.h:1779`), served by `Exec_MSG_REQShopList` (`_MSG_REQShopList.cpp:20`), which replies with `_MSG_ShopList` (opcode 124, `Basedef.h:1650`). Neither message is owned by this component; the Buy Handler only assumes the `TargetInvenPos` domain that `SendShopList` laid out.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol / mechanism | Data format | Error handling |
|-------------|------|---------|----------------------|-------------|----------------|
| Client socket (player → TMSrv) | Inbound network | Purchase request | custom encrypted TCP, `CPSock::ReadMessage` (`Server.cpp:3975`, `CPSock.cpp:353`) | binary `MSG_Buy` | length validated by `ReadMessage`; per-type size check disabled; handler returns `void` |
| Client socket (TMSrv → buyer) | Outbound network | Balance echo, item grant, messages, shop close | `CPSock::AddMessage` (`CPSock.cpp:513`) | binary packets | `AddMessage` returns `FALSE` on buffer full; only `SendEtc`/`SendHpMode` react (`CloseUser`) |
| Grid broadcast | Outbound network | Merchant removal, debug chat | `GridMulticast` (`SendFunc.cpp:843-1052`) | binary packet per observer | none |
| Item table `g_pItemList` | In-process data | Authoritative pricing/name | loaded by `BASE_ReadItemListFile` (`Basedef.cpp:4987`) | `STRUCT_ITEMLIST[]` | boot-time file failure returns `FALSE`; handler assumes loaded |
| Guild zone table `g_pGuildZone` | In-process data | City tax + owning guild | `ReadGuild` (`CReadFiles.cpp:772-825`) | `STRUCT_GUILDZONE[5]` | missing file logs and leaves defaults; `CityTax` clamped 0..20 on load |
| `Guild.txt` | Filesystem | Medal counter persistence | `CReadFiles::WriteGuild` (`CReadFiles.cpp:827`) | text (`fprintf`) | logs on open failure, no rollback |
| `pMobMerc` stock table | In-process data | Limited-stock merchants | loaded at boot (`Server.cpp:9341-9370`), renewed by timer (`ProcessSecMinTimer.cpp:1345`) | `STRUCT_MERC[128]` | none; stock not persisted |
| Impostor NPC exp | In-process state | Guild tax revenue | direct `MOB.Exp +=` | `long long`, capped `2e11` | none |
| DB server | **not integrated** | persistence | - | - | deferred to periodic/logout save |

---

## 9. Design Patterns & Architecture

| Pattern / decision | Implementation | Location | Purpose / assessment |
|--------------------|----------------|----------|----------------------|
| Single-file command handler | one free function per opcode | `_MSG_Buy.cpp:21` | uniform dispatcher contract; no class, no interface |
| Guard-clause gate ladder | sequential `if (...) { ...; return; }` | `:25-64`, `:70`, `:147`, `:159` | cheap early rejection; ordering encodes the rule precedence |
| Trust-boundary re-resolution | item read from `pMob[TargetID].MOB.Carry[...]` instead of packet | `:66-68` | avoids client-supplied item/price entirely (the component's best property) |
| Server-authoritative pricing pipeline | staged in-place mutation of `Price`/`Desconto` | `:104-145` | simple, but no value object and no arithmetic safety |
| Currency strategy branch | `Donate` vs coin, selected by `EF_DONATE != 0`, joined by `goto` | `:73-102`, `:101` → `:283` | two pricing models in one function; the join point is the root of the UB findings |
| Data-driven scripting via item effects | `EF_DONATE` = price; `Equip[0]` ∈ {259,230} = "self-destruct shop"; effect 56/57/85/59 = guild binding | `:73`, `:191-217`, `:308` | no hard-coded shop scripts; magic numbers instead |
| Table-driven stock | `pMobMerc[].Stock[slot]` with timer renew | `:240-255` | separate finite-stock mechanism from BR-22 |
| Identity-by-generator | merchant matched to `pMobMerc` by `GenerateIndex` | `:243` | survives mob respawn; breaks if two table rows share a generator |
| Tax revenue routing | exp credited to impostor NPC slots | `:258-273` | economy sink modelled as NPC experience |
| Audit-log pattern (ad-hoc) | free-text `ItemLog` with embedded item code | `:290-291`, `:98-99` | forensic trace, but string-formatting rather than structured records |
| Debug-in-production switch | global `FREEEXP == -2` enables chat trace | `:295` | configuration overload; observable to players |
| Fire-and-forget side effects | no error propagation from senders | `:274-309` | simple; a `CloseUser` mid-transaction is not detected |
| Non-transactional state group | coin debit, item copy, stock, tax, file write with no rollback | `:274-309` | ordering-dependent correctness |

Architectural boundary assessment: the component sits at the **network/gameplay boundary** (untrusted input) and the **economy/domain boundary** (authoritative money mutation). It crosses both without a separating domain layer - validation, pricing, inventory mutation, tax routing, persistence side effects and NPC lifecycle all live in one function. In the architecture report's taxonomy this is correct for a single-opcode handler in a decompiled 2000s-era server, but it is also why 26 rules can be enumerated from 313 lines and why 6 of them are defective.

---

## 10. Technical Debt & Risks

| Risk | Component area | Issue | Impact |
|------|----------------|-------|--------|
| High | Donate branch / `goto DonateBuy` | jump into the scope of `if (Price >= 0)` past the initialization of `Price`, `Price2`, `Village`, `CityTax`, `GuildTax`, `bPrice`, `Desconto` (`:101` → `:283`; reads at `:236`, `:290`, `:297`) | item delivery, audit price and tax routing become indeterminate for donate purchases; MSVC accepts the jump (warning-level), so it is silent in production |
| High | Zone lookup | `g_pGuildZone[Village].CityTax` read before the `Village >= 5` test (`:107` vs `:109`); `MAX_GUILDZONE = 5` | 4-byte out-of-bounds read on every purchase from a merchant outside a city |
| High | Guild tax | `GuildTax` never initialized when `LABEL_BUY1` is taken, yet used at `:263`/`:270` under a guard that re-tests `CityTax` from a *different* village variable | arbitrary exp credited to guild NPCs |
| High | Artisan discount | `Desconto` computed, `Price` unchanged, halved value discarded (`:135-145`) | the rule does nothing economically; logs and `SendSay` misreport it as applied |
| High | Input validation | `BASE_CheckPacket` fully commented out (`Basedef.cpp:6476-6594`), call sites `#ifdef _PACKET_DEBUG` | no packet-size validation; a short `MSG_Buy` frame causes the handler to read stale bytes inside `pRecvBuffer` as `TargetID`/`TargetInvenPos`/`MyInvenPos` |
| High | Test coverage | no tests, no test project, no assertion anywhere | every rule in section 3 is unverified; regressions are undetectable except in production |
| Medium | Response framing | `AddMessage((char*)m, m->Size)` uses the client's declared length (`:280`) | server response length differs from `sizeof(MSG_Buy)`; a client can receive a frame it did not negotiate |
| Medium | Shop list enforcement | `TargetInvenPos` accepted for all 64 slots while only 27 are displayed (0-8, 27-35, 54-62) | hidden/reserved stock enumerable and purchasable |
| Medium | Inventory domain | `MyInvenPos` allowed up to 63, while the rest of the server reserves 60-63 (`MAX_CARRY-4` convention) | purchased items can occupy slots 60/61 used by the item-3467 timed feature |
| Medium | Merchant inventory mutation | guild stamping writes into `pMob[TargetID].MOB.Carry[...]` and can rewrite `sIndex` to 532-534 (`:191-204`) | shop slot permanently re-personalised; 508 loses identity after the first sale; next buyer inherits the stamp |
| Medium | Arithmetic | `Price * CityTax` in `int` before any bound check; `7 * Price` guarded only by `Price <= 1999999999` (`:112-116`, `:132`) | signed overflow / UB for very expensive items; produces negative price then silent abort |
| Medium | Tax policy | `CityTax >= 30` rejected while the setter allows 30 (`:109` vs `_MSG_MessageChat.cpp:76`) | a 30% tax is advertised (`SendShopList`) but charged as 0% |
| Medium | Duplicate branches | identical `if (Price < 100000) ... else ...` bodies (`:112-116`); two dead `itemIndex == 0` checks (`:84-88`, `:153-157`) | residual decompilation artefacts; mislead readers about price tiers and validity guarantees |
| Medium | Persistence | no `SaveUser`/DB write; guild file written *before* coin debit (`:231` vs `:274`) | crash between the two persists a counter without the charge; economy state can be lost since last save |
| Medium | Lifecycle double-fire | BR-22 then BR-23 can both call `DeleteMob` on the same merchant | duplicate removal broadcast and double generator decrement (clamped) |
| Low | Trade teardown | `Trade.OpponentID` branch removes only the buyer (`:39-43`) | counterparty retains a stale trade reference |
| Low | Logging | error text names `_MSG_ReqBuy` inside `_MSG_Buy.cpp` (`:312`); donate audit is Portuguese free text (`:98`) and bypasses the message table; raw `g_pMessageStringTable[114]` used as a constant (`:183`, `:188`) | inconsistent/untranslatable telemetry, harder log correlation |
| Low | Global debug flag | `FREEEXP == -2` broadcasts price internals to nearby players (`:295`) | internal pricing exposed if the flag is left set |
| Low | Shared globals | `temp` is a single global buffer written by `sprintf` and consumed by `Log`/`ItemLog`; two `sprintf` calls use it in sequence | any interleaving or future re-entrancy corrupts log lines; also `char tmplog[2048]` is a 2 KB stack frame |

Code smells identified: magic numbers (`114`, `-2`, `56/57/59/85`, `259/230`, `787/481`, `1999999999`, `4096/2048`), `goto` used for both optimization (`LABEL_BUY1`) and join-point control flow (`DonateBuy`), duplicated blocks, dead code, unchecked return values for all senders, a 313-line function holding 26 rules, and no named constants for effect ids beyond `EF_DONATE`.

---

## 11. Test Coverage Analysis

| Component | Unit tests | Integration tests | Coverage | Test quality |
|-----------|------------|-------------------|----------|--------------|
| `Exec_MSG_Buy` | 0 | 0 | 0% (no executable test harness exists) | None - no assertions, no fixtures, no mocks, no golden data |
| `MSG_Buy` contract | 0 | 0 | 0% | The only contract check (`Basedef.cpp:6525`) is inside a commented-out function |
| Price pipeline (BR-10..BR-14) | 0 | 0 | 0% | No test values for tax tiers, discount interaction, or overflow boundaries |
| Guild item / medal rules (BR-15, BR-16) | 0 | 0 | 0% | No test for counter wrap (4096→2048), `ServerIndex` namespace, or `Guild.txt` rewrite |
| Stock / lifecycle rules (BR-17, BR-22, BR-23) | 0 | 0 | 0% | No test for stock refill timing, `PotionCount > POTIONCOUNT` boundary, or double `DeleteMob` |

### 11.1 Evidence that no tests exist

1. **No test directory.** The repository root contains only `Code`, `docs`, `openspec`, `.agents`, `.codegraph`, `.opencode`, `.git`, `.vs`. There is no `test`, `tests`, `spec`, `t/` or `__tests__` directory at any level under `Code`.
2. **No test project.** The solution `W2PP Code Project.sln` contains exactly two projects: `Code/DBSrv/DBSrv.vcxproj` and `Code/TMSrv/TMSrv.vcxproj`. Both are `<ConfigurationType>Application</ConfigurationType>`, `<PlatformToolset>v143</PlatformToolset>`, `<Platform>Win32</Platform>`. No test/benchmark/fixture project file exists.
3. **No test source files.** A recursive scan of all non-ignored files for names matching `test|spec|catch|gtest|doctest` returns no source file (the only hits are documentation under `docs/reports/` and skill files under the ignored `.agents` / `.opencode` folders).
4. **No assertions.** A search for `assert(` / `ASSERT(` across `Code/**` returns zero matches. There is no assert-based self-check, no debug-only invariant, and no `_DEBUG` verification block in the handler.
5. **No test framework dependency.** No `CMakeLists.txt`, no `*.yml`/`*.yaml` CI configuration, no package manifest, no third-party test library (Catch2/gtest/doctest) exists in the tree; the only build system is MSBuild.
6. **Static-index corroboration.** The code graph reports "no tests found within 3 caller hops" for `Exec_MSG_Buy` and for every collaborator examined (`ProcessClientMessage`, `SendClientMessage`, `SendItem`, `RemoveTrade`, `DeleteMob`, `BASE_GetVillage`, `ItemLog`, `WriteGuild`).

### 11.2 Consequence for this component

Every one of the 26 business rules in section 3 is verified **only by reading the code**. There is no executable evidence that:

- the coin path and donate path agree on delivery semantics (they do not - key finding #1);
- a 30% `CityTax` behaves as documented (it does not - key finding #11);
- the artisan discount reduces price (it does not - key finding #4);
- guild stamps do not leak between buyers (they do - key finding #9);
- stock is consumed exactly once per sale (it is, but donate sales consume none);
- tax revenue is conserved (it is not: payouts are 1.5x the collected surcharge).

No test file location can be reported for this component because none exists; the absence is itself the finding, and it applies project-wide (the `docs/reports/component-deep-analyzer/` reports for Attack, Quest, UseItem, Whisper, Dispatcher, Server Core and Client Contract all record the same absence).

---

## 12. Analysis Limitations

1. **Static analysis only.** No build was attempted, no binary was run, no packet was injected. Behavioural claims are derived from source control flow, not from execution traces.
2. **`sizeof(MSG_Buy)` is inferred, not measured.** It is documented as "24 bytes nominal" from the 12-byte `_MSG` header plus field layout under default MSVC alignment (`MSG_Buy` is declared outside every `#pragma pack` region in `Basedef.h`). Exact padding was not verified by compiling.
3. **Indeterminate-value claims are standard-level, not runtime-observed.** The `goto`-over-initialization findings (key findings #1 and #3) describe what the C++ standard calls indeterminate values; MSVC compiles such jumps with a warning, and the actual values depend on stack layout. The *possibility* is certain; the specific values are not predictable and were not observed.
4. **`MaxStock`/`Stock` semantics of `pMobMerc`** were read from the loader (`Server.cpp:9341-9370`) and the renew timer (`ProcessSecMinTimer.cpp:1341-1348`), not from the merchant configuration files themselves (no `MobMerc` data file is present in the repository tree).
5. **Message table contents were not read.** `g_pMessageStringTable` is populated from a language file at runtime; only the symbol names (`_NN_Only_Merchant_Sells`, `_DN_NeedDonate`, `_NN_Not_Enough_Money`, `_NN_NOSTOCK`) and the raw index `114` are referenced here, not the literal strings.
6. **`pMob` field layout for guild items assumed.** `MOB.GuildLevel`, `MOB.Guild`, `MOB.Class`, `LearnedSkill & 0x800`, `CurrentScore.Special[2]` were taken at face value as "guild level", "guild id", "class", "artisan skill bit", "intelligence special stat". The class-3/`0x800` meaning is inferred from context (BR-13) and is not documented in code comments.
7. **Cross-process effects excluded.** The DB server's save cadence (which determines how much of a purchase can be lost) was not analysed; only the fact that this handler does not trigger a save was established.
8. **No dynamic confirmation of merchant lifecycle.** `POTIONCOUNT` interaction with generator respawn was inferred from `DeleteMob` and the timer loop, not observed in a running server.

---

## 13. Summary of Referenced Files

| File | Role in this analysis |
|------|-----------------------|
| `Code/TMSrv/_MSG_Buy.cpp` | **analysed component** (313 lines, `Exec_MSG_Buy` at :21) |
| `Code/TMSrv/ProcessClientMessage.h` | declaration (:109), `GuildImpostoID` extern (:64) |
| `Code/TMSrv/ProcessClientMessage.cpp` | dispatch table (:38, case at :226-228), envelope checks (:42-65) |
| `Code/TMSrv/Server.h` | `pUser` (:64/229), `GuildImpostoID` (:60), `FREEEXP` (:196), `ServerIndex` (:202), `GuildCounter` (:205), `POTIONCOUNT` (:278), `pMobMerc` (:400) |
| `Code/TMSrv/Server.cpp` | `FREEEXP` (:43), `POTIONCOUNT` (:322), `pMobMerc` (:449), `AddCrackError` (:684), `Log` (:6701), `DeleteMob` (:7020), `RemoveTrade` (:7319), `ItemLog` (:9264), merc loader (:9341-9370) |
| `Code/TMSrv/SendFunc.cpp` | `SendClientMessage` (:27), `SendClientSignal` (:197), `GridMulticast` (:843), `SendItem` (:1055), `SendEtc` (:1195), `SendShopList` (:1369), `SendHpMode` (:1622), `SendSay` (:1647) |
| `Code/TMSrv/GetFunc.cpp` | `GetInView` (:640) |
| `Code/TMSrv/CReadFiles.cpp` | `ReadGuild` (:772), `WriteGuild` (:827), tax clamp (:811-812) |
| `Code/TMSrv/CUser.h` | `CUser`: `TradeMode` (:46), `Trade` (:59), `AutoTrade` (:65), `Donate` (:105) |
| `Code/TMSrv/CMob.h`, `CMob.cpp` | `PotionCount` (:74), constructor init (:35) |
| `Code/TMSrv/ProcessSecMinTimer.cpp` | `pMobMerc` stock renew (:1341-1348), item-3467 expiry (:571-595) |
| `Code/TMSrv/_MSG_REQShopList.cpp` | shop-list request handler (:20-55) - establishes the display window |
| `Code/TMSrv/_MSG_ReqBuy.cpp` | player autotrade purchase - contrast model (:20-199) |
| `Code/TMSrv/_MSG_Sell.cpp` | NPC sale counterpart, `MAX_CARRY-4` convention (:53), tax mirror (:182-192) |
| `Code/TMSrv/_MSG_MessageChat.cpp` | `guildtax` GM command, `CityTax` 0..30 (:62-101) |
| `Code/TMSrv/_MSG_UseItem.cpp` | reserved carry slots 60/61 semantics (:5666-5693) |
| `Code/Basedef.h` | constants (:56, :76, :77, :82, :86-87, :95-96, :107, :139, :148, :158), `STRUCT_ITEM` (:398), `STRUCT_MOB` (:438), `STRUCT_MERC` (:672), `STRUCT_GUILDZONE` (:714), `STRUCT_ITEMLIST` (:882), `_MSG` macro (:925), `MSG_Buy` (:1750), `MSG_ShopList` (:1650), `MSG_REQShopList` (:1779), `_MSG_CloseShop` (:1819) |
| `Code/Basedef.cpp` | `BASE_GetItemCode` (:1479), `BASE_GetItemAbility` (:1647), `BASE_GetSubGuild` (:4237), `BASE_GetVillage` (:4258), `BASE_CheckPacket` (commented, :6475-6596), `BASE_ReadItemListFile` (:4987) |
| `Code/ItemEffect.h` | `EF_DONATE` = 91 (:134) |
| `Code/CPSock.cpp`, `Code/CPSock.h` | `ReadMessage` (:353-...), `AddMessage` (:513-591), buffer sizes (:35-38) |
| `Code/DBSrv/CReadFiles.cpp` | `ImportItem` (:130) - item grant path, not persistence of this handler |
| `W2PP Code Project.sln`, `Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj` | build topology: 2 application projects, Win32, v143, **no test project** |
| `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` | architecture context (Buy Handler row at :138, Ca 1 / Ce 14) |
