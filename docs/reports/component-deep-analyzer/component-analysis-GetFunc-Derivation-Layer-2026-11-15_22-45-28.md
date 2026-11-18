# Component Deep Analysis Report — GetFunc Derivation Layer

| Attribute | Value |
|-----------|-------|
| Component | GetFunc Derivation Layer |
| Primary location | `Code/TMSrv/GetFunc.cpp` (49,713 bytes, 2,160 lines), `Code/TMSrv/GetFunc.h` (2,677 bytes, 64 lines) |
| Project root | `E:\wyd\test-setup\w2pp` |
| Analyzed scope | Entire project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec` |
| Subsystem | TMSrv game/world server (decompiled C/C++, Win32, MSVC) |
| Language / paradigm | C with C++ classes used only as data containers (`CMob`, `CItem`, `CUser`); free-function procedural module |
| Analysis date | 2026-11-15 22:45:28 |
| Analysis type | Static, read-only. No files modified, no code executed |

---

## 1. Executive Summary

The GetFunc Derivation Layer is TMSrv's shared, stateless-by-design "derivation and projection" module: a single translation unit (36 functions, 36 declarations in the matching header) that all other TMSrv subsystems call to *derive* values from world state and to *assemble* the wire DTOs the client expects. It sits directly beneath the message handlers and the AI/timer layer and directly above `Basedef` (numeric/domain rules), `CMob`/`CItem`/`CUser` (entity state) and `SendFunc`/`CPSock` (transport). The architecture report classifies it as a Query/Derivation component with afferent coupling ~29 and efferent ~7 (`docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:149`).

The module hosts five distinct families of logic that are only loosely related to one another:

1. **Recipe tables for the eight crafting NPC systems** — `GetMatchCombine`, `GetMatchCombineEhre`, `GetMatchCombineTiny`, `GetMatchCombineShany`, `GetMatchCombineAilyn`, `GetMatchCombineAgatha`, `GetMatchCombineOdin`, `GetMatchCombineAlquimia` (lines 33-560). Each is a hardcoded, order-sensitive item-pattern matcher that returns either a boolean, a recipe code, or an accumulated success rate.
2. **Combat and progression math** — `GetParryRate` (562-608) and `GetExpApply` (902-945), both closed-form formulas with clamps and class/quest gates.
3. **World queries and slot allocation** — `GetEmptyAffect`, `GetHide`, `GetInView`, `GetInHalf`, `GetAttribute`, `GetEmptyItemGrid`, `GetEmptyMobGrid`, `GetEmptyMobGridGreat` (610-656, 1257-1273, 1876-2044).
4. **Wire DTO assembly** — `GetCreateMob`, `GetCreateMobTrade`, `GetCreateItem`, `GetAction`, `GetAttack`, `GetAttackArea`, `GetAffect` (947-1255, 1274-1860).
5. **Persistent counter accessors on the `KILL_MARK` pseudo-item** — `GetCurKill`/`SetCurKill`, `GetTotKill`/`SetTotKill`, `GetPKPoint`/`SetPKPoint`, `GetGuilty`/`SetGuilty` (2046-2159), plus the pure pass-through `GetGuild` pair (1862-1874).

The module is **not** read-only, contrary to the architectural shorthand: five functions mutate globals as a side effect of a "get" (`GetTeleportPosition` decrements `KefraTicket`; `GetGuilty` zeroes stored guilt above 50; `GetAttack`/`GetAttackArea` clear `CurrentTarget`; the four `Set*` functions write the kill-mark item). It also performs no bounds validation on the `mob` / `idx` parameters of the DTO builders, deferring all validation to 32 distinct caller files.

### Key findings

| # | Finding | Evidence |
|---|---------|----------|
| F-01 | `GetMatchCombineTiny` can never return a match. Two conditions are tautologies (`!= 4 \|\| != 5`), so the function always returns 0 and the entire Tiny refinement path is unreachable. | `GetFunc.cpp:190`, `GetFunc.cpp:196` |
| F-02 | `GetAttackArea` has exactly one call site and it is unreachable (`if (0 == 1)` guard), making 298 lines of area-attack projection dead code. | `ProcessSecMinTimer.cpp:1945-1953` |
| F-03 | `GetHide` is declared and defined but never called anywhere in the project. | `GetFunc.h:35`, `GetFunc.cpp:627-638`, zero call sites |
| F-04 | `GetGuild(int conn)` is a no-op: it reads the guild item and the guild id into locals and returns nothing. Four call sites invoke it for no effect. | `GetFunc.cpp:1862-1867` |
| F-05 | `GetCreateItem` indexes `g_pGuildZone[zone]` with `BASE_GetVillage()`, which returns 5 when no city matches, while `MAX_GUILDZONE` is 5 (valid indices 0-4) — an out-of-range read. | `GetFunc.cpp:1217-1226`, `Basedef.cpp:4258-4267`, `Basedef.h:148` |
| F-06 | `GetEmptyItemGrid` dereferences caller-supplied coordinates with no bounds check, while its sibling `GetEmptyMobGrid` does validate. Two call sites ignore its return value entirely. | `GetFunc.cpp:1878`, `Server.cpp:7259`, `Server.cpp:9397` |
| F-07 | The empty-mob search validates `pHeightGrid[*ty][*tx]` (the *origin* cell) instead of the candidate `pHeightGrid[y][x]`, so mobs can be placed on blocked cells found by the ring scan. | `GetFunc.cpp:1929`, `1949`, `1969`, `1989`, `2033` |
| F-08 | `GetCreateMob` and `GetCreateMobTrade` disagree on `CreateType` flag derivation: one uses `else if (GuildLevel >= 6)` → 0x40, the other `if (GuildLevel != 0)` → 0x40. | `GetFunc.cpp:1006-1010` vs `1122-1126` |
| F-09 | `GetCreateMobTrade` logs a `%d` conversion for a `char*` argument and returns 0 for non-player mobs; every caller ignores the return value. | `GetFunc.cpp:1078-1082`, `SendFunc.cpp:314` |
| F-10 | The recipe matchers split eligibility across the component boundary: `GetMatchCombineOdin` returns a recipe code, while class/quest/level gating lives in the handler. | `GetFunc.cpp:427-496` vs `_MSG_CombineItemOdin.cpp:47-89` |
| F-11 | Absolute reliance on process-global mutable arrays (`pMob`, `pItem`, `pUser`, `pMobGrid`, `pItemGrid`, `pHeightGrid`, `g_pAttribute`) with zero ownership/const discipline. | `GetFunc.cpp:30`, `Server.h:309-311`, `Basedef.h:2469` |
| F-12 | No automated tests of any kind exist for this or any other TMSrv module. | Section 11 |

### Quantitative profile

| Metric | Value |
|--------|-------|
| Functions defined | 36 (35 exposed behaviours + 2 `GetGuild` overloads) |
| Public symbols declared | 36 (`GetFunc.h:25-60`) |
| Total lines | 2,160 |
| Blank lines | 464 |
| Comment lines | 116 |
| Non-blank, non-comment lines | 1,580 |
| Decision points (if/for/while/switch/case) | ~415 |
| Highest decision count | `GetAttackArea` 63, `GetAttack` 59, `GetTeleportPosition` 43 |
| Afferent call sites (measured) | 204 |
| Afferent caller files (measured) | 32 |
| Efferent modules | 7 (`Basedef`, `CMob`, `CItem`, `CUser`, `ItemEffect`, `Language`, `Server`/`SendFunc`) |
| External I/O | None (no network, DB or file access from this module) |
| Endpoints exposed | None (produces in-process DTOs only; protocol serialization happens in `SendFunc`/`CPSock`) |

---

## 2. Data Flow Analysis

The module has no single entry point; it is a call-through library. Data enters as scalar parameters plus implicit reads of process globals, and leaves either as a scalar return value or as a fully or partially populated message struct.

### 2.1 Flow A — Entity appearance broadcast (spawn / respawn / visual refresh)

```
1. Spawn or state change in ProcessSecMinTimer / ProcessDBMessage / MobKilled
2. SendFunc.SendCreateMob(otherconn) or SendFunc.SendCreateItem(item)
3. GetFunc.GetCreateMob(mob, &sm)          (GetFunc.cpp:947)
   a. reads pMob[mob].MOB (name, score, guild, equip, clan, guild level)
   b. calls GetCurKill / GetTotKill / GetPKPoint / GetGuilty  (KILL_MARK item, slot 63)
   c. packs kill/PK bytes into MobName[12..15]
   d. calls BASE_VisualItemCode / BASE_VisualAnctCode per equip slot (16 slots)
   e. resolves dead-mount flag (selfdead) and mount refinement encoding
   f. calls GetAffect into sm->Affect[32]
   g. applies BrState "hidden player" masking (1808-1744 region)
4. CPSock.AddMessage((char*)&sm, sizeof(MSG_CreateMob))  -> client
```

`GetCreateMobTrade` is the same pipeline with a different destination struct, an extra `pUser[mob].AutoTrade.Title` copy and a non-player rejection at `GetFunc.cpp:1078`.

### 2.2 Flow B — NPC melee attack projection

```
1. ProcessSecMinTimer.BattleProcessor selects Target, checks village/attribute
2. GetFunc.GetAttack(index, Target, &sm)       (GetFunc.cpp:1275)
   a. reads pMob[index] skill bar / skill bar[3] instance type / leader HP ratio
   b. selects skill + Motion (special2 randomization table)
   c. BASE_GetDamage(damage, target Ac, 0)    (Basedef.cpp:1225)
   d. applies elemental Resist scaling
   e. applies ReflectDamage, PvP 30% rule, map-attribute gating (GetAttribute)
   f. applies same-leader / same-guild friendly fire suppression
3. GetFunc.GetInHalf(Target, index)            (GetFunc.cpp:649)
4. MobAttack(index, sm)                        -> GridMulticast
```

### 2.3 Flow C — Player melee hit resolution (afferent into the math rules)

```
1. Exec_MSG_Attack (client -> server)
2. attackdex = Dex/5 (+100 skill, +500 Rsv 0x40)
3. GetFunc.GetParryRate(&pMob[idx].MOB, pMob[idx].Parry, attackdex, pMob[conn].MOB.Rsv)
4. rd = rand()%1000+1; rd < parryrate -> damage -3 (or -4 on Rsv 0x200 && rd<100)
5. GetFunc.GetAttribute(target tile / attacker tile) gates PvP damage
6. damage written to MSG_Attack.Dam[i]
```

### 2.4 Flow D — Crafting / combine resolution

```
1. Exec_MSG_CombineItem* (client -> server, MSG_CombineItem with 8 item slots + inventory positions)
2. anti-cheat: memcmp each submitted item against pMob[conn].MOB.Carry[InvenPos[i]]
3. GetFunc.GetMatchCombine* (m->Item)  -> 0 / recipe code / accumulated rate
4. handler-side eligibility gates (class, quest, coin, exp, level, requested sanc, rune completeness)
5. handler deletes submitted inventory cells (SendItem refresh each)
6. _rand = rand() % 115; if (_rand >= 100) _rand -= 15;
7. success if _rand <= rate (or <= g_pEhreRate[code], g_pOdinRate[code], g_pShanyBase)
8. handler mutates the result item (BASE_SetItemSanc, effect copy, soul assignment)
9. SendClientSignalParm(_MSG_CombineComplete, 0|1|2) + ItemLog
```

### 2.5 Flow E — Teleport / fare resolution

```
1. Exec_MSG_ReqTeleport: rejects tile (491,443)/4 = water-scroll-only zone
2. GetFunc.GetTeleportPosition(conn, &posx, &posy)   (GetFunc.cpp:658)
   a. aligns request to 4x4 tile cluster: xv = x & 0xFFFC, yv = y & 0xFFFC
   b. matches one of ~30 hardcoded source clusters
   c. rewrites *x/*y to destination + rand()%3 (or %2) jitter
   d. returns fare (0 or 700); may decrement pMob[conn].extra.KefraTicket
3. handler compares fare with Coin, charges, credits guild master 50% (GuildImpostoID[4])
4. DoTeleport(conn, posx, posy) if coordinates actually changed
5. Log line with fare
```

### 2.6 Flow F — Kill rewards and PK/guilt bookkeeping

```
1. MobKilled(conn, target, x, y)
2. per-recipient GetExpApply(pMob[conn].extra, mob exp, attacker level, target level)  (GetFunc.cpp:902)
3. party loop repeats GetExpApply for each eligible party member
4. GetCurKill / GetTotKill / GetPKPoint / GetGuilty snapshot at MobKilled.cpp:2111-2116
5. SetCurKill / SetTotKill / SetPKPoint / SetGuilty write back (MobKilled.cpp:2237-2432)
6. GetCreateMob re-broadcast for the killer and the corpse
```

### 2.7 Flow G — Affect (buff) slot acquisition

```
1. SetAffect(conn, skillnum, time, Level) / SetTick(mob, skillnum, Delay, Level)   (Server.cpp:8270, 8317)
2. GetFunc.GetEmptyAffect(mob, affectType)   (GetFunc.cpp:610)
   pass 1: return index of an existing affect with the same Type (refresh)
   pass 2: return first index with Type == 0 (free slot)
   fallback: -1 (table full)
3. caller writes Type/Value/Time/Level into pMob[mob].Affect[i]
4. GetFunc.GetAffect packs the array for the wire (GetFunc.cpp:1174)
```

### 2.8 Flow H — Ground item placement

```
1. Exec_MSG_DropItem validates GridX/GridY < 4096
2. GetFunc.GetEmptyItemGrid(&gridx, &gridy)   (GetFunc.cpp:1876)
3. Server.CreateItem writes pItem[empty] and sends
4. SendCreateItem -> GetFunc.GetCreateItem(itemIdx, &sm)  (GetFunc.cpp:1195)
   special cases: sIndex 3145 (guild flag) and sIndex 5700 (invisible marker)
```

### 2.9 Data transformation summary

```
 client message / timer tick
        |
        v
 [handlers: _MSG_* / ProcessSecMinTimer / MobKilled]
        |  (raw structs: MSG_CombineItem, MSG_Attack, pMob state)
        v
 +---------------------------------------------+
 | GetFunc Derivation Layer                    |
 |  recipe matchers -> code / rate (int)       |
 |  math            -> rate / exp (int)        |
 |  queries         -> slot / bool / attr      |
 |  assemblers      -> MSG_*_ (out-params)     |
 +---------------------------------------------+
        |            ^
        |            | pMob / pItem / pUser / grids / g_pAttribute / g_pItemList / g_pSpell
        v            |
 [SendFunc -> CPSock.AddMessage -> client]        [Basedef: BASE_GetItemAbility, BASE_GetItemSanc,
                                                   BASE_ItemAmount, BASE_VisualItemCode/AnctCode,
                                                   BASE_GetDamage, BASE_GetDistance, BASE_GetVillage,
                                                   BASE_GetGuild]
```

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | Validation + rate derivation | Refinement composition ("compositor"): target must be `nUnique` 41-49 with `Extra > 0`; every additional slot must carry a `+7/+8/+9` soul stone; rate = 1 + configured bonus per stone | `GetFunc.cpp:33-104` |
| BR-02 | Recipe classification | Ehre/Oriharucon recipes: 8 mutually exclusive patterns return codes 1-8, else 0 | `GetFunc.cpp:106-165` |
| BR-03 | Recipe validation (unreachable) | Tiny arch-armor refinement: permanent false result due to tautological type test | `GetFunc.cpp:167-215` |
| BR-04 | Recipe validation (boolean) | Shany merge: two `540/541` +9 items + (`540/541/633`) + four `413` stacks | `GetFunc.cpp:217-259` |
| BR-05 | Recipe + rate derivation | Ailyn duplicate merge: two identical same-grade items + item `1774`, sacrificial soul stones per grade tier | `GetFunc.cpp:261-375` |
| BR-06 | Recipe + rate derivation | Agatha arch merge: arch-type base + mortal/0-type partner at item level >= 4, both +9, four `3140` catalysts | `GetFunc.cpp:377-425` |
| BR-07 | Recipe classification | Odin recipes: 11 patterns (celestial item, +12 runes, rune path, level-40 unlock, four elemental secrets, crystal seed, celestial cape) | `GetFunc.cpp:427-496` |
| BR-08 | Recipe classification | Alquimia recipes: 10 patterns returning codes 0-9, `-1` when unmatched | `GetFunc.cpp:498-560` |
| BR-09 | Combat math | Parry rate = f(defender Dex, additive bonus, attacker Dex, attacker Rsv bits), clamped to [1, 650] | `GetFunc.cpp:562-608` |
| BR-10 | Slot allocation | Affect lookup is refresh-first, then first-free, then `-1` | `GetFunc.cpp:610-625` |
| BR-11 | Spatial predicates | Visibility is a 33x33 grid box; half-view is a 16x16 box; hidden state is affect type 28 | `GetFunc.cpp:627-656` |
| BR-12 | Lookup + mutation | Teleport table: 4x4-aligned source cluster -> fixed destination with 0-2 tile jitter, fare 700 only toward Noatum, Kefra ticket gate | `GetFunc.cpp:658-900` |
| BR-13 | Progression math | Experience multiplier from level gap, with class/quest gates and a 50% arch penalty | `GetFunc.cpp:902-945` |
| BR-14 | DTO assembly | Mob appearance packet: kill/PK bytes stuffed into name tail, equip visual codes, mount encoding, BrState masking, `selfdead` flag | `GetFunc.cpp:947-1070` |
| BR-15 | DTO assembly | Auto-trade mob packet: player-only, PK byte, guild, equipment, title | `GetFunc.cpp:1072-1172` |
| BR-16 | Encoding | Affect encoded as `(Type << 8) | (Time & 0xFF)` after clamping Time to 2,550,000 | `GetFunc.cpp:1174-1193` |
| BR-17 | DTO assembly | Ground item packet: `ItemID = idx + 10000`, height -204 default, guild-flag (3145) rewrite, 5700 suppression | `GetFunc.cpp:1195-1237` |
| BR-18 | DTO assembly | Action packet: speed 2, effect 1, route cleared, positions from mob state | `GetFunc.cpp:1239-1255` |
| BR-19 | Lookup | Map attribute sampling: `g_pAttribute[y/4][x/4]` with 10-bit wrap | `GetFunc.cpp:1257-1273` |
| BR-20 | Projection | Single-target attack packet: skill selection by distance, heal-skill override, damage pipeline, PvP 30% rule, friendly-fire suppression | `GetFunc.cpp:1275-1562` |
| BR-21 | Projection | Area attack packet: same pipeline over 13 enemy slots | `GetFunc.cpp:1564-1860` |
| BR-22 | Lookup | Guild id from item effects `EF_HWORDGUILD` (56) * 256 + `EF_LWORDGUILD` (57) | `GetFunc.cpp:1862-1874` |
| BR-23 | Search | Empty item grid: origin test then 3x3 scan, blocked height 127 | `GetFunc.cpp:1876-1899` |
| BR-24 | Search | Empty mob grid: origin test then rings 1-4 (or 0-29 for "Great") | `GetFunc.cpp:1901-2044` |
| BR-25 | Counter persistence | Kill/PK/guilt counters stored in `pMob[].MOB.Carry[63]` effect bytes with clamps 200 / 32767 / 1-150 / 0-50 | `GetFunc.cpp:2046-2159` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 Refinement Composition Match and Rate Accumulation

**Overview**:
`GetMatchCombine` decides whether a "compositor" (refinement) combine screen contains a legal recipe and, if so, what success rate the caller should roll against. It is the only recipe matcher that computes a probability instead of a recipe code.

**Detailed description**:

The function first scans all eight slots for item index `747`, which acts as a global combine blocker: if any slot holds 747 the function returns 0 regardless of the rest of the layout (lines 37-41). It then requires `item[0]` (the equipment being refined) to be a valid item index strictly inside `0 < target < MAX_ITEMLIST` (6500) and `item[1]` (the "soul stone" selector) to satisfy the same range, but with `<= 0` rejection (lines 43-49). The target must belong to the composable equipment family identified by `g_pItemList[target].nUnique` in the inclusive range 41-49, must have a non-zero `Extra` field (which stores the resulting refined item index, consumed by the handler at `_MSG_CombineItem.cpp:87`), and must not have `EF_MOBTYPE == 3` (lines 51-58). That last test excludes a specific mob-type family from refinement; the raw value 3 is not annotated anywhere in the codebase.

The rate starts at 1. Slots 2 through 7 are then scanned. Empty or out-of-range slots are skipped rather than rejected (lines 65-66), so a recipe may use anywhere from zero to six stones. Every non-empty extra slot must resolve `EF_POS` (effect 17) to a non-zero value, meaning it must be a slot-equippable item rather than a stackable consumable; otherwise the whole combine fails (lines 68-71). Each extra slot must also have `EF_ITEMLEVEL` (effect 87) less than or equal to the target's item level (lines 73-77), which encodes the rule "you cannot feed a higher-tier stone into a lower-tier item". The stone's refinement is read with `BASE_GetItemSanc`, and only the three literal values 7, 8 and 9 are accepted; each contributes a configured weight — `g_pAnctChance[0]`, `[1]`, `[2]` respectively, defaulting to 2, 4 and 10 and overridable through `CompRate.txt` keys `COMPOSITOR/ITEM_+7`, `ITEM_+8`, `ITEM_+9` (lines 79-98; defaults `Basedef.cpp:95`; loader `CReadFiles.cpp:569-585`). Any stone at a different refinement (including 0 or 10+) aborts the whole match with 0.

Because the function accumulates rather than matching a fixed pattern, mixing `+7`, `+8` and `+9` stones is legal and simply sums their weights. The caller subtracts nothing for failed attempts and does not model stone position. The returned rate is compared against a rolled value in the caller as `_rand <= combine`, where `_rand` is drawn as `rand() % 115` and values 100-114 are folded back to 85-99 (`_MSG_CombineItem.cpp:80-84`). The effective probability is therefore `(rate + 1) / 115` for rates up to 84, with the top of the range carrying double mass — a detail that makes high-rate recipes disproportionately likely compared with a uniform 0-99 roll. On success the handler replaces the target with `g_pItemList[target].Extra + (stone - 2441)` and forces refinement 7, i.e. the composited item is always produced at +7 regardless of which stone was consumed (`_MSG_CombineItem.cpp:86-100`).

**Rule workflow**:

```
1. any slot == 747                          -> 0 (blocked)
2. target <= 0 or >= 6500                   -> 0
3. stone (slot 1) <= 0 or >= 6500           -> 0
4. nUnique(target) not in [41..49]          -> 0
5. Extra(target) <= 0                       -> 0
6. EF_MOBTYPE(target) == 3                  -> 0
7. rate = 1
8. for slot 2..7 with 0 < sIndex < 6500:
     EF_POS == 0                            -> 0
     EF_ITEMLEVEL(target) > EF_ITEMLEVEL(s)  -> 0
     sanc == 7 -> rate += g_pAnctChance[0] (2)
     sanc == 8 -> rate += g_pAnctChance[1] (4)
     sanc == 9 -> rate += g_pAnctChance[2] (10)
     otherwise                              -> 0
9. return rate (>= 1)
```

**Inputs / outputs / edge cases**:
Inputs: 8 x `STRUCT_ITEM`, `g_pItemList`, `g_pAnctChance[3]`. Output: 0 = no legal recipe, otherwise a positive success weight. Edge cases: all-empty slots 2-7 yields rate 1 (a legal but low-odds combine); repeated identical stones stack additively with no cap; `nUnique` 50 is excluded although `BASE_GetItemAbilityNosanc` groups 41-50 together (`Basedef.cpp:1508`), so one item family is treated inconsistently between refine and combat paths.

---

### Business Rule: BR-02 Ehre (Oriharucon) Recipe Classification

**Overview**:
`GetMatchCombineEhre` returns a small integer recipe code (1-8) rather than a probability; the caller maps the code to `g_pEhreRate[code]` or to a hardcoded level-bracket rate for code 5, and branches its result mutation on the code.

**Detailed description**:

The function rejects the whole screen if any of the eight slots contains item 747 (lines 110-114), then validates all eight item indices with a *different* predicate than `GetMatchCombine`: it accepts zero (`< 0` only, lines 116-138). This means empty slots are legal input for this family, and the recipe conditions themselves must exclude them. The eight recipes are a strict if/else-if chain, so ordering determines precedence and only the first matching pattern wins (lines 140-163).

The recipes, in evaluation order, are: (1) two items `697` plus a third item whose refinement is at least 9 and which is not `3338` — "Oriharucon package"; (2) two items in the `5110-5133` rune range plus item `413` with amount at least 10 — "Mysterious Stone"; (3) two items in `661-663` plus item `633` at refinement >= 9 — "Spiritual Stone"; (4) two items in `661-663` plus item `3464` at refinement >= 9 — "Amunra Stone"; (5) two items `697` plus item `3338` at refinement <= 8 — "Blessed Purified Refinement"; (6) a mount costume in `2360-2389` plus a mount feed item in `4190-4199`; (7) a mount costume plus item `4899` — mount costume restoration; (8) three items all inside `2441-2444` — Soul melding. Recipe 5 is mutually exclusive with recipe 1 by construction (3338 vs. not-3338), and recipe 1 requires the third item to be refined while 5 requires it unrefined.

Because the function only *classifies*, all eligibility gates are in the handler: coin and experience requirements, class restrictions, refinement caps on the catalyst, and the exact soul-stone permutation. The handler additionally re-derives the outcome rate for recipe 5 from the player's level bracket (30 under 150, 35 under 160, 40 under 170, 50 under 180, 70 under 190, 100 up to the 199 celestial cap) and consumes 5,000,000 experience (`_MSG_CombineItemEhre.cpp:130-174`). Refinement `BASE_GetItemSanc` returns tier tokens 10/12/15/18/22/27 for `REF_10`..`REF_15`, so the literal `>= 9` tests mean "at least +9 plain", and the `<= 8` test on recipe 5 means "below +9".

**Rule workflow**:

```
1. any slot == 747                              -> 0
2. any of slots 0..7 index < 0 or >= 6500       -> 0
3. pattern chain (first match wins):
   (697,697,sanc>=9 & !=3338)                  -> 1  Pacote Oriharucon
   (5110..5133 twice, 413 amount>=10)          -> 2  Pedra Misteriosa
   (661..663 twice, 633 sanc>=9)               -> 3  Pedra Espiritual
   (661..663 twice, 3464 sanc>=9)              -> 4  Pedra Amunra
   (697,697,3338 sanc<=8)                      -> 5  Refinacao Abencoada
   (2360..2389, 4190..4199)                    -> 6  Traje Montaria
   (2360..2389, 4899)                          -> 7  Restaurar Traje
   (2441..2444 in slots 0,1,2)                 -> 8  Soul
   default                                     -> 0
4. return code
```

**Inputs / outputs / edge cases**: Output 0 is ambiguous — it means both "no match" and "blocked". Because slot 3-7 are validated but never referenced by any pattern, a screen can be rejected for an invalid far slot even though the pattern only uses slots 0-2.

---

### Business Rule: BR-03 Tiny Refinement Match (Permanently Unreachable)

**Overview**:
`GetMatchCombineTiny` is intended to validate a refined arch-armor transfer that costs 100,000,000 coins. As written it always returns 0 because of two tautological type comparisons.

**Detailed description**:

The function performs the standard 747 blocker scan and requires slots 0, 1 and 2 to be non-empty valid indices (lines 169-182). It then checks that slot 0 has `EF_MOBTYPE == ARCH` (1) and that its `g_pItemList.Grade` is within 5-8 (lines 184-188), that slots 1 and 2 have `EF_ITEMTYPE` of 4 or 5, that slot 1's grade is 5-8, that all three items share the same `nPos` value and that the shared `nPos` is 64 or 192 (armor slots), and that all three items are refined to at least +9 (lines 199-212). The success return is `g_pTinyBase + EF_ITEMLEVEL(item[1]) * 5`, with `g_pTinyBase` defaulting to 15 (`Basedef.cpp:90`, config key `TINY/CHANCEBASE`).

The critical defect is at lines 190 and 196: `if (BASE_GetItemAbility(&Item[1], EF_ITEMTYPE) != 4 || BASE_GetItemAbility(&Item[1], EF_ITEMTYPE) != 5) return 0;`. A single value can never be both 4 and 5, so the disjunction is always true and the function always returns 0 immediately after this test. The same construct is repeated for slot 2, so even if the first were corrected the second would still force failure. The intended predicate is evidently a conjunction (accept 4 *or* 5), matching the pattern used for `Grade` checks.

Consequently the entire Tiny pipeline is unreachable: the handler `_MSG_CombineItemTiny.cpp` rejects at `if (combine == 0)` with `_NN_Wrong_Combination` (lines 54-59), and the 100,000,000-coin charge, the effect transfer from slot 1 to slot 0 and the forced +7 rewrite can never execute. Nothing else in the project calls `GetMatchCombineTiny`, so the dead branch has no alternate consumer. This is the single most consequential logic defect found in the component because it is silent: no log entry or error is produced, the player simply receives "wrong combination" for a legitimate layout.

**Rule workflow**:

```
1. 747 blocker scan                        -> 0
2. slots 0,1,2 empty/invalid               -> 0
3. EF_MOBTYPE(slot0) != ARCH               -> 0
4. Grade(slot0) not in 5..8                -> 0
5. (EF_ITEMTYPE(slot1) != 4 || != 5)       -> 0   <-- always true
6. (EF_ITEMTYPE(slot2) != 4 || != 5)       -> 0   <-- always true
7. unreachable: return g_pTinyBase + itemlevel(slot1)*5
```

**Inputs / outputs / edge cases**: Effectively output is always 0. The return-rate arithmetic (base + 5 per item level) and the grade/pos/sanc preconditions are unreachable, so they constitute specification-only code.

---

### Business Rule: BR-04 Shany Merge Match

**Overview**:
`GetMatchCombineShany` validates a fixed seven-item layout and returns a *boolean* (1 or 0). The actual probability does not come from this function at all: the handler rolls against `g_pShanyBase`, default 35, configured at `CompRate.txt:SHANY/CHANCEBASE` (`_MSG_CombineItemShany.cpp:89`, `Basedef.cpp:91`).

**Detailed description**:

All seven slots must be occupied and within `0 < sIndex < MAX_ITEMLIST` (lines 219-244), so unlike the Ehre family this matcher requires a fully populated screen. Slots 0 and 1 must each be item `540` or `541` and refined to at least +9; the condition is written as `(a != 540 && a != 541) || sanc < 9` (lines 246-250). Slot 2 must be `540`, `541` or `633` with no refinement requirement. Slots 3 through 6 must all be exactly item `413` (lines 255-256), which is the stackable "Mysterious Stone" used elsewhere for amounts; here the amount is not inspected.

The function returns the literal 1 and nothing else. The handler then treats the result purely as "layout is legal", charges nothing, deletes all submitted cells, and rolls `_rand <= g_pShanyBase` (default 35, so roughly 36/115 ≈ 31% success). On success it grants item `633` via `PutItem` rather than transforming an existing item, which is a different result model from every other combine family. The handler also enforces a class gate before calling the matcher: reject if the character is MORTAL, or is ARCH and below level 355, written as `ClassMaster == MORTAL || ClassMaster == ARCH && Level < 355` (line 45).

Because the return is a boolean, `g_pShanyBase` is the only tunable and it is completely decoupled from the match function; changing the item layout requires editing code, changing the odds requires editing configuration. That split is inconsistent with the rate-accumulating families (Ailyn, Agatha, anct) where the matcher itself returns a rate.

**Rule workflow**:

```
1. 747 blocker                          -> 0
2. slots 0..6 non-empty and < 6500      -> 0 otherwise
3. slot0 in {540,541} and sanc >= 9     -> 0 otherwise
4. slot1 in {540,541} and sanc >= 9     -> 0 otherwise
5. slot2 in {540,541,633}               -> 0 otherwise
6. slots 3..6 == 413 (all four)         -> 0 otherwise
7. return 1   (handler rolls g_pShanyBase = 35)
```

**Inputs / outputs / edge cases**: No refinement or amount check on the four `413` stacks; quantity is irrelevant. Six of the eight combine slots are validated, so a legal layout with an eighth item present is still legal.

---

### Business Rule: BR-05 Ailyn Duplicate Merge Match and Rate

**Overview**:
`GetMatchCombineAilyn` matches two identical same-grade equipments plus catalyst `1774`, then produces a rate of `1 + g_pAilynBase` per sacrificial soul stone, where the stone index is grade-dependent.

**Detailed description**:

After the 747 blocker and non-empty validation of slots 0 through 6 (lines 263-288), the function requires `item[0].sIndex == item[1].sIndex` and equal `Grade` (lines 290-294), and `item[2].sIndex == 1774` (lines 296-297). The base item's `nPos` must be one of the nine equipment position bits `2, 4, 8, 16, 32, 64, 192, 128` (line 301), which are the head/chest/glove/boot/shield/weapon/cape/two-handed bitmask values used throughout the item system. Rate begins at 1 and each *non-empty* slot from 3 to 6 must hold the grade-specific soul stone, adding `g_pAilynBase` (default 10) per stone.

The grade-to-stone mapping is grade 5 -> stone `2441`, grade 6 -> `2442`, grade 7 -> `2443`, grade 8 -> `2444` (lines 304-358). For any other grade the function uses a different rule: slots 4-6 must equal `item[3]` exactly, and `item[3]` itself is unconstrained — including the possibility that it is zero, because empty slots are skipped by `continue` rather than rejected (lines 360-372). That asymmetry creates a legal layout with *no* stone at all: two identical items of grade 1-4 plus catalyst 1774 with slot 3 empty yields rate 1 and the handler proceeds. On success the handler calls `BASE_SetItemSanc(&Carry[ipos1], 10, m->Item[3].sIndex - 2441)` (`_MSG_CombineItemAilyn.cpp:119`); with `Item[3] == 0` the third argument becomes -2441, which `BASE_SetItemSanc` clamps to 0, producing refinement tier 10 with zero "success" stage (`Basedef.cpp:2239-2252`). The handler also charges 50,000,000 coins twice on success (once unconditionally before the roll at line 73 and again at line 124), which is observable behaviour of the consumer, not of this component.

The returned value is a rate in the same 0-99 style as BR-01 and is compared with the same `rand()%115` fold, so each stone adds roughly 8.7 percentage points of success. The handler's `if (rand() % 1 == 0)` at line 101 is always true, so the swap of which duplicate is consumed is deterministic rather than random.

**Rule workflow**:

```
1. 747 blocker                                   -> 0
2. slots 0..6 empty/invalid                      -> 0
3. sIndex(slot0) != sIndex(slot1)                -> 0
4. Grade(slot0) != Grade(slot1)                  -> 0
5. sIndex(slot2) != 1774                         -> 0
6. nPos(slot0) not one of 2,4,8,16,32,64,192,128 -> 0
7. rate = 1
8. grade == 5: slots 3..6 must be 0 or 2441, +g_pAilynBase each
   grade == 6: slots 3..6 must be 0 or 2442, +g_pAilynBase each
   grade == 7: slots 3..6 must be 0 or 2443, +g_pAilynBase each
   grade == 8: slots 3..6 must be 0 or 2444, +g_pAilynBase each
   other     : slots 3..6 must be 0 or equal to item[3], +g_pAilynBase each
9. return rate (1..1+4*g_pAilynBase)
```

**Inputs / outputs / edge cases**: Input `g_pAilynBase` (default 10, config `AILYN/CHANCEBASE`). Edge case: for grades outside 5-8 the zero-stone layout passes; the rate then reflects only the base 1.

---

### Business Rule: BR-06 Agatha Arch-Merge Match and Rate

**Overview**:
`GetMatchCombineAgatha` validates an arch-item merge where an arch-type base item absorbs a mortal-type counterpart of the same slot, refined to +9 on both sides, using four `3140` catalysts. It returns a rate derived from the base constant, the partner's grade and its item level.

**Detailed description**:

Only slots 0 through 5 are validated (lines 379-401), so slots 6 and 7 are ignored entirely. Slot 0 must resolve `EF_MOBTYPE == ARCH`; slot 1 must resolve `EF_ITEMTYPE` to either `MORTAL` (2) or 0 and must have `EF_ITEMLEVEL >= 4` (lines 403-410). Both must share the same `nPos` and both must be refined to at least +9 (lines 412-419). Slots 2, 3, 4 and 5 must all be exactly item `3140` (line 421). Deterministic validation, no randomness in the match itself.

The rate is computed as `g_pAgathaBase + Grade(item[1]) * 5 + (EF_ITEMLEVEL(item[1]) == 5 ? 30 : 1)` (line 424). With the default `g_pAgathaBase` of 15 (`Basedef.cpp:93`, config `AGATHA/CHANCEBASE`), a grade-8 level-5 partner yields 15 + 40 + 30 = 85, and a grade-5 level-4 partner yields 15 + 25 + 1 = 41. The sharp step at item level 5 (30 versus 1) means item level is a near-binary quality gate rather than a linear factor. Because the level term only distinguishes level 5 from everything else, all levels 4, 6, 7 etc. are treated identically — an unusual encoding for what is presumably a tier ladder.

The consumer treats the return as a probability with the same 115-fold roll (`_MSG_CombineItemAgatha.cpp:83-90`) and on success copies the partner's three effects onto the base item, forcing refinement 7, deleting the partner (`_MSG_CombineItemAgatha.cpp:92-103`). Notably the handler deletes slots 0, 2, 3, 4, 5 before the roll but explicitly preserves slot 1 (`if (i == 1) continue;` at line 60), because slot 1 is the donor whose effects are copied after the roll.

**Rule workflow**:

```
1. 747 blocker                                   -> 0
2. slots 0..5 empty/invalid                      -> 0
3. EF_MOBTYPE(slot0) != ARCH                     -> 0
4. EF_ITEMTYPE(slot1) not in {MORTAL, 0}         -> 0
5. EF_ITEMLEVEL(slot1) < 4                       -> 0
6. nPos(slot0) != nPos(slot1)                    -> 0
7. sanc(slot0) < 9 or sanc(slot1) < 9            -> 0
8. slots 2,3,4,5 != 3140                         -> 0
9. return g_pAgathaBase + Grade(slot1)*5 + (itemlevel(slot1)==5 ? 30 : 1)
```

**Inputs / outputs / edge cases**: `g_pAgathaBase` default 15. Edge case: a grade-0 partner (item index 0 rejected earlier, but a valid index with Grade 0 is possible) yields only base + 1 or + 30.

---

### Business Rule: BR-07 Odin Recipe Classification

**Overview**:
`GetMatchCombineOdin` recognises eleven fixed crafting patterns and returns a recipe code 1-11. It is the largest and most order-sensitive recipe table in the component (71 lines, 21 decision points), and it deliberately does *not* enforce class, level or quest preconditions.

**Detailed description**:

The function requires all eight slots to be valid item indices, using the lenient `< 0` test so zero is accepted (lines 429-457). Recipes are then evaluated as a strict if/else-if chain, so the order encodes precedence. Recipe 1 ("celestial item") requires slot 0 to equal `g_pItemList[slot1].Extra`, slot 0 refined >= 9, slot 1 refined to exactly `REF_15` (tier token 27), slot 2 to be `542` or `772`, and slots 3-6 to be `5334`, `5335`, `5336`, `5337` in that order (lines 459-461). Recipe 2 ("+12 and above") is the only recipe with a compound rune condition: either two `413` stacks of at least 10 each, or two `4043` items; a catalyst whose refinement is strictly between `REF_10` and `REF_15`; slots 3-6 each being a rune `5334-5337` or `3338`; and the catalyst's `nPos` restricted to the nine equipment bits (lines 463-466). Recipes 3 through 11 are exact index-vector matches: seven `413` (rune path), `4127,4127,5135,5113,5129,5112,5110` (level-40 celestial unlock), the four elemental secret permutations, `421..427` (crystal seed), and `4127,4127,5135,413,413,413,413` (celestial cape).

Two functional consequences follow from this design. First, because the chain is exact-match and order-sensitive, the same multiset of items in different slots is not equivalent: the rune sequences are positional puzzles, and the ordering of recipes 4-9 means a layout that happens to satisfy an earlier recipe is classified as that earlier recipe. Second, because `sIndex == 0` passes the range validation, a partially filled screen is still evaluated; the exact-match recipes then fail naturally, but recipe 2's disjunction `Item[0].sIndex == 4043 && Item[1].sIndex == 4043` combined with the `413` alternative means an empty screen cannot match either branch, so no false positive arises in practice.

Eligibility is deliberately outside this function. The handler re-checks: for recipe 2, that slots 3-6 contain a complete set of the four elemental runes (rejecting partial sets), that the catalyst is not already `REF_15` and not `EF_MOBTYPE == 3`; for recipe 4, that the player is exactly level 39, is CELESTIAL, and has not already completed the level-40 unlock; for recipe 11, that the player is neither MORTAL nor ARCH and has no cape refined >= 9 equipped (`_MSG_CombineItemOdin.cpp:47-89`). This split is a genuine two-phase contract: matcher = shape, handler = permission.

**Rule workflow**:

```
1. 747 blocker                                    -> 0
2. any of slots 0..7 index < 0 or >= 6500         -> 0
3. code 1  : item0 == Extra(item1), sanc0>=9, sanc1==REF_15(27),
             item2 in {542,772}, items3..6 == 5334,5335,5336,5337
4. code 2  : (item0==413 && amount>=10 && item1==413 && amount>=10) or
             (item0==4043 && item1==4043),
             REF_10 < sanc(item2) <= REF_15,
             items3..6 each in 5334..5337 or 3338,
             nPos(item2) in {2,4,8,16,32,64,192,128}
5. code 3  : items0..6 all == 413
6. code 4  : 4127,4127,5135,5113,5129,5112,5110
7. code 5  : 5125,5115,5111,5112,5120,5128,5119
8. code 6  : 5126,5127,5121,5114,5125,5111,5118
9. code 7  : 5131,5113,5115,5116,5125,5112,5114
10. code 8 : 5110,5124,5117,5129,5114,5125,5128
11. code 9 : 5122,5119,5132,5120,5130,5133,5123
12. code 10: 421,422,423,424,425,426,427
13. code 11: 4127,4127,5135,413,413,413,413
14. default: 0
```

**Inputs / outputs / edge cases**: Output 0 means no match (not an error). Recipe 2 is the only pattern where item *amounts* (`EF_AMOUNT`, 61) matter, and the only one reading `g_pItemList[].Extra` as a lookup key. Note that the celestial-item recipe requires the donor item's `Extra` to point back at the base item, so the table is bidirectional in the item prototype data.

---

### Business Rule: BR-08 Alquimia Recipe Classification

**Overview**:
`GetMatchCombineAlquimia` recognises ten alchemy recipes (codes 0-9) and returns `-1` when no recipe matches. The handler treats `-1` as failure and additionally requires the alchemist class.

**Detailed description**:

Unlike every other matcher, this function returns `-1` for an invalid screen (out-of-range slot) as well as for an unmatched screen, so the caller's single test `combine == -1` covers both. Validation accepts `sIndex == 0` (`< 0` test, lines 505-527). The ten recipes are pure index triples or quadruples: `413+2441+2442` (Sagacidade), `413+2443+2442` (Resistencia), three `4127` (Revelacao), `4127+4127+697` (Recuperacao), `412+2441+2444` (Absorcao), `412+2444+2443` (Protecao), `612` (refined >= 9) `+2441+2442` (Poder), `612+613+614+615` (Armazenagem), `614` (refined >= 9) `+2443+2444` (Precisao), and `615` (refined >= 9) `+697+697+697` (Magia).

Only three recipes inspect refinement, and all three use the plain `>= 9` threshold on slot 0 only. No recipe inspects `Grade`, `nPos`, item level, amount or `EF_MOBTYPE`. The function is therefore the most permissive matcher in the module and the most dependent on the handler for correctness: the handler requires `pMob[conn].MOB.Class == 3` (alchemist) and clears all submitted cells before rolling (`_MSG_CombineItemAlquimia.cpp:47-60`). The probability is not returned by this function at all; the handler computes `chance = (CurrentScore.Special[2] + 1) / 6` and rolls against that (line 82), so the alchemy success rate scales with the character's third special stat and is capped by the 115-fold roll.

The result model is also different: the handler writes `sIndex = 3200 + combine` and zeroes all three effect fields, i.e. recipes 0-9 map directly to item prototypes 3200-3209. That makes this table a pure index-translation layer between ingredient triples and craftable product ids, with the recipe code doubling as an array offset. Because recipe 0 is a valid code, the function's "no match" value must be -1; any caller that tested `== 0` (as all the other handlers do) would misclassify the first recipe.

**Rule workflow**:

```
1. 747 blocker                                  -> 0
2. any of slots 0..7 index < 0 or >= 6500       -> -1
3. 413,2441,2442                                -> 0
4. 413,2443,2442                                -> 1
5. 4127,4127,4127                               -> 2
6. 4127,4127,697                                -> 3
7. 412,2441,2444                                -> 4
8. 412,2444,2443                                -> 5
9. 612(sanc>=9),2441,2442                       -> 6
10. 612,613,614,615                             -> 7
11. 614(sanc>=9),2443,2444                      -> 8
12. 615(sanc>=9),697,697,697                    -> 9
13. default                                     -> -1
```

**Inputs / outputs / edge cases**: `-1` vs `0` is the critical contract distinction. Note the 747 blocker returns 0 (not -1), so a blocked screen is indistinguishable from recipe 0 for this function — an inconsistency that only the 747-holding item index prevents from mattering.

---

### Business Rule: BR-09 Parry Rate Derivation

**Overview**:
`GetParryRate` computes a defender's parry chance in per-mille against a specific attacker, combining a soft Dex curve, an additive bonus, the attacker's Dex contribution and three attacker `Rsv` flag bonuses.

**Detailed description**:

The additive parameter (`add`, supplied by the caller as `pMob[Target].Parry`) is clamped to 0-100 (lines 564-568). The defender's `Dex` is read from `CurrentScore` and capped at 1000 for the linear term (lines 570-573). Two piecewise terms extend the curve beyond the cap: `parryrate1 = clamp(Dex - 1000, 0, 2000)` contributing at 1/4 weight, and `parryrate2 = max(Dex - 3000, 0)` contributing at 1/8 weight with no upper clamp (lines 575-586). The raw rate is `targetdex/2 + add + parryrate1/4 + parryrate2/8 - attackerdex` (line 588). This produces a design where the first 1000 Dex give 0.5 per-mille each, Dex 1000-3000 give 0.25 per-mille each, and Dex above 3000 give a further 0.125 — a deliberate diminishing-returns curve that avoids a hard cap at 1000 while still saturating.

The attacker's `Rsv` bitmask then adds fixed bonuses: bit `0x20` (defined `RSV_HASTE`) adds 100, bit `0x80` (`RSV_BLOCK`) adds 50, and bit `0x200` — which has no named constant in `Basedef.h` — adds 50 (lines 590-599). The function is therefore not symmetric: it takes the *defender's* mob and Dex but the *attacker's* Rsv flags. The final rate is clamped to the inclusive range [1, 650] (lines 601-605), i.e. a floor of 0.1% and a ceiling of 65%, so parry can never be fully deterministic and can never exceed two thirds.

Consumers roll `rd = rand() % 1000 + 1` and compare `rd < parryrate` (`_MSG_Attack.cpp:1408-1416`, `ProcessSecMinTimer.cpp:1756-1764`), then optionally scale the rate to 30% for skill indexes 79 and 22 *before* the roll. On a successful parry the damage is replaced by the sentinel `-3`, or `-4` when the defender also carries `Rsv 0x200` and the roll was under 100 — meaning the high-value "perfect block" only exists inside the first 10% of the roll. The header declares the fourth parameter as `attackaccuracy` while the implementation names it `attackrsv` (`GetFunc.h:33` vs `GetFunc.cpp:562`), confirming through naming drift that the parameter changed semantics from accuracy to a flag mask at some point.

**Rule workflow**:

```
add        = clamp(add, 0, 100)
targetdex  = min(Dex, 1000)
pr1        = clamp(Dex - 1000, 0, 2000)
pr2        = max(Dex - 3000, 0)
rate       = targetdex/2 + add + pr1/4 + pr2/8 - attackerdex
if (rsv & 0x20)  rate += 100
if (rsv & 0x80)  rate += 50
if (rsv & 0x200) rate += 50
return clamp(rate, 1, 650)
```

**Inputs / outputs / edge cases**: Inputs: defender `STRUCT_MOB*`, additive bonus, attacker Dex (already divided by 5 and augmented by the caller), attacker `Rsv`. Output: per-mille in [1,650]. Edge case: with attacker Dex high enough the expression goes deeply negative and the floor of 1 applies, so a weak defender always retains 0.1% parry.

---

### Business Rule: BR-10 Affect Slot Lookup (Refresh-First Allocation)

**Overview**:
`GetEmptyAffect` returns a slot index for a new affect of a given type: an existing slot holding the same type takes priority, then the first unused slot, otherwise `-1`.

**Detailed description**:

The rule is a two-pass scan over the 32-entry `pMob[mob].Affect` array. Pass one looks for `Type == type` and returns that index immediately (lines 612-616), which implements "refresh in place" semantics: a second application of the same buff does not consume a second slot. Pass two looks for `Type == 0` and returns the first free index (lines 618-622). If both passes fail, `-1` is returned and the caller must refuse the effect.

The rule operates on `pMob[mob]` with no validation of `mob`, unlike the counter accessors which explicitly guard `mob <= 0 || mob >= MAX_USER`. Callers are `SetAffect` and `SetTick` (each of which validates the mob indirectly through other paths), plus direct uses in the item and quest handlers. The `type` parameter is compared against `StructureAffect.Type`, an unsigned char, so a caller passing a value > 255 would silently never match; the callers pass `g_pSpell[skillnum].AffectType` / `TickType`, which are ints read from the spell table.

The consequence for the game rules is that affect capacity is per-type-unique but not per-type-limited: because pass one returns the existing slot for any type already present, a mob can never hold two instances of the same affect type simultaneously, and can hold at most 32 distinct types. The caller `SetAffect` then compares the new duration with the stored one and keeps the longer (`Server.cpp:8296-8300`), so refresh semantics combine with this allocation rule to produce "same type always merges, longer duration wins".

**Rule workflow**:

```
for i in 0..31: if Affect[i].Type == type      -> return i
for i in 0..31: if Affect[i].Type == 0         -> return i
return -1
```

**Inputs / outputs / edge cases**: Output `-1` when all 32 slots are occupied by distinct non-zero types. No `mob` bounds check inside the function; no `type` range check.

---

### Business Rule: BR-11 Spatial Visibility and Hidden-State Predicates

**Overview**:
Three tiny predicates define what "in view" means for entity updates: `GetInView` (33x33 box), `GetInHalf` (16x16 box) and `GetHide` (presence of affect type 28). Only the first two are used.

**Detailed description**:

`GetInView` returns TRUE when the two mobs' `TargetX/TargetY` differ by at most `VIEWGRIDX`/`VIEWGRIDY` (both 33) in either axis, using a four-comparison box test (lines 640-647). `GetInHalf` is identical with `HALFGRIDX`/`HALFGRIDY` (16) and is used to validate melee reach after an attack resolves (`ProcessSecMinTimer.cpp:1670`). Because the constants are grid-cell sizes rather than half-widths, the effective boxes are 67x67 and 33x33 cells respectively, not 33x33 and 16x16 — the naming suggests half-extents but both bounds are applied symmetrically around the target.

`GetHide` returns TRUE if any of the 32 affects has `Type == 28` and FALSE otherwise, with an explicit guard rejecting `mob <= 0 || mob >= MAX_USER` (lines 627-638). The guard restricts it to player slots. Notably, no file in the project calls `GetHide`: hiding is instead implemented by other means (the `RSV_HIDE` 0x10 flag and affect processing elsewhere), leaving this function as an unused oracle.

None of the three validates that `a`/`b` are within `MAX_MOB`; `GetInView`/`GetInHalf` index `pMob[a]` and `pMob[b]` directly. All callers are in handlers that have already validated the mob identity from the client message or from the entity list, so the risk is contained by convention rather than by contract.

**Rule workflow**:

```
GetInView(a,b): |a.x - b.x| <= 33 and |a.y - b.y| <= 33
GetInHalf(a,b): |a.x - b.x| <= 16 and |a.y - b.y| <= 16
GetHide(m):     m in (0, MAX_USER) and any Affect[i].Type == 28
```

**Inputs / outputs / edge cases**: `GetInView`/`GetInHalf` read `TargetX/TargetY` (the logical position), not `NextX/NextY`, so movement in flight is not accounted for.

---

### Business Rule: BR-12 Teleport Destination and Fare Resolution

**Overview**:
`GetTeleportPosition` maps the player's current 4x4-aligned tile cluster to a fixed destination cluster, rewrites the coordinates in place with 0-2 tiles of jitter, and returns the fare (0 or 700). It is the only function in the component that mutates persistent player state for a non-combat reason.

**Detailed description**:

The function aligns the requested coordinates to a 4x4 cluster (`& 0xFFFC`) and compares against roughly thirty hardcoded source clusters (lines 660-837). Each match rewrites `*x`/`*y` to a destination base plus `rand() % 3` (occasionally `rand() % 2`), producing destinations that are always inside a cluster. Only three branches charge a fare: Armia-to-Noatum (2116,2100), Azran-to-Noatum (2480,1716) and Erion-to-Noatum (2456,2016) each set `Charge = 700` (lines 664-684). All other transitions are free, including the reverse Noatum-to-city routes. There is no fall-through default branch that clears the coordinates; if no cluster matches, the function returns 0 with `*x`/`*y` untouched, which the handler interprets as "nothing to do" because it compares the coordinates afterwards (`_MSG_ReqTeleport.cpp:54`).

Three conditional branches encode additional rules. Azran-to-vale (2548,1740) only produces a destination if the player wears item `3916` in equip slot 13, i.e. the cape is a literal ticket for that transition (lines 800-807); otherwise the coordinates stay unchanged and no teleport happens. Noatum-to-RvR (1056,1724) branches on the global `RvRState`: when it equals 2, clan 7 spawns at the blue camp and clan 8 at the red camp, otherwise the player lands at the neutral point (lines 845-866). Finally, the Kefra desert entry (2364,3892) requires `extra.KefraTicket > 0`, decrements the ticket, rewrites the destination, and sends a client message formatted with `_DN_CHANGE_COUNT` (message id 420) showing the remaining count (lines 868-878). The neighbouring Kefra branches are asymmetrical: desert-to-Kefra requires `KefraLive != 0` (line 881), while the Kefra return branch has no gate at all (line 888).

The fare is returned, not charged: the handler compares it with `MOB.Coin`, deducts it, and credits 50% to the guild master of zone 4 if the master is online and the guild matches (`_MSG_ReqTeleport.cpp:38-51`). This is the only place where the teleport table's `Charge` value has any effect, so a fare of 700 means "700 coins" and nothing else in the codebase documents the unit.

**Rule workflow**:

```
1. xv = x & 0xFFFC ; yv = y & 0xFFFC
2. match ~30 hardcoded (xv,yv) clusters in order
3. per branch:
     optional RvRState / Clan condition   -> two destination sets
     optional Equip[13] == 3916 condition -> no-op if absent
     optional KefraTicket > 0             -> decrement + client message
     optional KefraLive != 0              -> destination
     rewrite *x,*y = base + rand()%2 or %3
     maybe set Charge = 700
4. return Charge (0 or 700) with coordinates possibly unchanged
```

**Inputs / outputs / edge cases**: Inputs: conn (for Clan, Equip[13], extra.KefraTicket), in/out x/y. Outputs: mutated coordinates, fare. Edge cases: unknown clusters are silent no-ops; the Kefra ticket decrement happens even if the subsequent teleport is blocked by insufficient coins, because it occurs inside this function before any payment check.

---

### Business Rule: BR-13 Experience Application and Level-Gap Scaling

**Overview**:
`GetExpApply` converts a killed mob's experience value into the experience a specific attacker receives, applying class/quest eligibility gates, an arch penalty and a level-gap multiplier.

**Detailed description**:

The function takes the attacker's `STRUCT_MOBEXTRA` **by value** (a large struct copy), the raw experience, and both levels. For ARCH characters it first applies two quest gates: an attacker at level 354 or above with `QuestInfo.Arch.Level355 == 0` receives zero experience, and level 369 or above with `Level370 == 0` likewise (lines 904-911). If the gates pass, experience is halved (`exp = (int)(exp * 0.50)`, line 912). For CELESTIAL characters the analogous gates are level 39 without `QuestInfo.Celestial.Lv40` and level 89 without `Lv90`; unlike ARCH, no multiplier is applied when the gates pass (lines 914-921).

A second, independent block sets `attacker = MAX_LEVEL` (399) for the three celestial-tier classes (CELESTIAL, SCELESTIAL, CELESTIALCS) whenever the incoming experience is positive (lines 923-924). This is a penalty mechanism: the level-gap ratio is computed against a nominal level 399 attacker, which for a level 150 celestial yields roughly `(target+1)*100/400` — a heavy reduction. Because the block sits after the ARCH/CELESTIAL gate chain and uses `if` rather than `else if`, a CELESTIAL character can be both gated and then subjected to the level substitution.

The ratio itself: an early return passes experience through unchanged when `target > MAX_LEVEL + 1` (i.e. mob level above 400), `attacker < 0` or `target < 0` (lines 926-927); note that a high-level mob above 400 therefore grants full, unscaled experience. Otherwise both levels are incremented (avoiding division by zero, and shifting the comparison to 1-based) and `multiexp = (target * 100) / attacker`. Two corrections follow: if the ratio is below 80 and the (incremented) attacker is at least 50, the ratio is transformed to `2*multiexp - 100`, which turns a 79% ratio into 58% and a 50% ratio into 0%; if the ratio exceeds 200 it is capped at 200 (lines 934-937). Negative results are floored to 0. The final experience is `(exp * multiexp + 1) / 100`, where the `+1` avoids a zero result for very small products.

The caller applies this per recipient: once for the killer and once per eligible party member (`MobKilled.cpp:214, 255, 350, 444, 536`). Because the struct is passed by value and the function is pure with respect to globals, it is one of the few functions here that could be evaluated out of process; its only inputs are the parameter copy and the `MAX_LEVEL` constant.

**Rule workflow**:

```
if (ClassMaster == ARCH && exp > 0):
     level >= 354 && !Arch.Level355 -> 0
     level >= 369 && !Arch.Level370 -> 0
     exp = exp * 0.50
else if (ClassMaster == CELESTIAL && exp > 0):
     level >= 39 && !Celestial.Lv40 -> 0
     level >= 89 && !Celestial.Lv90 -> 0

if (ClassMaster in {CELESTIAL, SCELESTIAL, CELESTIALCS} && exp > 0):
     attacker = MAX_LEVEL (399)

if (target > 400 || attacker < 0 || target < 0) -> return exp
attacker++ ; target++
multiexp = target*100 / attacker
if (multiexp < 80 && attacker >= 50) multiexp = multiexp*2 - 100
else if (multiexp > 200) multiexp = 200
if (multiexp < 0) multiexp = 0
return (exp * multiexp + 1) / 100
```

**Inputs / outputs / edge cases**: Input `int exp` truncates the mob's `long long` experience at the call site. Edge cases: mob level 401+ gives full experience; attacker level 0-1 gives the 200% cap; a same-level kill gives ~100% for mortals; ARCH always receives at most 50% before the gap multiplier.

---

### Business Rule: BR-14 Mob Appearance DTO Assembly

**Overview**:
`GetCreateMob` builds the `MSG_CreateMob` packet: identity, position, score, guild, 16 equipment visuals with anti-cheat refinement encoding, 32 affects, PK/kill counters smuggled in the name tail, and a BrState-based identity mask.

**Detailed description**:

The packet is zeroed, typed `_MSG_CreateMob`, scenario `ESCENE_FIELD` (30000) and stamped with `CurrentTime` (lines 949-983). The mob name is copied with a 16-byte `strncpy`; the last four bytes are then overwritten for player mobs (`mob < MAX_USER`): byte 13 receives `GetCurKill`, bytes 14/15 receive the low and high bytes of `GetTotKill`, and byte 12 receives the PK point — except that the PK point is forced to 0 when `GetGuilty` returns a positive value (lines 955-976). This is a deliberate side-channel: the client reads PK/chaos and kill counters out of the name buffer rather than from dedicated fields. The consequence is that a 12-character name is the effective maximum before truncation becomes visible.

Equipment visuals are produced per slot with `BASE_VisualItemCode(item, i)` and `BASE_VisualAnctCode(item)` (lines 1022-1028). Slot 14 (mount) gets two special rules: if the visual code falls in `2360-2389` and the mount's `stEffect[0].sValue <= 0` the visual is zeroed and `selfdead` is set (lines 1030-1037); otherwise the mount's refinement is taken from `stEffect[1].cEffect / 10`, clamped to 0-13, shifted left 12 and OR-ed into the visual code (lines 1039-1055). This manual encoding compensates for the fact that `BASE_VisualItemCode`'s own mount branch is unreachable — its guard reads `sIndex >= 2360 && sIndex < 2360`, a contradiction (`Basedef.cpp:6607`), so the caller reinvents the encoding.

Two further rules modify identity. For non-player mobs (`mob >= MAX_USER`) the `Ac` score field is repurposed as a summon marker: 0 when `Clan == 4`, 1 otherwise (lines 995-1002). `CreateType` is built from guild level: bit 0x80 for guild level 9, else bit 0x40 for guild level 6 or above (lines 1006-1010). Finally, when `BrState != 0` and the mob stands inside the rectangle x∈[2604,2648], y∈[1708,1744], the name is erased and replaced with six question marks, equip slot 15 is cleared and the guild is zeroed (lines 1060-1067) — an anonymity rule tied to the battle-royale state machine, hardcoded to one map region.

`selfdead` (return value 1/0) is the only error-like signal. Every caller in the project ignores it: `SendFunc.cpp:306`, `MobKilled.cpp:57/98/2423/2436`, `Server.cpp:2829/2984/3221/3348/4486/7347/7828`, `_MSG_Attack.cpp:1045/1265/1379/1386`, `_MSG_Quest.cpp:2584`, `imple.cpp:562/770/783`, `ProcessDBMessage.cpp:992`, `_MSG_MessageWhisper.cpp:361/410/456/461/525`, `_MSG_InviteGuild.cpp:95` — none inspects the return.

**Rule workflow**:

```
1. zero packet; Type=_MSG_CreateMob; Size=sizeof; ID=ESCENE_FIELD; ClientTick=CurrentTime
2. MobName = pMob[mob].MOB.MobName (16 bytes)
3. if mob < MAX_USER:
     MobName[13]=curkill; [14]=totkill&0xFF; [15]=totkill>>8
     chaos = pkpoint; if guilty > 0 -> chaos = 0; MobName[12] = chaos
4. PosX/Y, MobID=mob, Score=CurrentScore, Guild, GuildMemberType=GuildLevel
5. if GuildDisable==1 -> Guild=0, GuildMemberType=0
6. if mob >= MAX_USER -> Score.Ac = (Clan==4 ? 0 : 1)
7. CreateType = 0x80 if GuildLevel==9 else 0x40 if GuildLevel>=6
8. Hold = extra.Hold ; Tab = pMob[mob].Tab (26 bytes)
9. for each of 16 equip slots:
     Equip[i] = BASE_VisualItemCode(item,i); AnctCode[i] = BASE_VisualAnctCode(item)
     slot 14 && 2360<=code<2390 && sValue<=0 -> Equip[14]=0, selfdead=1
     slot 14 && 2360<=code<2390 -> Equip[14] |= clamp(cEffect/10,0,13) << 12
10. GetAffect(sm->Affect, pMob[mob].Affect)
11. if BrState!=0 && tile in [2604..2648]x[1708..1744]:
      name="??????"; Equip[15]=0; Guild=0
12. return selfdead
```

**Inputs / outputs / edge cases**: No `mob` bounds validation (`pMob` is `MAX_MOB` = 25000). Guild is cleared but `GuildMemberType` is not cleared by the BrState mask, only by `GuildDisable`.

---

### Business Rule: BR-15 Auto-Trade Mob DTO Assembly

**Overview**:
`GetCreateMobTrade` builds `MSG_CreateMobTrade`, the variant used when the observed player is in auto-trade mode: player-only, with the shop title appended.

**Detailed description**:

The function is a near-copy of BR-14 with four behavioural differences. First, non-player mobs are rejected with a log line and a `return 0` after the name copy (lines 1078-1082); the check is written as `mob >= MAX_USER` *before* the `if (mob < MAX_USER)` block that follows, so the second block is provably always true. Second, the PK byte is written without the guilt override — `GetGuilty` is never consulted, so a guilty player's chaos value is still transmitted in this packet (lines 1097-1100). Third, `CreateType` uses `if (GuildLevel == 9) 0x80` followed by an independent `if (GuildLevel != 0) 0x40`, so a guild level 9 member gets both bits, whereas BR-14 gives only 0x80 (lines 1122-1126). Fourth, the packet carries `Desc` copied from `pUser[mob].AutoTrade.Title` with a 23-byte cap (line 1167).

The equipment loop, mount rules, affect packing and `selfdead` computation are duplicated verbatim from BR-14 (lines 1130-1169 versus 1022-1058). The `Ac` repurposing for non-players is present but dead, because non-players were already rejected at line 1078.

The return value is again the `selfdead` flag and again ignored by all three call sites (`SendFunc.cpp:314`, `_MSG_SendAutoTrade.cpp:115`, `_MSG_MessageWhisper.cpp:532`). The log line `"err,getcreatemob request by non player %d"` passes `pMob[mob].MOB.MobName` (a `char*`) where `Log` expects `(char* str1, char* str2, unsigned int ip)` (`Server.cpp:6701`), so the `%d` is never satisfied by an integer argument — the format string and argument list disagree.

**Rule workflow**:

```
1. Type=_MSG_CreateMobTrade; MobName copied
2. if mob >= MAX_USER: Log("...%d", MobName, 0); return 0
3. (mob < MAX_USER, always true) -> write PK byte, kill bytes
4. position, size, ID, MobID, ClientTick, Score memcpy, Guild
5. GuildDisable -> Guild=0
6. CreateType: 0x80 if GuildLevel==9 ; 0x40 if GuildLevel!=0
7. 16 equip visuals + mount rules (identical to BR-14)
8. Tab (26), Desc = pUser[mob].AutoTrade.Title (23)
9. GetAffect ; return selfdead
```

**Inputs / outputs / edge cases**: Guaranteed to return 0 for mob indices >= 1000 after emitting a malformed log. No `pUser[mob]` bounds concern because non-players are rejected first.

---

### Business Rule: BR-16 Affect Wire Encoding

**Overview**:
`GetAffect` flattens 32 `STRUCT_AFFECT` entries into 32 `unsigned short` wire values, packing type in the high byte and a truncated time in the low byte.

**Detailed description**:

For each of the 32 affect slots the function reads `Type` (unsigned char) and `Time` (unsigned int), clamps time to 2,550,000, and emits `(type << 8) + (time & 0xFF)` (lines 1176-1191). The low byte is therefore `Time mod 256` seconds, which is an intentional protocol compression: only the sub-256-second remainder is transmitted, and the client is expected to reconcile durations against its own expectation. There is no case for `Type == 0` — the commented-out `if (type == 0) continue;` at lines 1181-1182 shows the skip was deliberately disabled, so empty slots are still encoded (as `0x00 | (0 & 0xFF)` = 0 when both fields are zero).

Because the packing uses arithmetic `+` rather than a bitwise OR, and `type << 8` occupies bits 8-15 while `time & 0xFF` occupies 0-7, the two fields cannot collide; the choice of `+` is stylistically inconsistent but functionally equivalent. The clamp constant 2,550,000 is not named and does not correspond to any other timeout in the codebase.

The function is used in three places: inside BR-14 and BR-15, and directly by `SendFunc.cpp:1159` for a visual-update packet. Those callers pass `sm->Affect`, an `unsigned short[32]`, matching the `MAX_AFFECT` array length in the message structs.

**Rule workflow**:

```
for i in 0..31:
    type  = affect[i].Type
    value = min(affect[i].Time, 2550000)
    out[i] = (type << 8) + (value & 0xFF)
```

**Inputs / outputs / edge cases**: Durations beyond 255 seconds alias within the transmitted byte; only the low 8 bits survive, so a 300-second buff and a 44-second buff encode identically in the packet's time field.

---

### Business Rule: BR-17 Ground Item DTO Assembly and Guild-Flag Special Case

**Overview**:
`GetCreateItem` builds `MSG_CreateItem` for a ground item slot, assigning a synthetic item id, a default height, and two hardcoded special cases for guild flags (`3145`) and an invisible marker (`5700`).

**Detailed description**:

The packet is typed `_MSG_CreateItem`, sized, and assigned `ESCENE_FIELD`; position, rotation, the item struct and state are copied from `pItem[idx]`; `ItemID` is `idx + 10000`; and `Height` defaults to -204 (lines 1197-1213), which as an unsigned char field becomes 52. `idx` is not validated against `MAX_ITEM` (5000).

The first special case triggers on `sIndex == 3145`: the zone is resolved with `BASE_GetVillage(x, y)`, the item index is rewritten to `3145 + g_pGuildZone[zone].Victory`, the state is forced closed, effect 0 is set to `EF_HWORDGUILD` (56) with a value of `(ChargeGuild + (ServerIndex << 12)) >> 8`, and effect 1 is set to `EF_LWORDGUILD` (57) with `ChargeGuild` (lines 1215-1227). This encodes the owning guild and the server index into the flag item so the client can render the correct banner, and it makes the visual depend on the current war victory state. `BASE_GetVillage` returns 0-4 for city zones and 5 when the coordinate matches no city (`Basedef.cpp:4258-4267`); `MAX_GUILDZONE` is 5, so the fallback index reads one element past `g_pGuildZone`.

The second special case triggers on `sIndex == 5700` and returns immediately (lines 1229-1230), leaving `Height` at -204 and `State` as whatever `pItem[idx].State` held — the packet is emitted but the item is not given the "open" treatment below. For all other items, if the stored state equals `STATE_OPEN` (1), the real item height is copied and `Create` is explicitly set to 0 (lines 1232-1236). Because the packet is not zeroed at entry (unlike BR-14/BR-15), fields not explicitly written — `Create` for non-open items — carry whatever the caller's stack holds; `SendCreateItem` does memset its local before calling (`SendFunc.cpp:322-325`), but `Server.cpp:5927/5975/6051/6100` pass stack structs whose initialization must be verified per site.

**Rule workflow**:

```
1. ID=ESCENE_FIELD, Size, Type=_MSG_CreateItem
2. GridX/Y = pItem[idx].PosX/PosY ; ItemID = idx + 10000
3. Rotate, Item, State copied ; Height = -204
4. if sIndex == 3145:
     zone = BASE_GetVillage(x,y)
     Item.sIndex = 3145 + g_pGuildZone[zone].Victory
     State = STATE_CLOSED
     effect0 = {56, (ChargeGuild + (ServerIndex<<12))>>8}
     effect1 = {57, ChargeGuild}
5. if sIndex == 5700 -> return
6. if State == STATE_OPEN -> Height = pItem[idx].Height ; Create = 0
```

**Inputs / outputs / edge cases**: `zone == 5` reads outside `g_pGuildZone`; `idx >= 5000` reads outside `pItem`; the 5700 branch emits a packet that still carries `ItemID`/position, so the client sees an item it cannot interact with.

---

### Business Rule: BR-18 Action / Movement DTO Assembly

**Overview**:
`GetAction` builds `MSG_Action` for a mob's projected movement step: target tile, route cleared, fixed speed 2 and effect 1.

**Detailed description**:

The function writes `ID = mob` — the only packet builder in this component that uses the mob index as the scenario id instead of `ESCENE_FIELD` — plus `ClientTick = CurrentTime`, `Size`, `Type = _MSG_Action`, `TargetX/TargetY` from the parameters, `Route[0] = 0`, `Speed = 2`, `Effect = 1`, and `PosX/PosY` from the mob's current tile (lines 1241-1254). The route array is only cleared at index 0, so a caller reusing a dirty buffer would transmit stale route bytes; all observed callers pass fresh or zeroed structs.

`Effect = 1` is documented in the message struct as "teleporting" and 0 as "walking" (`Basedef.h:1702`), yet the movement callers immediately overwrite it to 0 (`ProcessSecMinTimer.cpp:1970`) or leave the default (`Server.cpp:7594`). The speed is likewise overwritten by `BASE_GetSpeed` in the timer path (`ProcessSecMinTimer.cpp:1969`), so the fixed values are defaults rather than rules. What the function genuinely owns is the position pair and the `_MSG_Action` framing.

The function validates nothing: `mob` is indexed directly into `pMob`, and `targetx/targety` are trusted. All nine call sites are in server-side movement code and handlers that have already bounded the mob.

**Rule workflow**:

```
sm.ID = mob ; sm.ClientTick = CurrentTime ; sm.Size = sizeof ; sm.Type = _MSG_Action
sm.TargetX/Y = params ; sm.Route[0] = 0
sm.Speed = 2 ; sm.Effect = 1
sm.PosX/Y = pMob[mob].TargetX/TargetY
```

**Inputs / outputs / edge cases**: No validation; `ID = mob` diverges from the `ESCENE_FIELD` convention used by the other five builders.

---

### Business Rule: BR-19 Map Attribute Sampling

**Overview**:
`GetAttribute(x, y)` returns the one-byte map attribute at a world tile by dividing coordinates by 4 and wrapping with a 10-bit mask.

**Detailed description**:

The function rejects coordinates outside `0..MAX_GRIDX`/`0..MAX_GRIDY` (4096) and then indexes `g_pAttribute[y/4][x/4]` after masking each quotient with `0x3FF` (lines 1257-1272). The attribute array is declared `unsigned char[1024][1024]` (`Basedef.h:2469`), so the mask is a deliberate defence: any quotient above 1023 wraps rather than overflowing. The bounds test itself is off by one — it accepts `x == 4096`, whose quotient 1024 is then masked to 0, silently returning the attribute of tile 0.

The attribute map is cell-based at 4x4 tile granularity, so a whole 4x4 cluster shares one attribute byte even though teleport matching uses the same 4-alignment for a different purpose. Bits used by consumers (not defined as named constants anywhere): `0x01` participates in the PvP gate, `0x20` marks guild-owned areas (`_MSG_Action.cpp:223`), `0x40` marks PvP-permitted tiles, `0x80` marks the newbie zone (`_MSG_Action.cpp:201-221`). Within this component the `& 1` / `& 0x40` pair is used by BR-20 and BR-21 to zero PvP damage; `_MSG_Attack.cpp:1428` uses the *inverse* bit-0 test (`(attr & 1) == 0` zeroes damage), so the two paths disagree on what bit 0 means.

**Rule workflow**:

```
if (x < 0 || x > 4096 || y < 0 || y > 4096) return 0
xx = (x / 4) & 0x3FF
yy = (y / 4) & 0x3FF
return g_pAttribute[yy][xx]
```

**Inputs / outputs / edge cases**: Returns 0 for out-of-range coordinates, which callers treat as "no attributes" — indistinguishable from a genuine zero attribute tile. Row/column order is `[y][x]`.

---

### Business Rule: BR-20 Single-Target Attack Packet Projection

**Overview**:
`GetAttack` produces `MSG_AttackOne` for one attacker/target pair: it selects the skill and motion for NPC attackers, applies a heal-skill override, computes damage through `BASE_GetDamage`, then applies resistances, reflection, the PvP 30% rule, map-attribute gating and friendly-fire suppression. It is the most complex rule in the component (59 decision points).

**Detailed description**:

The packet is zeroed and framed with `ESCENE_FIELD`, attacker id, current time, attacker and target positions, `DoubleCritical = 0`, `CurrentMp = -1`, `ReqMp = -1`, `SkillIndex = -1`, `SkillParm = 0`, `Motion = rand()%3 + 4` (lines 1277-1302). For NPC attackers (`mob >= MAX_USER`) only, the skill bar slots are selected by distance: at distance >= 3 slots 2 and 3 are used, otherwise slots 0 and 1 (lines 1307-1319). The special value 255 means "empty slot" and is converted to -1. Skill indices 121-128 are remapped to concrete skill ids and parameters (121→103/5, 122→105/1, 123→101/1, 124→101/2, 125→40/1, 126→40/2, 127→40/3, 128→33/-4 with motion -4); anything else becomes `SkillIndex = special, SkillParm = 1` (lines 1326-1365).

The second selected slot drives motion animation through a random table over `rand()%4`: value 3 adds 0 or 1, 6 adds 1 or 2, 7 adds 1 or 2 with different probabilities, 15 adds 0-3, 24 adds 3 or 4, 27 adds 0/1/3/4, 23 adds 0/1/2/4, default resets `SkillParm = 1` (lines 1366-1442). A separate mechanic then examines skill bar slot 3: when the random roll lands in 25-64 and slot 3 is not 255, the spell's `InstanceType` is read and, if it equals 6 (heal), the code compares the attacker's and leader's HP percentages; if either is at or below 8%, it retargets to the healthier of the two and sets damage to `MaxHp / 10`, then returns immediately (lines 1443-1481). This is a self/leader heal projected as an attack packet.

For damage, the remaining logic picks a skill by random bracket — slot 0 for rolls 0-49, slot 1 for 50-84, slot 2 for 85-99, each only if not 255 — and derives `Resist = g_pSpell[skill].InstanceType - 2` (lines 1482-1502). Base damage is `BASE_GetDamage(Damage, target Ac, 0)`; if `Resist` is within 0-3 the damage is scaled by `(200 - target.Resist[Resist]) / 100`; for player targets a positive `ReflectDamage` is subtracted with a floor of 1 (lines 1505-1519). The PvP branch then applies 30% (`3 * fisdam / 10`) when either side is a player (or a clan-4 summon), but only on tiles that satisfy "bit 0 clear AND bit 0x40 set" for *both* attacker and target — otherwise damage becomes 0 (lines 1521-1531). Finally, friendly fire is suppressed when leaders match or guilds match, with the guildless case disambiguated by mapping "both guild ids zero" to -1 so two unguilded players still fight, and `GuildDisable` mapping the target to -2 so they never match; suppression also clears `pMob[mob].CurrentTarget` (lines 1533-1556). The packet closes with `CurrentHp = -1`, `Progress = 52428` (0xCCCC) and the computed damage.

Two dead or redundant constructs are worth recording: `rand_ < 0` at lines 1482 and 1484 can never be true, and `sm->Dam[0].TargetID` is assigned 0, then immediately reassigned to `target` (lines 1293-1296).

**Rule workflow**:

```
1. zero packet; frame with ESCENE_FIELD, positions, CurrentTime
2. defaults: DoubleCritical=0, CurrentMp=-1, ReqMp=-1, SkillParm=0, SkillIndex=-1,
   Motion = rand()%3+4
3. if mob >= MAX_USER:
     slots = (dist >= 3) ? {2,3} : {0,1}
     map 121..128 special ids ; default SkillIndex=special, SkillParm=1
     if slot2 != 0 -> motion table over rand()%4
     rand_ = rand()%100
     if slot3 != 255 && 25<=rand_<=64 && InstanceType==6:
        if hp%<=8 or leader hp%<=8 -> retarget healthier, Dam = MaxHp/10, return
     choose skill by rand_ bracket (0-49 / 50-84 / 85-99)
     Resist = InstanceType - 2
4. fisdam = BASE_GetDamage(Damage, target.Ac, 0)
5. if 0 <= Resist <= 3: fisdam = (200 - target.Resist[Resist]) * fisdam / 100
6. if fisdam > 0 && target < MAX_USER && ReflectDamage > 0: subtract, floor 1
7. if both sides player-capable:
     fisdam = 3*fisdam/10
     if target tile has bit0 or lacks 0x40 -> 0
     if attacker tile has bit0 or lacks 0x40 -> 0
8. leader/guild friendly check -> CurrentTarget = 0, fisdam = 0
9. CurrentHp = -1 ; Progress = 52428 ; Dam[0].Damage = fisdam
```

**Inputs / outputs / edge cases**: `target` is indexed without validation (`pMob[target]`). The reflect floor differs from BR-21 (1 here, 0 there). Damage can be produced for a dead target — no HP check occurs here; the caller handles death.

---

### Business Rule: BR-21 Area Attack Packet Projection

**Overview**:
`GetAttackArea` is the multi-target twin of BR-20: it projects up to 13 entries from `EnemyList` into `MSG_Attack` with per-target skill selection and damage, but it is currently unreachable.

**Detailed description**:

The function frames `MSG_Attack`, clears `DoubleCritical`, sets both mana fields to -1, and copies the attacker's `EnemyList[13]` into `Dam[i].TargetID`, replacing out-of-range ids with 0 (lines 1566-1588). Targets with id 0 are skipped. For each surviving target the function repeats the entire BR-20 skill-selection and damage pipeline verbatim (lines 1603-1856), including the heal override and the motion table. Two structural differences exist: the packet has a single `TargetX/TargetY` pair which is overwritten inside the loop, so the transmitted coordinates are those of the *last* enemy processed; and the reflect floor is 0 rather than 1, so reflected damage can reduce a hit to nothing where the single-target path would leave 1.

The function does not zero the destination struct. It also never sets `CurrentHp` or `Progress`, which BR-20 does set to -1 and 0xCCCC. That contract is safe only if callers zero first — and the sole call site does (`memset(&sm, 0, sizeof(MSG_Attack))` at `ProcessSecMinTimer.cpp:1949`), but that call site is inside `if (0 == 1)`, a compile-time false guard immediately preceding a `KefraAttackLabel` label, so `GetAttackArea` never executes in the shipped binary. The label is not referenced by any `goto` in the file, confirming the whole area-attack path is shelved code retained for a future Kefra event.

Consequently 298 lines and 63 decision points — the second-largest and most complex function in the module — contribute nothing to runtime behaviour. Any business rule encoded there (13-target fan-out, per-target resist, per-target PvP gating) is dormant.

**Rule workflow**:

```
1. frame MSG_Attack (no memset)
2. for i in 0..12: Dam[i].TargetID = EnemyList[i]; if out of [0,MAX_MOB) -> 0
3. Motion = rand()%3+4
4. for each target with id != 0:
     TargetX/Y = target position (overwritten each iteration)
     if mob >= MAX_USER: same skill/motion/heal pipeline as BR-20
     fisdam = BASE_GetDamage(...) ; resist scaling
     reflect subtraction floor 0
     PvP 30% + attribute gating ; friendly suppression (clears CurrentTarget)
     Dam[i].Damage = fisdam
5. (no CurrentHp / Progress / memset)
```

**Inputs / outputs / edge cases**: Unreachable in the current build. If revived, it would rely on the caller zeroing the struct and would report only the last target's coordinates.

---

### Business Rule: BR-22 Guild Identifier Resolution

**Overview**:
Two overloads named `GetGuild`: one is a no-op over a connection id, the other extracts a guild id from an item's effect pair.

**Detailed description**:

`GetGuild(int conn)` reads `pMob[conn].MOB.Equip[12]` into a local pointer and `pMob[conn].MOB.Guild` into a local int, then returns void (lines 1862-1867). Both locals are unused. Four call sites invoke it and discard the (nonexistent) result: `Server.cpp:2761`, `Server.cpp:3138`, `ProcessDBMessage.cpp:777`, `_MSG_UseItem.cpp:3160`, `_MSG_TradingItem.cpp:359`. It is a vestige of an earlier design where the guild membership was derived from the cape item.

`GetGuild(STRUCT_ITEM *item)` delegates to `BASE_GetGuild(item)` (line 1871), which combines two one-byte effects: `EF_HWORDGUILD` (56) as the high byte and `EF_LWORDGUILD` (57) as the low byte, giving `high * 256 + low` (`Basedef.cpp:4207-4235`). That encoding is exactly the one emitted by BR-17 for guild flag items, so the two functions form a symmetric pair for cape/flag guild stamping. Call sites are `_MSG_Trade.cpp:162`, `_MSG_TradingItem.cpp:181/182/323/326` — all in trade validation paths where the guild of the items being offered must be re-derived to prevent guild-cape laundering.

The overload set is ambiguous to read (`GetGuild(conn)` and `GetGuild(&item)`), and the void version's presence means a mistyped call compiles silently. Nothing in the module validates that the item actually carries the two effects; an item without them yields 0, which is also a legitimate guild id, so "no guild" and "guild 0" are conflated.

**Rule workflow**:

```
GetGuild(conn): read Equip[12], read MOB.Guild, discard, return (void)
GetGuild(item): hword = cValue of effect whose cEffect == 56
                lword = cValue of effect whose cEffect == 57
                return hword * 256 + lword
```

**Inputs / outputs / edge cases**: Both effects are searched across all three effect slots, first match wins per byte; if the same effect type appears twice, only the first is read.

---

### Business Rule: BR-23 Empty Item Grid Search

**Overview**:
`GetEmptyItemGrid` finds a free ground cell for a dropped or created item, preferring the requested cell and then its 3x3 neighbourhood, treating height 127 as blocked.

**Detailed description**:

The initial test accepts the origin when `pItemGrid[y][x] == 0` and `pHeightGrid[y][x] != 127` (lines 1878-1879). If that fails, the function scans `y-1..y+1` crossed with `x-1..x+1`, skipping out-of-range candidates, and returns the first cell where both conditions hold (lines 1881-1896). The origin is re-tested inside the loop, which is harmless but means a caller passing an already-free origin returns via the fast path anyway. On success the coordinates are written back through the pointers and TRUE is returned; on exhaustion FALSE is returned with coordinates unchanged.

The height rule is a single sentinel: 127 means impassable/void. No other height is rejected (the commented-out test in `CreateItem` referencing `pHeightGrid[y][x] >= -15` suggests a richer rule once existed, `Server.cpp:7261-7262`). Because `pHeightGrid` is `char`, comparisons with 127 are signed-char comparisons; a stored byte 0x7F is 127 either way, so the sentinel is stable.

The missing origin bounds check is the material weakness: `_MSG_DropItem` validates `GridX/GridY < 4096` first (line 44) and `CreateItem` passes server-generated coordinates, so the practical exposure depends on each caller, not on this function. Two call sites discard the return value entirely (`Server.cpp:7259`, `Server.cpp:9397`), in which case a fully blocked neighbourhood leaves the item placed at the requested cell — `CreateItem` then rejects it via its own `if (pItemGrid[y][x])` check (line 7264), so the discarded result is partially compensated downstream.

**Rule workflow**:

```
if pItemGrid[*y][*x] == 0 && pHeightGrid[*y][*x] != 127 -> TRUE (unchanged)
for y in *y-1 .. *y+1:
  for x in *x-1 .. *x+1:
     skip if out of [0,MAX_GRIDX/Y)
     if pItemGrid[y][x] == 0 && pHeightGrid[y][x] != 127:
         *y = y ; *x = x ; return TRUE
return FALSE
```

**Inputs / outputs / edge cases**: No validation of the incoming coordinates; 9 candidates maximum; blocked-cell sentinel is 127.

---

### Business Rule: BR-24 Empty Mob Grid Search (Standard and Wide)

**Overview**:
`GetEmptyMobGrid` finds a free walkable cell for a mob within rings 1-4, and `GetEmptyMobGridGreat` does the same with rings 0-29. Both treat the mob's own current cell as valid and use height 127 as blocked.

**Detailed description**:

Both functions begin with the same three checks: reject coordinates outside the grid (logging `"GetEmptyMobGridOut of range"`), accept the origin if `pMobGrid[y][x] == mob` (the mob is already standing where we want it), and accept the origin if the cell is empty and not height 127 (lines 1903-1914, 2005-2016). The `mob` parameter is used only for the identity test; callers that do not care pass 0, which can never match a real mob index.

The ring scans then differ only in depth. The standard version unrolls four nested loops for radius 1, 2, 3 and 4, each scanning the full square `(2r+1)^2` and returning the first cell where `pMobGrid[y][x] == 0` (lines 1919-1997). The wide version uses a single loop `k = 0..29` over the same square pattern (lines 2021-2042), which is functionally the same algorithm with a 30-cell radius; the worst case is roughly 30 * 3,600 = 108,000 candidate tests versus 81 for the standard version. The wide version is used exactly once, to find a landing cell for a "great teleport" (`Server.cpp:8255`).

Both versions share the same defect: the blocked-cell test inside the loops reads `pHeightGrid[*ty][*tx]` — the current origin coordinates, modified as the search proceeds — instead of the candidate's `pHeightGrid[y][x]`. The condition therefore either blocks the entire search (if the origin is height 127) or passes unconditionally (otherwise), regardless of the candidate cell's height. Mobs can consequently be placed on height-127 cells when the first free `pMobGrid` cell happens to be blocked. Because the same expression is repeated eight times (four in each function), the defect is systematic rather than incidental.

Neither function validates that `mob` is a legal index, and neither detects the case where the target coordinates already hold a *different* mob — the origin test falls through to the ring scan, so a mob requesting an occupied cell is relocated rather than rejected.

**Rule workflow**:

```
if (*tx,*ty) outside grid                   -> Log, FALSE
if pMobGrid[*ty][*tx] == mob                -> TRUE (stay)
if pMobGrid[*ty][*tx] == 0 && height != 127 -> TRUE (stay)

standard: for r in 1..4: for y in *ty-r..*ty+r: for x in *tx-r..*tx+r:
              skip out-of-range ; skip if pMobGrid[y][x] != 0
              if pHeightGrid[*ty][*tx] == 127 -> continue   (origin, not candidate)
              *ty = y ; *tx = x ; return TRUE
wide:     for k in 0..29: same body, same origin-height test
return FALSE
```

**Inputs / outputs / edge cases**: Standard version max 81 candidates; wide version max ~108k. Both mutate the caller's coordinates only on success.

---

### Business Rule: BR-25 Persistent Kill / PK / Guilt Counters

**Overview**:
Seven accessors persist four counters inside the `KILL_MARK` (63) inventory slot of each player, using the effect bytes of a pseudo-item as storage: current kill streak, total kills, PK point and guilt. The writers clamp to fixed ranges; one reader mutates.

**Detailed description**:

Storage layout is: `Carry[63].stEffect[0].cValue` = current kills (0-255 byte), `stEffect[0].cEffect` = PK points, `stEffect[1].cEffect` = guilt, and `stEffect[1].cValue` + `stEffect[2].cValue * 256` = total kills (little-endian 16-bit). Byte-level access is through the `STRUCT_ITEM` union (`Basedef.h:398-412`), so the same 16-bit `sValue` is reinterpreted as `{cEffect, cValue}`.

Readers: `GetCurKill` and `GetTotKill` guard `conn <= 0 || conn >= MAX_USER` and return 0 outside that range, so mob indices are not readable (lines 2046-2069). `GetPKPoint` performs no guard at all and returns the raw effect byte 0-255 (lines 2105-2112), while `SetPKPoint` clamps the stored value to 1-150 (lines 2145-2159). The asymmetry means the writer can never store 0 but the reader can return 0, and the top of the readable range (151-255) is reachable only from data written by other code paths or older versions. `GetGuilty` reads `stEffect[1].cEffect`, and if the value exceeds 50 it zeroes it in place and returns 0 (lines 2114-2127) — a normalising getter with a side effect, needed because `SetGuilty` clamps to 0-50 but historical or corrupted saves may exceed it.

Writers: `SetCurKill` clamps to 0-200 and writes the low byte (lines 2071-2085); `SetTotKill` clamps to 0-32767 and splits into low/high bytes (lines 2087-2103); `SetGuilty` clamps to 0-50; `SetPKPoint` clamps to 1-150. All four reject `conn <= 0 || conn >= MAX_USER` and thus only operate on players. The clamps encode policy: a kill streak saturates at 200, total kills at 32767, guilt at 50, PK points between 1 and 150 (with 150 used as a maximum-penalty marker at `_MSG_UseItem.cpp:3745`).

Consumers of these counters are the kill resolution, PK-mode, whisper/status and quest systems: `MobKilled.cpp:2111-2116` snapshots all four before resolving a player kill and writes back through `SetCurKill`/`SetTotKill`/`SetPKPoint` (lines 2237-2432); `_MSG_Attack.cpp:1334-1374` increments PK points by 5 per player kill and sets guilt to 8; `GetCreateMob`/`GetCreateMobTrade` embed three of the four into the name bytes; the quest handler uses `SetCurKill(conn, curkill-100)` as a redemption cost (`_MSG_Quest.cpp:2576`). Because the storage is an inventory slot, `KILL_MARK` items are also subject to whatever generic inventory code touches slot 63 — a coupling that is not enforced anywhere in this component.

**Rule workflow**:

```
GetCurKill(conn) : guard -> Carry[63].stEffect[0].cValue
GetTotKill(conn) : guard -> stEffect[1].cValue + stEffect[2].cValue*256
SetCurKill(conn,v): guard ; v = clamp(v,0,200) ; stEffect[0].cValue = v
SetTotKill(conn,v): guard ; v = clamp(v,0,32767) ; stEffect[1].cValue=v&0xFF ; stEffect[2].cValue=v>>8
GetPKPoint(conn): (no guard) -> stEffect[0].cEffect
SetPKPoint(conn,v): guard ; v = clamp(v,1,150) ; stEffect[0].cEffect = v
GetGuilty(conn): stEffect[1].cEffect ; if > 50 -> write 0, return 0
SetGuilty(conn,v): guard ; v = clamp(v,0,50) ; stEffect[1].cEffect = v
```

**Inputs / outputs / edge cases**: `GetPKPoint` lacks the player guard the other six have; `SetPKPoint` cannot store 0; `GetGuilty` mutates on read; total kills round-trip up to 32767 while the field physically supports 65535.

---

## 4. Component Structure

```
Code/TMSrv/
├── GetFunc.h                         # 36 declarations, no data types, no inline code
│   ├── lines 25-32  combine matchers (8)
│   ├── lines 33-41  math + DTO builders (parry, affect, hide, view, teleport, exp, mob DTOs)
│   ├── lines 42-47  affect encode, item/action/attribute/attack DTOs
│   ├── lines 48-51  guild, grid searches
│   ├── lines 52-59  kill/PK/guilt counters
│   └── line  60     GetEmptyMobGridGreat
└── GetFunc.cpp                       # 2,160 lines; 36 definitions in 8 logical blocks
    ├── 19-30   includes + extern g_pItemList   (Windows.h, Basedef.h, ItemEffect.h,
    │                                            Language.h, Server.h, GetFunc.h, CMob.h, SendFunc.h)
    ├── 33-560  CRAFTING RECIPE TABLES
    │   ├──  33-104  GetMatchCombine            refinement combine, rate accumulation
    │   ├── 106-165  GetMatchCombineEhre        8 recipes, returns code 1-8
    │   ├── 167-215  GetMatchCombineTiny        unreachable (tautological type test)
    │   ├── 217-259  GetMatchCombineShany       boolean match, probability in handler
    │   ├── 261-375  GetMatchCombineAilyn       grade-tiered soul stones
    │   ├── 377-425  GetMatchCombineAgatha      arch/mortal merge
    │   ├── 427-496  GetMatchCombineOdin        11 exact-vector recipes
    │   └── 498-560  GetMatchCombineAlquimia    10 recipes, -1 sentinel
    ├── 562-945  COMBAT / PROGRESSION MATH
    │   ├── 562-608  GetParryRate               per-mille parry, clamp [1,650]
    │   └── 902-945  GetExpApply                class gates + level-gap multiplier
    ├── 610-900  WORLD QUERIES / LOOKUPS
    │   ├── 610-625  GetEmptyAffect             refresh-first slot lookup
    │   ├── 627-638  GetHide                    unused oracle (affect type 28)
    │   ├── 640-647  GetInView                  33x33 box
    │   ├── 649-656  GetInHalf                  16x16 box
    │   └── 658-900  GetTeleportPosition        ~30 hardcoded clusters + fare
    ├── 947-1255 DTO ASSEMBLY (mob/item/move)
    │   ├──  947-1070 GetCreateMob              MSG_CreateMob
    │   ├── 1072-1172 GetCreateMobTrade         MSG_CreateMobTrade
    │   ├── 1174-1193 GetAffect                 affect wire encoding
    │   ├── 1195-1237 GetCreateItem             MSG_CreateItem
    │   └── 1239-1255 GetAction                 MSG_Action
    ├── 1257-1860 DTO ASSEMBLY (combat) + attribute
    │   ├── 1257-1273 GetAttribute              g_pAttribute[y/4][x/4]
    │   ├── 1275-1562 GetAttack                 MSG_AttackOne (59 decisions)
    │   └── 1564-1860 GetAttackArea             MSG_Attack (63 decisions, unreachable)
    └── 1862-2159 LOOKUPS + SEARCHES + COUNTERS
        ├── 1862-1867 GetGuild(int conn)        no-op
        ├── 1869-1874 GetGuild(STRUCT_ITEM*)    BASE_GetGuild delegation
        ├── 1876-1899 GetEmptyItemGrid          3x3 scan
        ├── 1901-2002 GetEmptyMobGrid           rings 1-4
        ├── 2003-2045 GetEmptyMobGridGreat      rings 0-29
        ├── 2046-2056 GetCurKill
        ├── 2058-2069 GetTotKill
        ├── 2071-2085 SetCurKill
        ├── 2087-2103 SetTotKill
        ├── 2105-2112 GetPKPoint
        ├── 2114-2127 GetGuilty                 normalising getter
        ├── 2129-2143 SetGuilty
        └── 2145-2159 SetPKPoint
```

### Function inventory with metrics

| Function | Lines | Line range | Decisions | Returns |
|----------|-------|------------|-----------|---------|
| `GetMatchCombine` | 73 | 33-105 | 14 | rate (0 = no match) |
| `GetMatchCombineEhre` | 61 | 106-166 | 18 | code 0-8 |
| `GetMatchCombineTiny` | 50 | 167-216 | 15 | always 0 |
| `GetMatchCombineShany` | 44 | 217-260 | 13 | boolean |
| `GetMatchCombineAilyn` | 116 | 261-376 | 32 | rate |
| `GetMatchCombineAgatha` | 50 | 377-426 | 15 | rate |
| `GetMatchCombineOdin` | 71 | 427-497 | 21 | code 0-11 |
| `GetMatchCombineAlquimia` | 64 | 498-561 | 20 | code 0-9, -1 |
| `GetParryRate` | 48 | 562-609 | 11 | per-mille [1,650] |
| `GetEmptyAffect` | 17 | 610-626 | 4 | index or -1 |
| `GetHide` | 13 | 627-639 | 3 | bool (unused) |
| `GetInView` | 9 | 640-648 | 1 | bool |
| `GetInHalf` | 9 | 649-657 | 1 | bool |
| `GetTeleportPosition` | 244 | 658-901 | 43 | fare |
| `GetExpApply` | 45 | 902-946 | 11 | exp |
| `GetCreateMob` | 125 | 947-1071 | 13 | selfdead |
| `GetCreateMobTrade` | 102 | 1072-1173 | 11 | selfdead |
| `GetAffect` | 21 | 1174-1194 | 3 | void |
| `GetCreateItem` | 44 | 1195-1238 | 3 | void |
| `GetAction` | 18 | 1239-1256 | 0 | void |
| `GetAttribute` | 18 | 1257-1274 | 1 | byte |
| `GetAttack` | 289 | 1275-1563 | 59 | void |
| `GetAttackArea` | 298 | 1564-1861 | 63 | void |
| `GetGuild(conn)` | 7 | 1862-1868 | 0 | void (no-op) |
| `GetGuild(item)` | 7 | 1869-1875 | 0 | guild id |
| `GetEmptyItemGrid` | 25 | 1876-1900 | 5 | BOOL |
| `GetEmptyMobGrid` | 102 | 1901-2002 | 23 | BOOL |
| `GetEmptyMobGridGreat` | 43 | 2003-2045 | 9 | BOOL |
| `GetCurKill` | 12 | 2046-2057 | 1 | byte |
| `GetTotKill` | 13 | 2058-2070 | 1 | 16-bit |
| `SetCurKill` | 16 | 2071-2086 | 3 | void |
| `SetTotKill` | 18 | 2087-2104 | 3 | void |
| `GetPKPoint` | 9 | 2105-2113 | 0 | byte |
| `GetGuilty` | 15 | 2114-2128 | 1 | 0-50 |
| `SetGuilty` | 16 | 2129-2144 | 3 | void |
| `SetPKPoint` | 16 | 2145-2160 | 3 | void |

---

## 5. Dependency Analysis

### 5.1 Include graph (compile-time efferent)

`GetFunc.cpp:19-28` pulls in eight headers. Because `Server.h` is itself a header hub that includes `Basedef.h`, `CPSock.h`, `CItem.h`, `CMob.h`, `CUser.h` and `CNPCGene.h` (`Server.h:34-40`), the translation unit transitively compiles against the transport, entity and NPC-generation subsystems even though it uses none of their functions directly beyond `SendClientMessage` and the entity arrays.

| Include | Line | What GetFunc actually uses from it |
|---------|------|-----------------------------------|
| `<Windows.h>` | 19 | `BOOL`, `TRUE`/`FALSE`, `strncpy`, `sprintf` |
| `..\Basedef.h` | 21 | All `STRUCT_*` and `MSG_*` types, `MAX_*` limits, class constants, `REF_*`, `STATE_*`, `ESCENE_FIELD` |
| `..\ItemEffect.h` | 22 | `EF_POS` (17), `EF_ITEMLEVEL` (87), `EF_MOBTYPE` (112), `EF_ITEMTYPE` (113), `EF_SANC` (43), `EF_HWORDGUILD` (56), `EF_LWORDGUILD` (57) |
| `Language.h` | 23 | `_DN_CHANGE_COUNT` (420) message id used in the Kefra branch |
| `Server.h` | 25 | `pUser`, `pItem`, `pMobGrid`, `pItemGrid`, `pHeightGrid`, `CurrentTime`, `BrState`, `RvRState`, `KefraLive`, `ServerIndex`, `g_pGuildZone`, `temp`, `Log` |
| `GetFunc.h` | 26 | Self-check of declarations |
| `CMob.h` | 27 | `pMob[MAX_MOB]`, `CMob` field layout (`TargetX/Y`, `Leader`, `EnemyList`, `Parry`, `ReflectDamage`, `GuildDisable`, `extra`), `MAX_ENEMY` |
| `SendFunc.h` | 28 | `SendClientMessage` (used in the Kefra ticket branch) |

Plus one file-scope extern: `extern STRUCT_ITEMLIST g_pItemList[MAX_ITEMLIST];` at `GetFunc.cpp:30`, used by every recipe matcher and by the Odin `Extra` lookup.

### 5.2 Internal dependency chains

```
GetFunc
  ├── Basedef rules (Basedef.cpp)
  │     ├── BASE_GetItemAbility      1497/1647   EF lookups incl. static effects
  │     ├── BASE_GetItemSanc         2095        refinement tier decoding
  │     ├── BASE_GetItemAmount       2287        EF_AMOUNT (61)
  │     ├── BASE_GetItemGem          2143        gem index from tier
  │     ├── BASE_VisualItemCode      6598        equip visual + refinement nibble
  │     ├── BASE_VisualAnctCode      6662        ancient (anct) visual code
  │     ├── BASE_GetDamage           1225        damage curve with random delta
  │     ├── BASE_GetDistance         5828        Chebyshev-ish distance
  │     ├── BASE_GetVillage          4258        city zone lookup (0-4, else 5)
  │     └── BASE_GetGuild            4207        guild id from effects 56/57
  ├── Entity state (globals, no accessors)
  │     ├── pMob[MAX_MOB]  (CMob)     Server.h:230  read + write (KefraTicket, CurrentTarget, Carry[63], Affect)
  │     ├── pItem[MAX_ITEM] (CItem)   CItem.h:45   read only
  │     ├── pUser[MAX_USER](CUser)    Server.h:229  read (AutoTrade.Title) only
  │     ├── pMobGrid / pItemGrid / pHeightGrid      read + write (search results)
  │     └── g_pAttribute[1024][1024]  Basedef.h:2469 read only
  └── Config-driven tables
        ├── g_pItemList[MAX_ITEMLIST]  Basedef.h    read only
        ├── g_pSpell[MAX_SKILLINDEX]   Basedef.h:2464 read (InstanceType)
        ├── g_pGuildZone[5]            Basedef.h:2441 read (Victory, ChargeGuild, CityLimit*)
        ├── g_pAnctChance[3]           defaults 2/4/10
        ├── g_pTinyBase 15, g_pAilynBase 10, g_pAgathaBase 15
        └── g_pShanyBase 35 (consumed by the handler, not by this module)
```

### 5.3 Reverse dependency chains (who depends on GetFunc)

```
ProcessClientMessage dispatch
  ├── _MSG_Attack.cpp        -> GetParryRate, GetAttribute, GetGuilty, SetGuilty, GetPKPoint,
  │                             GetCreateMob, GetAction, GetEmptyMobGrid
  ├── _MSG_Action.cpp        -> GetAction, GetAttribute, GetEmptyMobGrid
  ├── _MSG_CombineItem.cpp   -> GetMatchCombine
  ├── _MSG_CombineItem{Ehre,Tiny,Shany,Ailyn,Agatha,Odin,Alquimia}.cpp
  │                          -> corresponding GetMatchCombine*
  ├── _MSG_ReqTeleport.cpp   -> GetTeleportPosition
  ├── _MSG_Quest.cpp         -> GetEmptyAffect, GetCurKill, GetPKPoint, SetCurKill, SetPKPoint,
  │                             GetCreateMob
  ├── _MSG_UseItem.cpp       -> GetEmptyAffect (x7), GetAttribute, GetGuild(conn), SetPKPoint,
  │                             GetCreateMob
  ├── _MSG_Trade.cpp         -> GetGuild(item)
  ├── _MSG_TradingItem.cpp   -> GetGuild(item) x4, GetGuild(conn)
  ├── _MSG_MessageWhisper.cpp-> GetPKPoint, GetCreateMob x6, GetCreateMobTrade, GetGuilty, GetAttribute
  ├── _MSG_Buy.cpp / _MSG_Sell.cpp / _MSG_REQShopList.cpp / _MSG_NoViewMob.cpp -> GetInView
  ├── _MSG_PKMode.cpp / _MSG_QuitTrade.cpp -> GetGuilty
  ├── _MSG_DropItem.cpp      -> GetEmptyItemGrid
  ├── _MSG_SendAutoTrade.cpp -> GetCreateMobTrade
  ├── _MSG_InviteGuild.cpp   -> GetCreateMob
  └── _MSG_UpdateItem.cpp    -> (none direct; uses SendCreateItem path)
Timer / AI / system
  ├── ProcessSecMinTimer.cpp -> GetAttack, GetAttackArea (unreachable), GetInHalf, GetParryRate,
  │                             GetEmptyMobGrid, GetAction, GetAttribute
  ├── CMob.cpp               -> GetEmptyMobGrid (x9, always with mob=0)
  ├── MobKilled.cpp          -> GetExpApply x5, GetCurKill, GetTotKill, GetPKPoint, GetGuilty,
  │                             SetCurKill, SetTotKill, SetPKPoint, GetCreateMob x4
  └── Server.cpp             -> GetAttribute x14, GetEmptyMobGrid x8, GetCreateItem x4,
                                GetCreateMob x7, GetGuild x2, GetPKPoint, SetPKPoint, GetGuilty,
                                SetGuilty, GetEmptyItemGrid x2, GetEmptyMobGridGreat,
                                GetParryRate, GetExpApply, GetAction x5, GetEmptyAffect x2
Output layer
  └── SendFunc.cpp           -> GetCreateMob, GetCreateMobTrade, GetCreateItem, GetAffect,
                                GetEmptyMobGrid x2, GetPKPoint/SetPKPoint, GetGuilty
Admin / DB
  ├── imple.cpp              -> GetCreateItem x5, GetCreateMob x3, SetPKPoint, SetTotKill,
  │                             GetAttribute
  └── ProcessDBMessage.cpp   -> GetAttribute, GetGuild(conn), GetEmptyMobGrid, GetCreateMob
```

### 5.4 External dependencies

| Dependency | Type | Version / Identifier | Purpose | Failure mode |
|------------|------|----------------------|---------|--------------|
| Windows CRT (`strncpy`, `sprintf`, `memset`, `rand`) | Platform lib | MSVC CRT | string/buffer/PRNG primitives | `strncpy` truncation without NUL; `sprintf` into the shared global `temp[4096]` |
| `g_pItemList` (item prototype table) | In-memory data file | 6,500 entries | recipe validation, `Extra` chain, grade/pos/unique | Out-of-range guarded in most paths; `Extra` trusted as item index |
| `g_pSpell` (skill table) | In-memory data file | `MAX_SKILLINDEX` | `InstanceType` for damage type and heal detection | SkillBar values are 0-255, table indexed without bounds check |
| `g_pGuildZone` | Config (guild file) | 5 zones | `Victory`, `ChargeGuild`, city rectangles | Index can reach 5 (out of range) via `BASE_GetVillage` |
| `CompRate.txt` | Config file | `../../Common/Settings/CompRate.txt` (`CReadFiles.cpp:37`) | combine rates | Read via `CReadFiles::ReadCompRate` (375-589); invalid values rejected with a modal dialog; missing file aborts load |
| `g_pAttribute` | Map data | 1024x1024 bytes | PvP/newbie/guild zone gating | Zero-filled if map attribute file is absent |
| **No** network / IPC / DB / filesystem access | — | — | — | Module is pure in-process computation |

---

## 6. Afferent and Efferent Coupling

In a C module the "components" are the functions and the shared data structures. Afferent coupling (Ca) is measured as distinct call sites and distinct caller translation units across the project (excluding `GetFunc.cpp`/`GetFunc.h` themselves); efferent coupling (Ce) counts distinct external symbols each function touches (foreign functions, globals, config tables).

### 6.1 Per-function coupling

| Component (function) | Afferent call sites | Afferent files | Efferent symbols | Criticality |
|----------------------|--------------------|----------------|------------------|-------------|
| `GetCreateMob` | 30 | 10 | 12 | Critical |
| `GetEmptyMobGrid` | 28 | 7 | 5 | Critical |
| `GetAttribute` | 25 (+4 internal) | 8 | 2 | Critical |
| `GetAction` | 16 | 4 | 4 | High |
| `GetCreateItem` | 11 | 3 | 7 | High |
| `GetGuild` (both overloads) | 10 | 5 | 4 | Medium |
| `GetEmptyAffect` | 10 | 3 | 2 | High |
| `SetPKPoint` | 9 | 7 | 3 | High |
| `GetPKPoint` | 9 | 6 | 2 | High |
| `GetGuilty` | 9 | 7 | 2 | High |
| `GetExpApply` | 6 | 2 | 2 | Critical |
| `GetInView` | 5 | 5 | 2 | Medium |
| `GetParryRate` | 3 | 3 | 2 | Critical |
| `GetCurKill` | 3 | 2 | 2 | High |
| `SetCurKill` | 3 | 2 | 2 | High |
| `GetEmptyItemGrid` | 3 | 2 | 3 | High |
| `GetCreateMobTrade` | 3 | 3 | 10 | Medium |
| `SetGuilty` | 3 | 2 | 2 | Medium |
| `SetTotKill` | 2 | 2 | 2 | Medium |
| `GetTotKill` | 1 | 1 | 2 | Medium |
| `GetAttack` | 1 | 1 | 11 | High (single consumer, high blast radius) |
| `GetAttackArea` | 1 (unreachable) | 1 | 9 | Low (dead) |
| `GetInHalf` | 1 | 1 | 2 | Medium |
| `GetTeleportPosition` | 1 | 1 | 9 | Medium |
| `GetAffect` | 1 (+2 internal) | 1 | 2 | High (packet correctness) |
| `GetEmptyMobGridGreat` | 1 | 1 | 5 | Low |
| `GetMatchCombine` | 1 | 1 | 6 | Medium |
| `GetMatchCombineEhre` | 1 | 1 | 6 | Medium |
| `GetMatchCombineOdin` | 1 | 1 | 4 | Medium |
| `GetMatchCombineAilyn` | 1 | 1 | 4 | Medium |
| `GetMatchCombineAgatha` | 1 | 1 | 4 | Medium |
| `GetMatchCombineAlquimia` | 1 | 1 | 4 | Low |
| `GetMatchCombineShany` | 1 | 1 | 4 | Low |
| `GetMatchCombineTiny` | 1 | 1 | 4 | Low (unreachable result) |
| `GetHide` | 0 | 0 | 2 | None (dead) |

**Totals**: 204 measured call sites across 32 caller files; module-level efferent count 7 (`Basedef`, `CMob`, `CItem`, `CUser`, `ItemEffect`, `Language`, `Server`+`SendFunc`, counted as the entity/transport group), matching the architecture report's ~29/7 estimate within the measurement method (the report's 29 is a module count; this analysis counts translation units).

### 6.2 Coupling characterisation

| Dimension | Assessment | Evidence |
|-----------|------------|----------|
| Coupling type | **Common/environmental coupling** dominant: functions read and write process-global arrays rather than receiving/returning state | `Server.h:229-311`, `Basedef.h:2464-2491` |
| Coupling strength | High on data, low on control: most consumers call one function and use one return value | 204 call sites / 36 functions |
| Interface width | Narrow and stable: 36 scalar/DTO signatures, no member functions, no callbacks | `GetFunc.h:25-60` |
| Direction | Purely downward from handlers/AI into GetFunc into Basedef; no callbacks into handlers | reverse-dependency scan |
| Temporal coupling | Present: `GetEmpty*` must be followed by the writer, `GetCreateMob` must be followed by multicast, `GetAffect` must follow affect mutation | `SendFunc.cpp:306-316`, `ProcessSecMinTimer.cpp:1964-1972` |
| Data coupling outliers | `GetAttack`/`GetAttackArea` mutate `pMob[].CurrentTarget` (hidden write) and `GetTeleportPosition` mutates `KeraTicket` | `GetFunc.cpp:1554`, `1853`, `870` |
| Cohesion of the module | **Low (coincidental)**: 5 unrelated concern families in one TU; the only shared element is the entity model | Sections 1, 4 |
| Fan-out hotspot | `GetCreateMob` (10 files, 30 sites) — every visibility refresh in the server funnels through it | Section 5.3 |

---

## 7. Integration Points

The component exposes no network endpoints and performs no I/O. Its integration surface is entirely in-process: function signatures, message structs, shared globals and configuration tables.

| Integration | Type | Purpose | Protocol / Mechanism | Data Format | Error Handling |
|-------------|------|---------|----------------------|-------------|----------------|
| `GetFunc.h` (36 declarations) | Internal API | The only legal entry surface | C linkage | Scalars, `STRUCT_*`, `MSG_*` out-params | Return 0 / -1 / FALSE / -1-index sentinels; no exceptions |
| `MSG_CreateMob` | Outbound DTO | Player/mob appearance and status | In-process struct then `CPSock.AddMessage` | Fixed-layout binary, `_MSG` header + 16 equip + 32 affect + score | Caller-sized `sizeof`; no field validation |
| `MSG_CreateMobTrade` | Outbound DTO | Auto-trade shop appearance | Same | Same + `Desc[24]` | Non-player rejected with log |
| `MSG_CreateItem` | Outbound DTO | Ground item appearance | Same | `_MSG` header + grid + `STRUCT_ITEM` + state/height | None; special cases inline |
| `MSG_Action` | Outbound DTO | Movement step | Same | `_MSG` header + positions + route[ ] | None |
| `MSG_AttackOne` / `MSG_Attack` | Outbound DTO | Combat resolution projection | Same | `_MSG` header + skill/motion + `STRUCT_DAM[]` | None; area path unreachable |
| `pMob[]` (`CMob`, 25,000 entries) | Shared global state | The authoritative entity model | Direct array access | `CMob` class layout | No bounds checks in DTO builders; guarded in counter accessors |
| `pItem[]` (`CItem`, 5,000 entries) | Shared global state | Ground items | Direct array access | `CItem` class layout | None |
| `pUser[]` (`CUser`, 1,000 entries) | Shared global state | Session/account state (`AutoTrade.Title`) | Direct array access | `CUser` class layout | Reached only for `mob < MAX_USER` |
| `pMobGrid` / `pItemGrid` / `pHeightGrid` (4096x4096) | Shared global state | Spatial occupancy | Direct 2D array access | `unsigned short` / `char` | Range-checked for mob grid; not for item grid origin |
| `g_pAttribute` (1024x1024) | Shared global state | Map attribute bytes | 10-bit masked index | `unsigned char` | Out-of-range returns 0 |
| `g_pItemList` (6,500) | Shared table | Item prototypes | Direct index | `STRUCT_ITEMLIST` | Range-checked in matchers (`< MAX_ITEMLIST`) |
| `g_pSpell` | Shared table | Skill instance types | Direct index by SkillBar byte | `STRUCT_SPELL` | Not range-checked |
| `g_pGuildZone` (5) | Shared table | Guild towns / victory state | Index from `BASE_GetVillage` | `STRUCT_GUILDZONE` | Index can reach 5 (out of range) |
| `CompRate.txt` | Config file | Combine rates | Loaded at startup by `CReadFiles::ReadCompRate` | `SECTION/KEY=VALUE` text | Invalid values rejected with `MessageBoxA`; loader aborts on missing file |
| `Log()` | Diagnostic sink | Out-of-range diagnostics, non-player trade rejection | Function call | `(char* str1, char* str2, ip)` | Format/argument mismatches present |
| `SendClientMessage()` | Outbound side channel | Kefra ticket count message | Function call + global `temp` | Formatted string | Uses shared `temp[4096]` |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose / Assessment |
|---------|----------------|----------|----------------------|
| **Module-level Façade** | One header exposes 36 free functions that hide `pMob`/`pItem`/grid complexity from 32 callers | `GetFunc.h`, throughout | Centralises derivation; also centralises coupling |
| **DTO Assembler / Mapper** | `GetCreateMob`, `GetCreateMobTrade`, `GetCreateItem`, `GetAction`, `GetAttack`, `GetAttackArea`, `GetAffect` fill caller-provided structs | 947-1860 | Separates wire layout from domain state; entities remain untouched (except `CurrentTarget`) |
| **Table-driven rules (hardcoded)** | Teleport cluster table (~30 branches), recipe tables, motion randomization tables, skill-id remap switch | 658-900, 33-560, 1326-1442 | Data-as-code; fast, but every rule change is a rebuild |
| **Strategy-per-NPC (duplicated, not polymorphic)** | Eight independent `GetMatchCombine*` functions with parallel structure, one handler per family | 33-560 + `_MSG_CombineItem*.cpp` | Achieves per-family variation by copy-paste rather than a shared engine; ~540 lines of near-duplicate validation |
| **Two-phase validation (match then authorise)** | Matcher returns shape/code; handler enforces class, quest, coin, exp, level, rune completeness | `GetFunc.cpp:427-496` vs `_MSG_CombineItemOdin.cpp:47-89`; Ehre/Ailyn/Shany handlers | Keeps the matcher pure (except for globals) but splits a single business rule across two files with no shared contract type |
| **Sentinel-value protocol** | 0 = no match (8 matchers), -1 = no match (Alquimia, `GetEmptyAffect`), 747 = combine blocker, 255 = empty skill slot, 127 = blocked cell, -3/-4 = parry sentinels, 30000 = server scenario id | throughout | Compact on the wire, but ambiguous (0 is both "no match" and a valid Alquimia recipe; 0 is both "no guild" and "guild 0") |
| **Self-normalising getter** | `GetGuilty` zeroes stored guilt above 50 on read | 2114-2127 | Lazy migration of legacy data; violates command-query separation |
| **Pseudo-item as record store** | `Carry[KILL_MARK=63]` effect bytes hold four counters | 2046-2159 | Zero schema cost, but binds gameplay persistence to an inventory slot and its generic handlers |
| **Side-channel field packing** | Kill/PK bytes into `MobName[12..15]`; mount refinement into visual code bits 12-15; guild into two effect bytes | 964-966, 1050-1052, 1222-1226 | Avoids protocol changes at the cost of implicit, undocumented contracts |
| **Compensating duplication** | `GetCreateMob` reimplements the mount visual encoding because `BASE_VisualItemCode`'s mount branch is unreachable | 1039-1055 vs `Basedef.cpp:6607` | Workaround embedded in a caller rather than the helper |
| **No dependency injection** | Everything resolved through `extern` globals declared in `Server.h`/`Basedef.h` | 30, `Server.h:229-311` | Zero wiring cost; untestable in isolation |
| **Guard-clause style** | Most small functions validate and return early (`GetCurKill`, `Set*`, `GetHide`, `GetInView`) | 2046-2159, 627-656 | Consistent and readable in the simple functions; absent in the complex ones |

### Architectural boundaries

| Boundary | Assessment |
|----------|------------|
| GetFunc ↔ handlers | Clear direction (handlers call GetFunc), but the crafting rules are split across the boundary (shape here, permission there), so neither side owns the rule |
| GetFunc ↔ Basedef | Clean delegation for item math/visuals; one leaked defect (`BASE_VisualItemCode` mount branch) is compensated in GetFunc |
| GetFunc ↔ entity classes | **Leaky**: direct field access on `CMob`/`CItem`/`CUser`, including writes, bypassing any invariants the classes might hold |
| GetFunc ↔ transport | Correct: no socket usage; DTOs are handed upward |
| GetFunc ↔ persistence | None: the only persistence-adjacent state is the kill-mark pseudo-item, which the DB layer copies as part of the inventory |
| Layer enforcement | Conventional only — nothing prevents a handler from duplicating a rule (and `_MSG_CombineItemOdin` and `_MSG_Attack` do so for rune completeness and map attributes) |

---

## 9. Technical Debt & Risks

| Risk | Component Area | Issue | Impact |
|------|----------------|-------|--------|
| **High** | `GetMatchCombineTiny` (167-215) | `!= 4 \|\| != 5` tautology at lines 190 and 196 makes the function always return 0 | Entire Tiny refinement feature is unreachable; players receive "wrong combination" with no diagnostic; 50 lines of dead specification |
| **High** | `GetAttackArea` (1564-1860) | Only call site is behind `if (0 == 1)` (`ProcessSecMinTimer.cpp:1945`) | 298 lines / 63 decisions of dead code that still compiles into the binary; the Kefra area-attack behaviour does not exist |
| **High** | `GetCreateItem` (1217-1226) | `BASE_GetVillage` may return 5 while `MAX_GUILDZONE` is 5 | Out-of-range read of `g_pGuildZone[5]`; `Victory`/`ChargeGuild` garbage can be embedded in the flag item packet |
| **High** | `GetEmptyMobGrid` / `Great` (1929, 1949, 1969, 1989, 2033) | Blocked-cell test reads the origin `pHeightGrid[*ty][*tx]` instead of the candidate `[y][x]` | Entities can be placed on height-127 (void/blocked) cells; world collision invariants violated silently |
| **High** | `GetAttack` (1275-1562) / `GetAttackArea` | No validation of `target`; direct `pMob[target]` dereference | A caller passing an out-of-range target id reads/writes arbitrary entity memory |
| **High** | Module-wide | No automated tests; no assertions; no invariant checks | Every rule in Section 3 is unverified by construction; regressions are invisible until players report them |
| Medium | `GetEmptyItemGrid` (1878) | Origin dereference without bounds check, unlike its mob-grid sibling | Garbage coordinates cause out-of-bounds reads; two call sites also discard the FALSE result (`Server.cpp:7259`, `9397`) |
| Medium | `GetCreateMob` vs `GetCreateMobTrade` (1006-1010 vs 1122-1126) | Divergent `CreateType` derivation (`>= 6` + else-if vs `!= 0` + independent if) | The same player shows different guild badges in the field and in the auto-trade window |
| Medium | `GetCreateMob` mount encoding (1039-1055) | Reimplements encoding that `BASE_VisualItemCode` fails to apply (`Basedef.cpp:6607` unreachable guard) | Two sources of truth for mount visuals; fixing the helper would double-apply the shift |
| Medium | `GetAffect` (1186-1191) | Time clamped to 2,550,000 then truncated to its low byte | Client-visible durations alias every 256 units; the clamp is effectively meaningless for the wire value |
| Medium | `GetGuilty` (2114-2127) | Getter mutates stored state | Read paths have side effects; any diagnostic read silently repairs data, making corruption untraceable |
| Medium | `SetPKPoint` (2150) vs `GetPKPoint` | Writer floor is 1, reader range is 0-255 | PK point 0 is unreachable through the writer but observable through the reader; 151-255 is writable only by other paths |
| Medium | `GetPKPoint` (2105-2112) | Missing `conn` guard present in six sibling accessors | Out-of-range `conn` indexes `pMob` |
| Medium | `GetCreateMobTrade` (1078-1082) | `%d` format for a `char*` argument; return value ignored by all callers | Malformed diagnostics; non-player requests are silently dropped without a usable log |
| Medium | `GetCreatemob`/`Trade` return values | `selfdead` is computed and returned but never consumed anywhere | Dead contract; dead-mount detection has no behavioural effect |
| Medium | `GetGuild(int conn)` (1862-1867) | No-op with unused locals, called from 5 sites | Misleads readers into believing guild re-derivation occurs; maintenance trap |
| Medium | `GetHide` (627-638) | Defined, declared, never called | Dead public API that suggests hide detection exists here |
| Medium | Ready/recipe split (BR-01/02/05/06/07/08 vs handlers) | Matchers return probabilities while handlers own eligibility, pricing and result mutation | A single business rule spans two files with no shared type; 8 parallel implementations of the same "validate, delete, roll, mutate" skeleton |
| Medium | `GetAttribute` (1259) | Bounds test accepts `x == 4096`/`y == 4096`, then masks the quotient to 0 | Silently samples tile 0 instead of rejecting |
| Medium | `_MSG_Attack` vs `GetAttack` map-attribute polarity (`_MSG_Attack.cpp:1428` vs `GetFunc.cpp:1527`) | Opposite interpretations of attribute bit 0 | PvP damage gating differs between the handler path and the projection path |
| Low | `GetEmptyMobGridGreat` (2021-2042) | 30 nested full-square scans (~108k candidate tests worst case) per call | Latency spike on "great teleport" in dense areas |
| Low | `GetExpApply` (902) | `STRUCT_MOBEXTRA` passed by value (hundreds of bytes) per party member per kill | Avoidable copying in the hottest progression path (5 calls per kill observed) |
| Low | `GetTeleportPosition` (658-900) | ~30 hardcoded coordinate clusters, no data table | Map changes require code edits; unreachable clusters cannot be detected |
| Low | `GetTeleportPosition` (870) | Kefra ticket decremented before the handler's coin check | A player with no coins loses a ticket without being teleported |
| Low | `GetMatchCombineEhre` (116-138) | Validates slots 3-7 that no recipe uses | Screens rejected for reasons unrelated to the matched recipe |
| Low | `GetMatchCombineAilyn` (360-372) | Zero-stone layout accepted for grades 1-4; handler then computes `0 - 2441` | Legitimate-looking but degenerate result refinement (tier 10, stage 0) |
| Low | `GetMatchCombineAlquimia` (500-504) | 747 blocker returns 0, which the handler would read as recipe 0 | Contract inconsistency masked only by the blocker item's own index |
| Low | `GetMatchCombine` (51) vs `Basedef.cpp:1508` | `nUnique` 41-49 here, 41-50 in ability logic | One item family is combinable in principle but rejected by the matcher |
| Low | `GetAction` (1241) | `ID = mob` instead of `ESCENE_FIELD` | Inconsistent scenario id across builders |
| Low | `GetCreateItem` (1195-1237) | Destination struct not zeroed; `Create` left undefined for non-open items | Field values depend on caller stack hygiene |
| Low | `GetCreateItem` (1225-1226) | `(ChargeGuild + (ServerIndex << 12)) >> 8` stored into an 8-bit `cValue` | Server/guild encoding truncates; only the low byte survives |
| Low | Magic literals | 747, 3140, 1774, 3916, 5334-5337, 5700, 2604/2648/1708/1744, 52428, -204, 2,550,000, 0x200, 0x40, 0x80 | Rules cannot be audited without hunting each literal; several have no symbolic name |
| Low | `GetFunc.cpp:1482,1484` | `rand_ < 0` conditions on an `int` in [0,99] | Dead sub-conditions obscure the actual skill-selection rule |
| Informational | Header hub transitivity | `Server.h` drags `CPSock`, `CNPCGene`, `CItem`, `CUser` into every GetFunc compile | Long rebuild chains and conceptual coupling (documented project-wide in the architecture report) |

---

## 10. Test Coverage Analysis

### 10.1 Test artefacts found

**None.** There is no test harness, test project, test runner or assertion framework anywhere in the analysed scope.

Evidence gathered:

| Search | Scope | Result |
|--------|-------|--------|
| Files/directories named `*test*`, `*spec*`, `*mock*`, `*fixture*` | whole project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec` | 0 matches (directory scan and recursive file scan) |
| Tokens `gtest`, `catch2`, `CTest`, `unittest`, `doctest`, `assert(` | `Code/**` (`*.cpp`, `*.h`, `*.c`, `*.hpp`) | 0 matches |
| Solution/project files | `W2PP Code Project.sln`, `Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj` | Only two buildable projects (game server, DB server); no test project |
| Additional build/config artefacts | `*.vcxproj`, `*.sln`, `*.props`, `Makefile`, `CMakeLists.txt`, `*.yml`, `*.json`, `*.ini`, `*.cfg` | Only the two `.vcxproj` files, the `.sln`, and Visual Studio `.vs/` state files |
| Documentation | `docs/**` | Only analysis reports (`MANIFEST.md`, dependency audit, architecture report, 24 component reports); no test plans or coverage data |

`GetFunc.cpp` and `GetFunc.h` are compiled into `TMSrv.vcxproj` (`ClCompile` line 100, `ClInclude` line 178), so the module is part of the production binary and is exercised only through live server execution.

### 10.2 Coverage assessment

| Component area | Unit tests | Integration tests | Measured coverage | Test quality / verifiability |
|----------------|-----------|-------------------|-------------------|------------------------------|
| Recipe matchers (8 functions, 33-560) | 0 | 0 | 0% | Unverifiable. Behaviour depends on `g_pItemList` contents and on a 115-modulus roll performed in a different file. Only the Tiny tautology is provable by inspection |
| `GetParryRate` (562-608) | 0 | 0 | 0% | Pure formula with clamps — the most trivially testable function in the module, yet untested |
| `GetExpApply` (902-945) | 0 | 0 | 0% | Pure with respect to globals (parameter copy) — testable in isolation, untested |
| `GetEmptyAffect` (610-625) | 0 | 0 | 0% | Untested; requires a `pMob` fixture |
| Spatial predicates (627-656) | 0 | 0 | 0% | Pure functions over `pMob`; untested |
| `GetTeleportPosition` (658-900) | 0 | 0 | 0% | ~30 branches, 1 of 30 call patterns ever exercised in practice; untested |
| `GetCreateMob` / `Trade` (947-1173) | 0 | 0 | 0% | 30+ call sites, no assertion of packet contents; untested |
| `GetCreateItem` / `GetAction` / `GetAffect` | 0 | 0 | 0% | Untested; the guild-flag out-of-range path is unverified |
| `GetAttribute` (1257-1273) | 0 | 0 | 0% | Untested; boundary behaviour at 4096 unverified |
| `GetAttack` (1275-1562) | 0 | 0 | 0% | 59 decisions, 11 external dependencies, 1 caller; the highest-risk untested code in the module |
| `GetAttackArea` (1564-1860) | 0 | 0 | 0% | Unreachable, therefore 0% by definition |
| Grid searches (1876-2044) | 0 | 0 | 0% | Untested; the height-cell defect cannot be caught without fixtures |
| Kill/PK/guilt counters (2046-2159) | 0 | 0 | 0% | Untested; clamp boundaries (0/200/32767/1-150/0-50) unverified |
| `GetGuild` pair (1862-1874) | 0 | 0 | 0% | Untested; the void overload's no-op is not covered by any assertion |

**Aggregate**: 0 unit tests, 0 integration tests, 0% line coverage, 0% branch coverage, 36 of 36 functions uncovered.

### 10.3 Observable consequences of the missing coverage

The defects catalogued in Section 9 are all of a kind that a minimal fixture-based suite would have detected: the Tiny tautology is caught by a single positive-case assertion; the `g_pGuildZone[5]` read by one boundary test; the height-cell defect by a fixture with a 127 cell adjacent to the origin; the `CreateType` divergence by comparing the two DTOs for the same mob. None of these paths produce a log line or an error code at runtime, so the only feedback channel is player behaviour. This is consistent with the project-wide finding in the architecture report that TMSrv has no verification layer of any kind.

---

## Appendix A — Symbol and Constant Reference

| Symbol | Value / Definition | Location |
|--------|--------------------|----------|
| `MAX_COMBINE` | 8 | `Basedef.h:113` |
| `MAX_ITEMLIST` | 6500 | `Basedef.h:139` |
| `MAX_AFFECT` | 32 | `Basedef.h:122` |
| `MAX_USER` | 1000 | `Basedef.h:56` |
| `MAX_MOB` | 25000 | `Basedef.h:107` |
| `MAX_EQUIP` | 16 | `Basedef.h:75` |
| `MAX_TARGET` | 13 | `Basedef.h:173` |
| `MAX_GRIDX` / `MAX_GRIDY` | 4096 | `Basedef.h:100-101` |
| `VIEWGRIDX` / `VIEWGRIDY` | 33 | `Basedef.h:95-96` |
| `HALFGRIDX` / `HALFGRIDY` | 16 | `Basedef.h:97-98` |
| `MAX_LEVEL` | 399 | `Basedef.h:117` |
| `MAX_CLEVEL` | 199 | `Basedef.h:118` |
| `NAME_LENGTH` | 16 | `Basedef.h:132` |
| `KILL_MARK` | 63 | `Basedef.h:166` |
| `ESCENE_FIELD` | 30000 | `Basedef.h:170` |
| `MORTAL` / `ARCH` / `CELESTIAL` / `CELESTIALCS` / `SCELESTIAL` | 2 / 1 / 3 / 4 / 5 | `Basedef.h:178-182` |
| `REF_10`..`REF_15` | 10 / 12 / 15 / 18 / 22 / 27 | `Basedef.h:196-201` |
| `STATE_OPEN` / `STATE_CLOSED` | 1 / 2 | `Basedef.h:1835-1836` |
| `EF_POS` / `EF_SANC` / `EF_HWORDGUILD` / `EF_LWORDGUILD` / `EF_AMOUNT` / `EF_ITEMLEVEL` / `EF_MOBTYPE` / `EF_ITEMTYPE` | 17 / 43 / 56 / 57 / 61 / 87 / 112 / 113 | `ItemEffect.h:21,83,96,97,101,133,149,150` |
| `RSV_HASTE` / `RSV_BLOCK` | 0x20 / 0x80 | `Basedef.h:189,191` |
| `g_pAnctChance[3]` | {2, 4, 10} | `Basedef.cpp:95` |
| `g_pTinyBase` / `g_pShanyBase` / `g_pAilynBase` / `g_pAgathaBase` | 15 / 35 / 10 / 15 | `Basedef.cpp:90-93` |
| `COMP_RATE_PATH` | `../../Common/Settings/CompRate.txt` | `CReadFiles.cpp:37` |
| `MSG_CreateMob` / `MSG_CreateMobTrade` / `MSG_CreateItem` / `MSG_Action` / `MSG_AttackOne` / `MSG_Attack` | protocol structs | `Basedef.h:1554,1526,1593,1696,2064,2012` |
| `_MSG` macro | `Size, KeyWord, CheckSum, Type, ID, ClientTick` | `Basedef.h:925-930` |
| `STRUCT_AFFECT` | `Type, Value, Level, Time` | `Basedef.h:593-599` |
| `STRUCT_ITEM` | `sIndex` + 3 x union{`sValue`, {`cEffect`,`cValue`}} | `Basedef.h:398-412` |
| `CMob` fields consumed | `MOB`, `Affect[32]`, `Leader`, `TargetX/Y`, `EnemyList[13]`, `Parry`, `ReflectDamage`, `GuildDisable`, `CurrentTarget`, `extra`, `Tab` | `CMob.h:37-100` |
| `CItem` fields consumed | `ITEM`, `PosX/Y`, `State`, `Rotate`, `Height` | `CItem.h:27-39` |
| `STRUCT_GUILDZONE` fields consumed | `ChargeGuild`, `Victory`, `CityLimit*`, `Clan` | `Basedef.h:714-744` |

## Appendix B — Verification Method

All statements in this report were established by reading the source files listed below; no code was executed and no file was modified.

| File | Purpose in this analysis |
|------|--------------------------|
| `Code/TMSrv/GetFunc.cpp` | Full read (2,160 lines) — the analysed component |
| `Code/TMSrv/GetFunc.h` | Full read (64 lines) — public contract |
| `Code/Basedef.h` | Structures, constants, message layouts |
| `Code/Basedef.cpp` | `BASE_*` helper semantics, config defaults |
| `Code/ItemEffect.h` | `EF_*` effect identifiers |
| `Code/TMSrv/CMob.h`, `Code/TMSrv/CItem.h`, `Code/TMSrv/Server.h` | Entity layout and global declarations |
| `Code/TMSrv/CReadFiles.cpp` | `CompRate.txt` parsing and validation |
| `Code/TMSrv/SendFunc.cpp` | Primary consumer for appearance DTOs |
| `Code/TMSrv/ProcessSecMinTimer.cpp` | AI/attack projection consumers |
| `Code/TMSrv/MobKilled.cpp` | Experience and PK/guilt consumers |
| `Code/TMSrv/Server.cpp` | Affect allocation, grid consumers, `Log`, `CreateItem` |
| `Code/TMSrv/_MSG_CombineItem*.cpp` (8 files) | Recipe matcher consumers and roll semantics |
| `Code/TMSrv/_MSG_ReqTeleport.cpp`, `_MSG_DropItem.cpp`, `_MSG_Attack.cpp`, `_MSG_Action.cpp`, `_MSG_UseItem.cpp`, `_MSG_Quest.cpp`, `_MSG_Trade.cpp`, `_MSG_TradingItem.cpp`, `_MSG_MessageWhisper.cpp`, `_MSG_PKMode.cpp`, `_MSG_QuitTrade.cpp`, `_MSG_NoViewMob.cpp`, `_MSG_Buy.cpp`, `_MSG_Sell.cpp`, `_MSG_REQShopList.cpp`, `_MSG_SendAutoTrade.cpp`, `_MSG_InviteGuild.cpp`, `imple.cpp`, `CMob.cpp`, `ProcessDBMessage.cpp` | Afferent call-site inventory and contract verification |
| `Code/TMSrv/TMSrv.vcxproj` | Build integration confirmation (lines 100, 178) |
| `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` | Component role and coupling baseline (line 149) |

Metrics (line counts, blank/comment split, per-function decision counts, call-site census) were produced with read-only text scanning over the same files.
