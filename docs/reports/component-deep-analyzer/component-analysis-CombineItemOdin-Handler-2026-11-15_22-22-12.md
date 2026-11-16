# Component Deep Analysis Report

**Component:** CombineItemOdin Handler
**Primary unit:** `Code/TMSrv/_MSG_CombineItemOdin.cpp` (18,292 bytes, 580 lines)
**Secondary units analysed:** `Code/TMSrv/GetFunc.cpp` (`GetMatchCombineOdin`), `Code/Rates` configuration path (`Code/TMSrv/CReadFiles.cpp`), `Code/Basedef.cpp` (item/sanctification primitives)
**Project:** W2PP — decompiled C/C++ (Win32, MSVC) W2 MMORPG server emulator
**Analysis scope:** entire project root `E:\wyd\test-setup\w2pp`
**Excluded from analysis:** `.agents`, `.codegraph`, `.opencode`, `openspec`
**Report date:** 2026-11-15 22:22:12

---

## 1. Executive Summary

`Exec_MSG_CombineItemOdin` is a single, monolithic client-command handler that implements the "Odin" NPC crafting family for the world server (TMSrv). It is a request/response transaction: the client submits up to eight `(STRUCT_ITEM descriptor, inventory slot)` pairs, the handler revalidates every descriptor byte-for-byte against the server-side inventory, resolves the set against an eleven-recipe table (`GetMatchCombineOdin`), consumes **all** submitted ingredients, performs one shared pseudo-random success roll, and then applies one of three outcomes — a produced/reforged item, a downgraded item, or an item destroyed / converted to a fragment — replying to the client with a three-valued acknowledgement signal.

It is not a data-driven crafting engine. Recipes are hardcoded index tuples inside `GetMatchCombineOdin` (`Code/TMSrv/GetFunc.cpp:427-496`), while only the *success rates* are externalised to `../../Common/Settings/CompRate.txt` via the `ODIN` block (`Code/TMSrv/CReadFiles.cpp:485-567`) and to the compile-time defaults `g_pOdinRate[12]`, `g_pItemSancRate12[11]`, `g_pItemSancRate12Minus[4]` (`Code/Basedef.cpp:80-102`).

Architectural position, confirmed against `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:133`: role = "Command Handler (crafting)", afferent coupling 1, efferent coupling 14. Its only caller is the dispatcher (`Code/TMSrv/ProcessClientMessage.cpp:266-269`), which routes **two** distinct message types — `_MSG_CombineItemOdin` (722) and `_MSG_CombineItemOdin2` (738) — into the same function. The handler exports no API of its own; its entire contract is the binary packet layout plus the `_MSG_CombineComplete` signal.

Key findings, in descending severity:

1. **Unconditional destruction before outcome resolution.** All submitted ingredients are cleared from the inventory (`:92-99`) *before* the recipe switch is entered and *before* the `combine == 0` "no recipe matched" case is reached. An unmatched request therefore consumes every item and reports failure (`:571-579`). The sibling handler `Exec_MSG_CombineItemEhre` guards this explicitly (`Code/TMSrv/_MSG_CombineItemEhre.cpp:47-52`); the Odin handler does not.
2. **Duplicate-slot ingredient bypass.** The validation loop (`:24-43`) bounds-checks each `InvenPos` and `memcmp`s it against the inventory, but never checks that the eight slot indices are *distinct*. Because consumption is slot-based and idempotent, a single inventory slot referenced from several recipe positions satisfies a multi-item requirement while consuming one item. This is exploitable on every recipe that requires two or more copies of the same item index (recipes 2, 3, 4 and 11).
3. **`rand()%1 == 0` makes the "possible break" deterministic.** The refine-break branch (`:270-321`) is gated on `rand()%1 == 0`, which is always true for a non-negative integer. Whenever the trigger conditions hold, the item is destroyed (or converted to a fragment) with certainty, not with probability.
4. **Process-global scratch buffer abused with unbounded `strcat`.** The ingredient-dump loop appends item codes into `temp` (`:103-113`, `strcat` at `:111`) without resetting it first. `temp` is the single `char temp[4096]` shared by every handler and by `Log` (`Code/TMSrv/Server.cpp:277`). The audit line therefore concatenates onto arbitrary leftover content, and can overflow the 4 KB global.
5. **Cape refine accepts an empty equipment slot.** Recipe 11 refines `Equip[15]` but never checks that a mantle is present; with an empty slot the handler consumes seven ingredients, then stamps a sanctification effect onto an item with `sIndex == 0`.
6. **`LOCALSERVER` grants 100 % success** on every branch (`:124`, `:224`, `:357`, `:389`, `:416`, `:448`, `:480`, `:512`). The flag is set by a literal DB-address match (`Code/TMSrv/Server.cpp:3584-3588`).
7. **Zero automated tests.** No test project, test directory, test framework, assertion, CI pipeline, or build script exists anywhere in the repository (see Section 12 for the evidence trail).

Secondary but material: slot 7 of the packet is unconstrained by the recipe table yet is still consumed; the roll distribution is deliberately non-uniform (`rand()%115` with 100–114 folded onto 85–99); the multi-valued `REF_*` constants (10/12/15/18/22/27) are sentinels rather than levels; and the packet is never size-validated, so an undersized message is parsed from stale receive-buffer bytes.

---

## 2. Data Flow Analysis

```
1.  Client emits a binary TCP message: Type = 722 (_MSG_CombineItemOdin) or 738 (_MSG_CombineItemOdin2)
      payload = MSG_CombineItem { _MSG header (12 B), STRUCT_ITEM Item[8] (64 B), char InvenPos[8] }
      Code/Basedef.h:1948, :1951, :1956-1963
2.  CPSock framing: length/keysum validated, Size in [sizeof(HEADER), 8192]
      Code/CPSock.cpp:386-416, Code/CPSock.h:38
3.  ProcessClientMessage: ID bounds check, ServerDown gate, LastReceiveTime stamp, ClientTick gate,
      then switch(Type) -> case _MSG_CombineItemOdin / _MSG_CombineItemOdin2
      Code/TMSrv/ProcessClientMessage.cpp:38-66, :266-269
4.  Exec_MSG_CombineItemOdin(conn, pMsg): reinterpret payload as MSG_CombineItem
      _MSG_CombineItemOdin.cpp:22
5.  Phase A - descriptor revalidation (loop i = 0..7)
      5.1 skip empty descriptors (sIndex == 0)                                   :26-27
      5.2 read InvenPos[i] as char -> int                                       :29
      5.3 bounds check 0 <= invPos < pMob[conn].MaxCarry
          on failure: RemoveTrade(conn) then silent return                      :31-35
      5.4 memcmp(Carry[invPos], Item[i]) must be byte-identical
          on failure: ItemLog("err,msg_CombineOdin - item remove or changed."),
          SendClientSignalParm(conn, ESCENE_FIELD, _MSG_CombineComplete, 0), return
                                                                                :37-42
6.  Phase B - recipe resolution: GetMatchCombineOdin(m->Item) -> combine in 0..11
      _MSG_CombineItemOdin.cpp:45  ->  Code/TMSrv/GetFunc.cpp:427-496
7.  Phase C - pre-consumption eligibility guards (no state mutated yet)
      7.1 combine == 2: elemental-stone all-or-none parity                        :47-71
      7.2 combine == 2: reject target sanc >= REF_15 or target EF_MOBTYPE == 3    :73-77
      7.3 combine == 4: require Level == 39, Lv40 flag == 0, ClassMaster == CELESTIAL
                                                                                  :79-83
      7.4 combine == 11: reject MORTAL/ARCH class master, sanc >= 9               :85-89
8.  Phase D - unconditional ingredient consumption (loop i = 0..7)
      memset(Carry[InvenPos[i]], 0) + SendItem(ITEM_PLACE_CARRY, InvenPos[i])     :92-99
9.  Phase E - audit journal (3 writes)
      "*** Item combine odin. ***" / BASE_GetItemCode dump via strcat(temp, ...) /
      "*** ------------------- ***"                                              :101-115
10. Phase F - single shared roll
      _rand = rand() % 115; if (_rand >= 100) _rand -= 15;                        :117-119
11. Phase G - outcome dispatch (combine)
      1  celestial item synthesis          -> result in InvenPos[1]               :122-159
      2  refinement (+12 family)           -> result/downgrade/shard in InvenPos[2] :161-353
      3  rune track                        -> slot InvenPos[0].sIndex = 5134      :355-385
      4  Lv40 unlock                       -> flag only, no item                  :387-412
      5  fury stone                        -> slot InvenPos[0].sIndex = 3020      :414-444
      6..9 elemental stones                -> slot InvenPos[0].sIndex = 5334+(n-6) :446-476
      10 crystal seed                      -> slot InvenPos[0].sIndex = 4032      :478-508
      11 celestial cape refine             -> Equip[15] sanctification +1         :510-569
      0 / other                            -> failure path (items already gone)   :571-579
12. Phase H - notification: SendClientSignalParm(ESCENE_FIELD, _MSG_CombineComplete, 0|1|2),
      SendClientMessage / SendNotice, SendItem, ItemLog
13. Persistence: none. Mutations live in pMob[conn] memory only; the character is written to
      the DB later by the normal save path (no DBServerSocket traffic in this handler).
```

Three properties of this flow matter more than the step list:

* **Phases C and D are inverted relative to safety.** Every guard that can *reject* a request without side effects (7.1-7.4) runs before consumption, but the "recipe not found" condition is *not* one of them — it is only detected after consumption, in the terminal `else` of Phase G.
* **One roll serves all recipes** (Phase F). The roll is materialised before the dispatch, so `combine == 4` (a pure flag purchase) and `combine == 11` (an equipment mutation) draw from the same distribution as item production.
* **The result slot is client-chosen.** Phases G.1/G.2 write into `InvenPos[1]`/`InvenPos[2]`; G.3/G.5/G.6-9/G.10 write into `InvenPos[0]`. Because those slots were just cleared in Phase D, the write target is always empty — but its identity is entirely under client control.

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | Validation | Every non-empty client item descriptor must be byte-identical to the server inventory slot at the supplied position, otherwise abort with signal 0 | `_MSG_CombineItemOdin.cpp:24-43` |
| BR-02 | Validation | `InvenPos` must be in `[0, pMob[conn].MaxCarry)`, otherwise the trade session is reset and the function returns silently | `:31-35` |
| BR-03 | Validation (absent) | Slot indices are not required to be unique; consumption is slot-based and idempotent | `:24-43`, `:92-99` |
| BR-04 | Business Logic | Recipe resolution is an ordered first-match chain over 11 hardcoded item-index tuples; no match yields 0 | `GetFunc.cpp:427-496` |
| BR-05 | Validation | Any descriptor equal to item 747 disqualifies the whole set (returns 0) | `GetFunc.cpp:429-433` |
| BR-06 | Validation | Item indices must be in `[0, MAX_ITEMLIST)` for all 8 slots, including the unused slot 7 | `GetFunc.cpp:435-457` |
| BR-07 | Validation | For the refine recipe: if any of 5334-5337 is present in slots 3-6, all four must be present | `_MSG_CombineItemOdin.cpp:47-71` |
| BR-08 | Validation | Refine target must not be `EF_MOBTYPE == 3` (celestial-grade) and must have sanc < `REF_15` | `:73-77` |
| BR-09 | Business Logic | Refine target level window is `REF_11..REF_14` (levels 11-14); the post-consumption re-test of the same window is unreachable | `:206-210`, `GetFunc.cpp:463-466` |
| BR-10 | Validation | Lv40 unlock requires `Level == 39` exactly, `QuestInfo.Celestial.Lv40 == 0`, `ClassMaster == CELESTIAL` | `:79-83` |
| BR-11 | Validation | Cape refine requires `ClassMaster` not MORTAL/ARCH and cape sanc < 9; the presence of a cape is never verified | `:85-89`, `:514-543` |
| BR-12 | Configuration | Success rates come from `CompRate.txt` `ODIN <KEY> <0..100>` lines, defaulting to compile-time tables | `CReadFiles.cpp:485-567`, `Basedef.cpp:80-102` |
| BR-13 | Business Logic | One shared roll: `rand()%115`, with results 100-114 folded to 85-99 (non-uniform distribution) | `_MSG_CombineItemOdin.cpp:117-119` |
| BR-14 | Business Logic | Success iff `_rand <= rate` (recipes 1, 3-11) or `rate >= _rand` (recipe 2); `LOCALSERVER` forces success | `:124`, `:224`, `:357-512` |
| BR-15 | Business Logic | Refine success chance = 1 + stone bonuses - level penalty, clamped implicitly by the roll domain | `:163-222` |
| BR-16 | Business Logic | Refine success: sanc +1 (11→12, 12→13, 13→14, 14→15), gem index preserved | `:224-267` |
| BR-17 | Business Logic | Refine failure: sanc -1, except `EF_MOBTYPE == 1` items at rate <= 9 which are destroyed or converted | `:268-351` |
| BR-18 | Business Logic | Break is deterministic (`rand()%1`), shields (`nPos == 128`) are exempt, two protection items (4043) convert the loss into a fragment | `:270-321` |
| BR-19 | Business Logic | Fragment mapping: 3021 + offset for `nPos` 4/8/16/32, otherwise 3026 with effect value 11 | `:285-306` |
| BR-20 | Business Logic | All submitted non-empty descriptors are consumed unconditionally — before the outcome is known | `:92-99` |
| BR-21 | Business Logic | An unmatched set (combine == 0) consumes everything and reports failure | `:571-579` |
| BR-22 | Business Logic | Celestial item synthesis: result id = `Extra` of `Item[0]`'s template, effects taken from `Item[1]`, sanc/gem zeroed | `:122-159` |
| BR-23 | Business Logic | Fixed-output recipes write a new `sIndex` into `InvenPos[0]` (5134, 3020, 5334-5337, 4032) | `:355-508` |
| BR-24 | Business Logic | Cape refine: allocates `EF_SANC` (43) into the first free effect slot, resets gem, caps at +9 | `:510-569` |
| BR-25 | Notification | Three-valued `_MSG_CombineComplete` contract: 0 = request rejected/no-op, 1 = success, 2 = failure after consumption | `:40`, `:68`, `:75`, `:81`, `:87`, `:135`, `:154`, ... |
| BR-26 | Notification | Success is announced server-wide via `SendNotice` for recipes 1 and 2 only | `:133`, `:264` |
| BR-27 | Audit | Every attempt writes a 3-part item-log entry; successes and failures add a dedicated line | `:39`, `:101-115`, `:141`, `:155`, `:261`, `:348`, ... |
| BR-28 | Security | No packet-size validation and no `AddCrackError` escalation on malformed input | `:22`, dispatcher `ProcessClientMessage.cpp:38-66` |
| BR-29 | Design limit | Slot 7 is unconstrained by every recipe yet is consumed and can contribute rate in recipe 2 | `GetFunc.cpp:456-457`, `_MSG_CombineItemOdin.cpp:167-204` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Server-authoritative revalidation of the eight item descriptors

**Overview:**
Before a crafting transaction is allowed to touch state, each of the eight item descriptors supplied by the client must be proven to exist, unchanged, in the exact inventory slot the client claims.

**Detailed description:**

The handler treats the inbound `MSG_CombineItem` as an untrusted assertion. For every index `i` where `m->Item[i].sIndex != 0`, it re-reads `pMob[conn].MOB.Carry[m->InvenPos[i]]` from its own memory and compares the whole eight-byte `STRUCT_ITEM` with `memcmp` (`:37`). `STRUCT_ITEM` is `{short sIndex; union {short sValue; struct {unsigned char cEffect; unsigned char cValue;};} stEffect[3];}` (`Code/Basedef.h:398-412`), so the comparison covers the item id and all three effect/effect-value pairs. A single differing byte aborts the entire request: an `ItemLog` entry `"err,msg_CombineOdin - item remove or changed."` is written with the account name and IP, and the client receives `_MSG_CombineComplete` with parameter 0 (`:39-41`).

This rule is the component's primary anti-duplication and anti-desync mechanism. Its practical effect is that the client cannot name an item it does not own, cannot substitute a different refinement or effect payload for an owned item, and cannot craft from a stale inventory snapshot — the classic "I moved the item to another bag while the craft window was open" case results in a clean rejection rather than silent consumption of the wrong item. Note the deliberate distinction between the two rejection paths: a bounds failure (BR-02) returns *silently*, whereas a content mismatch is logged and answered with signal 0. Only the latter is a protocol-visible rejection.

The rule's weakness is scope. It validates the *descriptor ↔ slot* binding, not the *slot set*. It also only looks at `Carry`, so an item that the server has already equipped or moved to cargo cannot be used as an ingredient — which is correct — but it also means a client that submits an `InvenPos` beyond `MaxCarry` (but below `MAX_CARRY = 64`) is rejected even though the physical slot exists, an intentional strictness shared with the rest of the combine family.

**Rule workflow:**
```
for i in 0..7:
    if Item[i].sIndex == 0: continue
    pos = (int)InvenPos[i]
    if pos < 0 or pos >= pMob[conn].MaxCarry:          -> BR-02 (silent + RemoveTrade)
    if memcmp(&Carry[pos], &Item[i], sizeof(STRUCT_ITEM)) != 0:
         ItemLog("err,msg_CombineOdin - item remove or changed.", account, ip)
         SendClientSignalParm(conn, ESCENE_FIELD, _MSG_CombineComplete, 0)
         return
```

---

### Business Rule: BR-02 — Inventory slot bounds validation with trade-session invalidation

**Overview:**
`InvenPos` values are validated against the player's current carry capacity; an out-of-range position is treated as evidence of a corrupt or hostile client and additionally tears down any open trade session.

**Detailed description:**

`m->InvenPos` is declared `char InvenPos[MAX_COMBINE]` (`Code/Basedef.h:1962`), i.e. eight *signed* bytes on MSVC. The handler widens each byte to `int` (`:29`) and accepts it only if `0 <= invPos < pMob[conn].MaxCarry` (`:31`). Because `char` is signed, a client-supplied byte of 200 becomes -56 and is rejected by the lower bound; a byte of 60 is accepted only if the player's `MaxCarry` is greater than 60. `MaxCarry` is a dynamic per-character value initialised to 30 and raised to 45/60 by the bag items in slots 60/61 (`Code/TMSrv/ProcessDBMessage.cpp:751-757`, `Code/TMSrv/_MSG_UseItem.cpp:5688-5694`), always below the physical `MAX_CARRY = 64` (`Code/Basedef.h:76`). The check is therefore strictly stronger than a raw array bound, and there is no path from this handler to an out-of-bounds `Carry` index.

The failure action is unusual in two respects. First, it calls `RemoveTrade(conn)` (`Code/TMSrv/Server.cpp:7319-7337`), which zeroes `pUser[conn].Trade` and `AutoTrade`, resets the trade slot map to -1, and — if the user is in `USER_PLAY` and has `TradeMode` set — re-broadcasts the player's appearance via `GridMulticast`. Second, it returns *without* emitting `_MSG_CombineComplete` and without an item-log entry (`:33-34`). The client therefore receives no acknowledgement for this class of failure and must rely on a timeout or on the trade-window teardown signal (900) that `RemoveTrade` sends.

The asymmetry is defensible as "this cannot happen with a legitimate client", but it produces two observable consequences: (a) the actor's UI can remain in a busy/waiting state after a malformed request, since no completion signal arrives; and (b) no audit trail exists for the most obviously hostile input the handler can receive — the only invalid-position attempts recorded are those that later cause a `memcmp` mismatch. A silent path also means the operator cannot distinguish "client packet corruption" from "packet forgery" in the logs.

**Rule workflow:**
```
pos = (int)m->InvenPos[i]
if pos < 0 or pos >= pMob[conn].MaxCarry:
    RemoveTrade(conn)        // clears Trade/AutoTrade, resets slot map, resyncs appearance
    return                   // no _MSG_CombineComplete, no ItemLog
```

---

### Business Rule: BR-03 — Ingredient uniqueness is not enforced (slot-based consumption)

**Overview:**
The eight recipe positions are addresses, not identities. Nothing prevents two or more positions from pointing at the same inventory slot, and consumption is idempotent per slot.

**Detailed description:**

The validation loop of BR-01 compares each descriptor against `Carry[m->InvenPos[i]]`. When two positions carry the *same* slot index and the *same* descriptor, both comparisons succeed — the loop has no memory of positions it has already seen. The consumption loop (`:92-99`) then `memset`s and re-sends `Carry[m->InvenPos[i]]` for each non-empty descriptor; clearing an already-cleared slot is a no-op. The net effect is that a recipe demanding N copies of an item can be satisfied by one inventory slot referenced N times, with exactly one unit consumed.

This is exploitable on every recipe that requires a repeated item id:

* **Recipe 2 (refine).** `GetMatchCombineOdin` requires `Item[0]` and `Item[1]` to be two 413 stacks of at least 10 units each (`GetFunc.cpp:463`), or both 4043. Four slots (3-6) accept 5334-5337 *or* 3338. A player can therefore submit `InvenPos[3..6]` all pointing at one +9 3338 stone: the parity rule (BR-07) only inspects elemental stones, so four identical 3338 references pass, each adds `g_pItemSancRate12[10] = 15` to the success rate (`:202-203`), and only one stone is destroyed.
* **Recipe 3 (rune track).** The matcher requires seven 413 items (`GetFunc.cpp:468`); a single 413 stack of >= 10 referenced from all seven positions passes and yields item 5134.
* **Recipe 4 (Lv40 unlock).** Two 4127 items (`GetFunc.cpp:471`) can be one reference.
* **Recipe 11 (cape).** Two 4127 plus four 413 (`GetFunc.cpp:492`) can be three references.

Recipes 1 and 5-10 require mutually distinct item ids and are therefore not exploitable this way (an attempt collapses recipe 1's `Item[0] == Extra(Item[1])` identity, and recipes 5-10 demand seven different indices).

The rule has no compensating control elsewhere: the dispatcher does not inspect `MSG_CombineItem`, and no server-side inventory reconciliation runs before the handler. The practical impact scales with the value of the duplicated ingredient — the +9 booster stone in recipe 2 and the rune-track material in recipe 3 are the economically significant cases. Recipes 1, 5-10 remain the only economically sound members of the family from this perspective.

**Rule workflow:**
```
Positions P[i] -> Carry slot S.  No P[i] uniqueness test exists.
If P[a] == P[b] == S and Item[a] == Item[b]:
    BR-01 passes for both
    Consume(S) executes twice -> second is a no-op
=> N-unit requirement satisfied with 1 unit
```

---

### Business Rule: BR-04 — Ordered first-match recipe table

**Overview:**
The recipe catalogue is an `if / else if` chain of eleven exact index tuples evaluated in a fixed order, returning a small integer code that the handler switches on.

**Detailed description:**

`GetMatchCombineOdin(STRUCT_ITEM *Item)` (`Code/TMSrv/GetFunc.cpp:427-496`) is a pure function of the packet's item array (it never touches `pMob`), returning 0 for "no recipe" and 1-11 for the recipes below. Ordering matters: recipe 1 is tested before 2, and both before the fixed-output recipes, so a set that happens to satisfy several patterns resolves to the earliest. All patterns are exact `sIndex` equalities except recipe 2, which combines a family test (413/4043) with a `nPos` class test on the target.

| Code | Comment (source) | Slot requirements (exact) | Notes |
|------|------------------|---------------------------|-------|
| 1 | `//Item celestial` | `Item[0].sIndex == Extra(Item[1])`, `sanc(Item[0]) >= 9`, `sanc(Item[1]) == REF_15` (27), `Item[2] ∈ {542, 772}`, `Item[3..6] == 5334, 5335, 5336, 5337` | `GetFunc.cpp:459-461` |
| 2 | `//Ref+12+` | (`Item[0] == 413 && amount >= 10` && `Item[1] == 413 && amount >= 10`) OR (`Item[0] == Item[1] == 4043`); `REF_10 < sanc(Item[2]) <= REF_15`; `Item[3..6] ∈ {5334..5337, 3338}`; `nPos(Item[2]) ∈ {2,4,8,16,32,64,192,128}` | `GetFunc.cpp:463-466` |
| 3 | `//Pista de runas` | 7 × 413 | `GetFunc.cpp:468-469` |
| 4 | `//Level40 Celestial` | 4127, 4127, 5135, 5113, 5129, 5112, 5110 | `GetFunc.cpp:471-472` |
| 5 | `//Pedra da fúria` | 5125, 5115, 5111, 5112, 5120, 5128, 5119 | `GetFunc.cpp:474-475` |
| 6 | `//Secreta da Agua` | 5126, 5127, 5121, 5114, 5125, 5111, 5118 | `GetFunc.cpp:477-478` |
| 7 | `//Secreta da Terra` | 5131, 5113, 5115, 5116, 5125, 5112, 5114 | `GetFunc.cpp:480-481` |
| 8 | `//Secreta do Sol` | 5110, 5124, 5117, 5129, 5114, 5125, 5128 | `GetFunc.cpp:483-484` |
| 9 | `//Secreta do Vento` | 5122, 5119, 5132, 5120, 5130, 5133, 5123 | `GetFunc.cpp:486-487` |
| 10 | `//Semente de cristal` | 421, 422, 423, 424, 425, 426, 427 | `GetFunc.cpp:489-490` |
| 11 | `//Capa` | 4127, 4127, 5135, 413, 413, 413, 413 | `GetFunc.cpp:492-493` |

Two structural consequences follow. First, the table is not extensible without a recompile, and the eleven tuples overlap in material identity (4127 appears as a catalyst in recipes 4 and 11; 413 appears in 2, 3 and 11; 5110/5111/5112/5113/5114/5115/5119/5120/5125/5128/5129 are shared by several elemental recipes), so the ordering in the chain is load-bearing. Second, because every tuple stops at `Item[6]` and only range-checks `Item[7]`, the eighth position is a free rider on every recipe (BR-29).

**Rule workflow:**
```
if any Item[i].sIndex == 747                     -> 0        (BR-05)
if any Item[i].sIndex outside [0, MAX_ITEMLIST)  -> 0        (BR-06)
if recipe1(Item) -> 1
else if recipe2(Item) -> 2
... else if recipe11(Item) -> 11
else -> 0
```

---

### Business Rule: BR-05 — Item 747 disqualifies the entire request

**Overview:**
If any of the eight descriptors is item 747, the matcher returns 0 regardless of how well the set otherwise matches a recipe.

**Detailed description:**

The very first loop of `GetMatchCombineOdin` (`GetFunc.cpp:429-433`) scans all eight slots for `sIndex == 747` and short-circuits to 0. Contextual evidence identifies 747 as the crown item: `Code/TMSrv/Server.cpp:646` and `:659` exclude 747 from the equipment/carry loops that push character items to the client, with the inline comment `// 747 = Crown on itemlist`, and `Code/Basedef.cpp:4365` singles out `item->sIndex != 747` as the only legal non-ring/×3500 weapon for non-mortal classes. The check exists so that a crown — presumably a non-tradable, non-consumable quest or royalty item — can never be fed into a crafting transaction.

The placement of the check is significant. It runs before the index-range validation and before any recipe test, so it is airtight: no ordering of slots, no combination of other ingredients, and no duplicate-slot trick can smuggle a 747 into a recipe. However, its *consequence* interacts badly with BR-21: returning 0 sends the request to the terminal `else` at `:571-579`, which occurs **after** the consumption loop. A player who accidentally includes a crown therefore has the crown (and every other submitted ingredient) destroyed and receives only the generic failure message.

There is no early-exit for `combine == 0` between the matcher call (`:45`) and the consumption loop (`:92`), so the intended protection ("crowns cannot be crafted") is implemented as a protection that costs the player the crown. The rule is correct in intent, incorrect in placement.

**Rule workflow:**
```
for i in 0..7:
    if Item[i].sIndex == 747: return 0
... continue matching
handler: combine == 0 -> (items already consumed) -> failure
```

---

### Business Rule: BR-06 — Global item-index range validation for all eight slots

**Overview:**
Every descriptor must reference a legal item id, including the position that no recipe reads.

**Detailed description:**

`GetMatchCombineOdin` rejects the set if any `Item[0..7].sIndex` is negative or `>= MAX_ITEMLIST` (6500; `Code/Basedef.h:139`), with one explicit test per slot (`GetFunc.cpp:435-457`). Because `sIndex` is a `short`, client-supplied values above 32767 wrap negative and are caught by the lower bound. The check protects the subsequent `g_pItemList[Item[i].sIndex]` dereferences — the matcher reads `Extra` of `Item[1]` (`:459`), `nPos` of `Item[2]` (`:465`), and `Extra`/`nPos`/`Grade` indirectly through the handler — from indexing outside the 6500-entry template array (`Code/Basedef.cpp:753`).

The per-slot repetition is verbose but complete: there is no slot that escapes the check, and no arithmetic is performed on `sIndex` before validation. Note the dependence on BR-01 for the *semantic* guarantee: range validity alone would be satisfiable by any item id, and it is the `memcmp` that ties the id to real ownership.

The rule also silently does the work that the handler's "not null" tests do not: because a zero index is the canonical empty marker and 0 is a legal array index, an all-zero packet passes BR-06, matches nothing, and falls through to the destructive failure path. The check is therefore a necessary but insufficient guard, and its insufficiency is exactly BR-21.

**Rule workflow:**
```
for i in 0..7:
    if Item[i].sIndex < 0 or Item[i].sIndex >= MAX_ITEMLIST(6500): return 0
```

---

### Business Rule: BR-07 — Elemental-stone all-or-none parity in the refine recipe

**Overview:**
For refinement, the four ingredient slots may contain either four *distinct* elemental stones (5334 water, 5335 earth, 5336 sun, 5337 wind) or booster stones (3338) — never a partial set of elemental stones.

**Detailed description:**

After the matcher returns 2, the handler builds four boolean flags by scanning slots 3-6 for each elemental id (`:49-64`), then applies a rejection expression that fires when any flag is set and some other flag is clear (`:66`). The expression is written as four symmetric disjuncts, one per element, each of the form `sX && (sY == 0 || sZ == 0 || sW == 0)`. The result is a clean rule: the variable part of the recipe is *either* zero elemental stones *or* exactly one of each. When all four are present, the four slots are fully occupied, so no booster stone can accompany them; when none are present, all four slots hold booster stones.

The rule exists because the two ingredient kinds are not interchangeable in value: each elemental stone contributes a flat `g_pItemSancRate12[0]` (default 1) to the success rate (`:169-170`), whereas a booster stone contributes 2-15 depending on its own refinement level (`:175-203`). Allowing a 3-elemental + 1-booster mix would produce a rate that is neither the cheap deterministic recipe nor the expensive one, and — more importantly — would let a player pair a rare single elemental with three cheap boosters. The all-or-none structure makes the two recipes economically distinct.

A genuine defect hides in the same block: the flags are computed from `m->Item[3..6]` only, so an elemental stone placed in slot 7 is invisible to the parity check, contributes its +1 to the rate, and is consumed. A player can therefore run the "four booster" variant with a fifth stone reporting as an unrecognised extra, or run "four elemental" plus a stray fifth — the exact scenario the rule was written to prevent, reachable through the one slot the rule does not read.

**Rule workflow:**
```
sAgua  = any(Item[3..6].sIndex == 5334)
sTerra = any(Item[3..6].sIndex == 5335)
sSol   = any(Item[3..6].sIndex == 5336)
sVento = any(Item[3..6].sIndex == 5337)
if any flag set AND any flag clear:
    SendClientSignalParm(..., _MSG_CombineComplete, 0); return   // items NOT consumed
```

---

### Business Rule: BR-08 — Celestial-grade equipment exclusion and sanc ceiling for refinement

**Overview:**
A refine target that is celestial-grade (`EF_MOBTYPE == 3`) or already at `REF_15` cannot enter the refinement recipe, and the rejection is non-destructive.

**Detailed description:**

Immediately after the parity rule, a single compound guard rejects `combine == 2` when the target's sanctification is `>= REF_15` (sanc 15) or when `BASE_GetItemAbility(&Item[2], EF_MOBTYPE) == 3` (`:73`). `EF_MOBTYPE` is effect id 112 (`Code/ItemEffect.h:149`) and the class constants are `ARCH = 1`, `MORTAL = 2`, `CELESTIAL = 3` (`Code/Basedef.h:178-180`); `BASE_GetItemAbility` resolves the effect from the static template plus the instance's effect triple (`Code/Basedef.cpp:1647-1716`). A celestial-grade item is bound to the celestial class (`Code/Basedef.cpp:4362`), so excluding it from this path reserves it for the celestial-specific upgrade route (recipes 1 and 11).

Both conditions are evaluated *before* the consumption loop, so this is one of the four genuinely safe rejections in the handler: the player keeps all ingredients and receives signal 0. The upper bound of 15 is also redundant with the matcher, which already requires `sanc <= REF_15`; here it closes the equality case.

The rule is asymmetric in an informative way. It blocks celestial-grade *targets* but permits celestial-grade *fodder*: nothing prevents a celestial item from occupying slots 0/1 (they must be 413 or 4043), 3-6 (must be an elemental or booster stone) or 7 (unconstrained). Slot 7 is therefore a legal disposal path for any celestial item, which, combined with BR-20, means a high-value celestial item placed there is silently destroyed — the same failure mode as the crown rule (BR-05) but without even a disqualification.

**Rule workflow:**
```
if combine == 2 and (sanc(Item[2]) >= REF_15(27) or EF_MOBTYPE(Item[2]) == 3):
    SendClientSignalParm(..., _MSG_CombineComplete, 0); return   // nothing consumed
```

---

### Business Rule: BR-09 — Refinement target level window and its unreachable re-test

**Overview:**
Only levels 11-14 (encoded `REF_11`..`REF_14`) can be refined, and the handler re-tests this window *after* consuming the ingredients with a condition that can never be true.

**Detailed description:**

The matcher admits `REF_10 < sanc <= REF_15` (`GetFunc.cpp:465`), and BR-08 removes `REF_15`, leaving `sanc ∈ {REF_11, REF_12, REF_13, REF_14}`. Those four symbolic values are not refinement levels: `REF_10 = 10`, `REF_11 = 12`, `REF_12 = 15`, `REF_13 = 18`, `REF_14 = 22`, `REF_15 = 27` (`Code/Basedef.h:196-201`), and the encoding behind them is `230..253` split into four-gem bands per level (`Code/Basedef.cpp:2123-2138`: 230-233 → `REF_10`, 234-237 → `REF_11`, ... 250-253 → `REF_15`). So `REF_11` *means* level 11, `REF_12` means level 12, and so on; the numeric magnitude is a sentinel, not a level.

Given that, the guard at `:206-210` — `if (sanc <= REF_10 || sanc >= REF_15) { signal 0; return; }` — is dead code. Its lower arm cannot fire because the matcher demands `> REF_10`; its upper arm cannot fire because BR-08 already rejected `>= REF_15`. The only way it could execute is a mismatch between the matcher's view and the handler's view of the same array, which BR-01 excludes (the array is the client's copy and is not mutated between the two calls). Its location is nonetheless a latent hazard: it sits *after* the consumption loop (`:92-99`), so any future widening of the matcher's window would silently convert it into a "consume everything and reject" path.

The real business rule is the four-step ladder: level 11 → 12, 12 → 13, 13 → 14, 14 → 15. Level 10 and below are handled by a different subsystem (`BASE_GetSuccessRate`, `Code/Basedef.cpp:2202-2224`, and the refine items in `_MSG_UseItem.cpp`), and level 15 is terminal for this handler.

**Rule workflow:**
```
matcher:  REF_10 < sanc(Item[2]) <= REF_15
handler:  sanc(Item[2]) < REF_15          (BR-08, pre-consumption)
=> admissible: REF_11, REF_12, REF_13, REF_14   (levels 11..14)
:206-210  re-test of the same window      (unreachable)
:212-222  level-specific rate penalty
```

---

### Business Rule: BR-10 — Lv40 unlock eligibility (recipe 4)

**Overview:**
The celestial level-40 unlock is purchasable exactly once, only at character level 39, only by a celestial-class character, and only with seven specific materials.

**Detailed description:**

The recipe consumes `4127, 4127, 5135, 5113, 5129, 5112, 5110` (`GetFunc.cpp:471`) and produces no item at all — its output is the boolean `pMob[conn].extra.QuestInfo.Celestial.Lv40 = 1` (`:399`), written only on a successful roll. The field lives in the nested `QuestInfo.Celestial` structure (`Code/Basedef.h:519-537`, `Lv40` at `:525`), the celestial quest-progress block.

Eligibility is enforced by one guard (`:79-83`) with three conjunctive conditions: `MOB.CurrentScore.Level != 39` (must be *exactly* 39, not "at least"), `QuestInfo.Celestial.Lv40 == 1` (already unlocked), and `extra.ClassMaster != CELESTIAL` (3). The exact-level form is deliberate: the unlock is positioned as the final act of the level-39 grind, so a character that has already reached 40 cannot buy it retroactively — and a character that dies and de-levels to 39 with the flag already set cannot re-buy. All three failures return signal 0 *before* consumption, so they are non-destructive.

Two behavioural asymmetries deserve note. First, this is the only recipe whose failure is unrecoverable in a different sense from item loss: the seven ingredients are consumed and the flag stays 0, so a failed attempt is a pure loss with a 9.57 % default success rate (BR-14) — by far the worst expected-value proposition in the handler. Second, the success log is the only one that records the character name rather than the account name, using `sprintf(temp, "destrave,mobname:%s lv:40", pMob[conn].MOB.MobName)` (`:395-397`) and no item code, which is consistent with the absence of a produced item.

**Rule workflow:**
```
if combine == 4 and (Level != 39 or Lv40 == 1 or ClassMaster != CELESTIAL):
    signal 0; return                                  // nothing consumed
[consume 7 ingredients]
if roll <= g_pOdinRate[4]:
    Lv40 = 1; ItemLog("destrave,mobname:..."); signal 1
else:
    ItemLog fail; signal 2                            // ingredients lost
```

---

### Business Rule: BR-11 — Celestial cape refine eligibility

**Overview:**
Only celestial-class characters may refine the mantle in equipment slot 15, the mantle must be below +9, and — as implemented — the mantle is never required to exist.

**Detailed description:**

Recipe 11 (`4127, 4127, 5135, 413, 413, 413, 413`) targets `pMob[conn].MOB.Equip[15]`, the mantle/cloak slot. Context confirms the slot semantics: `Code/TMSrv/ProcessDBMessage.cpp:918-961` remaps `Equip[15]` to the guild-clan mantle variants, `Code/Basedef.cpp:3181` reads it as `int mantle`, and `Code/TMSrv/_MSG_UseItem.cpp:3223-3237` writes clan cloaks there.

The guard (`:85-89`) requires `ClassMaster` to be neither `MORTAL` (2) nor `ARCH` (1), and the mantle sanc to be `< 9`. The first test is written as a double negative rather than as `!= CELESTIAL`, which means `ClassMaster == 0` — the default value for a freshly zeroed `STRUCT_MOBEXTRA` (`Code/Basedef.h:815`) — also passes. The second test caps the ladder at +9, matching the pre-check that also appears in the item-based refine paths (`Code/TMSrv/_MSG_UseItem.cpp:4286`, `:4416`, `:4552`, `:4693`).

The missing condition is the important one: there is no `Equip[15].sIndex != 0` test. For a character with an empty mantle slot, `BASE_GetItemSanc` returns 0 and all three effect triples are zero, so the "sanc == 0" allocation branch at `:516-541` takes its `else` arm and writes `stEffect[0].cEffect = 43` (`EF_SANC`, `Code/ItemEffect.h:83`) with value 0 into an item whose `sIndex` is 0. `BASE_SetItemSanc(..., 1, 0)` then sets that value to 1 and the handler pushes the slot with `SendItem(conn, ITEM_PLACE_EQUIP, 15, ...)` (`:556`). Seven ingredients have been consumed to produce an invisible sanctification effect on a non-existent item — a persistent, if benign-looking, corruption of the equipment slot that survives until the slot is overwritten.

**Rule workflow:**
```
if combine == 11 and (ClassMaster == MORTAL or ClassMaster == ARCH or sanc(Equip[15]) >= 9):
    signal 0; return
[consume 7 ingredients]
if roll <= g_pOdinRate[11]:
    sanc = sanc(Equip[15]);  if sanc == 0: allocate EF_SANC slot
    BASE_SetItemSanc(Equip[15], sanc + 1, 0)
    SendItem(ITEM_PLACE_EQUIP, 15)
```

---

### Business Rule: BR-12 — Externalised success-rate configuration

**Overview:**
Eleven rate knobs and fifteen refine parameters are loaded from `../../Common/Settings/CompRate.txt` under the `ODIN` namespace, validated to `[0, 100]`, with compile-time defaults baked into `Basedef.cpp`.

**Detailed description:**

`CReadFiles::ReadCompRate` (`Code/TMSrv/CReadFiles.cpp:375-590`) opens the file resolved relative to the TMSrv working directory (`:37`), parses three whitespace-separated tokens per line, and dispatches on `str1 == "ODIN"` (`:485`). Inside that block, `str2` selects the knob and `val3` is validated to `0..100` with a modal error dialog and `continue` on violation (`:487-491`). The mapping is: `ITEM_CELESTIAL → g_pOdinRate[1]`; `ITEM_12_SECRETA → g_pItemSancRate12[0]`; `ITEM_12_REF_0..9 → g_pItemSancRate12[1..10]`; `ITEM_12_MINUS_12..15 → g_pItemSancRate12Minus[0..3]`; `PISTA → g_pOdinRate[3]`; `DESTRAVE_LV40 → g_pOdinRate[4]`; `PEDRA_DA_FURIA → g_pOdinRate[5]`; `SECRETA_AGUA/TERRA/SOL/VENTO → g_pOdinRate[6..9]`; `SEMENTE_CRISTAL → g_pOdinRate[10]`; `CAPA_CELESTIAL → g_pOdinRate[11]` (`:493-566`).

The compile-time defaults are `g_pOdinRate = {0, 35, 100, 40, 10, 10, 100, 100, 100, 100, 100, 100}` (`Code/Basedef.cpp:80-83`), `g_pItemSancRate12 = {1,2,3,4,5,6,7,8,9,12,15}` (`:97-101`) and `g_pItemSancRate12Minus = {2,3,4,5}` (`:102`). Index 0 of `g_pOdinRate` is never read, and index 2 is also never read because recipe 2 computes its rate instead — both are vestigial slots in a 12-element array that indexes 1..11 meaningfully. Loading happens from the constructor invoked at boot (`Code/TMSrv/Server.cpp:3627`) and can be repeated at runtime through the admin `reloadfile` command (`Code/TMSrv/imple.cpp:250-262`), which re-runs `ReadCompRate` after re-reading the base definitions and the language table.

The design gives operators live tuning of the entire economy without a restart, but it also means the "value" of every recipe is environment-dependent and unrecoverable from the source alone; and because the file is opened with `"r"` and failures only raise a modal message box, a malformed key silently leaves the compile-time default in place while a malformed value is skipped with a dialog — behaviour that is invisible to the players who then experience the default rates.

**Rule workflow:**
```
ReadCompRate(): for each line: sscanf("%s %s %s")
  if str1 == "ODIN":
      if val3 < 0 or val3 > 100: MessageBox; continue
      match str2 -> write g_pOdinRate[n] | g_pItemSancRate12[n] | g_pItemSancRate12Minus[n]
  unknown keys are ignored
```

---

### Business Rule: BR-13 — The shared, non-uniform success roll

**Overview:**
Every recipe draw uses one pre-computed value, `_rand = rand() % 115`, with the top fifteen results folded down by 15 — producing a distribution in which 0-84 occur once and 85-99 occur twice per 115 outcomes.

**Detailed description:**

`int _rand = rand() % 115; if (_rand >= 100) _rand -= 15;` (`:117-119`) is executed once per request, after consumption and before the recipe dispatch, so all recipes share the same roll domain. The fold is not a uniform trim: values 0-84 arise from exactly one residue class, values 85-99 from two (85-99 directly, and 100-114 after subtraction). Consequently `P(_rand = v) = 1/115` for `v <= 84` and `2/115` for `85 <= v <= 99`, and success probability for a threshold `T` (`_rand <= T`) is:

| Threshold T | Outcomes satisfying the test | Probability |
|-------------|------------------------------|-------------|
| 0-84 | T+1 | (T+1)/115 |
| 85-98 | 85 + 2(T-84) | (2T-83)/115 |
| >= 99 | all | 100 % |

The consequence is that low thresholds are *rarer* than their nominal percentage while high thresholds are *more common*: `T = 5` yields 5.22 %, `T = 40` yields 35.65 %, `T = 61` yields 53.91 %, `T = 85` yields 75.65 %. A reader who assumes `rand() % 100 < rate` semantics will mis-model every rate in the file. The fold appears to be a deliberate "sweeten the high end" hack rather than an accident, since the `-15` is exact and the array sizes (115 vs. 100) are otherwise unexplained.

The rule that no draw is made per ingredient — only one per transaction — is equally load-bearing. Recipe 2's rate can depend on up to eight ingredient contributions plus a target-level penalty, yet the entire outcome hangs on a single value, so the outcome is fully determined by `(rate, _rand)` and reproducible by an auditor who knows the seed.

**Rule workflow:**
```
_rand = rand() % 115          // 0..114
if _rand >= 100: _rand -= 15  // 100..114 -> 85..99
// domain: 0..99, with 85..99 double-weighted
success_recipe_N = (_rand <= g_pOdinRate[N])          // N in {1,3..11}
success_recipe_2 = (rate >= _rand)                    // note reversed comparison
```

---

### Business Rule: BR-14 — Threshold comparison and the `LOCALSERVER` success override

**Overview:**
For every recipe except the refine family, success is `_rand <= g_pOdinRate[combine]`; a compile-time server flag forces that predicate true on every branch.

**Detailed description:**

Each recipe branch re-tests the roll with the same predicate shape, differing only in which array slot it reads (`:124`, `:357`, `:389`, `:416`, `:448`, `:480`, `:512`), while recipe 2 uses the mirrored form `rate >= _rand` (`:224`). Because the roll domain is 0-99, a configured rate of 100 (the default for recipes 6-11) makes success unconditional, and there is no way to express "never succeed" other than 0, which still succeeds on `_rand == 0` (1/115 = 0.87 %).

Every predicate is OR-ed with `LOCALSERVER` (`Code/TMSrv/Server.cpp:380`, declared and initialised to 0). The flag is set when the resolved DB server address is the literal string `"192.168.0.50"` (`:3584-3588`), and it is used elsewhere to enable event servers and hidden behaviour (`Code/TMSrv/ProcessSecMinTimer.cpp:2040`, `Code/TMSrv/Server.cpp:6286`). Within this component it converts every roll into a guaranteed success, including the destructive refine branch's *success* path, which means a LOCALSERVER world cannot reproduce production failure rates at all.

The defaults produce the following table (derived from `Basedef.cpp:80-83` and the BR-13 distribution):

| Code | Recipe | Config key | Default | Success probability |
|------|--------|-----------|---------|---------------------|
| 1 | Celestial item | `ODIN ITEM_CELESTIAL` | 35 | 36/115 = 31.30 % |
| 2 | Refinement | computed (BR-15) | 1-76 | see BR-15 |
| 3 | Rune track | `ODIN PISTA` | 40 | 41/115 = 35.65 % |
| 4 | Lv40 unlock | `ODIN DESTRAVE_LV40` | 10 | 11/115 = 9.57 % |
| 5 | Fury stone | `ODIN PEDRA_DA_FURIA` | 10 | 11/115 = 9.57 % |
| 6 | Water secret | `ODIN SECRETA_AGUA` | 100 | 100 % |
| 7 | Earth secret | `ODIN SECRETA_TERRA` | 100 | 100 % |
| 8 | Sun secret | `ODIN SECRETA_SOL` | 100 | 100 % |
| 9 | Wind secret | `ODIN SECRETA_VENTO` | 100 | 100 % |
| 10 | Crystal seed | `ODIN SEMENTE_CRISTAL` | 100 | 100 % |
| 11 | Celestial cape | `ODIN CAPA_CELESTIAL` | 100 | 100 % |

**Rule workflow:**
```
if (combine == 2)  success = (rate >= _rand) || LOCALSERVER
else               success = (_rand <= g_pOdinRate[combine]) || LOCALSERVER
```

---

### Business Rule: BR-15 — Refinement success-rate composition

**Overview:**
The refine rate is an accumulator: base 1, plus additions from the four ingredient slots (and slot 7), minus a penalty determined by the target's current level.

**Detailed description:**

`int rate = 1;` (`:163`) establishes a floor. The loop over all eight slots (`:167-204`) then adds: `g_pItemSancRate12[0]` (default 1) for each elemental stone 5334-5337; `g_pItemSancRate12[1..10]` (defaults 2,3,4,5,6,7,8,9,12,15) for each booster stone 3338, selected by that stone's *own* sanctification level 0-9. Because the recipe admits boosters only in slots 3-6, and slot 7 is unconstrained (BR-29), the practical maximum under dual defaults is `1 + 4×15 + 15 = 76`; the practical minimum with a legal recipe is `1 + 4×1 = 5`.

The penalty block (`:212-222`) subtracts `g_pItemSancRate12Minus[0..3]` (defaults 2,3,4,5) when the target is at `REF_12`, `REF_13`, `REF_14` or `REF_15` respectively — i.e. when upgrading +12→+13, +13→+14, +14→+15, and the unreachable +15 case. Note the asymmetry: a target at level 11 (upgrading to +12, the step the recipe is named for) receives **no** penalty, while each subsequent step is progressively harder. The `REF_15` arm is dead code under BR-08, as is `g_pItemSancRate12Minus[3]`.

Two smaller defects sit in this block. The booster-stone test chain (`:175-203`) is an `if/else if` ladder re-evaluating `BASE_GetItemSanc` up to ten times per slot, which is correct but quadratic in the number of effect slots scanned internally; and the `protect` counter (`:172-173`) is incremented for *any* slot containing 4043, including slot 2, without checking whether 4043's `nPos` can legally satisfy the target test — an ambiguity in the item data that this analysis cannot resolve from the repository (no item template file is present).

Derived default success probabilities for representative legal sets:

| Ingredient set | rate | Success | Break risk on failure |
|----------------|------|---------|-----------------------|
| 4 elemental stones (5) | 5 | 6/115 = 5.22 % | yes (rate <= 9) |
| 4 × booster at +0 (9) | 9 | 10/115 = 8.70 % | yes (rate <= 9) |
| 3 elemental + 1 booster at +9 (19) | 19 | 20/115 = 17.39 % | no |
| 4 × booster at +9 (61), target +11 | 61 | 62/115 = 53.91 % | no |
| 4 × booster at +9 (61), target +14 | 57 | 58/115 = 50.43 % | no |
| 4 × booster at +9 + slot-7 booster (76), target +11 | 76 | 77/115 = 66.96 % | no |

**Rule workflow:**
```
rate = 1
for i in 0..7:
    if Item[i].sIndex in 5334..5337: rate += g_pItemSancRate12[0]
    if Item[i].sIndex == 4043:       protect++
    if Item[i].sIndex == 3338:       rate += g_pItemSancRate12[1 + sanc(Item[i])]
if sanc(Item[2]) == REF_12: rate -= minus[0]
elif sanc(Item[2]) == REF_13: rate -= minus[1]
elif sanc(Item[2]) == REF_14: rate -= minus[2]
elif sanc(Item[2]) == REF_15: rate -= minus[3]   // unreachable
```

---

### Business Rule: BR-16 — Refine success semantics (level +1, gem preserved, item identity preserved)

**Overview:**
A successful refinement copies the target item into the same slot, raises its sanctification by exactly one level, keeps its gem index, and announces the result locally and globally.

**Detailed description:**

The success path (`:224-267`) first restores the target into its (already cleared) slot with `memcpy(&Carry[InvenPos[2]], &m->Item[2], sizeof(STRUCT_ITEM))`, then reads `sanc` and `gem` from the client-supplied copy and dispatches on `sanc` to `BASE_SetItemSanc(slot, sanc+1, gem)` with an explicit `NewSanc` bookkeeping variable (`:231-252`). The four arms are level 11→12, 12→13, 13→14, 14→15. `BASE_SetItemSanc` (`Code/Basedef.cpp:2239-2285`) clamps the level to `[0,15]` and the gem/`success` argument to `[0,20]`, then encodes `230 + (level-10)*4 + gem` for levels 10-15, writing the value into whichever effect slot already carries `EF_SANC` (43) or an id in 116-125.

Preserving `gem` is the distinguishing choice: the item keeps its gem counter, so a +13 item with two successful gem insertions remains a two-gem item after becoming +14. The declared `NewSanc` local is assigned in every arm and never read — dead state that suggests the author intended to log or transmit the new level; the audit line at `:257-261` instead dumps the item code, which already encodes the new sanctification.

The notification sequence is: `_MSG_CombineComplete` with 1 (`:255`), a `sprintf`/`strcat` into `temp` followed by `ItemLog(temp, "*** Combine odin sucess ***", ip)` (`:257-261`), `SendItem(ITEM_PLACE_CARRY, InvenPos[2], ...)` (`:262`), and finally `SendNotice(g_pMessageStringTable[_SS_Combin_12Succ])` (`:264`) which broadcasts the message to every `USER_PLAY` session in the world (`Code/TMSrv/SendFunc.cpp:47-62`). Note the ordering inversion relative to the celestial branch, where the notice precedes the completion signal — cosmetic, but it means the global announcement can reach the actor before their own UI learns the outcome.

**Rule workflow:**
```
memcpy(Carry[pos], Item[2])
sanc = sanc(Item[2]); gem = gem(Item[2])
switch (sanc):
   REF_11: SetItemSanc(pos, 12, gem); new = 12
   REF_12: SetItemSanc(pos, 13, gem); new = 13
   REF_13: SetItemSanc(pos, 14, gem); new = 14
   REF_14: SetItemSanc(pos, 15, gem); new = 15
signal 1; ItemLog(success); SendItem(CARRY, pos); SendNotice(_SS_Combin_12Succ)
```

---

### Business Rule: BR-17 — Refine failure semantics and the deterministic break

**Overview:**
A failed refinement normally costs one level; if the computed rate is at most 9 and the item is `EF_MOBTYPE == 1`, the item is instead destroyed or replaced by a fragment, deterministically.

**Detailed description:**

The failure path (`:268-351`) branches on `rate <= 9 && BASE_GetItemAbility(&Item[2], EF_MOBTYPE) == 1` (`:270`). `EF_MOBTYPE == 1` is the `ARCH` class constant (`Code/Basedef.h:179`), so low-rate refine attempts are dangerous only for arch-class equipment. Inside that branch, shields are exempted by `g_pItemList[Item[2].sIndex].nPos == 128` (`:274-275`), which jumps to the downgrade label. Everything else enters `if (rand()%1 == 0)` (`:279`) — an expression that is unconditionally true, making the "possible break" comment at `:271` a misnomer: the break is guaranteed.

Within the break, `protect == 2` — which, given the recipe, means both `Item[0]` and `Item[1]` were 4043 protection items — converts the loss into a fragment instead of a total loss (`:281-311`); otherwise the slot is `memset` to zero and pushed to the client (`:314-315`). Both branches then fall through to the common failure epilogue: the localized `_NN_CombineFailed` message (id 269, `Code/Language.h:289`), `_MSG_CombineComplete` with 2, and a `"*** Combine odin fail ***"` audit line (`:344-348`).

The downgrade path (`Label_VoltaRef`, `:324-343`) restores the item and applies `BASE_SetItemSanc(slot, sanc-1, gem)`: level 11→10, 12→11, 13→12, 14→13, gem preserved. Reachable by four routes — the `else` arm when the item is not arch-type, the `else` arm when the rate exceeds 9, the shield exemption, and the (impossible) `rand()%1 != 0` — it is the only failure mode that returns a usable item. The label is jumped into from inside the preceding `if`, which is legal C++ here only because no variable with a non-trivial initialiser is crossed; it remains a maintenance hazard, since any future declaration between the `if` and the label would become illegal or uninitialised.

**Rule workflow:**
```
if failure:
  if rate <= 9 and EF_MOBTYPE(Item[2]) == 1:
      if nPos(Item[2]) == 128: goto downgrade
      if rand()%1 == 0:                        // always true
          if protect == 2: -> fragment (BR-19)
          else:            -> destroy slot
      else: goto downgrade                     // unreachable
  else:
  downgrade:
      memcpy(Carry[pos], Item[2])
      SetItemSanc(pos, level-1, gem)           // 11->10, 12->11, 13->12, 14->13
      SendItem(CARRY, pos)
  msg _NN_CombineFailed; signal 2; ItemLog(fail)
```

---

### Business Rule: BR-18 — Protection-item semantics

**Overview:**
Two 4043 items in the base positions convert a broken refinement into a fragment rather than a total loss; one or zero confer no such protection.

**Detailed description:**

The counter `protect` is incremented for every slot whose `sIndex == 4043` (`:172-173`) and is tested with strict equality `protect == 2` (`:281`). Because the matcher requires slots 0 and 1 to be either two 413 stacks or two 4043 items (`GetFunc.cpp:463`) and confines slots 3-6 to stones, the only realistic way to reach 2 is the "both bases are 4043" variant. That variant is itself the alternative recipe input: the protection items *are* the base cost, replacing the 20 lactos of the normal path.

The semantic is therefore "insurance priced in the base slot", not "optional additive". A single 4043 is not a legal base (the matcher needs both), so `protect == 1` is reachable only if 4043 appears in slot 2 or 7 with a `nPos` that satisfies the target class test in slot 2, or in slot 7 freely — and in that case the protection is *not* honoured, because the test is `== 2`. A player who places a spare protection item in slot 7 alongside two 413 bases gains nothing and loses the 4043, which is a surprising and undocumented interaction.

The protection only ever converts a *break*; it does not soften the downgrade path, does not change the roll, and does not prevent the failure message or the signal-2 acknowledgement. It also does not restore the other ingredients — every 413, stone and booster is consumed regardless.

**Rule workflow:**
```
protect = count(Item[i].sIndex == 4043 for i in 0..7)
on break:  if protect == 2: fragment    else: destroy
```

---

### Business Rule: BR-19 — Fragment conversion mapping

**Overview:**
When a protected break occurs, the destroyed equipment is replaced in place by one of six fragment items selected by the equipment's `nPos` class.

**Detailed description:**

The protected branch (`:283-311`) first copies the target back into the slot, then overwrites `sIndex = 3021` and stamps `stEffect[0] = { EF_ITEMLEVEL (87), 10 }` (`:285-288`). It then adjusts the fragment id by the *original* item's `nPos`: `2 → 3021` (unchanged), `4 → 3022`, `8 → 3023`, `16 → 3024`, `32 → 3025`, and any other class (including 64, 128, 192) → `3026` with `cValue = 11` (`:290-306`).

Two consequences follow. First, the fragment inherits the original item's `stEffect[1]` and `stEffect[2]` verbatim, because only effect slot 0 is overwritten; any gem or attribute carried on the destroyed equipment is spliced onto a fragment whose template almost certainly does not expect it. Second, the `EF_ITEMLEVEL` marker (10 or 11) is the only discriminator retained, so the six fragment ids plus that marker encode "what was destroyed" for whatever downstream recipe is meant to consume them — and that consumer is not in this component, making the encoding unverifiable here.

The `else` mapping is over-broad: `nPos == 128` (shields) can never reach this code because of the exemption at `:274`, but `nPos == 64` and `192` (the two other equipment classes admitted by the matcher) collapse into the same 3026/11 fragment as any unrecognised value. There is also no `SendItem` on the destroy path's message side other than the item push itself, so the client learns of the conversion only by observing the slot change.

**Rule workflow:**
```
memcpy(Carry[pos], Item[2])
sIndex = 3021; stEffect[0] = {87, 10}
nPos == 4  -> sIndex = 3022
nPos == 8  -> sIndex = 3023
nPos == 16 -> sIndex = 3024
nPos == 32 -> sIndex = 3025
else       -> sIndex = 3026; stEffect[0].cValue = 11
SendItem(CARRY, pos)
```

---

### Business Rule: BR-20 — Unconditional, pre-outcome ingredient consumption

**Overview:**
Every non-empty descriptor is cleared from the inventory before the recipe outcome — success, failure, or even recipe rejection — is determined.

**Detailed description:**

The consumption loop (`:92-99`) sits between the eligibility guards and the roll. It walks all eight positions, skips descriptors with `sIndex == 0`, and for each remaining one `memset`s the inventory slot and pushes the now-empty slot to the client with `SendItem(conn, ITEM_PLACE_CARRY, ...)`. It does not consult `combine`, does not consult the roll, and does not defer. The design intent is clear — "the NPC takes everything you offered, then tells you what happened" — and it matches the crafting fiction of the other combine handlers.

The consequence is that no branch below can ever *decline* the transaction after this point without the loss already being realised. Three of the handler's failure modes rely on that being acceptable (the ordinary failure paths, where the loss is the intended cost). One does not: the terminal `else` at `:571-579`, which is the "no recipe matched" case (BR-21). A further latent hazard is the dead guard at `:206-210`, which returns signal 0 from a position where the items are already gone.

Because the loop pushes each cleared slot individually, a full eight-item transaction generates eight `_MSG_SendItem` packets plus one completion signal; the client's bag view is therefore authoritative and synchronised even when the transaction fails. That part of the design is sound and is what makes the destructive behaviour observable rather than silently desynchronising the client.

**Rule workflow:**
```
for i in 0..7:
    if Item[i].sIndex == 0: continue
    memset(&Carry[InvenPos[i]], 0, sizeof(STRUCT_ITEM))
    SendItem(conn, ITEM_PLACE_CARRY, InvenPos[i], &Carry[InvenPos[i]])
ItemLog("*** Item combine odin. ***")
[item-code dump] [roll] [dispatch]
```

---

### Business Rule: BR-21 — The unmatched-set rule consumes everything and reports failure

**Overview:**
When `GetMatchCombineOdin` returns 0, all submitted ingredients have already been destroyed and the client is told the combination failed.

**Detailed description:**

The dispatch chain covers codes 1-11 explicitly; code 0 falls through to `else` at `:571-579`, which sends `_NN_CombineFailed` (269), `_MSG_CombineComplete` with 2, and a failure audit line. Signal 2 is the "you attempted a craft and it failed" acknowledgement — the same value used for a genuine unlucky roll — so from the client's perspective an invalid recipe is indistinguishable from a failed roll. The player's items are gone either way.

This is the most severe behavioural defect in the component. The cheapest reproduction is a packet with eight valid but non-matching items (for example eight 413 stacks with no target): the matcher returns 0, the consumption loop clears all eight, and the failure path reports a roll failure that never happened. The crown rule (BR-05) and the range rule (BR-06) both funnel into the same destructive path, so "protected" inputs are protected only in the sense that they cannot be used *productively*.

The sibling handler demonstrates the correct shape: `Exec_MSG_CombineItemEhre` tests `if (combine == 0) { SendClientMessage(_NN_Wrong_Combination); SendClientSignalParm(..., 0); return; }` *before* its consumption loop (`Code/TMSrv/_MSG_CombineItemEhre.cpp:47-52`). The Odin handler has no equivalent, which strongly suggests the guard was dropped during derivation rather than deliberately omitted — the two files otherwise share an identical preamble (BR-01's log message, the `RemoveTrade` call, the consumption loop).

**Rule workflow:**
```
combine = GetMatchCombineOdin(m->Item)
[guards for 2, 4, 11]
[consume all]                      <- happens for combine == 0 too
roll
switch (combine):
    1..11 -> ...
    default -> SendClientMessage(_NN_CombineFailed)
               SendClientSignalParm(..., _MSG_CombineComplete, 2)
               ItemLog("*** Combine odin fail ***")
```

---

### Business Rule: BR-22 — Celestial item synthesis result construction

**Overview:**
Recipe 1 consumes an upgraded base item, a celestial counterpart that must be at least +9, a catalyst (542 or 772) and the four elemental stones, and emits an item whose id is the `Extra` field of the celestial counterpart's template while keeping the base item's effect payload.

**Detailed description:**

The matcher requires `Item[0].sIndex == g_pItemList[Item[1].sIndex].Extra` with `sanc(Item[0]) >= 9` and `sanc(Item[1]) == REF_15` (`GetFunc.cpp:459`). `Extra` is a `short` in the item template (`Code/Basedef.h:904`) used throughout the codebase as the "upgraded variant" link — the same field feeds `GetMatchCombine`'s family-1 recipe (`GetFunc.cpp:57-64`) and the crown test (`GetFunc.cpp:54`). So `Item[1]` is the base equipment at maximum refinement and `Item[0]` is its celestial-linked counterpart at +9 or better.

On success (`:126-129`) the handler copies `Item[1]` into its slot, overwrites `sIndex` with `g_pItemList[Item[0].sIndex].Extra` — the upgrade of the *counterpart*, not of the base — and calls `BASE_SetItemSanc(slot, 0, 0)` to zero the sanctification and gem. The result therefore combines the base item's effect triple with a template chosen by a two-hop `Extra` chain; whether that is the intended "next tier" or an inversion depends on how the item data populates `Extra`, and **that data is not part of this repository** (no item template file exists under the project root, and `g_pItemList` is populated by the DB server's `ImportItem` pipeline, `Code/DBSrv/CReadFiles.cpp:42-43`). This is a documented ambiguity, not a defect claim.

Two implementation notes. `BASE_SetItemSanc(..., 0, 0)` only writes when the target already carries an `EF_SANC` (43) or 116-125 effect; if the base item has none, the call returns FALSE and the item simply has no sanctification effect, which `BASE_GetItemSanc` reports as 0 — equivalent outcome, different on-the-wire representation. And the failure path (`:148-157`) is the only failure in the component that returns the *primary* item: it copies `Item[1]` back, so a failed synthesis costs the celestial counterpart, the catalyst and the four stones while preserving the base equipment.

**Rule workflow:**
```
precondition (matcher): Item[0] == Extra(Item[1]), sanc(Item[0]) >= 9, sanc(Item[1]) == REF_15
                        Item[2] in {542,772}, Item[3..6] == 5334,5335,5336,5337
success: memcpy(Carry[InvenPos[1]], Item[1])
         sIndex = Extra(Item[0]); SetItemSanc(0, 0)
         SendNotice(_SS_Combin_12Succ); signal 1; SendItem
failure: memcpy(Carry[InvenPos[1]], Item[1]); msg; signal 2
```

---

### Business Rule: BR-23 — Fixed-output recipes and result placement

**Overview:**
Recipes 3, 5, 6-9 and 10 do not transform an item — they overwrite the first ingredient slot's `sIndex` with a constant or a formula of the recipe code.

**Detailed description:**

The four blocks are structurally identical: on success they assign `pMob[conn].MOB.Carry[m->InvenPos[0]].sIndex` to a constant, send `_NN_Processing_Complete` (158), signal 1, log the resulting item code, and push the slot. The outputs are 5134 for the rune track (`:359`), 3020 for the fury stone (`:418`), `5334 + (combine - 6)` for the elemental stones (`:450`, yielding 5334/5335/5336/5337 for codes 6/7/8/9), and 4032 for the crystal seed (`:482`).

The overwrite is safe only because BR-20 has already zeroed the target slot; the result is a clean item with no inherited effects. It is worth being explicit about this, because the code *looks* like an in-place mutation of an ingredient and would splice the ingredient's effects onto the output if the consumption loop were ever moved below the dispatch. The same pattern in reverse is exactly the bug present in the protected-break fragment path (BR-19), which does retain effects.

Placement in `InvenPos[0]` is a client-visible contract: the first slot in the packet becomes the output slot. A client that sorts its ingredient list differently between attempts will see the result appear elsewhere, and there is no server-side normalisation. Recipes 6-9 additionally share the same output family as the recipe-2 *inputs*, so a chain of elemental-stone crafting feeds the refinement loop — which is presumably the economic intent, and which also means the parity rule (BR-07) is the gate protecting that chain.

**Rule workflow:**
```
on success:
  Carry[InvenPos[0]].sIndex = { 5134 | 3020 | 5334 + (combine - 6) | 4032 }
  SendClientMessage(_NN_Processing_Complete)
  SendClientSignalParm(..., 1)
  ItemLog(success); SendItem(CARRY, InvenPos[0])
on failure: SendClientMessage(_NN_CombineFailed); signal 2; ItemLog(fail)
```

---

### Business Rule: BR-24 — Cape refine application semantics

**Overview:**
A successful cape refine raises `Equip[15]`'s sanctification by one, allocating an `EF_SANC` effect slot if the item has none, resetting the gem index, and capping at +9 by pre-condition.

**Detailed description:**

The block (`:510-569`) reads `sanc = BASE_GetItemSanc(&Equip[15])`, and only when `sanc == 0` performs slot allocation (`:516-541`). The allocation test is a triple-nested compound condition that effectively asks, for each effect slot in order, "is this slot free of a real attribute?" — where "real" is encoded as `cEffect != 0 && cEffect != 43 && (cEffect < 116 || cEffect > 125)`, i.e. not empty, not already a sanctification marker, and not one of the 116-125 sanctification bands. The first free slot receives `{ cEffect = 43, cValue = 0 }`. If all three slots hold real attributes, the handler emits `_NN_Cant_Refine_More` (75, `Code/Language.h:95`) and returns — after the ingredients were consumed — without sending a completion signal.

`BASE_SetItemSanc(&Equip[15], sanc + 1, 0)` (`:543`) then writes the new level. The gem argument is hardcoded `0`, so any existing gem counter on the mantle is destroyed on every successful refine — a silent loss of an item property that the equipment-refine path (BR-16) carefully preserves. The level ladder tops out at 9 because the pre-check (`:85`) rejects `>= 9`, so the highest reachable state is +9, and because `sanc + 1` can reach 9 but never 10, the `REF_10` encodings are never produced here.

Two further defects live in this block. The audit line (`:551-553`) builds the item code from `pMob[conn].MOB.Carry[m->InvenPos[0]]` — the *first ingredient slot*, emptied by the consumption loop — instead of from `Equip[15]`, so every cape-refine success logs an empty item; the subsequent `SendItem(conn, ITEM_PLACE_EQUIP, 15, &Equip[15])` (`:556`) is correct, producing a log/packet divergence. And the announcement path is the only success branch that uses `_NN_Processing_Complete` rather than a recipe-specific message, so players cannot tell a cape refine from a rune-track craft in their chat log.

**Rule workflow:**
```
sanc = sanc(Equip[15])
if sanc == 0:
    if slot0 real -> if slot1 real -> if slot2 real -> _NN_Cant_Refine_More; return
                                     else slot2 = {43, 0}
                    else slot1 = {43, 0}
    else slot0 = {43, 0}
SetItemSanc(Equip[15], sanc + 1, 0)      // gem reset to 0
_NN_Processing_Complete; signal 1; ItemLog(WrongItemCode); SendItem(EQUIP, 15)
```

---

### Business Rule: BR-25 — The three-valued completion contract

**Overview:**
Every terminal path answers with `_MSG_CombineComplete` whose parameter distinguishes "rejected without change" (0), "succeeded" (1) and "failed after consumption" (2).

**Detailed description:**

`SendClientSignalParm(conn, ESCENE_FIELD, _MSG_CombineComplete, parm)` (`Code/TMSrv/SendFunc.cpp:208-218`) emits a `MSG_STANDARDPARM` with `ID = ESCENE_FIELD (30000, Code/Basedef.h:170)`, `Type = _MSG_CombineComplete (167 | FLAG_GAME2CLIENT | FLAG_CLIENT2GAME, Code/Basedef.h:1965)` and the parameter in `Parm`. The handler uses 0 for the four non-destructive rejections (descriptor mismatch, elemental parity, celestial-target exclusion, Lv40 ineligibility, cape ineligibility — five sites: `:40`, `:68`, `:75`, `:81`, `:87`) and for the dead guard at `:208`; 1 for every success; 2 for every failure, including the no-recipe fallback.

The semantic mapping is the client's only way to distinguish "nothing happened, your items are safe" from "the attempt failed, your items are gone". That contract is coherent for codes 0/1/2 as used on the four real rejection paths, and it is precisely what makes BR-21 visible as a bug: signal 2 is emitted when no roll occurred, so the client's UI will present it as a failed attempt. Signal 0 is also emitted from one unreachable site (`:208`), which is harmless but means the constant is not a reliable indicator of "guard fired".

Two paths violate the contract entirely: the `invPos` bounds failure (`:33-34`) returns with no signal at all, and the "cannot refine more" allocation failure (`:524-525`) sends a chat message but no completion signal. In both cases the client's craft UI is left waiting, and a subsequent legitimate request may be rejected by the client's own busy-state logic.

**Rule workflow:**
```
0 -> request rejected / no state change (4 real sites + 1 dead site)
1 -> success (recipes 1..11)
2 -> failure after consumption (including combine == 0)
(none) -> invPos bounds failure; cape-slot allocation failure
```

---

### Business Rule: BR-26 — Global success broadcast

**Overview:**
Successful item synthesis (recipe 1) and successful refinement (recipe 2) are announced to every connected player, not just the actor.

**Detailed description:**

Both success branches call `SendNotice(g_pMessageStringTable[_SS_Combin_12Succ])` (`:133`, `:264`), where `_SS_Combin_12Succ` is string id 462 (`Code/Language.h:481`). `SendNotice` prefixes `"not "`, writes a system log line, then loops over `MAX_USER` delivering `SendClientMessage` to every session whose `Mode == USER_PLAY` (`Code/TMSrv/SendFunc.cpp:47-62`). The message text itself is not parameterised — the actor's name and the produced item are appended only to the *file* audit trail, not to the broadcast — so the announcement conveys "someone succeeded" without identifying who or what.

This rule is a deliberate economy-visibility mechanism (a global "rare craft succeeded" signal). Its side effects are operational: the broadcast is O(MAX_USER) per success with no rate limiting, and it is triggered from inside the packet handler, so a burst of successful refinements produces a proportional burst of outbound messages to every session. The remaining nine recipes are silent, so the absence of an announcement does not imply failure — a distinction players cannot make from the notice channel alone.

The rule also participates in a minor ordering inconsistency: recipe 2 announces after sending the completion signal (`:255` then `:264`), recipe 1 announces before it (`:133` then `:135`), so the relative order of "your craft resolved" and "the world was told" differs between two adjacent branches.

**Rule workflow:**
```
on success of recipe 1 or 2:
    SendNotice(_SS_Combin_12Succ)  -> "not <msg>" to every USER_PLAY session + system log
```

---

### Business Rule: BR-27 — Audit journaling of every attempt

**Overview:**
Every request produces a multi-line entry in the daily item log: an attempt header, a per-ingredient item-code dump, a footer, and an outcome line.

**Detailed description:**

`ItemLog` (`Code/TMSrv/Server.cpp:9264-9284`) writes to a per-day file `.\item_log\TM_dd_mm_yyyy_hh_mm_ss.txt` opened by the daily rollover (`:9257-9261`), formatting `timestamp [IP] str2 str1`. The handler uses it four ways: the descriptor-mismatch error (`:39`), the attempt header `"*** Item combine odin. ***"` (`:101`), the ingredient dump with footer (`:114-115`), and the outcome line (`:141`, `:155`, `:261`, `:348`, `:371`, `:381`, `:397`, `:408`, `:430`, `:440`, `:462`, `:472`, `:494`, `:504`, `:555`, `:565`, `:576`).

The dump loop (`:103-113`) is the component's most dangerous code. For each non-empty descriptor it declares `char itemlog[2048]`, formats the item via `BASE_GetItemCode` — which emits `" <Name> : <index>.<c0>.<v0>.<c1>.<v1>.<c2>.<v2>"` (`Code/Basedef.cpp:1479-1495`) — and appends it to the **process-global** `temp` with `strcat` (`:111`). `temp` is `char temp[4096]` (`Code/TMSrv/Server.cpp:277`), shared with `Log`, `CrackLog`, `SendNotice` and every other handler, and it is **never reset in this function**. Two failure modes follow: the dump is prepended with whatever the previous operation left in `temp` (usually a log line, sometimes another handler's partial output), and the `strcat` is unbounded — eight long item names plus a nearly full 4096-byte buffer overflows into adjacent globals.

For the operator the log is nonetheless the only forensic record: it captures the account name, IP, all eight item codes with their effect triples, and the outcome, which is enough to reconstruct any transaction — provided `temp` was not already polluted. The outcome lines are inconsistently parameterised (some pass the account name as `str1` and the marker as `str2`, the refine/cape successes pass the account name *plus* the item code in `temp`, and recipe 4 passes the character name), so automated parsing of the log requires per-recipe handling.

**Rule workflow:**
```
ItemLog("*** Item combine odin. ***", account, ip)
for i in 0..7: if Item[i].sIndex: BASE_GetItemCode(Item[i], itemlog); strcat(temp, itemlog)
ItemLog(temp, account, ip)            // temp not cleared; unbounded append
ItemLog("*** ------------------- ***", account, ip)
... outcome line per branch ...
```

---

### Business Rule: BR-28 — Absent packet validation and fraud escalation

**Overview:**
The handler assumes the payload is a well-formed `MSG_CombineItem` and never escalates malformed input to the crack-detection counter.

**Detailed description:**

`MSG_CombineItem` is 84 bytes (12-byte `_MSG` header + 8 × 8-byte `STRUCT_ITEM` + 8 `char` positions; `Code/Basedef.h:1956-1963`, with the structure outside any `#pragma pack` region and naturally aligned). The framing layer accepts any message whose declared `Size` is between `sizeof(HEADER)` and `MAX_MESSAGE_SIZE = 8192` (`Code/CPSock.cpp:397`, `Code/CPSock.h:38`) and hands the raw pointer down; `ProcessClientMessage` validates only `ID` (`Code/TMSrv/ProcessClientMessage.cpp:42-51`); the handler casts unconditionally (`:22`) and reads all eight descriptors.

An undersized packet therefore makes the handler consume bytes beyond the declared message — residual content of the 128 KB receive buffer (`Code/CPSock.h:35`, `Code/CPSock.cpp:66-67`). This is a stale-data read rather than an out-of-bounds one while `nProcPosition + 84 <= 131072`, which the framing loop normally guarantees, but it means the recipe is resolved against bytes the client did not send in *this* message. BR-01's `memcmp` makes a productive exploit implausible (the residual bytes would have to match real inventory contents), but the parse is undefined from a protocol standpoint and the failure is indistinguishable from a legitimate rejection.

Separately, the handler never calls `AddCrackError`, unlike several other handlers that escalate protocol violations into eventual disconnect. A client can therefore submit arbitrary invalid combine traffic indefinitely with no cumulative penalty; the only consequence is a `_MSG_CombineComplete 0` (or, for position violations, nothing at all) and — where BR-21 applies — the loss of its own items.

**Rule workflow:**
```
CPSock:  size in [12, 8192] only
Dispatcher: ID bounds only
Handler: cast to MSG_CombineItem, read 84 bytes, no Size comparison, no AddCrackError
```

---

### Business Rule: BR-29 — The unconstrained eighth slot

**Overview:**
Slot 7 is range-validated, consumed, and in recipe 2 can even contribute success rate — but no recipe requires it or constrains its content.

**Detailed description:**

Every recipe tuple in `GetMatchCombineOdin` addresses `Item[0]` through `Item[6]`; `Item[7]` is only tested for `747` and for range (`GetFunc.cpp:456-457`). The handler's loops, by contrast, iterate `MAX_COMBINE = 8` (`Code/Basedef.h:113`) — so slot 7 is validated (BR-01), consumed (BR-20), logged (BR-27) and, in the refine rate loop (`:167-204`), *counted*: a 5334-5337 adds `g_pItemSancRate12[0]`, and a 3338 adds up to 15.

This is an inconsistency between two views of the same array: the matcher treats slot 7 as unused padding, the rate calculator treats it as a legal bonus slot. The player-visible consequences are all negative or exploitable: an item placed there is destroyed for nothing (the common case), or — for refine attempts — silently increases the rate while bypassing the elemental parity rule (BR-07), which only reads slots 3-6. The maximum default rate of 76 rather than 61 (BR-15) is a direct consequence.

The same slot is also the only place where the BR-05 crown rule and the BR-08 celestial exclusion can be bypassed *as consumption targets*: a celestial-grade item or a crown in slot 7 disqualifies the set via BR-05 (crown) or is destroyed (celestial). No validation ties slot 7 to "must be empty".

**Rule workflow:**
```
matcher: Item[7] -> only 747 test + range test
handler: Item[7] -> memcmp (BR-01), memset (BR-20), item-code log (BR-27)
recipe 2 rate loop: Item[7] contributes like any other slot
```

---

## 4. Component Structure

```
Code/
├── Basedef.h                         # STRUCT_ITEM (:398-412), STRUCT_ITEMLIST (:882-906),
│                                     # MAX_COMBINE (:113), MAX_EQUIP/MAX_CARRY (:75-76),
│                                     # MORTAL/ARCH/CELESTIAL (:178-180), REF_10..REF_15 (:196-201),
│                                     # ESCENE_FIELD (:170), MSG_CombineItem (:1956-1963),
│                                     # _MSG_CombineItemOdin (:1948), _MSG_CombineItemOdin2 (:1951),
│                                     # _MSG_CombineComplete (:1965), g_pOdinRate extern (:2485)
├── Basedef.cpp                       # rate tables (:80-102), g_pItemList (:753),
│                                     # BASE_GetItemCode (:1479), BASE_GetItemAbility (:1647),
│                                     # BASE_GetItemSanc (:2095), BASE_GetItemGem (:2143),
│                                     # BASE_SetItemSanc (:2239), BASE_GetItemAmount (:2287)
├── ItemEffect.h                      # EF_SANC 43 (:83), EF_ITEMLEVEL 87 (:133), EF_MOBTYPE 112 (:149)
├── CPSock.h / CPSock.cpp             # framing, RECV_BUFFER_SIZE (:35), MAX_MESSAGE_SIZE (:38), ReadMessage (:386-416)
└── TMSrv/
    ├── _MSG_CombineItemOdin.cpp      # THE COMPONENT — 580 lines, one exported function (Exec_MSG_CombineItemOdin)
    ├── ProcessClientMessage.cpp      # dispatcher; routes Odin + Odin2 (:266-269)
    ├── ProcessClientMessage.h        # declaration (:118)
    ├── GetFunc.cpp                   # GetMatchCombineOdin recipe chain (:427-496)
    ├── GetFunc.h                     # declaration (:31)
    ├── SendFunc.cpp                  # SendClientMessage (:27), SendNotice (:47),
    │                                 # SendClientSignalParm (:208), SendItem (:1055)
    ├── Server.cpp                    # pMob/pUser globals, char temp[4096] (:277), LOCALSERVER (:380, :3587),
    │                                 # RemoveTrade (:7319), ItemLog (:9264), ReadConfig (:761)
    ├── Server.h                      # extern temp (:382), extern LOCALSERVER (:325)
    ├── CReadFiles.cpp                # ReadCompRate ODIN block (:485-567), COMP_RATE_PATH (:37)
    ├── Language.h                    # message ids: 75, 158, 269, 462
    └── TMSrv.vcxproj                 # compile unit registered at :128
```

Internal organisation of the single translation unit:

| Lines | Region | Responsibility |
|-------|--------|----------------|
| 1-18 | Header block | GPL notice, `#include "ProcessClientMessage.h"` |
| 20-22 | Entry | Signature; payload cast to `MSG_CombineItem*` |
| 24-43 | Phase A | Descriptor/slot revalidation loop |
| 45 | Phase B | Recipe resolution (`GetMatchCombineOdin`) |
| 47-89 | Phase C | Four eligibility guards (recipes 2, 4, 11) |
| 92-99 | Phase D | Unconditional ingredient consumption |
| 101-115 | Phase E | Audit journal (header, dump, footer) |
| 117-119 | Phase F | Shared roll |
| 122-159 | Phase G.1 | Recipe 1 — celestial item synthesis |
| 161-353 | Phase G.2 | Recipe 2 — refinement (the largest region: rate accumulation, success, break, downgrade) |
| 355-385 | Phase G.3 | Recipe 3 — rune track |
| 387-412 | Phase G.4 | Recipe 4 — Lv40 unlock |
| 414-444 | Phase G.5 | Recipe 5 — fury stone |
| 446-476 | Phase G.6-9 | Recipes 6-9 — elemental stones (single parameterised block) |
| 478-508 | Phase G.10 | Recipe 10 — crystal seed |
| 510-569 | Phase G.11 | Recipe 11 — cape refine |
| 571-579 | Phase H | Fallback failure |

The unit exports exactly one symbol and declares no static helpers, no classes, and no state of its own; every effect it has is on process globals. Its 25 decision points (per `component-analysis-Client-Handler-Interface-Contract-2026-11-15_22-09-12.md:585`) are concentrated in the refine branch, which alone accounts for 193 of the 580 lines.

---

## 5. Dependency Analysis

**Internal dependencies (compile-time):**

```
Exec_MSG_CombineItemOdin
├── ProcessClientMessage.h  (declaration + transitive includes)
├── GetMatchCombineOdin                     GetFunc.cpp:427   [pure function, no globals]
│   ├── BASE_GetItemSanc                    Basedef.cpp:2095
│   ├── BASE_GetItemAmount                  Basedef.cpp:2287
│   └── BASE_GetItemAbility                 Basedef.cpp:1647
├── BASE_GetItemSanc / BASE_SetItemSanc / BASE_GetItemGem / BASE_GetItemCode / BASE_GetItemAbility
├── SendClientMessage / SendClientSignalParm / SendNotice / SendItem     SendFunc.cpp
├── ItemLog                                 Server.cpp:9264
└── RemoveTrade                             Server.cpp:7319
```

**Internal dependencies (data / runtime globals read or written):**

| Symbol | Kind | Use in this component |
|--------|------|------------------------|
| `pMob[conn]` (`CMob[]`, `MAX_MOB`) | global array | `MOB.Carry[]` read+write, `MOB.Equip[15]` read+write, `MOB.CurrentScore.Level`, `MOB.MobName`, `MaxCarry`, `extra.ClassMaster`, `extra.QuestInfo.Celestial.Lv40` |
| `pUser[conn]` (`CUser[]`, `MAX_USER`) | global array | `AccountName`, `IP` (audit), `Mode` (through `SendItem`/`SendClientMessage`), `Trade`/`AutoTrade` (through `RemoveTrade`) |
| `temp` (`char[4096]`) | global buffer | `strcat`/`sprintf` for audit lines and chat text — **not reset** |
| `g_pItemList[MAX_ITEMLIST]` | global array | `Extra` of `Item[0]`, `Name` via `BASE_GetItemCode`, `nPos` of the refine target |
| `g_pOdinRate[12]` | global array | thresholds for recipes 1, 3-11 |
| `g_pItemSancRate12[11]` | global array | refine additions |
| `g_pItemSancRate12Minus[4]` | global array | refine penalties |
| `g_pMessageStringTable[]` | global table | ids 75, 158, 269, 462 |
| `LOCALSERVER` | global flag | force-success override |
| `g_pItemGrid` / item grid tables | indirectly | through `BASE_GetItemCode` |

**External dependencies:**

| Dependency | Type | Purpose | Contract |
|------------|------|---------|----------|
| W2 game client | remote peer | submits the craft request, consumes the reply | custom binary TCP protocol; `MSG_CombineItem` 84 B in, `MSG_CombineComplete`/`MSG_SendItem`/`MSG_MessagePanel` out |
| Socket layer (`CPSock`) | in-process runtime | framing, keysum, buffering | 128 KB receive buffer, 8 KB max message |
| `../../Common/Settings/CompRate.txt` | configuration file | all success rates | `ODIN <KEY> <0..100>`; missing file ⇒ modal box + compile-time defaults |
| `g_pItemList` template data | external data set, loaded by DBSrv `ImportItem` from `../../Common/ImportItem/*.*` (`Code/DBSrv/CReadFiles.cpp:42-43`) | item names, `Extra` links, `nPos` classes | not present in this repository |
| `Language.txt` message table | configuration file | localized strings (ids 75, 158, 269, 462) | loaded via `BASE_InitializeMessage`, reloadable |
| Item log filesystem | OS | daily `./item_log/TM_*.txt` | `fopen("wt")` per day; silent no-op if the handle is null |
| C runtime | library | `rand`, `memcmp`, `memcpy`, `memset`, `strcat`, `sprintf` | MSVC CRT; `rand()` unseeded ⇒ deterministic sequence per process |
| DB server | indirect | persistence of the mutated character | none from this handler — no `DBServerSocket` traffic in the translation unit |

There is **no** direct database, network-peer, or service dependency at runtime. The component is a pure in-memory mutation followed by client notification; durability is deferred to the character-save path, which means a crash between this handler and the next save loses both the produced item and the consumed ingredients.

---

## 6. Afferent and Efferent Coupling

Units are the file-level C functions/arrays that participate in this component, per the project's procedural paradigm (the architecture report classifies each handler with afferent 1 / efferent 14, `architectural-report-2026-11-15_21-58-11.md:133`).

| Component | Afferent Coupling | Efferent Coupling | Critical | Notes |
|-----------|-------------------|-------------------|----------|-------|
| `Exec_MSG_CombineItemOdin` | 1 | 14 | High | Single caller (dispatcher, two labels); 12 module functions + 9 globals + libc |
| `GetMatchCombineOdin` | 1 | 4 | Medium | Only this handler calls it; depends on 3 `BASE_*` helpers + `g_pItemList` |
| `BASE_SetItemSanc` | 12+ | 0 | High | Shared sanctor; encoding (`230 + (lvl-10)*4 + gem`) is a de-facto contract |
| `BASE_GetItemSanc` | 20+ | 0 | High | Decodes the same encoding; any change breaks every refine path |
| `BASE_GetItemAbility` | 100+ | 2 | High | Resolves `EF_MOBTYPE` for BR-08/BR-17 |
| `BASE_GetItemCode` | 10+ | 1 | Medium | Audit formatting; output length drives the `strcat` risk |
| `BASE_GetItemGem` | 5 | 0 | Low | Gem index preservation in BR-16/BR-17 |
| `SendClientSignalParm` | 40+ | 0 | Medium | Carries the 0/1/2 acknowledgement contract |
| `SendItem` | 60+ | 0 | Medium | Pushes every mutated slot; fails silently unless `USER_PLAY` |
| `SendNotice` | 20+ | 0 | Medium | O(MAX_USER) broadcast on recipe 1/2 success |
| `SendClientMessage` | 100+ | 0 | Low | Chat/panel text |
| `ItemLog` | 40+ | 1 | Low | File append; formats `timestamp [IP] str2 str1` |
| `RemoveTrade` | 20+ | 2 | Medium | Side effects beyond the failed request (re-broadcasts appearance) |
| `g_pOdinRate` | 2 writers (`Basedef.cpp:80`, `CReadFiles.cpp:493-566`) | 1 reader | High | All thresholds; index 0 and 2 unused |
| `g_pItemSancRate12` | 2 writers | 1 reader | Medium | Refine additions |
| `g_pItemSancRate12Minus` | 2 writers | 1 reader | Medium | Refine penalties; index 3 unreachable |
| `temp` (global buffer) | 200+ | 200+ | High | Shared scratch; the only unbounded `strcat` in this unit |
| `pMob[].MOB.Carry[]` | 200+ | 200+ | High | Direct, unmediated item array |

Interpretation. The handler's **efferent** coupling is dominated by the item primitives (`BASE_*`, 5 of 12 call sites) and by notification (`Send*`, 4 of 12); the remaining three are audit and trade-reset helpers. Its **afferent** coupling of 1 is the lowest possible for a handler and is an artefact of the flat dispatcher — no other code path can reach it, so a change to its behaviour is invisible to the rest of the server except through client-visible effects. That combination (single inbound edge, wide outbound fan-out, no interface abstraction) is the classic "leaf handler" shape: cheap to change in isolation, expensive to verify, because nothing exercises it except a live client.

Cohesion is low by construction. The unit performs five unrelated jobs — packet validation, recipe resolution, RNG, item mutation across three different data shapes (inventory item, refinement ladder, equipment slot), and multi-channel notification — inside one 580-line function with no extracted helpers. The only genuine abstraction it uses is `GetMatchCombineOdin`, and that abstraction is incomplete: the matcher knows the *ingredients* but the handler owns the *outcomes*, so the recipe table and the effect logic cannot be changed independently.

Two coupling asymmetries are worth recording. First, the component is tightly coupled to `BASE_GetItemSanc`'s sentinel encoding (`REF_10 = 10 … REF_15 = 27`) but expresses the dependency as if the constants were levels, which is why the "level 11..14" window reads like a range comparison and why the `REF_15` arms of BR-15/B-17 are dead. Second, the component is coupled to the item template's `Extra` field (BR-22) and `nPos` set (BR-04/BR-19) without any local validation — a data change to `Extra` or to a `nPos` class silently rewrites the recipe semantics.

---

## 7. Endpoints

This component speaks the W2 binary TCP protocol, not REST/GraphQL/gRPC. The "endpoints" are message types on the TMSrv client socket.

| Message | Type value | Direction | Payload | Handler | Response(s) |
|---------|-----------|-----------|---------|---------|-------------|
| `_MSG_CombineItemOdin` | `210 \| FLAG_CLIENT2GAME` = 722 | client → server | `MSG_CombineItem` (84 B): 12 B header + `STRUCT_ITEM Item[8]` + `char InvenPos[8]` | `Exec_MSG_CombineItemOdin` | `_MSG_CombineComplete` (0/1/2 or none), `_MSG_SendItem` ×N, `_MSG_MessagePanel`, optional `_MSG_MessagePanel` broadcast to all |
| `_MSG_CombineItemOdin2` | `226 \| FLAG_CLIENT2GAME` = 738 | client → server | identical layout, routed to the same function | `Exec_MSG_CombineItemOdin` | identical |

Emitted responses (from this component):

| Message | Type value | Parameters | Meaning as used here |
|---------|-----------|------------|----------------------|
| `_MSG_CombineComplete` | `167 \| FLAG_GAME2CLIENT \| FLAG_CLIENT2GAME` = 935 | `MSG_STANDARDPARM.Parm` | 0 = rejected/no change; 1 = success; 2 = failure after consumption |
| `_MSG_SendItem` | via `SendItem(conn, ITEM_PLACE_CARRY=1 or ITEM_PLACE_EQUIP=0, slot, item)` | slot + `STRUCT_ITEM` | pushes cleared slots and the produced/downgraded item |
| `_MSG_MessagePanel` | via `SendClientMessage` | text | `_NN_CombineFailed`, `_NN_Processing_Complete`, `_NN_Cant_Refine_More` |
| `_MSG_MessagePanel` (all sessions) | via `SendNotice` | text | `_SS_Combin_12Succ` broadcast on recipes 1 and 2 |

The absence of any size check on the inbound payload (BR-28) is the only deviation from a strict endpoint contract: a message shorter than 84 bytes is accepted by the framing layer and parsed anyway.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| W2 game client | External peer | Craft request in, outcome out | TCP, custom binary, keysum-obfuscated | `MSG_CombineItem` (84 B) / `MSG_STANDARDPARM` / `MSG_SendItem` / `MSG_MessagePanel` | Descriptor mismatch ⇒ signal 0 + log; position violation ⇒ `RemoveTrade` + silent; oversize/undersize ⇒ framing reject or stale-byte parse |
| `CompRate.txt` (`ODIN` block) | External config file | All success rates | Local text file, whitespace tokens | `ODIN <KEY> <int 0..100>` | Missing file ⇒ modal box, defaults retained; out-of-range value ⇒ modal box + skip; unknown key ⇒ silently ignored |
| `Language.txt` message table | External config file | Localized strings 75/158/269/462 | Local text file via `BASE_InitializeMessage` | indexed string table | Out-of-range/missing registration not checked at call sites |
| `g_pItemList` item templates | External data set (DBSrv `ImportItem`) | `Extra`, `Name`, `nPos`, `Grade` lookups | Binary/CSV import into a 6500-entry array | `STRUCT_ITEMLIST` | No local validation; out-of-range reads guarded only by BR-06 for packet indices |
| Item log filesystem | Local file | Forensic audit | `fprintf` on a daily `FILE*` | plain text, `timestamp [IP] str2 str1` | Silent no-op when `fItemLogFile == NULL` |
| DB server (character save) | Indirect | Persistence of the mutated character | not used by this component | — | Deferred to the normal save path; no transactional coupling |
| `SendNotice` fan-out | In-process broadcast | Global success announcement | push to every `USER_PLAY` socket | `MSG_MessagePanel` | O(MAX_USER) per success, no backpressure |
| CRT `rand()` | Library | Success roll | in-process PRNG | `rand() % 115` | Never seeded per request; deterministic sequence per process |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Dispatcher / Command Handler | `switch(std->Type)` → `Exec_MSG_*` | `ProcessClientMessage.cpp:66-311` | Message-type routing; two labels collapse onto one handler |
| Guard-clause validation | Sequential early returns before mutation | `_MSG_CombineItemOdin.cpp:24-43`, `:47-89` | Reject unowned/illegal input without side effects |
| First-match recipe chain | `if / else if` over 11 tuples | `GetFunc.cpp:427-496` | Table-free recipe catalogue; ordering is semantic |
| Table-driven configuration | `g_pOdinRate` / `g_pItemSancRate12` / `...Minus` + `CompRate.txt` | `Basedef.cpp:80-102`, `CReadFiles.cpp:485-567` | Rate tuning without recompilation, hot-reloadable |
| Sentinel encoding | `REF_10..REF_15` = 10/12/15/18/22/27 mapping onto `230..253` bands | `Basedef.h:196-201`, `Basedef.cpp:2123-2138`, `:2252-2268` | Packs sanctification level + gem index into one byte |
| Strategy-by-recipe-code | One `else if` per code with its own success/failure semantics | `:122-579` | Distinct outcomes per craft type without polymorphism |
| Shared pre-roll ("one roll, many outcomes") | `_rand` computed once before dispatch | `:117-119` | Uniform difficulty model across recipes |
| Audit journaling (write-ahead attempt log) | Header + item dump + footer + outcome, all to the daily item log | `:101-115`, plus 16 outcome sites | Post-hoc reconstruction of item flow |
| Three-state acknowledgement | `_MSG_CombineComplete` parm 0/1/2 | `SendFunc.cpp:208-218` | Only channel by which the client learns whether state changed |
| Copy-paste variant family | 11 near-identical `CombineItem*` handlers sharing a preamble | `_MSG_CombineItem*.cpp` | Variant-per-NPC crafting without a shared engine |
| Global singleton state | `pMob`, `pUser`, `temp`, `g_pItemList` accessed directly | `Server.cpp:270-277` | Avoids passing context; makes the unit non-reentrant |
| Goto-based shared exit | `Label_VoltaRef` jumped into from a nested `if` | `:275`, `:319`, `:324` | Avoids duplicating the rollback block |
| Test-mode bypass flag | `|| LOCALSERVER` on every predicate | `:124`, `:224`, `:357-512` | 100 % success in the local/QA world |

Architectural decisions recorded in the code, with their observable consequences:

1. **Recipes live in code, rates live in data.** The split is asymmetric by design: an operator can change a probability but not add a recipe. Adding a recipe requires editing `GetMatchCombineOdin` *and* adding a branch in the handler, because outcomes are not table entries.
2. **Consumption precedes resolution.** The "take first, decide later" ordering (`:92-99` before `:117`) makes every outcome branch a post-hoc description of an already-final state. It is what allows the ten fixed recipes to be eight-line blocks, and it is what makes BR-21 destructive.
3. **The client is trusted for slot choice, distrusted for item content.** `InvenPos` is used verbatim (after bounds checking) to choose where the result appears; item descriptors are byte-verified. This is the right split for a UI-driven craft window, but it leaves the *set* of slots unvalidated (BR-03).
4. **A single global RNG domain (0-99, doubly weighted at the top end) is shared by all recipes**, rather than per-recipe dice. Simpler to reason about per transaction, harder to state as a probability (BR-13).
5. **Notification is layered:** private chat text, structured completion signal, per-slot item pushes, and a global broadcast. Each layer carries different information, and none is sufficient alone.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | Outcome dispatch `:571-579` | `combine == 0` is only handled after the consumption loop; no early guard exists (contrast `_MSG_CombineItemEhre.cpp:47-52`) | Any non-matching or crown-containing request destroys every submitted item and reports a fake roll failure |
| High | Validation loop `:24-43` + consumption `:92-99` | `InvenPos` uniqueness is never checked; consumption is slot-based and idempotent | Ingredient-count bypass: one slot satisfies multi-item requirements on recipes 2, 3, 4 and 11; a single +9 booster can supply 60 rate points |
| High | `:103-113` (`strcat(temp, itemlog)` at `:111`) | Process-global `char temp[4096]` (`Server.cpp:277`) is appended to without reset and without bounds | Log contamination across handlers; overflow into adjacent globals when `temp` is already near-full |
| High | `:270-321` | `rand()%1 == 0` is always true, so the "possible break" is deterministic | Understated player risk; a rate <= 9 attempt on arch-type gear is a guaranteed loss, and the code/comments say otherwise |
| High | `:510-569` | Cape refine never verifies `Equip[15].sIndex != 0` | Seven ingredients consumed to write an `EF_SANC` effect onto an empty slot (`sIndex == 0`); persistent slot corruption |
| High | `:124`, `:224`, `:357-512` | `LOCALSERVER` forces success on every branch; set by a literal DB-address match (`Server.cpp:3584-3588`) | Production misconfiguration silently removes all crafting risk; failure paths unreachable on that host |
| Medium | `GetFunc.cpp:456-457` + `:167-204` | Slot 7 is unconstrained by recipes but consumed and rate-contributing | Silent destruction of arbitrary items; rate ceiling 76 instead of 61; parity rule (BR-07) bypassable |
| Medium | `:206-210` | Post-consumption guard is unreachable (matcher + BR-08 cover the same window) | Dead code in a destructive position; becomes a live item-loss path if the matcher window is ever widened |
| Medium | `:22` | No `Size >= sizeof(MSG_CombineItem)` check; dispatcher validates only `ID` | Undersized packets parsed from stale receive-buffer bytes; protocol contract violated |
| Medium | whole unit | No `AddCrackError` on malformed input | Invalid craft traffic is never accumulated or escalated; no disconnect for repeat offenders |
| Medium | `:33-34`, `:524-525` | Two terminal paths emit no `_MSG_CombineComplete` | Client craft UI can be left waiting; no audit record on the `invPos` path |
| Medium | `:551-553` | Cape-refine success logs the item code of the cleared ingredient slot instead of `Equip[15]` | Audit trail records an empty item for every cape refine |
| Medium | `:283-306` | Fragment conversion overwrites only `stEffect[0]`, retaining the destroyed item's effects 1 and 2 | Fragment items carry spliced attribute data; downstream behaviour depends on undefined combinations |
| Medium | `:543` | Cape refine always passes gem `0` to `BASE_SetItemSanc` | Existing gem counter destroyed on every successful refine (BR-16 preserves it for inventory items) |
| Medium | `:231-252` | `NewSanc` assigned in all four arms, never read | Dead state; compiler warning suppressed only by project settings (Level3, `_CRT_SECURE_NO_WARNINGS`) |
| Medium | `:79-83` | Lv40 unlock requires `Level == 39` exactly and is destructive on failure | A 9.57 % success rate with total loss of seven materials; no refund path exists |
| Medium | `:85-89` | Class check written as `!= MORTAL && != ARCH` instead of `== CELESTIAL` | `ClassMaster == 0` (uninitialised default) passes the cape gate |
| Medium | `:212-222` | `REF_15` penalty arm and `g_pItemSancRate12Minus[3]` are unreachable | Configured knob silently has no effect; operator cannot tell from the code |
| Low | `:92-99` | Result slot is `InvenPos[0..2]`, entirely client-chosen | Result placement is a client contract with no server-side normalisation |
| Low | `:101-115` | Audit lines are unseparated concatenations of item codes | Log parsing requires per-recipe handling; the dump has no framing |
| Low | `:161-353` | 193-line single branch with a `goto` into an `else` block | High cyclomatic complexity at the component's highest-risk logic |
| Low | `:396`, `:400` | Recipe 4 logs `MobName` while every other branch logs `AccountName` | Inconsistent audit identity; cross-referencing requires two fields |
| Low | `:133` vs `:135`, `:255` vs `:264` | Notice/signal ordering differs between recipes 1 and 2 | Cosmetic inconsistency in the client's observed sequence |
| Low | whole unit | Zero static helpers; five responsibilities in one function | Cannot be unit-tested or reasoned about in isolation |
| Low | `:117-119` | `rand()` is never seeded per request (process-wide MSVC CRT state) | Outcomes are sequence-dependent and not reproducible across restarts |
| Informational | `:126-129` | Result id is `Extra` of `Item[0]`'s template — a two-hop link whose target data is absent from the repository | Cannot verify intent; documented ambiguity (BR-22) |

---

## 11. Test Coverage Analysis

**Result: there is no automated test coverage of this component, or of any component in this repository.**

Evidence gathered across the full project root (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`):

| Check | Command / method | Result |
|-------|------------------|--------|
| Test source files | recursive filename scan for `*test*`, `*spec*`, `*fixture*`, `*mock*`, `*assert*` (excluding `Debug`/`Release` build dirs) | **0 matches** |
| Test directories | recursive directory listing to depth 2 | only `Code`, `Code/ClientPatch_v7662`, `Code/DBSrv`, `Code/TMSrv`, `Code/release`, `docs`, plus build output dirs; **no `test`, `tests`, `spec`, `unit`** |
| Test frameworks | content grep for `gtest`, `catch2`, `doctest`, `CppUnit`, `BOOST_TEST`, `TEST_F(` | **0 matches** |
| Assertions | content grep for `assert(` in `*.cpp` | **0 matches** |
| Test projects | `*.sln` / `*.vcxproj` inventory | exactly two C++ projects (`Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj`) plus one `.vcxproj.user`; **no test project** |
| Build automation | scan for `CMakeLists.txt`, `Makefile`, `Jenkinsfile`, `.github`, `azure-pipelines*`, `.gitlab-ci*`, `.travis*`, `appveyor*` | **0 matches**; only the Visual Studio solution `W2PP Code Project.sln` |
| Component registration | `TMSrv.vcxproj` | `_MSG_CombineItemOdin.cpp` compiled at `:128`; no conditional test configuration |
| Test harness in code | grep for stubbing/mocking seams in the handler | none — the unit reads `pMob`/`pUser`/`temp`/`g_pItemList`/`g_pOdinRate` directly with no injection point |

**Coverage table:**

| Component / Area | Unit Tests | Integration Tests | Coverage | Test Quality |
|------------------|-----------|-------------------|----------|--------------|
| `Exec_MSG_CombineItemOdin` (all branches) | 0 | 0 | 0 % | None — no assertions, no fixtures, no manual test scripts in-tree |
| Recipe resolution (`GetMatchCombineOdin`, 11 recipes + 2 reject paths) | 0 | 0 | 0 % | None — 11 tuples and 8 range checks entirely unexercised |
| Descriptor revalidation (`:24-43`) | 0 | 0 | 0 % | None — neither the mismatch path nor the `invPos` bounds path is verified |
| Consumption / destruction (`:92-99`, `:571-579`) | 0 | 0 | 0 % | None — the destructive `combine == 0` behaviour has no regression test |
| Refine rate composition (`:163-222`) | 0 | 0 | 0 % | None — bonuses, penalties and the dead arms are unverified |
| Refine outcome matrix (success / downgrade / break / fragment) | 0 | 0 | 0 % | None — 4 outcomes × 2 protection states untested |
| Roll distribution (`:117-119`) | 0 | 0 | 0 % | None — the non-uniform fold is not covered by any statistical or boundary test |
| Configuration loading (`CReadFiles::ReadCompRate` ODIN block) | 0 | 0 | 0 % | None — including the 0..100 validation and unknown-key handling |
| Cape refine (`:510-569`) | 0 | 0 | 0 % | None — including the empty-slot case |
| Zero-risk verification | 0 | 0 | 0 % | Cannot be tested without a live client; no seam exists |

**What stands in for tests.** Three informal mechanisms exist and are the only verification available: (a) the `_PACKET_DEBUG` compile flag (`TMSrv.vcxproj:61`) which logs `Type/Size` for every received packet through `BASE_CheckPacket` (`Code/TMSrv/Server.cpp:3980-3988`); (b) the item-log audit trail (BR-27), which is the sole post-hoc record of ingredient flow and outcomes; and (c) the `LOCALSERVER` flag, which is the de-facto "test mode" — it forces success so that the output construction paths can be observed without depending on the roll. None of these is a test: (a) and (b) are passive observation, and (c) removes the failure branches that carry the highest risk.

**Risk statement.** The component mutates the player's permanent inventory across 11 distinct flows, with five destructive failure modes, and has no executable specification of any of them. The four highest-severity findings in this report (BR-21's destructive no-match, BR-03's slot aliasing, the deterministic `rand()%1` break, and the `temp` overflow) would each be caught by a single unit test on the matcher and a single test on the consumption loop. Because the matcher is a pure function of `STRUCT_ITEM[8]` and consumes no globals, it is the cheapest possible entry point for a first test — and the fact that none exists is the clearest measure of this component's untested state.

---

## 12. Analysis Notes, Ambiguities and Boundaries

* **Item-template data is absent.** No file under the project root contains `g_pItemList` data; the table is populated at runtime by the DB server's `ImportItem` pipeline (`Code/DBSrv/CReadFiles.cpp:42-43`). Item *names* (413, 542, 772, 3338, 4127, 5134, 5135, 5334-5337, 4032, 3020-3026, 5110-5133) are therefore characterised by index and by the source's own Portuguese comments (`//Pista de runas`, `//Capa`, `//Semente de cristal`, `//Pedra da fúria`, `//Secreta da Agua` ...), not by their in-game names. Recipe-to-name mapping is a documentation gap that cannot be closed from this repository.
* **BR-22's `Extra` direction is ambiguous.** Whether `g_pItemList[Item[0].sIndex].Extra` yields the intended upgrade tier or the base item depends on how the item data populates `Extra` for celestial pairs. The code path is reported exactly; the semantic intent is not asserted.
* **4043's `nPos` is unknown.** BR-18's `protect == 2` analysis assumes 4043 cannot satisfy the slot-2 target test; if it can, `protect == 1` becomes reachable and the "protection requires both bases" conclusion would need revision.
* **The client's interpretation of signal 0/1/2 is inferred**, not verified: no client source is present (only an empty `ClientPatch_v7662` project directory). The mapping in BR-25 is derived from consistent server-side usage across 11 combine handlers.
* **`MaxCarry` is bounded by the character-save path** (30/45/60). If any future code path raises it above `MAX_CARRY` (64), the `invPos` bound check at `:31` would stop being a sufficient array bound; the check does not clamp against `MAX_CARRY` explicitly.
* **No files were modified.** This report is read-only analysis.
