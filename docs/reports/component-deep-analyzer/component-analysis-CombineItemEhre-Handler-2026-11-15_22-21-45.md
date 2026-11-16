# Component Deep Analysis Report - CombineItemEhre Handler

**Component:** `CombineItemEhre Handler`
**Primary location:** `Code/TMSrv/_MSG_CombineItemEhre.cpp` (395 lines, 12,119 bytes)
**Secondary locations in scope:** `Code/TMSrv/GetFunc.cpp:106-165` (`GetMatchCombineEhre`), `Code/Basedef.cpp:75-78` (`g_pEhreRate`), `Code/TMSrv/CReadFiles.cpp:375-435` (`ReadCompRate` EHRE block)
**Project:** W2PP - decompiled Win32/MSVC MMORPG server emulator (W2)
**Service:** TMSrv (game/world server)
**Analysis date:** 2026-11-15 22:21:45
**Method:** static source analysis only. No code was executed, modified, or built.

---

## 1. Executive Summary

### 1.1 Purpose

The CombineItemEhre Handler implements the "Ehre" crafting/refining station of the game world: a single client-originated TCP message (`_MSG_CombineItemEhre`, wire Type `723` / `0x02D3`) is turned into one of eight distinct item recipes. The handler is the whole vertical slice for that NPC: it re-validates the submitted item set against the authoritative inventory, resolves a recipe identifier from a hardcoded recipe table, applies per-recipe level/coin/stat gates, destroys the input materials, rolls a success chance against a configurable rate table, and finally writes the crafted result (or the failure state) back into the player's inventory and to the client's UI.

The component is a *refine/combination terminal*, not a generic crafting engine: recipe knowledge is encoded as a chain of `if/else if` comparisons over raw `sIndex` values and refine levels, and the effect application is a second, equally hardcoded chain of mutations keyed by the recipe identifier.

### 1.2 Role in the system

- **Architectural layer:** presentation-to-domain adapter at the message boundary of TMSrv. It is one `case` label in the client message dispatcher (`Code/TMSrv/ProcessClientMessage.cpp:246-248`).
- **Caller:** exactly one, `ProcessClientMessage`, which is invoked from the WSA read path in `Code/TMSrv/Server.cpp:4001`.
- **Callees:** the shared inventory/message/score primitives (`SendItem`, `SendClientMessage`, `SendClientSignalParm`, `SendEtc`, `SendScore`, `SendEmotion`), the item-attribute primitives (`BASE_GetItemAbilityNosanc`, `BASE_GetItemSanc`, `BASE_SetItemSanc`, `BASE_GetItemCode`), the progression primitives (`CheckGetLevel`, `SetCircletSubGod`, `BASE_GetBonusScorePoint`, `GetCurrentScore`), and two pieces of private domain knowledge (`GetMatchCombineEhre`, `g_pEhreRate`).
- **Sibling components:** eleven near-identical `_MSG_CombineItem*.cpp` handlers (Tiny, Shany, Ailyn, Agatha, Odin/Odin2, Lindy, Alquimia, Extracao, generic). Ehre is one member of a copy-paste family, sharing the same 20-line validation preamble and the same `rand()%115` roll idiom.

### 1.3 Key findings

| # | Finding | Severity |
|---|---------|----------|
| F-01 | **Recipe 8 (Soul) reads `Carry[-1]`.** `ipos` is only assigned for recipes 1-7 (`:117-121`); the Soul branch at `:356` passes `&pMob[conn].MOB.Carry[ipos]` with `ipos == -1`, an out-of-bounds read of the mob structure. | High |
| F-02 | **Recipe 5 level recompute can reset a character to level 0.** `newlevel` is initialized to `0` (`:152`); if the post-cost EXP falls outside every band of `g_pNextLevel_2` (i.e. EXP > 3.98e9 at max level), no loop iteration matches and `BaseScore.Level` is written as 0 (`:160`). The same idiom in `_MSG_UseItem.cpp:3464` correctly seeds `newlevel` with the current level. | Critical |
| F-03 | **Effective success probability never equals the configured rate.** `rand() % 115` with `if (_rand >= 100) _rand -= 15` (`:109-111`) folds the 15 out-of-range values back into 85-99, doubling their weight. A configured rate of 40 yields 41/115 = 35.65%; a rate of 10 yields 11/115 = 9.57%. | Medium |
| F-04 | **Soul combinations are order-sensitive.** `{2441,2442,2443}` yields `SOUL_ID` while `{2442,2441,2443}` yields `SOUL_I` (`:332-343`). Identical inventories produce different character state depending on the slot ordering sent by the client. | Medium |
| F-05 | **Unmapped Soul permutations are silently consumed.** Only 10 of the 20 ordered triples (4^3 = 64 raw combinations, 20 valid multisets) are mapped; any unmapped triple destroys three stones, changes nothing, and still reports success (`:350-361`). | High |
| F-06 | **Slots 3-7 are consumed but never validated.** `GetMatchCombineEhre` inspects only `Item[0..2]`, yet the clearing loop (`:84-91`) destroys every non-empty slot up to index 7. A client that attaches extra items loses them. | Medium |
| F-07 | **`LOCALSERVER` disables all crafting randomness.** `|| LOCALSERVER` (`:176`) makes every roll succeed; the flag is set from a hardcoded DB address string `"192.168.0.50"` (`Server.cpp:3587-3588`). | High |
| F-08 | **Global `temp` buffer is appended to without reset.** `strcat(temp, itemlog)` (`:103`) writes into the shared 4096-byte global declared at `Server.cpp:277`, which is reused by unrelated handlers. | Medium |
| F-09 | **No packet-size validation for this message type.** `CPSock::ReadMessage` enforces only `Size >= sizeof(HEADER)` (12 bytes) and `<= 8192` (`CPSock.cpp:397-411`); the handler unconditionally reads 84 bytes. | Medium |
| F-10 | **`extra.Soul` is write-only in TMSrv.** No code path in the server reads it for stat, damage, or resistance computation; it is only written here, by the admin `soul` command (`imple.cpp:1628-1633`) and by celestial save/restore (`_MSG_UseItem.cpp:2959,3111,3125`). | Informational |
| F-11 | **Zero automated test coverage.** No test project, no test framework, no test files anywhere in the repository. | High (process) |
| F-12 | **Failure disclosures the internal roll.** `sprintf(temp, "%s(%d/%d)", _NN_CombineFailed, _rand, rate)` (`:387`) sends the raw dice value and the rate to the client. | Low |

---

## 2. Data Flow Analysis

### 2.1 Macro flow (network packet to world state)

```
1.  Client writes Type=723 (_MSG_CombineItemEhre = 211|FLAG_CLIENT2GAME)              Basedef.h:1952
2.  WSA_READ message pump -> CPSock::ReadMessage                                       Server.cpp:3975 / CPSock.cpp:353
      - INITCODE check (0x1F11F311), Size in [12, 8192], decrypt-in-place, CheckSum
3.  Checksum/format error (Error==1 or 2) -> log + disconnect, handler NOT called       Server.cpp:3990-3999
4.  ProcessClientMessage(conn, pMsg, FALSE)                                            ProcessClientMessage.cpp:38
      - std->ID in [0, MAX_USER) else log "err,packet"
      - ServerDown >= 120 -> drop
      - pUser[conn].LastReceiveTime = SecCounter
      - Type == _MSG_Ping -> drop; ClientTick == SKIPCHECKTICK -> drop
5.  case _MSG_CombineItemEhre -> Exec_MSG_CombineItemEhre(conn, pMsg)                  ProcessClientMessage.cpp:246
6.  Cast pMsg to MSG_CombineItem* (84 bytes: 12 header + 8x8 items + 8 slots)           _MSG_CombineItemEhre.cpp:22
7.  PHASE A - ownership validation loop, i = 0..7                                       :24-43
      a. skip when Item[i].sIndex == 0
      b. invPos = InvenPos[i]; reject if <0 or >= pMob[conn].MaxCarry -> RemoveTrade, silent return
      c. memcmp(Carry[invPos], Item[i], 8) != 0 -> ItemLog "err,msg_CombineEhre - item remove or changed."
         + _MSG_CombineComplete(0) + return
8.  PHASE B - recipe resolution: combine = GetMatchCombineEhre(m->Item)                 GetFunc.cpp:106
      - returns 1..8 (recipe id, misleadingly named "rate") or 0
9.  PHASE C - gate chain
      a. combine == 0                                    -> _NN_Wrong_Combination, signal 0
      b. combine == 5: Exp >= 5,000,000 && ClassMaster not MORTAL/ARCH && Level >= 39
      c. combine == 6|7: Coin >= 1,000,000
      d. combine == 3|4: item[2] static HP/MP/crit bonus under ceilings
10. PHASE D - destruction: for each non-empty slot -> memset Carry[slot] + SendItem(clear)  :84-91
11. PHASE E - audit: ItemLog banner, 8x BASE_GetItemCode into global temp, ItemLog(temp), footer  :93-107
12. PHASE F - RNG: _rand = rand()%115; if (_rand >= 100) _rand -= 15                    :109-111
13. PHASE G - rate lookup: rate = g_pEhreRate[combine]                                  Basedef.cpp:75
14. PHASE H - output slot selection: ipos = InvenPos[2] for 1..5; InvenPos[0] for 6|7   :115-121
15. PHASE I - cost deduction (pre-roll)
      a. combine 6|7 and Coin >= 1,000,000: Coin -= 1,000,000; SendEtc                 :124-128
      b. combine 5 and Exp >= 5,000,000: Exp -= 5,000,000; rate = f(level band);
         BaseScore.Level = recomputed band; CheckGetLevel; possible SetCircletSubGod;
         BASE_GetBonusScorePoint; GetCurrentScore; SendEtc; SendScore; SendEmotion(14,3)  :130-174
16. PHASE J - resolution: if (_rand <= rate || LOCALSERVER) -> SUCCESS else FAILURE      :176
17. PHASE K - SUCCESS mutation per recipe 1..8                                           :178-362
18. PHASE L - SUCCESS notification: _NN_Processing_Complete, signal 1, ItemLog, SendItem  :364-377
19. PHASE M - FAILURE: recipe 5 restores Item[2]; _NN_CombineFailed(_rand/rate); signal 2  :379-393
20. Return to dispatch loop
```

### 2.2 Data mutation map

| Phase | Mutated state | Mutating call | Reversible on failure |
|-------|---------------|---------------|-----------------------|
| A | `pUser[conn].Trade`, `pUser[conn].AutoTrade` (only on invalid slot) | `RemoveTrade` (`Server.cpp:7319`) | No (session reset) |
| D | `pMob[conn].MOB.Carry[pos]` = zeroed for every non-empty submitted slot | inline `memset` + `SendItem` | **No** - materials are gone before the roll |
| I.a | `pMob[conn].MOB.Coin` -= 1,000,000 (recipes 6,7) | inline | **No** - cost charged before the roll |
| I.b | `pMob[conn].MOB.Exp` -= 5,000,000 (recipe 5) | inline | **No** |
| I.b | `pMob[conn].MOB.BaseScore.Level` = recomputed band | inline | **No** - this is the de-level mechanic |
| I.b | `MOB.BaseScore.MaxHp/MaxMp`, `SkillBonus`, `SpecialBonus`, `Ac` (only if level-up) | `CheckGetLevel` (`CMob.cpp:1034`) | No |
| I.b | Circlet stat bonuses on `Equip[1]` at levels 120/150/180 | `SetCircletSubGod` (`Server.cpp:8855`) | No |
| I.b | `BaseScore` STR/INT/DEX/CON clamp | `BASE_GetBonusScorePoint` (`Basedef.cpp:858`) | No |
| I.b | `MOB.CurrentScore` full recompute | `GetCurrentScore` (`CMob.h:113`) | Derived |
| K | `Carry[ipos]` = crafted item | inline writes | Recipe 5 only (restores `Item[2]`) |
| K (8) | `pMob[conn].extra.Soul` | inline | **No, and no item is produced** |

### 2.3 Two-phase commit shape (and its absence)

The handler is deliberately ordered "validate everything, then destroy, then roll". Phase A/P has no rollback point: once phase D zeroes the input slots there is no copy of the consumed items other than the client's own message (`m->Item[]`), which is the reason recipe 5 can restore `Item[2]` on failure (`:383`) and every other recipe cannot. No transaction, journal, or snapshot mechanism exists; a crash between phase D and phase K loses the materials permanently.

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule | Rule Type | Rule Description | Location |
|------|-----------|------------------|----------|
| BR-01 | Validation | Empty slots (`sIndex == 0`) in the 8-slot submission are skipped, not rejected | `_MSG_CombineItemEhre.cpp:26-27` |
| BR-02 | Validation | `InvenPos` must satisfy `0 <= pos < pMob[conn].MaxCarry`; violation resets the trade session and returns with no reply | `:29-35` |
| BR-03 | Anti-desync | Each submitted item must byte-match the server copy in `MOB.Carry[pos]` (full `STRUCT_ITEM`, 8 bytes) | `:37-42` |
| BR-04 | Business Logic | A recipe id must resolve; id 0 means "wrong combination" | `:45-52` |
| BR-05 | Constraint | Item `747` anywhere in the 8 descriptors disqualifies the whole submission | `GetFunc.cpp:110-114` |
| BR-06 | Validation | Every one of the 8 descriptors must be in `[0, MAX_ITEMLIST)` | `GetFunc.cpp:116-138` |
| BR-07 | Recipe | Recipe 1 - Pacote Oriharucon: `697 + 697 + (sanc>=9, !=3338)` -> item `412` x10 | `GetFunc.cpp:140-141`, `:178-183` |
| BR-08 | Recipe | Recipe 2 - Pedra Misteriosa: two items in `[5110,5133]` + item `413` amount>=10 -> item `4148` x10 | `GetFunc.cpp:143-144`, `:185-190` |
| BR-09 | Recipe | Recipe 3 - Pedra Espiritual: two items in `[661,663]` + item `633` sanc>=9 -> base item with refine forced to +7 and jewel stat injected | `GetFunc.cpp:146-147`, `:192-291` |
| BR-10 | Recipe | Recipe 4 - Pedra Amunra: two items in `[661,663]` + item `3464` sanc>=9 -> same as BR-09 | `GetFunc.cpp:149-150`, `:192-291` |
| BR-11 | Recipe | Recipe 5 - Refinacao Abencoada Purificada: `697 + 697 + item 3338 sanc<=8` -> same item with refine +1 | `GetFunc.cpp:152-153`, `:294-302` |
| BR-12 | Recipe | Recipe 6 - Traje Montaria: mount `[2360,2389]` + item `[4190,4199]` -> mount with `stEffect[2].cValue = 11 + (id-4190)` | `GetFunc.cpp:155-156`, `:304-309` |
| BR-13 | Recipe | Recipe 7 - Restaurar Traje Montaria: mount `[2360,2389]` + item `4899` -> mount with `stEffect[2].cValue = 0` | `GetFunc.cpp:158-159`, `:311-316` |
| BR-14 | Recipe | Recipe 8 - Soul: three items in `[2441,2444]` -> `extra.Soul` assignment from a 10-entry permutation table | `GetFunc.cpp:161-162`, `:318-362` |
| BR-15 | Gate | Recipe 5 requires `Exp >= 5,000,000`, `Level >= 39`, and `ClassMaster ∉ {MORTAL, ARCH}` | `:54-59` |
| BR-16 | Gate | Recipes 6 and 7 require `Coin >= 1,000,000` | `:61-66` |
| BR-17 | Gate | Recipes 3 and 4 reject base items whose static HP/MP/crit bonus exceeds the ceilings | `:69-81` |
| BR-18 | Consumption | All submitted non-empty slots are zeroed and pushed to the client before resolution | `:84-91` |
| BR-19 | Audit | Three audit records per attempt: banner, item-code list, footer/success/failure marker | `:93-107`, `:359`, `:374`, `:391` |
| BR-20 | Probability | Single roll `rand()%115` folded at `>=100` by `-15`; success iff `_rand <= rate` | `:109-111`, `:176` |
| BR-21 | Configuration | Rate table `g_pEhreRate[10]` is a code default overridden at boot/reload from `CompRate.txt` | `Basedef.cpp:75-78`, `CReadFiles.cpp:407-435` |
| BR-22 | Progression | Recipe 5 costs 5,000,000 EXP and re-derives `BaseScore.Level` from the remaining EXP; the character can de-level | `:130-174` |
| BR-23 | Probability | Recipe 5 ignores `g_pEhreRate[5]` and uses a level band: <150:30, <160:35, <170:40, <180:50, <190:70, <=199:100 | `:134-150` |
| BR-24 | Guarantee | Recipes 1, 2, 6, 7, 8 ship with rate 100 = unconditional success | `Basedef.cpp:77` |
| BR-25 | Output | Success produces exactly one item in a single output slot (`InvenPos[2]` for 1-5, `InvenPos[0]` for 6-7, none for 8) | `:115-121` |
| BR-26 | Failure | Failure consumes materials and cost; only recipe 5 refunds its base item; the roll is disclosed as `fail(rand/rate)` | `:379-393` |
| BR-27 | Protocol | `_MSG_CombineComplete` parameter: 0 = rejected/aborted, 1 = success, 2 = roll failure | `:40,50,57,64,78,352,368,390` |
| BR-28 | Determinism | Recipe 8 is order-dependent: identical multisets in different `Item[]` positions yield different souls | `:320-348` |
| BR-29 | Gap | Unmapped Soul triples still destroy the stones and report success (no soul change) | `:350-361` |
| BR-30 | Refine | Written refine is clamped: `<0 -> 0`, `>15 -> 0` (a +16 request resets the item to +0) | `Basedef.cpp:2241-2248` |
| BR-31 | Test hook | `LOCALSERVER` short-circuits the roll to always succeed | `:176` |
| BR-32 | Item routing | Recipe 6 and 7 output is written back to `InvenPos[0]`, i.e. the mount's own slot | `:306,313` |
| BR-33 | Refine semantics | Recipes 3/4 set refine to exactly 7 (absolute), not `+1` | `:291` |
| BR-34 | Soul storage | Result is stored on the character (`extra.Soul`), not as an inventory item; nothing is sent back | `:321-361` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 / BR-03 - Submission Presence and Ownership Integrity

**Overview:**
The eight-slot `MSG_CombineItem` payload is a client-authored description of inventory items. The handler treats it as untrusted and re-derives ownership from the server-side `pMob[conn].MOB.Carry` array before looking at the recipe table.

**Detailed description:**
The handler walks all eight descriptor slots in order. A descriptor with `sIndex == 0` is treated as "not part of this submission" and is skipped; it is never used to index the inventory, so a client may leave any subset of slots empty and no positional contract is broken at this stage. For non-empty descriptors the slot byte `m->InvenPos[i]` is promoted to `int` and range-checked against the live inventory width `pMob[conn].MaxCarry`, which the server maintains as 30, 45 or 60 depending on the character's inventory-expansion state (`ProcessDBMessage.cpp:751-757`, `ProcessSecMinTimer.cpp:576-596`). A violation does not produce a game-visible error at all: the handler calls `RemoveTrade(conn)` — which clears the pending trade and auto-trade arrays and re-broadcasts the character — and then returns silently, leaving the client waiting for a `_MSG_CombineComplete` that never arrives. This asymmetry (hard rejection with no client signal) is unique to this validation step; every later rejection at least sends signal 0.

The ownership check itself is a full-structure `memcmp` of the 8-byte `STRUCT_ITEM` (`short sIndex` plus three `union { short sValue; unsigned char cEffect, cValue; }` slots, `Basedef.h:398-412`). Comparing all three effect pairs — not just the item id — means a client cannot re-roll a jewel or refine value in transit: any divergence in effect code or value fails the comparison, is logged as `err,msg_CombineEhre - item remove or changed.` and answered with `_MSG_CombineComplete(0)`. This is the component's primary anti-duplication and anti-desync control, and it is the reason the handler can later re-read `m->Item[2]` as an authoritative copy of what was in the slot (used by recipe 5's refund and by every success branch's `memcpy`).

Two residual weaknesses are visible. First, the check only covers slots that carry a descriptor; the handler never verifies that descriptors cover the items the recipe *actually* needs — that is deferred entirely to `GetMatchCombineEhre`. Second, because `InvenPos` is declared `char` (`Basedef.h:1962`) and char is signed under MSVC, values 128-255 arrive as negative and are correctly rejected by `invPos < 0`, but the same byte is later reused unvalidated in the destruction loop; only the earlier gate makes that safe.

**Rule workflow:**
```
for i in 0..7:
    if Item[i].sIndex == 0: continue
    invPos = InvenPos[i]
    if invPos < 0 or invPos >= pMob[conn].MaxCarry:
        RemoveTrade(conn); return            # no client signal
    if memcmp(&Carry[invPos], &Item[i], 8) != 0:
        ItemLog("err,msg_CombineEhre - item remove or changed.", account, ip)
        _MSG_CombineComplete(0); return
```

---

### Business Rule: BR-04 / BR-06 - Recipe Resolution Contract

**Overview:**
`GetMatchCombineEhre` is the component's kitchen: a pure function that maps the submitted descriptors to an integer recipe id 1-8, or 0 for "no recipe". Despite the local variable being named `rate`, the return value is an identifier, not a percentage.

**Detailed description:**
Resolution is a flat priority chain of eight `if / else if` branches evaluated in source order, so the first match wins. Before any branch is evaluated the function applies two global preconditions. Item `747` anywhere in the eight descriptors aborts resolution (BR-05) — an exclusion for a special item that must never be consumed by this NPC. Then each of the eight descriptors is bounds-checked against `[0, MAX_ITEMLIST)` where `MAX_ITEMLIST = 6500` (`Basedef.h:139`); note the comparison uses `< 0` on a `short`, which is unreachable, so the effective test is only the upper bound. Slots left at 0 are *not* rejected here, which is what allows recipes with three ingredients to coexist with an eight-slot payload.

The eight branches read only `Item[0]`, `Item[1]` and `Item[2]`, with fixed positional roles: for the packet-table recipes (1, 2, 5) `Item[2]` is the "base" object that is upgraded, for the jewel recipes (3, 4) `Item[0]`/`Item[1]` are consumed jewels and `Item[2]` the base equipment, for the mount recipes (6, 7) `Item[0]` is the mount and `Item[1]` the voucher, and for Soul (8) all three are equivalent. Slots 3-7 are never read for matching yet are destroyed in phase D, so any item placed there is a pure loss (F-06). Because the chain is strict "exactly these ids", there is no wildcard, no grade check, no `nPos`/`nUnique` check and no class check in the resolver — all semantic eligibility for recipes 1, 2, 6, 7, 8 is decided purely by item id ranges.

The function's use of helper primitives is worth noting for correctness review: `BASE_GetItemSanc` (BR-09/10/11 prerequisites) decodes the refine encoding, and `BASE_GetItemAmount` (BR-08) reads the stack count. `BASE_GetItemSanc` has a special case returning `FALSE` for ids 2330-2389 (the mount block), so refine-based mounts can never satisfy a sanc gate; and it normalises the packed 230-253 encoding back to `REF_10..REF_15`. `BASE_GetItemAmount` semantics matter for recipe 2: it must return at least 10 for item 413, and the destroy phase removes the whole stack regardless of the amount.

**Rule workflow:**
```
GetMatchCombineEhre(Item[8]):
    for i in 0..7: if Item[i].sIndex == 747: return 0
    for i in 0..7: if Item[i].sIndex >= MAX_ITEMLIST: return 0
    if I0==697 and I1==697 and sanc(I2)>=9 and I2.sIndex!=3338      -> 1
    elif I0 in 5110..5133 and I1 in 5110..5133 and I2==413 and amount(I2)>=10 -> 2
    elif I0,I1 in 661..663 and I2==633  and sanc(I2)>=9             -> 3
    elif I0,I1 in 661..663 and I2==3464 and sanc(I2)>=9             -> 4
    elif I0==697 and I1==697 and I2==3338 and sanc(I2)<=8           -> 5
    elif I0 in 2360..2389 and I1 in 4190..4199                      -> 6
    elif I0 in 2360..2389 and I1==4899                              -> 7
    elif I0,I1,I2 all in 2441..2444                                 -> 8
    return 0
```

---

### Business Rule: BR-15 - Blessed Refine Eligibility Gate

**Overview:**
Recipe 5 ("Refinacao Abencoada Purificada") is the only late-game gate in the component. It is available exclusively to characters who have progressed past the mortal/arch progression tracks, are at least level 39, and hold at least 5,000,000 accumulated experience.

**Detailed description:**
The gate is a single compound expression: `combine == 5 && (Exp < 5000000 || ClassMaster == MORTAL || ClassMaster == ARCH || CurrentScore.Level < 39)`. The class check is expressed as an exclusion of `MORTAL` (2) and `ARCH` (1), which in practice admits `CELESTIAL` (3), `CELESTIALCS` (4) and `SCELESTIAL` (5) — the three "celestial" progression tracks whose level table is `g_pNextLevel_2` rather than `g_pNextLevel`. That coupling is not incidental: the handler's later level recompute (`:152-160`) scans `g_pNextLevel_2`, so the gate is what guarantees the correct EXP table is used. A MORTAL character would be measured against a table that does not describe its progression.

Level 39 is not arbitrary either — it is the first celestial tier boundary (`CReadFiles::QuestLevel` starts at 39, `CReadFiles.cpp:57-64`), and it matches the celestial quest gate at `CMob.cpp:1072` (`cur == 39 && QuestInfo.Celestial.Lv40 == 0`). The EXP threshold is exactly the recipe's cost, so the gate doubles as an affordability check: a character that passes the gate can always pay. Note that the gate reads `CurrentScore.Level`, i.e. the derived combat level after buffs and equipment, while the affordability and recompute use `BaseScore.Level`/`MOB.Exp` — a mismatch only observable if a buff modifies `CurrentScore.Level`, which the server-side derivation does not do.

Rejection is soft: `_NN_Wrong_Combination` (message-table index 271) plus `_MSG_CombineComplete(0)`. The client therefore cannot distinguish "your items do not form a recipe" from "you are too low level", and the diagnostic burden falls on player support rather than the protocol.

**Rule workflow:**
```
if combine == 5:
    if Exp < 5_000_000: reject
    if ClassMaster == MORTAL or ClassMaster == ARCH: reject
    if CurrentScore.Level < 39: reject
```

---

### Business Rule: BR-16 - Mount Costume Price Gate

**Overview:**
Recipes 6 and 7 (applying and removing a mount costume) are the only coin-priced recipes: 1,000,000 coins, charged before the roll and not refunded on failure.

**Detailed description:**
The pre-check at `:61-66` rejects the attempt when `MOB.Coin < 1000000`, again with the generic "wrong combination" message. The actual deduction at `:124-128` repeats the coin test and subtracts 1,000,000, then pushes the updated currency with `SendEtc` (`_MSG_UpdateEtc` carries `Coin`, `Exp`, `SpecialBonus`, `SkillBonus`, `ScoreBonus`, `Learn`). Because both branches ship with `g_pEhreRate[6] = g_pEhreRate[7] = 100` (BR-24), the failure path is currently unreachable for these two recipes and the "cost charged before roll" design has no practical downside — but the code path exists and would silently eat 1,000,000 coins and the voucher if an operator lowered the rate below 100 in `CompRate.txt`. Both the gate and the deduction are pre-roll, so the ordering is honest about intent: the coin is the price of the *attempt*, not of the result.

Coin is a plain `int` (`Basedef.h:448`) with no overflow guard, but since the check is `>= 1000000` before subtraction, the value can never wrap here. The deduction is not logged individually; only the generic success/failure audit line at the end records the attempt, so a coin loss on a failed roll would not be attributable from the item log alone.

**Rule workflow:**
```
if combine in {6,7}:
    if MOB.Coin < 1_000_000: _NN_Wrong_Combination; signal 0; return
...
if MOB.Coin >= 1_000_000 and combine in {6,7}:
    MOB.Coin -= 1_000_000
    SendEtc(conn)
```

---

### Business Rule: BR-17 - Jewel Injection Ceilings (Recipes 3 and 4)

**Overview:**
Recipes 3 and 4 consume two jewels (items 661-663, i.e. MP, HP and critical jewels) and merge their bonus into a base item. To stop stacking the merge into an unbounded stat, the handler refuses base items whose static bonus already reaches the ceiling.

**Detailed description:**
The gate reads three *static* item-list attributes of `Item[2]` through `BASE_GetItemAbilityNosanc` — `EF_HPADD2` (69), `EF_MPADD2` (70) and `EF_CRITICAL2` (71), the "tier 2" encodings of HP-add, MP-add and critical. "Nosanc" means the value is summed from `g_pItemList[idx].stEffect[]` only and never from the instance's own effect slots, so the gate measures the item's manufacturing template, not its current state. The rejection expression is `hp >= 20 || mp >= 20 || critico >= 100 || (hp >= 10 && mp >= 10) || (hp >= 10 && critico >= 50) || (hp >= 10 && mp >= 10) || (mp >= 10 && critico >= 50)`.

The expression is visibly a product of copy-paste: the clause `hp >= 10 && mp >= 10` appears twice, so the intended fourth and fifth combinations are duplicated and some intended pair is missing. What is certain is the effective policy — an item may not already carry 20+ of either HP or MP add, may not already carry +100 critical, and may not carry a 10/10 HP+MP pair, a 10 HP / 50 crit pair, or a 10 MP / 50 crit pair. Because these are "already upgraded" markers, the rule reads as "one jewel merge per item per axis": the ceilings are exactly reachable only after repeated merges, and further merges are refused rather than clamped.

The merge itself (in the success branch) writes to `stEffect[1]` or `stEffect[2]` of the output item, with an explicit overflow rule: MP (`EF_MPADD2`) `+= 2` per jewel and wraps to 20 when it reaches 100; HP (`EF_HPADD2`) `+= 2` capped at 20; critical (`EF_CRITICAL2`) `+= 10` capped at 100. The wrap for MP (100 -> 20) rather than a cap is an outlier and is worth flagging as a probable defect: a jewel that pushes MP-add from 98 to 100 resets the bonus to 20 rather than leaving it at 100. Since the gate forbids base items with `mp >= 20`, the only way to reach 98 is repeated successful merges within a single item's lifetime, so the wrap is reachable.

**Rule workflow:**
```
if combine in {3,4}:
    hp      = BASE_GetItemAbilityNosanc(Item[2], EF_HPADD2)      # 69
    mp      = BASE_GetItemAbilityNosanc(Item[2], EF_MPADD2)      # 70
    critico = BASE_GetItemAbilityNosanc(Item[2], EF_CRITICAL2)   # 71
    if hp>=20 or mp>=20 or critico>=100
       or (hp>=10 and mp>=10)
       or (hp>=10 and critico>=50)
       or (hp>=10 and mp>=10)          # duplicate clause
       or (mp>=10 and critico>=50):
        _NN_Wrong_Combination; signal 0; return
```

---

### Business Rule: BR-20 / BR-21 - Probability Model and Rate Table

**Overview:**
Every recipe resolves through one shared roll. The nominal percentage comes from `g_pEhreRate[combine]`, but the dice construction makes the realised probability lower than the configured value for every rate below 100.

**Detailed description:**
The roll is `int _rand = rand() % 115;` followed by `if (_rand >= 100) _rand -= 15;`. The first statement produces a uniform 0-114. The second folds the 15 values 100-114 down onto 85-99, so the final distribution has weight 1 for values 0-84 and weight 2 for values 85-99, summing to 115. Success is `_rand <= rate` (or `LOCALSERVER`). Because the folded band 85-99 is exactly the "near-miss" region for the interesting rates, the clamp systematically inflates the failure mass rather than merely preventing an out-of-range roll. The realised success probabilities are:

| rate | formula | realised | nominal |
|------|---------|----------|---------|
| 1 | 2/115 | 1.74% | 1% |
| 10 | 11/115 | 9.57% | 10% |
| 30 | 31/115 | 26.96% | 30% |
| 35 | 36/115 | 31.30% | 35% |
| 40 | 41/115 | 35.65% | 40% |
| 50 | 51/115 | 44.35% | 50% |
| 70 | 71/115 | 61.74% | 70% |
| 85 | 87/115 | 75.65% | 85% |
| 100 | 115/115 | 100% | 100% |

The rate source is `int g_pEhreRate[10] = {0, 100, 100, 40, 10, 10, 100, 100, 100, 100}` (`Basedef.cpp:75-78`): index 0 is a placeholder (never used because recipe 0 is rejected earlier), index 5 holds a dead default (`10`) that is always overwritten by the level-band rule BR-23 when recipe 5 is actually executed, and index 9 is dead. Operators override indices 1, 2, 3, 4, 6, 7, 8 from `../../Common/Settings/CompRate.txt` (`CReadFiles.cpp:37`) through `ReadCompRate`, which parses `EHRE <NAME> <value>`, uppercases only the first token, and rejects `value < 0 || value > 100` with a modal message box ("Indice invalido. (1)") while continuing to the next line (`CReadFiles.cpp:399-435`). The mapping is `PACOTE_ORI->1, MISTERIOSA->2, ESPIRITUAL->3, AMUNRA->4, TRAJE_MONTARIA->6, RETIRAR_TRAJE_MONTARIA->7, SOUL->8`; there is deliberately no key for index 5 because that rate is level-derived. Reload is possible at runtime through the administrative `reloadfile` command (`imple.cpp:250-262`).

Because the array is indexed by the recipe id returned from `GetMatchCombineEhre` with no bounds enforcement at the call site, the safety of `g_pEhreRate[combine]` depends entirely on the resolver returning 0..8. It does, by construction.

**Rule workflow:**
```
_rand = rand() % 115                  # 0..114
if _rand >= 100: _rand -= 15          # 100..114 -> 85..99  (doubles weight of 85..99)
rate = g_pEhreRate[combine]           # 0..100, may be overridden from CompRate.txt
success = (_rand <= rate) or LOCALSERVER
```

---

### Business Rule: BR-22 / BR-23 - Experience Cost, De-level and Dynamic Difficulty

**Overview:**
Recipe 5 is paid in experience, not items: it removes exactly 5,000,000 EXP, re-derives the character's level from the remaining EXP (which can demote the character), and then sets the success chance from the resulting level band.

**Detailed description:**
The cost block at `:130-174` is the most consequential part of the component. It first re-checks the affordability (`Exp >= 5000000`) and subtracts 5,000,000. It then overwrites the rate obtained from `g_pEhreRate[5]` with a level band computed from `CurrentScore.Level`: `<150 -> 30`, `<160 -> 35`, `<170 -> 40`, `<180 -> 50`, `<190 -> 70`, `<= MAX_CLEVEL (199) -> 100`. Because the chain is `else if` and the last clause is `<= MAX_CLEVEL`, a level above 199 falls through and leaves the rate at the code default of 10 — relevant only for impossible state, since celestial progression caps at 199.

The level recompute is the mechanism that makes the recipe a gamble: `int newlevel = 0;` then a scan over `i = 0..MAX_CLEVEL-1` looking for the band where `g_pNextLevel_2[i] <= Exp <= g_pNextLevel_2[i+1]`, and finally `BaseScore.Level = newlevel`. `g_pNextLevel_2` is linear at 20,000,000 EXP per level (`Basedef.cpp:656-679`: 0, 20e6, 40e6, ...), so subtracting 5,000,000 from a character whose EXP sits just above a level boundary drops it a full level. The scan is also the component's most dangerous construct: the sentinel is `0`, so if no band matches, the character is written to level 0. Two families of input produce no match — EXP above `g_pNextLevel_2[MAX_CLEVEL]` (a level-199 celestial that kept accruing EXP, since `CMob::CheckGetLevel` stops incrementing at the cap but does not stop EXP accumulation) and EXP at 0 (reachable in principle only if a character held exactly 5,000,000 EXP with the level gate already satisfied). The identical recompute in `_MSG_UseItem.cpp:3464` seeds `newlevel` with `CurrentScore.Level` instead, which is direct evidence that the `0` sentinel here is an oversight rather than a designed wipe.

After the write, the handler repairs derived state in a specific order: `CheckGetLevel()` (which, based on the new base level, may itself promote one level if the remaining EXP exceeds the next threshold and may award stat/skill points or clamp stats via `BASE_GetBonusScorePoint`, `CMob.cpp:1034-1152`), `SetCircletSubGod(conn)` when the return is 4 (circlet bonuses at levels 120/150/180 for `SCELESTIAL`), `BASE_GetBonusScorePoint` again, `GetCurrentScore(conn)`, then `SendEtc` + `SendScore` + `SendEmotion(conn, 14, 3)` to resynchronise the client. The double `BASE_GetBonusScorePoint` (once inside `CheckGetLevel`, once explicitly) is idempotent by construction because it recomputes from `BaseScore` rather than accumulating, but the explicit call is reached even when `CheckGetLevel` returned 0.

Note the ordering subtleties that a future change must preserve: the rate band is computed from `CurrentScore.Level` *before* the recompute (so the pre-cost level sets difficulty), whereas the affordability and the recompute use `MOB.Exp`/`BaseScore.Level`. The client message panel and score update are also sent on the cost path regardless of the eventual roll outcome.

**Rule workflow:**
```
if MOB.Exp >= 5_000_000 and combine == 5:
    MOB.Exp -= 5_000_000
    rate = band(CurrentScore.Level)        # 30/35/40/50/70/100
    newlevel = 0                            # <-- sentinel defect
    for i in 0..MAX_CLEVEL-1:
        if Exp >= g_pNextLevel_2[i] and Exp <= g_pNextLevel_2[i+1]: newlevel = i
    BaseScore.Level = newlevel              # may demote, may zero
    check = CheckGetLevel()
    if check == 4: SetCircletSubGod(conn)
    BASE_GetBonusScorePoint(&MOB, &extra)
    GetCurrentScore(conn)
    SendEtc(conn); SendScore(conn); SendEmotion(conn, 14, 3)
```

---

### Business Rule: BR-07 / BR-08 / BR-24 - Table Recipes (Oriharucon Pack, Mysterious Stone)

**Overview:**
Recipes 1 and 2 are the only "pack" recipes: two identical catalysts plus a qualifying base produce a fixed output item with a stack amount of 10, at a rate fixed to 100.

**Detailed description:**
Recipe 1 matches `Item[0].sIndex == 697`, `Item[1].sIndex == 697`, `BASE_GetItemSanc(Item[2]) >= 9` and `Item[2].sIndex != 3338`. The `!= 3338` exclusion exists because recipe 5 has the mirror requirement on that exact id (`3338` with sanc `<= 8`), and the two recipes share the same 697 catalyst pair; without the exclusion a `+9` item 3338 would satisfy both. Success overwrites the output slot with a *freshly constructed* item rather than a copy of the base: `sIndex = 412`, `stEffect[0] = {cEffect 61 (EF_AMOUNT), cValue 10}` — i.e. the refine and any jewels on the base are destroyed by design, and the payload is a stack of ten.

Recipe 2 matches two items in the `[5110, 5133]` band plus `Item[2].sIndex == 413` with `BASE_GetItemAmount(Item[2]) >= 10`. The band is a contiguous 24-item family, so any two members (including two identical ones) qualify; the candidate set is validated by range only, with no `nUnique`, `Grade` or `nPos` filter. Success yields `sIndex = 4148` with `EF_AMOUNT = 10`. Note the asymmetry with the material side: the base item `413` must carry a stack of at least 10, but the entire stack is destroyed in phase D regardless of how many were required, so a player with 99 in the slot spends 99 to make 10. There is no partial consumption anywhere in the component.

Both recipes ship with `g_pEhreRate` = 100 and are therefore deterministic. Since the roll is `_rand <= 100` and `_rand` never exceeds 99, they cannot fail — the success branch is entered on every attempt. The recipes are consequently pure "convert" operations, and their only economic role is the destruction of the inputs' refine/jewel state (recipe 1) or the size of the consumed stack (recipe 2).

**Rule workflow:**
```
# Recipe 1
if I0==697 and I1==697 and sanc(I2)>=9 and I2.sIndex!=3338:
    output.sIndex = 412
    output.stEffect[0] = { EF_AMOUNT(61), 10 }
# Recipe 2
if I0 in [5110..5133] and I1 in [5110..5133] and I2==413 and amount(I2)>=10:
    output.sIndex = 4148
    output.stEffect[0] = { EF_AMOUNT(61), 10 }
```

---

### Business Rule: BR-09 / BR-10 / BR-33 - Jewel Merge into Equipment (Pedra Espiritual, Pedra Amunra)

**Overview:**
Recipes 3 and 4 fuse two jewels (661/662/663) into an existing piece of equipment (items 633 and 3464 respectively), incrementing one of three stats and forcing the item's refine to +7.

**Detailed description:**
The two recipes are identical from the mutation point of view: the only difference is the base item id (`633` versus `3464`) and the rate (`g_pEhreRate[3] = 40` versus `g_pEhreRate[4] = 10`). Both require the base to be refined at least +9, which is the strongest item-quality precondition in the component and makes the recipe a late-game sink. On success the base item is reconstructed by copying `Item[2]` into the output slot and then looping over the two jewel slots `i = 0,1`, so *both* jewels are applied — a submission may therefore carry two different jewel types and receive both bonuses in one attempt, provided the pre-gates allow it.

Jewel application follows a fixed preference order per jewel type: if `stEffect[1]` already holds the matching effect code the value is incremented, else if `stEffect[2]` holds it that slot is incremented, else the first empty slot among 1 and 2 is initialised. Slot 0 is never used for jewel bonuses. The per-jewel deltas and caps are: MP (`70` / item 661) `+2` with an anomalous wrap to `20` at `>= 100`; HP (`69` / item 662) `+2` capped at `20`; critical (`71` / item 663) `+10` capped at `100`. Crucially, if both slots 1 and 2 are already occupied by *other* effect codes, the jewel is silently discarded — the loop has no third option and no message is sent — so a fully-jewelled item can consume two jewels for nothing. That is a second silent-loss path alongside F-06.

After the jewel loop the handler calls `BASE_SetItemSanc(&Carry[ipos], 7, 0)`, which sets the refine encoding to absolute 7 rather than incrementing. Given the +9 prerequisite, the operation therefore *reduces* the refine to +7 while adding the jewel bonus — a deliberate trade of refine level for a permanent stat, and the reason the recipe is not simply "upgrade". `BASE_SetItemSanc` will refuse to write if no refine slot exists (it returns FALSE when slot 2 is not a refine/gem code, `Basedef.cpp:2276-2282`) but the return value is ignored here, so an item whose three effect slots are all occupied by non-refine effects would come out of the recipe unrefined and un-jewelled.

**Rule workflow:**
```
if combine in {3,4}:                                  # gate BR-17 first
    memcpy(output, Item[2])
    for i in 0..1:
        if Item[i].sIndex == 661:  apply MP   (+2, wrap to 20 at >=100)
        elif Item[i].sIndex == 662: apply HP   (+2, cap 20)
        elif Item[i].sIndex == 663: apply CRIT (+10, cap 100)
    BASE_SetItemSanc(output, 7, 0)                     # absolute, ignores return
```

---

### Business Rule: BR-11 / BR-30 - Blessed Refine (+1, refund on failure)

**Overview:**
Recipe 5 increments the refine level of item 3338 by exactly one, at the price of 5,000,000 EXP and a level-band success chance; unlike every other recipe it returns the base item when the roll fails.

**Detailed description:**
The recipe consumes the same two `697` catalysts as recipe 1 plus item `3338` refined at most +8 (`BASE_GetItemSanc(Item[2]) <= 8`). The `<= 8` bound is load-bearing: `BASE_SetItemSanc` maps any `sanc > 15` to `0` (`Basedef.cpp:2243-2244`), so without the bound a +15 item could be "refined" back to +0. With the bound, the maximum written value is 9, which is also the highest value the game's non-encoded refine scale supports before the packed 230+ encodings begin (`BASE_GetItemSanc`, `Basedef.cpp:2123-2138`).

The mutation is a three-step sequence at `:296-301`: copy `Item[2]` into `ipos`, force `stEffect[0].cEffect = EF_SANC` (43), then read the refine back with `BASE_GetItemSanc` and write `sanc + 1`. Forcing slot 0 to the `EF_SANC` code is how the handler guarantees that the new refine has a home: `BASE_SetItemSanc` writes to the first slot whose effect is `EF_SANC` or a 116-125 gem code (`Basedef.cpp:2270-2282`), so by stamping slot 0 the handler makes the write deterministic. The read-back order matters: `BASE_GetItemSanc` checks slots 0, 1, 2 for a 116-125 gem code *before* falling back to `EF_SANC` (`Basedef.cpp:2102-2119`), so if the original refine lives in slot 1 or 2 as a gem code the increment is computed from that slot and correctly preserved; if the original refine lives in slot 1 or 2 as a plain 43 code, the newly stamped slot 0 wins and the increment is computed from whatever value slot 0 happened to carry. The three cases are behaviourally different, and the middle one is a plausible corruption path for items whose refine was stored as a plain `EF_SANC` in a non-zero slot.

The failure branch is unique in the component: it restores `m->Item[2]` into `m->InvenPos[2]` and calls `SendItem` so the player gets the base item back (`:381-385`). The 5,000,000 EXP and the two catalysts are still lost, so the "refund" covers only the item being refined, not the full cost. This is the intended risk model: the player can never lose the equipment, only the currency and the catalysts.

**Rule workflow:**
```
success:
    memcpy(Carry[InvenPos[2]], Item[2])
    Carry[ipos].stEffect[0].cEffect = EF_SANC(43)
    sanc = BASE_GetItemSanc(&Carry[ipos])
    BASE_SetItemSanc(&Carry[ipos], sanc + 1, 0)
failure:
    memcpy(Carry[InvenPos[2]], Item[2]); SendItem(...)     # only refund in the component
```

---

### Business Rule: BR-12 / BR-13 / BR-32 - Mount Costume Apply and Reset

**Overview:**
Recipes 6 and 7 manipulate the visual "costume" tier stored in a mount's third effect slot: recipe 6 stamps a tier derived from the consumed voucher id, recipe 7 clears it back to zero.

**Detailed description:**
Both recipes take a mount as `Item[0]` (`sIndex` in the 2360-2389 block, the ten-mount range also special-cased throughout `BASE_GetItemAbilityNosanc`) and a voucher as `Item[1]`. Recipe 6 accepts vouchers 4190-4199, ten of them, and writes `stEffect[2].cValue = 11 + (Item[1].sIndex - 4190)`, producing the values 11 through 20 — a dense one-to-one mapping from voucher id to stored tier with no gaps. Recipe 7 accepts exactly voucher 4899 and writes `stEffect[2].cValue = 0`, i.e. it is the inverse operation and resets whatever tier was stored. Neither recipe writes `stEffect[2].cEffect`, so the effect *code* is inherited from the mount by the `memcpy` at `:306`/`:313`.

That inheritance is the reason the value is written rather than the whole slot: mounts use `stEffect[2]` for `EF_MOUNTFEED` (code) and `EF_MOUNTKILL` (value) in the mount special case of `BASE_GetItemAbilityNosanc` (`Basedef.cpp:1570-1574`). Recipe 6 therefore overwrites the mount's kill counter with the costume tier. Whether that is intentional reuse of the slot or an unintended clobber cannot be determined from this component alone — no other code writes `stEffect[2].cValue` on a mount — so it is recorded as an ambiguity rather than a defect. `BASE_GetItemSanc` short-circuits for the 2330-2389 range entirely (`Basedef.cpp:2097-2098`), which means mount refine is encoded elsewhere and is unaffected by these writes.

The output slot for both recipes is `m->InvenPos[0]` — the mount's own slot — so the operation is in-place. The voucher slot is zeroed in phase D and never restored, so a failed recipe 6/7 (unreachable at rate 100) would destroy both the voucher and the coin. Because the vouchers are the only variant-specific input and the output tier is a pure function of the voucher id, the recipe sets are fully enumerated: 10 vouchers for apply, 1 for reset.

**Rule workflow:**
```
# Recipe 6 (apply)
memcpy(Carry[InvenPos[0]], Item[0])
Carry[InvenPos[0]].stEffect[2].cValue = 11 + (Item[1].sIndex - 4190)   # 11..20
# Recipe 7 (reset)
memcpy(Carry[InvenPos[0]], Item[0])
Carry[InvenPos[0]].stEffect[2].cValue = 0
```

---

### Business Rule: BR-14 / BR-28 / BR-29 / BR-34 - Soul Combination

**Overview:**
Recipe 8 converts three soul stones (2441-2444) into a permanent character soul value stored in `pMob[conn].extra.Soul`. It is the only recipe that produces no item, and it is order-sensitive and incompletely enumerated.

**Detailed description:**
The resolver only checks that all three descriptors are inside `[2441, 2444]`, which admits 20 distinct multisets and 64 raw ordered triples. The application code then tries to recognise ten exact ordered triples and assigns a `SOUL_*` constant:

| Item[0] | Item[1] | Item[2] | Result | Constant value |
|---------|---------|---------|--------|----------------|
| 2441 | 2441 | 2441 | `SOUL_FC` | 8 |
| 2442 | 2442 | 2442 | `SOUL_IC` | 11 |
| 2443 | 2443 | 2443 | `SOUL_DC` | 14 |
| 2444 | 2444 | 2444 | `SOUL_FD` | 7 |
| 2441 | 2442 | 2443 | `SOUL_ID` | 10 |
| 2441 | 2443 | 2444 | `SOUL_CD` | 17 |
| 2442 | 2443 | 2444 | `SOUL_F` | 2 |
| 2442 | 2441 | 2443 | `SOUL_I` | 3 |
| 2443 | 2442 | 2444 | `SOUL_C` | 5 |
| 2444 | 2441 | 2443 | `SOUL_D` | 4 |

The table is not a function of the multiset. `{2441, 2442, 2443}` maps to `SOUL_ID` in that order and to `SOUL_I` when the first two are swapped; `{2441, 2443, 2444}` maps to `SOUL_CD` while `{2444, 2441, 2443}` maps to `SOUL_D`. Since the client chooses the slot order, two players holding identical stones can obtain different souls, and a single player can obtain either by rearranging slots. Whether this is a designed "recipe by placement" mechanic or an artefact of an iterative development cannot be determined from the source; `GetMatchCombineEhre` gives no hint of position semantics (it treats the three slots as a bag for range checking), while the application treats them as a sequence. The most defensible reading is that placement was intended to distinguish combinations, and that the triples were enumerated by hand from a partial mapping.

The unrecognised permutations are the sharper problem. Because the recipe id is 8 for any triple in range and `g_pEhreRate[8] = 100`, the success branch is always entered; if none of the ten `if/else if` clauses matches, the code falls through to `SendClientMessage(_NN_Processing_Complete)` and `SendClientSignalParm(..., 1)` and returns at `:361`. Three stones are destroyed, `extra.Soul` is unchanged, and the player is told the process completed. There is no message distinguishing "no soul produced" from a successful combination.

Two further specifics: the result is written to character state rather than to `ipos`, which is why the branch returns early and never calls `SendItem` — the cleared slots are the only visual change, and `extra.Soul` reaches the client only via the ordinary character/score serialisation. And `ipos` is never assigned for recipe 8 (`:117-121` covers only 1-5 and 6-7), so the audit call at `:356` evaluates `&pMob[conn].MOB.Carry[-1]` — an out-of-bounds read whose result is a garbage item code fed into `BASE_GetItemCode`, which returns an empty string only if the garbage `sIndex` happens to be outside `[1, MAX_ITEMLIST)`. The audit line is therefore unreliable for recipe 8 and the read is undefined behaviour (F-01).

**Rule workflow:**
```
if combine == 8:
    match exact ordered triple of Item[0], Item[1], Item[2]        # 10 cases
        -> pMob[conn].extra.Soul = SOUL_<x>
    # unmatched triple: no assignment, no error
    _NN_Processing_Complete; _MSG_CombineComplete(1)
    BASE_GetItemCode(&Carry[-1], tt)          # OOB read
    ItemLog(...); return                       # no SendItem, no new item
```

---

### Business Rule: BR-18 / BR-26 - Destruction and Loss Semantics

**Overview:**
All submitted non-empty slots are destroyed before the roll. Success yields exactly one item in one slot; failure yields nothing except for recipe 5's base-item refund.

**Detailed description:**
The destruction loop (`:84-91`) is unconditional once all gates have passed: for every slot with a non-empty descriptor it `memset`s the 8-byte structure and pushes the empty item to the client with `SendItem(conn, ITEM_PLACE_CARRY, slot, ...)`. Two consequences follow. First, the ordering guarantees the client sees the materials disappear before it sees the outcome, which is what prevents a duplication race if the client disconnects mid-resolution. Second, a slot that carries a descriptor but plays no role in the recipe is destroyed anyway — slots 3-7 are never inspected by the resolver, so a "3 ingredients + 5 extras" payload silently loses the extras (F-06). There is no validation that the submission contains exactly the expected number of items.

On success the handler writes into `ipos` and finally calls `SendItem` once for that slot, except for recipe 8 which returns before the `SendItem` because no item exists. On failure the only compensation is recipe 5's `memcpy` of `Item[2]` back into `InvenPos[2]`; recipes 1-4 and 6-8 lose everything. The failure message discloses the internal arithmetic: `sprintf(temp, "%s(%d/%d)", _NN_CombineFailed, _rand, rate)`, i.e. the player is told the exact roll and the exact threshold.

Because the consumed items exist only in the request payload after phase D, an exception, disconnect, or server crash between `:91` and `:377` is unrecoverable: the inventory is already empty server-side, and the client's copy is not authoritative. There is no journal, no lock, and no idempotency key, so a replayed packet after a partial failure would be re-validated against the (now empty) inventory and rejected by BR-03.

**Rule workflow:**
```
for i in 0..7:
    if Item[i].sIndex == 0: continue
    Carry[InvenPos[i]] = zero; SendItem(CARRY, InvenPos[i], zero)
... roll ...
success: write Carry[ipos]; SendItem(CARRY, ipos, Carry[ipos])      # except recipe 8
failure: recipe 5 only -> restore Item[2] into InvenPos[2]; SendItem
```

---

### Business Rule: BR-19 - Audit Trail

**Overview:**
Every attempt that passes the gates writes three records to the item log: a banner, a comma-free list of the submitted item codes, and a footer or outcome marker.

**Detailed description:**
The audit uses `ItemLog(char *str1, char *str2, unsigned int ip)` (`Server.cpp:9264-9284`), which prefixes a timestamp and dotted IP, then writes `"<timestamp/ip> <str2> <str1>"` to the day's `item_log/TM_*.txt` file opened by `StartItemLog`. The handler calls it three times: `"*** Item combine ehre. ***"`, then a concatenation of `BASE_GetItemCode` output for each submitted item, then `"*** ------------------- ***"`. `BASE_GetItemCode` renders `" <Name> : <sIndex>.<c0>.<v0>.<c1>.<v1>.<c2>.<v2>"`, so the log captures the full pre-state of every consumed item, including refine and jewel encodings — this is the only reconstruction path available for a lost item, and it is why the pre-state is logged before destruction.

The concatenation target is the process-global `char temp[4096]` declared at `Server.cpp:277` and reused by the dispatcher, the socket layer and dozens of handlers. The handler never clears it before the loop, so the record begins with whatever previous content the buffer held (leading garbage in the log line), and it appends 8 item descriptions of up to ~48 bytes. The buffer is large enough for 8 items in isolation but the accumulated content of a preceding tenant is unbounded in principle; with `strcat` and no length check, this is a latent overflow. The same pattern exists verbatim in the sibling handlers (e.g. `_MSG_CombineItemAgatha.cpp:69-80`), so it is a family-wide characteristic rather than an Ehre-specific mistake. The success path additionally logs `"*** Combine ehre sucess ***"` with `AccountName` (note the misspelling and the argument-order variation: account name is passed as `str1` and the marker as `str2`), and the failure path logs `"*** Combine ehre fail ***"`.

The log is best-effort: `ItemLog` silently does nothing when `fItemLogFile` is NULL, and the log file is rotated by `StartItemLog` on a day boundary. There is no error return to act on, so audit failure is invisible to the handler.

**Rule workflow:**
```
ItemLog("*** Item combine ehre. ***", account, ip)
for i in 0..7: if Item[i].sIndex: strcat(temp, BASE_GetItemCode(&Item[i]))   # temp never cleared
ItemLog(temp, account, ip)
ItemLog("*** ------------------- ***", account, ip)
...
success -> ItemLog("<account> <itemcode>", "*** Combine ehre sucess ***", ip)
failure -> ItemLog("*** Combine ehre fail ***", account, ip)
```

---

### Business Rule: BR-25 / BR-27 - Output Slot Selection and Completion Signal

**Overview:**
The protocol contract is a single `_MSG_CombineComplete` standard-parameter message with value 0 (aborted), 1 (success) or 2 (roll failure), and the crafted item always lands in the slot the client designated for the recipe's base object.

**Detailed description:**
`ipos` is chosen by recipe family: recipes 1-5 write into `m->InvenPos[2]` (the base object's slot), recipes 6-7 into `m->InvenPos[0]` (the mount's slot), and recipe 8 writes nowhere. The choice is positional rather than semantic — it reuses the fact that the resolver already assigns fixed roles to `Item[0..2]` — which means a player who submits the correct items in the correct *order* but with the base in a different inventory slot still succeeds, because the slot is read from the payload. It also means the client and server must agree on the ordering convention; there is no server-side re-derivation of "which of these three is the base".

The three signal values are used consistently: 0 for every pre-resolution rejection (ownership mismatch, unknown recipe, level/coin/stat gate), 1 for a completed operation including the degenerate recipe-8 case, and 2 for a roll failure. Value 1 is sent *before* `SendItem` on the success path, so a client that reacts to the signal by re-reading inventory could observe the pre-write state; in practice the following `SendItem` in the same send buffer corrects it. Recipe 8 sends 1 with no accompanying `SendItem`, which is the only case where the signal and the visible inventory change are decoupled.

**Rule workflow:**
```
ipos = InvenPos[2]  if combine in 1..5
ipos = InvenPos[0]  if combine in {6,7}
ipos = none         if combine == 8
signal 0 -> rejection (multiple sites)
signal 1 -> success (items 1-7) / soul stored (8)
signal 2 -> roll failure
```

---

### Business Rule: BR-31 - Test-Server Randomness Bypass

**Overview:**
`LOCALSERVER` forces every crafting roll to succeed, independently of the configured rates.

**Detailed description:**
The success predicate is `if (_rand <= rate || LOCALSERVER)` (`:176`), and `LOCALSERVER` is a process-wide `int` initialised to 0 (`Server.cpp:380`) and set to 1 when the configured DB server address compares equal to the literal `"192.168.0.50"` (`Server.cpp:3587-3588`). It is therefore not an operator switch but an environment fingerprint: any deployment whose database is reachable at that address has all combine rolls, sanctification rolls (`_MSG_UseItem.cpp:1603`), and several drop/experience modifiers bypassed. The same bypass appears in every sibling combine handler (`_MSG_CombineItem.cpp:84`, `_MSG_CombineItemAgatha.cpp:90`, `_MSG_CombineItemOdin.cpp:124`, etc.), so it is a family-wide convention rather than an Ehre-only decision.

The practical risk is deployment-dependent and high: the flag is invisible in the rate configuration, is not logged at startup, and cannot be disabled without recompiling or changing the DB address. On a test server it is exactly what is wanted (deterministic crafting for QA); on a production server with a matching internal address it silently converts every probabilistic recipe into a guaranteed one, including recipe 5 (`rate` is still computed, but is irrelevant).

**Rule workflow:**
```
if AddressMatches("192.168.0.50"): LOCALSERVER = 1
...
success = (_rand <= rate) || LOCALSERVER      # bypasses BR-20 entirely
```

---

## 4. Component Structure

### 4.1 Physical structure

```
Code/TMSrv/
├── _MSG_CombineItemEhre.cpp        # 395 lines - the entire handler (this component)
├── ProcessClientMessage.h          # :114 declares void Exec_MSG_CombineItemEhre(int, char*)
├── ProcessClientMessage.cpp        # :246-248 the only dispatch site (afferent = 1)
├── GetFunc.cpp                     # :106-165 GetMatchCombineEhre  (recipe table)
├── GetFunc.h                       # :26 prototype
├── CReadFiles.cpp                  # :375-435 ReadCompRate EHRE block (g_pEhreRate override)
├── CReadFiles.h                    # :49 COMP_RATE_PATH = "../../Common/Settings/CompRate.txt"
├── SendFunc.cpp                    # SendClientMessage :27, SendClientSignalParm :208,
│                                   # SendEmotion :825, SendItem :1055, SendScore :1136, SendEtc :1195
├── Server.cpp                      # RemoveTrade :7319, SetCircletSubGod :8855, ItemLog :9264,
│                                   # global temp :277, LOCALSERVER :380, address fingerprint :3587
├── CMob.cpp                        # CheckGetLevel :1034
└── CMob.h                          # GetCurrentScore :113, MaxCarry :96

Code/
├── Basedef.cpp                     # g_pEhreRate :75-78, g_pNextLevel_2 :656,
│                                   # BASE_GetItemCode :1479, BASE_GetItemAbilityNosanc :1497,
│                                   # BASE_GetItemSanc :2095, BASE_GetBonusScorePoint :858,
│                                   # BASE_SetItemSanc :2239, BASE_GetItemAmount :2287
├── Basedef.h                       # MSG_CombineItem :1956-1963, MAX_COMBINE :113, STRUCT_ITEM :398
├── ItemEffect.h                    # EF_HPADD2 :111 (69), EF_MPADD2 :112 (70), EF_CRITICAL2 :113 (71)
└── Language.h                      # _NN_Processing_Complete :158, _NN_CombineFailed :289,
                                    # _NN_Wrong_Combination :271
```

### 4.2 Internal region map (single translation unit, 395 lines)

| Lines | Region | Responsibility |
|-------|--------|----------------|
| 1-19 | Licence header + include | GPL notice (2015, VK/Charles TheHouse); includes only `ProcessClientMessage.h` |
| 20-43 | Phase A | Presence, slot range, ownership `memcmp` |
| 45-52 | Phase B | Recipe resolution + unknown-recipe rejection |
| 54-66 | Phase C | Level/mastery gate (recipe 5) and coin gate (recipes 6,7) |
| 69-81 | Phase C | Jewel-injection ceiling gate (recipes 3,4) |
| 84-91 | Phase D | Material destruction + client sync |
| 93-107 | Phase E | Audit logging (3 records) |
| 109-121 | Phase F/G/H | RNG, rate lookup, output slot selection |
| 124-128 | Phase I | Coin cost (recipes 6,7) |
| 130-174 | Phase I | EXP cost, level recompute, progression repair (recipe 5) |
| 176-362 | Phase K/L | Success branch, per-recipe mutation, recipe 8 early return |
| 364-377 | Phase L | Generic success notification |
| 379-394 | Phase M | Failure: recipe 5 refund, message, signal 2, log |

There are no functions, classes, namespaces, or static helpers inside the file: one 375-line function body. Every operation is inline, which is why the component has no unit-testable seams.

### 4.3 Declared interface

```c
void Exec_MSG_CombineItemEhre(int conn, char *pMsg);   // ProcessClientMessage.h:114
```

Preconditions assumed by the implementation (all enforced outside this component):

| Precondition | Enforced by |
|--------------|-------------|
| `conn` in `[0, MAX_USER)` | `ProcessClientMessage.cpp:42-51` (partially; `conn` and `std->ID` are independent) |
| Packet passed the checksum | `CPSock::ReadMessage` + `Server.cpp:3990-3999` |
| Packet is at least 84 bytes | **nothing** — only `>= 12` is enforced (F-09) |
| `pUser[conn].Mode == USER_PLAY` | **nothing at dispatch**; only the `Send*` helpers check it |
| `pMob[conn]` is a loaded character | **nothing**; a stale mob yields an empty inventory and thus an ownership mismatch |

### 4.4 Owned vs borrowed data

| Data | Ownership | Notes |
|------|-----------|-------|
| `m->Item[8]`, `m->InvenPos[8]` | Client-supplied, untrusted | Validated by BR-01/02/03/06 only |
| `pMob[conn].MOB.Carry` | Server-authoritative | Read for validation, written for effects |
| `pMob[conn].MOB.Coin`, `.Exp` | Server-authoritative | Decremented in phase I |
| `pMob[conn].MOB.BaseScore.Level` | Server-authoritative | Recomputed in phase I (BR-22) |
| `pMob[conn].extra.Soul` | Server-authoritative | Written by recipe 8 |
| `g_pEhreRate` | Shared config | Read-only here; written by `CReadFiles` and `imple.cpp` reload |
| `temp[4096]` | Global, shared | Written by `strcat` without clearing (F-08) |
| `g_pItemList` | Shared item database | Read via `BASE_*` helpers |

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains

```
ProcessClientMessage (dispatch)
  └─> Exec_MSG_CombineItemEhre                       Code/TMSrv/_MSG_CombineItemEhre.cpp:20
        ├─> RemoveTrade                              Code/TMSrv/Server.cpp:7319
        │     ├─> SendClientSignal (900)
        │     └─> GetCreateMob / GridMulticast       (trade-mode teardown)
        ├─> ItemLog                                  Code/TMSrv/Server.cpp:9264
        │     └─> fItemLogFile / localtime
        ├─> SendClientMessage                        Code/TMSrv/SendFunc.cpp:27
        ├─> SendClientSignalParm                     Code/TMSrv/SendFunc.cpp:208
        ├─> GetMatchCombineEhre                      Code/TMSrv/GetFunc.cpp:106
        │     ├─> BASE_GetItemSanc                   Code/Basedef.cpp:2095
        │     └─> BASE_GetItemAmount                 Code/Basedef.cpp:2287
        ├─> BASE_GetItemAbilityNosanc                Code/Basedef.cpp:1497
        ├─> BASE_GetItemCode                         Code/Basedef.cpp:1479
        ├─> BASE_GetItemSanc / BASE_SetItemSanc      Code/Basedef.cpp:2095 / 2239
        ├─> SendEtc                                  Code/TMSrv/SendFunc.cpp:1195
        ├─> CMob::CheckGetLevel                      Code/TMSrv/CMob.cpp:1034
        │     ├─> BASE_GetBonusScorePoint            Code/Basedef.cpp:858
        │     └─> CMob::GetCurrentScore              Code/TMSrv/CMob.cpp
        ├─> SetCircletSubGod                         Code/TMSrv/Server.cpp:8855
        ├─> BASE_GetBonusScorePoint                  Code/Basedef.cpp:858
        ├─> CMob::GetCurrentScore
        ├─> SendScore                                Code/TMSrv/SendFunc.cpp:1136
        ├─> SendEmotion                              Code/TMSrv/SendFunc.cpp:825
        └─> SendItem                                 Code/TMSrv/SendFunc.cpp:1055

Configuration (out-of-band, runtime):
  CReadFiles::CReadFiles()  ──> CReadFiles::ReadCompRate()   Code/TMSrv/CReadFiles.cpp:66-75, 375
                                  └─> ../../Common/Settings/CompRate.txt  -> g_pEhreRate[1,2,3,4,6,7,8]
  imple.cpp "reloadfile"    ──> CReadFiles::ReadCompRate()   Code/TMSrv/imple.cpp:250-262
  Server.cpp config read    ──> LOCALSERVER = 1 when DBServerAddress == "192.168.0.50"   :3584-3588
```

### 5.2 External dependencies

| Dependency | Version / origin | Kind | Purpose | Failure mode |
|------------|------------------|------|---------|--------------|
| Win32 sockets (WSA) | Windows SDK | Platform | Delivers the packet that reaches this handler | Handled upstream (`Server.cpp:3975-3999`) |
| MSVC CRT `rand()` | MSVC runtime | Library | Success roll | Deterministic per-process seed if never seeded; no quality requirement met |
| MSVC CRT `memcmp/memcpy/memset/strcat/sprintf` | MSVC runtime | Library | Validation, mutation, logging | Unbounded `strcat` (F-08) |
| `localtime`, `time` | MSVC CRT | Library | Item log timestamps | Return value unchecked |
| `CompRate.txt` | Runtime data, `../../Common/Settings/` | Config file | Overrides `g_pEhreRate` | Absent -> modal MessageBox then rates stay at code defaults (`CReadFiles.cpp:381-385`) |
| `Language.txt` message table | Runtime data | Config file | `_NN_*` strings | Loaded at boot/reload (`imple.cpp:253`) |
| Item database (`g_pItemList`) | Runtime data | In-memory database | `Name`, `nUnique`, `nPos`, `stEffect[]`, `ReqLvl` | Indices are range-checked against `MAX_ITEMLIST` before use |
| `system.db` / character persistence | DBSrv via `FLAG_GAME2DB` | Service | Persists `Exp`, `Coin`, `BaseScore.Level`, `extra.Soul` | Out of component scope; no acknowledgement is awaited here |

### 5.3 Coupling to global state (quantitative)

| Global | Kind | Accesses in this file |
|--------|------|-----------------------|
| `pMob[]` | `CMob[MAX_MOB]` | 43 distinct member accesses |
| `pUser[]` | `CUser[MAX_USER]` | 9 (AccountName, IP, Trade via RemoveTrade) |
| `g_pEhreRate[]` | `int[10]` | 1 |
| `g_pMessageStringTable[]` | message table | 4 |
| `g_pNextLevel_2[]` | `long long[401]` | 2 |
| `temp` | `char[4096]` | 5 |
| `LOCALSERVER` | `int` | 1 |
| `g_pItemList[]` | via `BASE_*` only | indirect |
| `rand()` global seed | CRT | 1 |

### 5.4 Compile-time vs runtime dependency graph

- **Compile-time (linked symbols):** `ProcessClientMessage.h` → `Server.h` → `Basedef.h`/`ItemEffect.h`/`Language.h`. The handler therefore sees the entire server symbol surface; it has no private header and no forward-declared seam.
- **Runtime (state coupling):** the handler assumes `pMob[conn]` is a fully loaded character, `g_pItemList` is populated, `g_pEhreRate` has been initialised (code default or `CompRate.txt`), and `g_pNextLevel_2` is the celestial table. None of these are asserted; all are implicit.
- **Temporal coupling:** `ReadCompRate` must complete before the first combine; the constructor chain `CReadFiles::CReadFiles()` (`CReadFiles.cpp:66-75`) guarantees this at boot, but `reloadfile` re-runs it on a live server with no locking, so a combine concurrent with a reload can observe a partially updated `g_pEhreRate`.

---

## 6. Afferent and Efferent Coupling

### 6.1 Component-level (module granularity)

| Component (C++ translation unit / function / global) | Afferent (Ca) | Efferent (Ce) | Instability Ce/(Ca+Ce) | Criticality |
|---|---|---|---|---|
| `Exec_MSG_CombineItemEhre` (function) | 1 | 26 | 0.96 | Medium |
| `_MSG_CombineItemEhre.cpp` (unit) | 1 | 14 (distinct called functions) | 0.93 | Medium |
| `GetMatchCombineEhre` (function) | 1 | 4 | 0.80 | **High** (recipe truth) |
| `g_pEhreRate` (global array) | 2 | 0 | 0.00 | **High** (economy) |
| `MSG_CombineItem` (struct) | 11 | 0 | 0.00 | **High** (wire contract) |
| `_MSG_CombineItemEhre` (wire constant) | 1 | 0 | 0.00 | Low |
| `LOCALSERVER` (global flag) | 20+ | 0 | 0.00 | **Critical** (global RNG bypass) |
| `temp[4096]` (global buffer) | 100+ | 0 | 0.00 | **High** (shared mutable buffer) |

The afferent figure of 1 for the handler matches the architecture report. The efferent figure of 14 in that report counts *distinct functions called*; at symbol level (functions + globals + types + constants) the efferent surface is ~30, which is the more honest measure of what this unit must be re-verified against on any change.

### 6.2 Symbol-level coupling (what the handler calls, per phase)

| Phase | Outgoing symbol | File:line | Coupling type |
|-------|-----------------|-----------|---------------|
| A | `RemoveTrade` | `Server.cpp:7319` | Control (early exit) |
| A,E,L,M | `ItemLog` | `Server.cpp:9264` | Data (write-only log) |
| A,C,M | `SendClientSignalParm` | `SendFunc.cpp:208` | Data (protocol) |
| B | `GetMatchCombineEhre` | `GetFunc.cpp:106` | Control + data (recipe id) |
| B | `BASE_GetItemSanc`, `BASE_GetItemAmount` | `Basedef.cpp:2095,2287` | Data (indirect via B) |
| C | `SendClientMessage` | `SendFunc.cpp:27` | Data (protocol) |
| C | `BASE_GetItemAbilityNosanc` | `Basedef.cpp:1497` | Data (item template) |
| D,L | `SendItem` | `SendFunc.cpp:1055` | Data (inventory sync) |
| E,L | `BASE_GetItemCode` | `Basedef.cpp:1479` | Data (log rendering) |
| I | `SendEtc` | `SendFunc.cpp:1195` | Data (currency/exp sync) |
| I | `CheckGetLevel` | `CMob.cpp:1034` | Control + state (mutates level/stats) |
| I | `SetCircletSubGod` | `Server.cpp:8855` | State (equipment mutation) |
| I | `BASE_GetBonusScorePoint` | `Basedef.cpp:858` | State (stat clamp) |
| I | `GetCurrentScore` | `CMob.cpp` | State (derived recompute) |
| I | `SendScore`, `SendEmotion` | `SendFunc.cpp:1136,825` | Data (client sync) |
| K | `BASE_GetItemSanc`, `BASE_SetItemSanc` | `Basedef.cpp:2095,2239` | State (refine write) |

### 6.3 Cohesion assessment

| Cohesion dimension | Assessment |
|---|---|
| Functional cohesion | **Moderate.** One purpose, but 8 recipes are 8 independent behaviours fused into one function; the unit conflates "recipe dispatch", "economy transaction" and "progression repair". |
| Temporal cohesion | **High** but for the wrong reason: phases are coupled only by the order in which they happen, not by shared data abstractions. |
| Communicational cohesion | **High**: all phases share `pMob[conn]`. |
| Logical cohesion | Present in the success branch, which is an `if/else` chain over recipe id — the classic logical-cohesion shape. |
| Coupling type | **Content/Common coupling**: direct indexing of `pMob[]`, `pUser[]`, `g_pEhreRate[]`, `g_pNextLevel_2[]`, and mutation of the shared `temp`. |
| Coupling strength | **High** (pathological, per Myers' taxonomy): the handler modifies global state owned by no one in particular and writes into a buffer shared with 100+ sites. |
| Fan-out | 26 symbols, of which 3 mutate progression state. |

---

## 7. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Game client (TCP) | Inbound message | Recipe submission | Custom binary over TCP, WSA, `INITCODE` handshake + obfuscation + checksum | `MSG_CombineItem` (84 bytes: 12 header + 8x8 item + 8 slot bytes) | Size validated only against `[12, 8192]` (F-09); checksum enforced upstream (`Server.cpp:3990-3999`); rejections answered with `_MSG_CombineComplete(0)` except for BR-02 which is silent |
| Game client (TCP) | Outbound messages | Inventory sync, currency/exp sync, score sync, emotion, completion signal | Same channel, `pUser[conn].cSock.AddMessage` | `_MSG_SendItem`, `_MSG_UpdateEtc`, `_MSG_UpdateScore`, `_MSG_Emotion`, `_MSG_STANDARDPARM` (`_MSG_CombineComplete`) | All senders validate `conn` range, `Mode == USER_PLAY` and socket != 0; drops silently |
| `CompRate.txt` | Config file | Override the 8 recipe rates | Local file, `fopen`/`fgets`/`sscanf` | Whitespace-separated `EHRE <NAME> <0..100>` | Missing file -> modal MessageBox, defaults retained; invalid value -> MessageBox, line skipped, **no rollback**; case-sensitive key match on the name token |
| Item database (`g_pItemList`) | In-memory table | Item names, static effects, stack amounts, refine decode | Direct array access through `BASE_*` helpers | `STRUCT_ITEMLIST` (`Basedef.h:882-906`) | Index range-checked (`!=0` and `< MAX_ITEMLIST`); no version or consistency check |
| Level tables (`g_pNextLevel_2`) | Static array | EXP->level recompute for recipe 5 | Direct access | `long long[401]`, 20M per level | **No bound check on the resulting index**; unmatched band writes level 0 (F-02) |
| Character persistence (DBSrv) | Service (indirect) | Persist `Exp`, `Coin`, `BaseScore.Level`, `extra.Soul` | `FLAG_GAME2DB` messages via `cFileDB` | Binary account/mob blob | Not handled here; the handler does not wait for an acknowledgement, so a DB failure after mutation is silent to the client |
| `Language.txt` message table | Config file | User-facing strings | `g_pMessageStringTable[]` | Index-based string array | Missing index -> out-of-bounds read (no bound check at the call sites) |
| Item log file (`item_log/TM_*.txt`) | File system | Audit trail | `fprintf` through `ItemLog` | Plain text | `fItemLogFile == NULL` -> silently no-op; rotation by `StartItemLog` |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose | Assessment |
|---------|----------------|----------|---------|------------|
| Command Handler / Message Dispatch | One `case` label → one `Exec_*` function | `ProcessClientMessage.cpp:246` | Decouples wire type from behaviour | Consistent with 57 sibling handlers |
| Table-driven dispatch (attempted) | `g_pEhreRate[recipeId]` | `Basedef.cpp:75` | Per-recipe rate lookup | Only the *rate* is table-driven; recipes, gates and outputs are code |
| Hardcoded recipe chain (used instead) | 8 `if/else if` on raw item ids | `GetFunc.cpp:140-162` | Recipe matching | High cyclomatic cost, no data separation, id magic numbers |
| Two-phase validate-then-mutate | Validation loop, then destruction, then resolution | `:24-43`, `:84-91` | Prevent crafting from an out-of-sync inventory | Correct in intent, no rollback after phase D |
| Guard/early-return chain | 4 sequential gates with `return` | `:47-81` | Keep the happy path linear | Clean; but all failures collapse to one generic message |
| Anti-desync sentinel (memcmp oracle) | Full-structure compare against the server copy | `:37-42` | Detect item substitution and inventory drift | Effective; logs but does not penalise |
| Snapshot-and-refund | `m->Item[2]` replayed into the slot on failure | `:383` | Guarantee the base item survives | Only implemented for recipe 5 |
| Config-over-code defaults | `g_pEhreRate` code literal + `CompRate.txt` override | `Basedef.cpp:75`, `CReadFiles.cpp:407` | Operator tuning without recompiling | Good; no validation of the reload race |
| Environment fingerprint / feature kill-switch | `LOCALSERVER` derived from the DB IP literal | `Server.cpp:3587` | Disable RNG on test environments | **Anti-pattern**: implicit, unlogged, server-wide |
| Copy-paste variant family | 11 near-identical `_MSG_CombineItem*.cpp` files | `Code/TMSrv/` | One handler per crafting NPC | Duplicated validation, duplicated RNG idiom, duplicated global-buffer log |
| God-function | 375 lines, no extracted helpers, 30 dependencies | whole file | - | No seams for testing |
| Stringly / magic-number constants | 412, 4148, 697, 413, 633, 3464, 3338, 4899, 4190-4199, 5110-5133, 2441-2444 inline in code | `GetFunc.cpp`, `_MSG_CombineItemEhre.cpp` | - | Item ids encode all domain knowledge; no symbolic names |

Architectural decisions observable in the component:

1. **Recipes are code, not data.** Unlike `g_pSancRate`, `g_pOdinRate` or the quest tables, there is no recipe list structure: matching, gating and output construction are three separate inline chains that must be kept in sync by hand. Adding a recipe requires edits in three places (`GetFunc.cpp`, the gate block, the success block).
2. **Server is authoritative on ownership, client is authoritative on placement.** Items are validated byte-for-byte, but the *slot layout* (`Item[0]` vs `Item[2]` roles) is taken from the client. This is what makes BR-28 (order-sensitive souls) observable at all.
3. **Cost precedes outcome.** Both coin and EXP are charged before the roll and are never refunded, which is a deliberate "pay per attempt" economy model.
4. **Failure is destructive by default.** Only the equipment in recipe 5 is protected; catalysts and jewels are always lost. That is a strong design statement about how punishing the crafting economy is intended to be.
5. **Progression is repaired, not delegated.** Because recipe 5 mutates `BaseScore.Level` directly, the handler must manually reconstruct level, stats, circlet bonuses, and client-facing scores in a fixed order — duplicating logic that `CheckGetLevel` already owns.

---

## 9. Technical Debt & Risks

### 9.1 Risk register

| Risk | Severity | Component area | Issue | Impact |
|------|----------|----------------|-------|--------|
| R-01 | **Critical** | `_MSG_CombineItemEhre.cpp:152-160` | `newlevel` sentinel is `0`; an EXP above `g_pNextLevel_2[199]` (or an unmatched band) writes `BaseScore.Level = 0` | Permanent level wipe for max-level celestial characters; stats are not rolled back, so the character is left inconsistent and takes hundreds of kills to recover |
| R-02 | **High** | `_MSG_CombineItemEhre.cpp:356` | `ipos == -1` for recipe 8 → `&pMob[conn].MOB.Carry[-1]` read | Out-of-bounds read of another mob's memory; corrupt audit record; undefined behaviour (benign today, unsafe under any layout change) |
| R-03 | **High** | `_MSG_CombineItemEhre.cpp:320-348` | Only 10 of the in-range soul triples are mapped; unmatched triples still consume 3 stones and report success | Silent item loss; player-visible "success" with no effect; support burden |
| R-04 | **High** | `_MSG_CombineItemEhre.cpp:176` + `Server.cpp:3587` | `LOCALSERVER` bypasses every roll; set from a hardcoded DB IP literal, unlogged | Any deployment matching the address has deterministic crafting (and deterministic sanc, drops) |
| R-05 | **High** | component-wide | Zero automated tests; no seams (no functions, no injected dependencies) | Regressions in an 8-recipe economy component are only detectable in production |
| R-06 | Medium | `_MSG_CombineItemEhre.cpp:75` | Duplicated clause `hp >= 10 && mp >= 10` in the jewel gate; intended pair is missing | Gate is stricter/looser than designed in a way that is not reconstructable from the source alone |
| R-07 | Medium | `_MSG_CombineItemEhre.cpp:204-205` | MP jewel wrap `>= 100 -> 20` while HP caps at 20 and crit caps at 100 | A near-max item loses 80 points of MP bonus on a "successful" merge |
| R-08 | Medium | `_MSG_CombineItemEhre.cpp:84-91` vs `GetFunc.cpp:140-162` | Slots 3-7 are destroyed but never validated | Accidental or client-induced destruction of unrelated inventory |
| R-09 | Medium | `_MSG_CombineItemEhre.cpp:103` | `strcat(temp, itemlog)` on the shared 4096-byte global, never cleared | Log corruption; latent buffer overflow; cross-handler data bleed |
| R-10 | Medium | `_MSG_CombineItemEhre.cpp:109-111` | `rand()%115` folded at `>=100` doubles the weight of 85..99 | Realised success rates are below configured rates for every value under 100 (BR-20 table) |
| R-11 | Medium | packet ingress | No per-type size validation (`CPSock.cpp:397-411` enforces only `>= 12`) | Handler reads 84 bytes from a possibly 12-byte packet; reads adjacent buffer bytes |
| R-12 | Medium | `_MSG_CombineItemEhre.cpp:291` | `BASE_SetItemSanc` return value ignored | Items with no free refine slot silently lose refine; no error is reported |
| R-13 | Medium | `_MSG_CombineItemEhre.cpp:296-301` | Forcing `stEffect[0].cEffect = EF_SANC` before reading the sanc changes which slot `BASE_GetItemSanc` resolves | Correct for gem-encoded refines in any slot, incorrect for plain `EF_SANC` in slots 1/2; item can be corrupted |
| R-14 | Medium | `_MSG_CombineItemEhre.cpp:89-91` … `:377` | No transaction/rollback/journal between destruction and result | Crash or disconnect in the window loses materials with no recovery path |
| R-15 | Low | `_MSG_CombineItemEhre.cpp:387` | Failure message discloses `_rand` and `rate` | Client can measure the true distribution, including the BR-20 bias |
| R-16 | Low | `_MSG_CombineItemEhre.cpp:33-34` | Invalid slot → `RemoveTrade` + silent return | Client waits forever for `_MSG_CombineComplete`; indistinguishable from packet loss |
| R-17 | Low | `_MSG_CombineItemEhre.cpp:160` | `BaseScore.Level = newlevel` without clamping to `[0, MAX_CLEVEL]` | Relies entirely on the loop for bounds; no defensive clamp |
| R-18 | Low | `CReadFiles.cpp:407-435` | `EHRE` sub-keys are matched case-sensitively on the raw token while only the section token is uppercased | A lowercase key in `CompRate.txt` is silently ignored (rate stays at default); no diagnostic |
| R-19 | Low | `CReadFiles.cpp:409-413` | Invalid value → MessageBox + `continue`, leaving the previous value | A typo leaves the old rate live with no log entry; dialog blocks the server thread |
| R-20 | Low | `imple.cpp:250-262` | `reloadfile` re-runs `ReadCompRate` with no locking against in-flight combines | A combine can read a half-updated rate table (harmless today: independent int writes) |
| R-21 | Informational | `extra.Soul` | Written by recipe 8 but never read anywhere in TMSrv | The recipe's gameplay effect is invisible server-side; correctness depends entirely on the client/DB contract |

### 9.2 Code smells

| Smell | Evidence | Consequence |
|-------|----------|-------------|
| God function / long method | 375-line body, single function | Untestable, unreviewable diff surface |
| Magic numbers | `412, 4148, 697, 413, 633, 3464, 3338, 4899, 4190, 5110, 5133, 2441..2444, 5000000, 1000000, 115, 15` | Recipe semantics are not discoverable; no symbolic names or tables |
| Duplicated code | validation loop identical to 10 siblings; `rand()%115`/`-15` identical to Agatha; success-notification block identical to the family | Every fix must be applied 11 times |
| Shotgun surgery | Adding a recipe requires edits in `GetFunc.cpp`, the gate chain, `ipos` selection and the success chain | High regression probability |
| Copy-paste defect | duplicated `hp>=10 && mp>=10` clause (`:75`) | Gate does not match its documented intent |
| Misleading naming | `GetMatchCombineEhre` returns an id, stored in `int rate`; `rate` later overwritten by a real percentage | Two different meanings of "rate" in one function |
| Global mutable state | `temp`, `LOCALSERVER`, `pMob[]`, `pUser[]` | Reentrancy impossible; testing impossible; hidden temporal coupling |
| Inconsistent error signalling | silent return (BR-02) vs message+signal 0 vs signal 2 | Client cannot reliably distinguish causes |
| Dead configuration | `g_pEhreRate[0]`, `[5]`, `[9]` are never read meaningfully (`[5]` always overwritten) | Operators may tune a value that has no effect |
| Comment as documentation | `//Ref Abençoada` at `:55` and `:382`, `//MP`, `//HP`, `//Critico`, `//Pacote Oriharucon` | The only record of recipe semantics is inline Portuguese comments |
| Inconsistent log argument order | `:359` passes `AccountName` as `str1` and the marker as `str2`; other calls reverse them | Log parsing requires special-casing |
| Unused local | `char tt[256]` declared at `:364` after the early return at `:361` | Dead-scope declaration; harmless but signals haste |

### 9.3 Security posture summary

| Control | Status | Evidence |
|---------|--------|----------|
| Server-authoritative item ownership | **Present, strong** | Full 8-byte `memcmp` against `MOB.Carry` (`:37`) |
| Slot index bounds | **Present** | `invPos` vs `MaxCarry` (`:31`), `sIndex` vs `MAX_ITEMLIST` (`GetFunc.cpp:116-138`) |
| Item id allow-listing | **Present** | Every recipe is an explicit id/range test |
| Refine decode safety | **Present** | `BASE_GetItemSanc` normalises 230-253 encodings (`Basedef.cpp:2121-2140`) |
| Packet size validation | **Absent** | Only `[12, 8192]` (`CPSock.cpp:397`) |
| Packet checksum | **Present** | `CPSock::ReadMessage`, enforced at `Server.cpp:3990` |
| Rate limiting / anti-spam | **Absent** | No `LastCombineTime`, no `AddCrackError`, no per-second counter |
| Intrusion telemetry | **Partial** | Ownership mismatch is logged (`:39`) but does not increment `AddCrackError`; the handler never calls it (contrast `_MSG_Action.cpp:36-149`) |
| Session state validation | **Absent at dispatch** | No `Mode == USER_PLAY` gate; only the `Send*` helpers check it |
| Balance/tamper protection | **Weak** | `LOCALSERVER` silently disables all RNG |
| Audit completeness | **Partial** | Pre-state logged, but the log can be corrupted by `temp` residue (F-08) and recipe 8's entry is garbage (R-02) |

The handler's core anti-cheat property is strong: because the server re-reads every item from its own inventory and compares all six effect bytes, a client cannot upgrade an item, forge a refine, or duplicate by replay. The weaknesses are all structural — packet framing, rate limiting, RNG bypass, and the untestability of the recipe logic — rather than a hole in the item-ownership model.

---

## 10. Test Coverage Analysis

### 10.1 Direct answer: there are no tests

No test artifacts of any kind exist for this component, or anywhere in the repository. The evidence:

| Probe | Result |
|-------|--------|
| Files matching `*test*`, `*Test*`, `*spec*`, `*_test*`, `*.Tests*` outside `docs/`, `.agents/`, `.codegraph/`, `.opencode/`, `openspec/` | **0 matches** |
| Directories named `test` / `tests` / `spec` (excluding agent-skill folders) | **0** (`openspec/specs` contains only `.gitkeep`) |
| Test frameworks (`gtest`, `catch2`, `unittest`, `doctest`, CppUnit) referenced in `Code/**` | **0 matches** |
| `assert(` usage in `Code/**` | **0 matches** |
| Build projects | **2** — `Code/DBSrv/DBSrv.vcxproj`, `Code/TMSrv/TMSrv.vcxproj` (no test project in `W2PP Code Project.sln`) |
| Solution-level test configuration | None |
| CI configuration (`.github/`, `azure-pipelines.yml`, `Jenkinsfile`) | None |
| `openspec/specs/` | Empty (`.gitkeep` only) |

### 10.2 Coverage table

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|-----------|-------------------|----------|--------------|
| `Exec_MSG_CombineItemEhre` | 0 | 0 | 0% | None — no assertions, no fixtures, no mocks |
| `GetMatchCombineEhre` (recipe table) | 0 | 0 | 0% | None |
| `g_pEhreRate` + `ReadCompRate` (config path) | 0 | 0 | 0% | None |
| Recipe 5 progression repair (`:130-174`) | 0 | 0 | 0% | None — the highest-risk block is entirely unverified |
| Soul permutation table (`:320-348`) | 0 | 0 | 0% | None — 10 branches, 0 exercised |
| Failure/refund path (`:379-393`) | 0 | 0 | 0% | None |

### 10.3 Why the component is currently untestable

1. **No seams.** `Exec_MSG_CombineItemEhre` is a single 375-line function with no extracted helpers, no dependency injection, no interface. The only way to exercise a branch is to construct a full `pMob[conn]`/`pUser[conn]` world and a valid 84-byte packet.
2. **Global-state dependency.** `pMob`, `pUser`, `g_pEhreRate`, `g_pNextLevel_2`, `g_pItemList`, `g_pMessageStringTable`, `temp`, `LOCALSERVER` are all file-scope globals in other translation units. A harness would have to link the entire TMSrv.
3. **Non-deterministic outcome.** `rand()` has no injection point, so a test cannot pin the roll without controlling the CRT seed process-globally.
4. **Win32 coupling.** The build is a Win32 GUI application (`MessageBoxA`, `hWndMain`, WSA, `SetTimer`), so the surrounding subsystem does not run headless.
5. **No data fixtures.** `g_pItemList` is loaded from external runtime files that are not in the repository (`CReadFiles::ImportItem`), so the item ids used by the recipes (597/697/413/633/... ) cannot be resolved to names or attributes in a test environment without the game data.

### 10.4 Observable behavioural evidence in lieu of tests

The following invariants are statically provable and are the closest thing to a specification the codebase offers:

| Invariant | Proof |
|-----------|-------|
| Every rejection path sends either `_MSG_CombineComplete(0)` or nothing | `:40,50,57,64,78` (signal 0) vs `:33-34` (silent) |
| Exactly one item can be produced per attempt | Single `ipos`, single `SendItem` on the success path (`:375`) |
| Recipe 8 never produces an item | Early `return` at `:361` precedes `SendItem` at `:375` |
| Rates 1, 2, 6, 7, 8 are unconditionally successful when unmodified | `rate = 100` and `_rand <= 99` |
| Materials are always consumed on any path that reaches `:84` | Unconditional `memset` loop |
| The only refund is `Item[2]` in recipe 5 | Single `memcpy` back at `:383` |
| `g_pEhreRate` is indexed only by a value in `[1,8]` at that point | `combine == 0` is rejected at `:47`; resolver returns `0..8` |

### 10.5 Verification targets (observation only, no implementation)

If coverage were ever added, the branches with the highest consequence-to-effort ratio are: the level-0 sentinel (R-01), the `ipos == -1` read (R-02), the ten Soul permutations plus the unmatched-triple fallthrough (R-03/BR-29), the MP wrap at 100 (R-07), the duplicate gate clause (R-06), and the realised-vs-nominal rate table (R-10). Each is a small, pure computation once the surrounding state is stubbed.

---

## 11. Analysis Limitations

1. **Item identity is unresolved.** The repository contains no item database (`Common/Settings/`, `ImportItem` data files are absent), so ids `412`, `4148`, `697`, `413`, `633`, `3464`, `3338`, `4899`, `4190-4199`, `5110-5133`, `2441-2444` are documented by role and by the Portuguese inline comments (`Pacote Oriharucon`, `Pedra Misteriosa`, `Pedra espiritual`, `Pedra Amunrá`, `Refinação abençoada Purificada`, `Traje montaria`, `Restaurar Traje montaria`, `Soul`, `MP`, `HP`, `Critico`). Their names and grades are not verifiable from the source tree.
2. **Mount slot semantics are ambiguous.** `stEffect[2].cValue` is written by recipes 6/7 while the mount branch of `BASE_GetItemAbilityNosanc` (`Basedef.cpp:1570-1574`) reads that slot as `EF_MOUNTKILL`. Whether costume tier and kill count are intentionally the same field cannot be determined from this component; recorded as ambiguous (confidence: low that the overlap is intended).
3. **`extra.Soul` consumption is out of scope.** No TMSrv code reads the field, so the in-game effect of each `SOUL_*` value (and hence whether the order-sensitive mappings are semantically correct) cannot be established. Confidence in the mapping being intentional: low.
4. **`LOCALSERVER` deployment reality is unknown.** The literal `"192.168.0.50"` may or may not match a production configuration; the risk is conditional on deployment.
5. **`_rand` distribution claim assumes an unseeded/standard `rand()`.** `srand` is not called from this component; the CRT default seed (1) applies unless another unit seeds it, which would make the sequence deterministic across restarts. Not verified beyond this translation unit.
6. **The "recipe id" naming conflict** (`GetMatchCombineEhre` returns 1-8 but the variable is `rate`, and the real percentage is also `rate` at `:113`) is documented as observed; the intent to reuse the name is inferred.
7. **No runtime evidence.** All conclusions are from source reading; the BR-20 probability table is derived analytically from `rand()%115` + the fold, not measured.
8. **Business rules marked as such are behavioural inferences** from control flow. Where a rule's purpose is not stated in code or comments, it is labelled as inferred and its confidence is stated in the text.

---

## 12. Summary of Referenced Files

| File | Lines referenced | Role |
|------|------------------|------|
| `Code/TMSrv/_MSG_CombineItemEhre.cpp` | 1-395 (entire file) | The component under analysis |
| `Code/TMSrv/ProcessClientMessage.h` | 114 | Declaration |
| `Code/TMSrv/ProcessClientMessage.cpp` | 38-66, 246-248 | Dispatch, pre-dispatch guards |
| `Code/TMSrv/GetFunc.cpp` | 106-165 | `GetMatchCombineEhre` recipe table |
| `Code/TMSrv/GetFunc.h` | 26 | Prototype |
| `Code/TMSrv/CReadFiles.cpp` | 37, 66-75, 375-435 | `COMP_RATE_PATH`, constructor, EHRE config parsing |
| `Code/TMSrv/CReadFiles.h` | 49 | Path constant |
| `Code/TMSrv/SendFunc.cpp` | 27-45, 208-218, 825, 1055-1082, 1136, 1195-1224 | Outbound message primitives |
| `Code/TMSrv/Server.cpp` | 277, 380, 3584-3588, 3975-4001, 7319-7352, 8855-8894, 9264-9284 | `temp`, `LOCALSERVER`, address fingerprint, read pump, `RemoveTrade`, `SetCircletSubGod`, `ItemLog` |
| `Code/TMSrv/CMob.cpp` | 1034-1152 | `CheckGetLevel` |
| `Code/TMSrv/CMob.h` | 96, 113 | `MaxCarry`, `GetCurrentScore` |
| `Code/TMSrv/imple.cpp` | 250-262, 1628-1633 | `reloadfile`, admin `soul` command |
| `Code/TMSrv/ProcessDBMessage.cpp` | 751-757 | `MaxCarry` initialisation |
| `Code/TMSrv/_MSG_CombineItemAgatha.cpp` | 21-110 | Sibling handler (copy-paste evidence) |
| `Code/TMSrv/_MSG_UseItem.cpp` | 1603, 2959, 3111, 3125, 3440-3479 | `LOCALSERVER` use, celestial save/restore, alternative level recompute |
| `Code/Basedef.cpp` | 75-78, 656-679, 858-897, 1479-1495, 1497-1600, 2095-2141, 2239-2285, 2287 | Rate table, level table, `BASE_GetBonusScorePoint`, `BASE_GetItemCode`, `BASE_GetItemAbilityNosanc`, `BASE_GetItemSanc`, `BASE_SetItemSanc`, `BASE_GetItemAmount` |
| `Code/Basedef.h` | 76, 113, 117-118, 139, 210-232, 398-412, 448-450, 882-906, 925-950, 1943-1965, 2454-2455, 2484 | Constants, structs, message layout, externs |
| `Code/CPSock.cpp` | 353-467 | `ReadMessage` framing, checksum |
| `Code/CPSock.h` | 35-50 | Buffer limits, `HEADER` layout |
| `Code/ItemEffect.h` | 83, 101, 111-113 | `EF_SANC`, `EF_AMOUNT`, `EF_HPADD2`, `EF_MPADD2`, `EF_CRITICAL2` |
| `Code/Language.h` | 158, 269, 271 | `_NN_Processing_Complete`, `_NN_CombineFailed`, `_NN_Wrong_Combination` |

**Ignored per instructions:** `.agents`, `.codegraph`, `.opencode`, `openspec`.

---

*End of report.*
