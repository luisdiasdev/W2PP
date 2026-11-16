# Component Deep Analysis Report — Basedef Rules Engine

| Attribute | Value |
|-----------|-------|
| Component | Basedef Rules Engine |
| Primary location | `Code/Basedef.cpp` (161,052 bytes, 6,814 lines) |
| Companion contract (out of scope) | `Code/Basedef.h` (60,163 bytes, 2,505 lines) — analysed separately as *Basedef Domain Contract*; used here only as the declaration surface and constant/type source |
| Secondary include | `Code/ItemEffect.h` (6,534 bytes, 153 lines) — `EF_*` effect-code dictionary parsed at runtime by this component |
| Project root | `E:\wyd\test-setup\w2pp` |
| Analyzed scope | Entire project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec` |
| Host executables | `TMSrv` (game/world server) and `DBSrv` (DB server) — `Code/TMSrv/TMSrv.vcxproj:91`, `Code/DBSrv/DBSrv.vcxproj:94` |
| Language / paradigm | C (Win32, MSVC, Win32/x64 configurations) with C++ only for `STRUCT_MOB &` reference parameters and a POD struct constructor in the header |
| Analysis date | 2026-11-15 22:56:57 |
| Analysis type | Static, read-only. No files modified, no code executed, no tests run |

---

## 1. Executive Summary

The **Basedef Rules Engine** is the shared, statically linked game-rules library of W2PP. It is one translation unit (`Code/Basedef.cpp`) compiled independently into both binaries of the solution, and it holds every numeric and content rule that both the game server and the DB server must agree on: class baselines, experience tables, ability/score accounting, item ability resolution and refinement encoding, mob score derivation, combat hit/damage math, grid routing and height-occupancy, and all CSV/binary content loaders. It exposes 101 function definitions and ~30 process-global tables; it includes only two project headers (`Basedef.h`, `ItemEffect.h`) and no other subsystem, which is why the architecture report measures efferent coupling of 3 and afferent coupling of ~49 (`docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:162`).

The component is a **library, not a service**. It has no threads, no sockets, no endpoints, no persistence layer of its own, and no ownership of entity state. Its entire "interface" is (a) 101 free functions with C linkage (100 `BASE_*` names plus the unprefixed `GetItemPointer`) and (b) 30-plus mutable globals that 47 other translation units read and, in several cases, write directly. That global data plane is the true contract: `g_pMessageStringTable` is referenced by 46 files, `g_pGuildZone` by 15, `g_pItemList` by 16, `g_pSpell` by 5. Measured call sites into the component are **639 `BASE_*` calls** across 59 of its 93 unique exported names — a full 34 names are never called from outside the file — plus **24** calls to `GetItemPointer` in 6 files, for **663** external call sites in total.

The component's centre of gravity is `BASE_GetCurrentScore` (`Basedef.cpp:2973-4113`): a 1,141-line, 204-decision single function that recomputes a mob's entire `CurrentScore` — HP/MP, damage, AC, stats, specials, magic, attack speed, run speed, critical, regen, resistances, and five out-parameters — by walking equipment, 32 affect slots, soul multipliers, class skill bits, refinement bonuses and mount tables. It is called from exactly one place (`CMob.cpp:674`) and is the highest-risk code in the component by both size and blast radius.

The second largest risk cluster is the **content loader family** (`BASE_ReadItemListFile`, `BASE_InitializeMessage`, `BASE_InitializeMobname`, `BASE_InitializeSkill`, `BASE_InitializeGuildName`, `BASE_InitializeServerList`, `BASE_InitializeAttribute`, `BASE_InitializeEffectName`, `BASE_ReadInitItem`). These parse operator-edited CSV/text/binary files at process start, write binary caches back to `../../Common`, and emit C headers into the source tree. They contain the component's densest population of latent defects: wrong handle checks, `memset` fill-value errors, off-by-one effect-name bounds, and integrity checks that are compiled out or commented out.

A third cluster is **validation that silently does nothing**. `BASE_CheckPacket` (`Basedef.cpp:6475-6597`) — the only per-message-type size validator in the system, a 123-line function containing 111 decision points — has its entire body commented out and unconditionally returns `FALSE`. All five call sites are inside `#ifdef _PACKET_DEBUG` logging blocks, so the 100-entry message-type/size table is inert in every configuration.

### Key findings

| # | Finding | Evidence |
|---|---------|----------|
| F-01 | `BASE_CheckPacket` is a no-op: lines 6476-6594 are one block comment, only `return FALSE;` executes. The 100-entry per-message-type size table therefore never runs, in Debug or Release. | `Basedef.cpp:6475-6597`; callers `CPSock.cpp:547`, `TMSrv/Server.cpp:3898`, `TMSrv/Server.cpp:3983`, `DBSrv/Server.cpp` (2 sites), `TMSrv/Server.cpp` (1 site) all inside `#ifdef _PACKET_DEBUG` |
| F-02 | `BASE_GetDestByAction` increments the *pointers* `x`/`y` (`*x++` parses as `*(x++)`) instead of the values they point to, so any non-empty route leaves the decoded position equal to the start position. | `Basedef.cpp:5378`, `5381`, `5384`, `5387`, `5391-5392`, `5397-5398`, `5403-5404`, `5409-5410` |
| F-03 | Route encoder and route decoder disagree on the vertical axis: `BASE_GetRoute` emits `'2'` for `y-1` and `'8'` for `y+1`, while `BASE_GetDestByAction` decodes `'8'` as `y-1` and `'2'` as `y+1`. `'3'` is likewise mirrored (`x+1,y-1` vs `x+1,y+1`). | `Basedef.cpp:5576-5582` vs `5383-5387`; `5584-5591` vs `5395-5399` |
| F-04 | `BASE_NeedLog` contains two always-true predicates, so for any in-range item index it always returns `TRUE` (i.e. every item is logged). | `Basedef.cpp:799` (`idx >= 551 \|\| idx <= 562`, `cEffect != 0 \|\| cEffect != 59`) |
| F-05 | Four of the five item-ability readers validate `idx <= 0 \|\| idx > MAX_ITEMLIST`, which **admits index 6500 into a 6500-element array** (`g_pItemList[MAX_ITEMLIST]`). Only `BASE_GetItemAbility` uses the safe `idx >= MAX_ITEMLIST`. | `Basedef.cpp:1502`, `1849`, `2002`, `2061` vs `1653` |
| F-06 | `BASE_GetCurrentScore` doubles `MaxHp` and `MaxMp` on every non-summon evaluation (`MaxHp += (MaxHp + HPDelta)`), unconditionally — not only when a soul affect is present. | `Basedef.cpp:3117-3122` |
| F-07 | `BASE_GetCurrentScore`'s first loop dereferences `Affect[i]` with no NULL guard (`isSummon == 0` path), while its two later loops do guard `Affect == 0`. | `Basedef.cpp:3005-3007` vs `3258`, `3783` |
| F-08 | `BASE_GetClientGuildName` indexes the 4-dimensional `g_pGuildName[group][server][guild][len]` with two subscripts (`g_pGuildName[Server][Guild]`), reading `[server][guild][0]`. | `Basedef.cpp:1379-1381` vs `1330-1332` |
| F-09 | `BASE_CanCarry`/`BASE_CanCargo` build the occupancy map with stride `CARRYGRIDX`/`CARGOGRIDX` but consult it with stride `CARRYGRIDY`/`CARGOGRIDY`, so collision tests read the wrong cells. | `Basedef.cpp:4449` vs `4468`; `4516` vs `4531` |
| F-10 | `BASE_GetMobCheckSum` zeroes `mob->BaseScore` as a side effect (`memset`) before computing a 2-loop checksum, making it destructive despite the "Get" name; it has no in-project caller. | `Basedef.cpp:2508-2516` |
| F-11 | All file-integrity checks are disabled: skill/item/message checksum comparisons are commented out or inside `#ifndef _DEBUG` blocks that no longer compare. | `Basedef.cpp:2815-2818`, `2887-2902`, `2895-2896`, `4951-4954` |
| F-12 | `BASE_GetSum`/`BASE_GetSum2` have a dangling-`else` construction: the `p[i]/3` (resp. `p[i]/5`) term is attached to `if (mod == 5)`, so it is applied to every residue except 5, and residues 0-4 also apply their own term. The two checksum functions therefore execute a different algorithm than their names and comments suggest. | `Basedef.cpp:2654-2657`, `2686-2690` |
| F-13 | `memset(g_pMessageStringTable, MAX_STRING, 128)` uses `MAX_STRING` (2000 → byte `0xD0`) as the *fill value* and `128` as the *length*, so it writes 128 bytes of `0xD0` into table row 0 instead of zeroing the table. The remaining 1,999 rows are never cleared, so the initializer and the binary reader both leave stale content in place. | `Basedef.cpp:4639`, `4934` |
| F-14 | `BASE_InitializeItemList` compares the loop index instead of the resolved effect code (`if (j == MAX_EFFECTINDEX)` where `v` was the lookup result), so unregistered effect names are written to the table as `v == MAX_EFFECTINDEX` and the intended diagnostic never fires. | `Basedef.cpp:5128-5139` |
| F-15 | `BASE_InitializeServerList` tests the wrong handle (`if (fp != NULL)` after testing `fpBin`), and obfuscates/de-obfuscates the in-memory list with `char += int` arithmetic that wraps on every entry above 127. | `Basedef.cpp:6264-6301` |
| F-16 | `BASE_CanEquip` computes `weapontype %= 10` *before* deriving `divweapon = weapontype / 10`, so `divweapon` is always 0 and the intended 150 % requirement penalty for weapon family 6 is dead code. | `Basedef.cpp:4375-4386` |
| F-17 | Mount-related range checks are unreachable: `if (Item->sIndex >= 2360 && Item->sIndex < 2360)` is never true. | `Basedef.cpp:6607` |
| F-18 | `BASE_GetDoubleCritical` contains two self-cancelling blocks (`if ((*cProgress & 0x3FF) < 0)` on an unsigned-ish mask; `*sProgress = *cProgress;` followed by a comparison against `*sProgress`), so the intended 256-step wraparound protection never engages. | `Basedef.cpp:5473-5507` |
| F-19 | Seven `BASE_GetLanguage` overloads are provided, and every one of them treats the *catalogue entry as the format string* (`sprintf(str, g_pMessageStringTable[idx], ...)`), so a stray `%s`/`%n` in `Language.txt` changes behaviour and reads stack memory. The buffer has no size parameter; all four in-project call sites pass the shared global `char temp[4096]` (`Server.h:382`), which bounds the immediate risk but also means a message longer than 4096 bytes overflows a global shared with every other handler. The out-of-range path writes `"[UNHANDLED]"` with an unbounded `strcpy`. | `Basedef.cpp:1258-1312`; `Server.h:382`; sites `_MSG_GetItem.cpp:98`, `_MSG_Quest.cpp:1627`, `:1634`, `:1731` |
| F-20 | The component holds ~12.6 MiB (~13.2 MB) of process-global writable data (`g_pGuildName` 7.5 MiB, `g_pClientGuildName` 3.0 MiB, `g_pAttribute` 1.0 MiB, `g_pItemList` ~0.9 MiB, `g_pMessageStringTable` 0.25 MiB) in each of the two executables, with no encapsulation and no thread-safety discipline. | `Basedef.cpp:739-755` |
| F-21 | 34 of the 93 unique exported `BASE_*` names are never called from outside the component (full list in Section 6.4) — including `BASE_GetHitRate`, `BASE_GetDamageRate`, `BASE_GetAccuracyRate`, `BASE_GetMobCheckSum`, `BASE_GetDestByAction`, `BASE_UpdateItem2` and the entire content/checksum/build-helper family; `BASE_GetHttpRequest` is a hard-coded `return 1;` stub with no caller, and the 3-argument `BASE_GetSkillDamage(dam,ac,combat)` overload is shadowed by its 4-argument sibling at every call site. | Section 6.4; `Basedef.cpp:6337-6341`, `:1446` vs `:6342` |
| F-22 | Server-path error handling uses blocking `MessageBox`/`MessageBoxA` modal dialogs (31 call sites) for missing/corrupt content files, so a missing CSV in a service context stalls the process instead of failing deterministically. | `Basedef.cpp:2752`, `2809`, `2839`, `4680`, `4701`, `4756`, `4778-4795`, `4836-4892`, `4975`, `4999`, `5070-5079`, `5136`, `5177`, `5224`, `5231`, `5865`, `5919`, `5958`, `6010`, `6122`, `6165`, `6193`, `6229`, `6325` |

### Quantitative profile

| Metric | Value |
|--------|-------|
| Total lines | 6,814 |
| Blank lines | 1,686 |
| Comment lines (incl. one 118-line block comment) | ~199 (~2.9 %) |
| Non-blank, non-comment lines | ~4,929 |
| Function definitions | 101 |
| Function declarations in `Basedef.h` | 101 (`Basedef.h:2331-2435`) |
| Overload families | `BASE_GetLanguage` (7), `BASE_GetSkillDamage` (2), `BASE_GetGuild` (2), `BASE_UpdateItem`/`BASE_UpdateItem2` |
| Extern data symbols declared in header | 47 (`Basedef.h:2441-2501`) |
| File-scope tables/globals defined | 30 |
| Decision points (`if`/`for`/`while`/`switch`/`case`) | 1,234 (111 of them inside the disabled `BASE_CheckPacket` body) |
| Largest functions | `BASE_GetCurrentScore` 1,141 lines / 204 decisions; `BASE_GetRoute` 290 / 31; `BASE_ReadItemListFile` 176 / 20; `BASE_InitializeMobname` 191 / 27 |
| Afferent call sites (measured, excl. self) | 639 `BASE_*` + 24 `GetItemPointer` = 663 |
| Afferent caller translation units (measured) | 47 |
| Distinct exported names actually called externally | 59 of 93 unique names (34 never used outside the file) |
| Efferent project dependencies | 2 headers (`Basedef.h`, `ItemEffect.h`) + 1 content data plane |
| Approximate writable global footprint | ~12.6 MiB (~13.2 MB) per executable |

---

## 2. Data Flow Analysis

### 2.1 Primary startup / content-load flow (both executables)

```
1. Process start (TMSrv WinMain / DBSrv WinMain)
2. BASE_InitModuleDir()                     -> SetCurrentDirectory(exe dir)          Basedef.cpp:2608
3. BASE_InitializeServerList()              -> serverlist.txt  -> g_pServerList      Basedef.cpp:6218
                                              -> writes ./serverlist.bin (obfuscated)
4. BASE_InitializeBaseDef()                 -> orchestration only                   Basedef.cpp:2564
   4.1 BASE_InitializeSkill()               -> SkillData.csv   -> g_pSpell[103]     Basedef.cpp:6001
   4.2 BASE_InitializeEffectName()          -> ItemEffect.h    -> EffectNameTable   Basedef.cpp:5163
   4.3 BASE_InitializeItemList()            -> itemlist.csv    -> g_pItemList       Basedef.cpp:4963
                                             -> extraitem.csv
                                             -> BASE_WriteItemList -> ItemList.bin + ../ItemCSum.h
   4.4 BASE_InitializeInitItem()            -> InitItem.csv    -> g_pInitItem       Basedef.cpp:5947
   4.5 BASE_InitializeGuildName()           -> Guilds.txt      -> g_pGuildName      Basedef.cpp:6153
   4.6 BASE_InitializeAttribute()           -> AttributeMap.dat-> g_pAttribute       Basedef.cpp:6307
5. BASE_ApplyAttribute(pHeightGrid, 4096)   -> stamp 127 on attributed cells        Basedef.cpp:2583
6. BASE_InitializeMessage("Language.txt")   -> g_pMessageStringTable                 Basedef.cpp:4637
7. BASE_InitializeHitRate()                 -> g_pHitRate[1024] (lazy prerequisite)  Basedef.cpp:5244
8. BASE_InitializeMobname("MobName.txt", 0) -> g_pEnglish[600][3][16]                Basedef.cpp:4712
9. Server consumes g_pInitItem[] -> CreateItem() per entry                            TMSrv/Server.cpp:7166
```

Two important ordering facts: (a) `BASE_InitializeMessage` and `BASE_InitializeHitRate` are **not** called by `BASE_InitializeBaseDef` — they are invoked separately by the host (`imple.cpp:253`, `Server.cpp:3476`, `Server.cpp:7160`), so any consumer of `BASE_GetLanguage`/`BASE_GetDoubleCritical` that runs before those calls sees an uninitialised table; (b) `BASE_InitializeBaseDef` returns `1` unconditionally and ignores the return value of every loader it calls, so a partially failed load is indistinguishable from success.

### 2.2 Score-recomputation flow (the hot path)

```
1. CMob::GetCurrentScore(idx) iterates mobs and calls
   BASE_GetCurrentScore(MOB, Affect, &extra, &ExpBonus, &ForceMobDamage,
                        isSummon, &Accuracy, &HpAbs, &ForceDamage)      CMob.cpp:674
2.   preserve CurrentScore.Hp/Mp
3.   CurrentScore = BaseScore                                            Basedef.cpp:2979
4.   BASE_GetMobAbility(EF_AC, EF_ACADD, EF_DAMAGE, EF_HP, EF_MP,
                        EF_STR, EF_INT, EF_DEX, EF_CON)                  Basedef.cpp:2984-2995
5.     -> per equipped item: BASE_GetItemAbility(...)
6.          -> g_pItemList[idx] static effects (12 slots)
7.          -> item instance effects (3 slots)
8.          -> sanc decode (BASE_GetItemSanc) and (sanc+10)/10 scaling
9.          -> mount tables g_pMountBonus / g_pMountTempBonus
10.  soul affect (Type 29) -> Int/Con multipliers -> HP/MP doubling      Basedef.cpp:3005-3122
11.  specials (clamp 320), magic, Run, Att, regen, critical              Basedef.cpp:3126-3168
12.  clan/mantle normalisation, class-0 armour skill                     Basedef.cpp:3170-3216
13.  HP%/MP% scaling (EF_HPADD + EF_HPADD2 + 100)                        Basedef.cpp:3219-3239
14.  affect loop #1 (types 1..42)                                        Basedef.cpp:3255-3777
15.  affect loop #2 (types 8, 34, 35)                                    Basedef.cpp:3780-3900
16.  sanc>=9 refined-slot bonuses (slots 1..7)                           Basedef.cpp:3902-3934
17.  class-3 skill bits 2/7/18/22                                        Basedef.cpp:3936-3977
18.  Kibita soul override (Str/Int 2000, MaxHp 10000)                    Basedef.cpp:3979-3992
19.  naked-face damage bonus                                             Basedef.cpp:3995-3996
20.  DAMAGEMULTI percentage                                              Basedef.cpp:3998-3999
21.  mount speed floor, Run/Att clamps, AttackRun repack                 Basedef.cpp:4022-4055
22.  final clamps: Hp/Mp<=Max, Magic<=254, Critical<=255, Resist 0..100   Basedef.cpp:4058-4111
23. CMob::GetCurrentScore post-processing (fairy bonuses, etc.)          CMob.cpp:676+
```

### 2.3 Combat resolution flow

```
1. Client sends _MSG_Attack
2. TMSrv/_MSG_Attack.cpp validates target/range (outside this component)
3. BASE_GetDoubleCritical(&MOB, &cProgress, &m->Progress, &flags)        _MSG_Attack.cpp:440
     -> g_pHitRate[cProgress] vs 100*((AttackRun>>4)-5); rand()%255 vs Critical
4. damage base assembled by the handler; master/AC passed in
5. BASE_GetDamage(dam, ac, master)                                       _MSG_Attack.cpp:457
     -> ac/2 subtraction, 12-combat band roll, three-regime curve, floor 1
6. for skills: BASE_GetSkillDamage(skillnum, MOB, Weather, WeaponDamage) _MSG_Attack.cpp:557/594
     -> instance-type switch, class branch, weather multiplier, Magic scaling
7. mana charge: BASE_GetManaSpent(skillnum, SaveMana, Special)           _MSG_Attack.cpp:241
8. BASE_GetHitRate / BASE_GetAccuracyRate / BASE_GetDamageRate are NOT used
```

### 2.4 Movement / routing flow

```
1. Client _MSG_Action carries PosX/PosY + Route[24] (client-computed path)
2. TMSrv/_MSG_Action.cpp:262 BASE_GetRoute(PosX, PosY, &destx, &desty,
                                           m->Route, steps, pHeightGrid)
     -> rewrites Route[] with server-validated digits; clamps dest to last legal cell
3. Alternative decode: BASE_GetDestByAction(&x, &y, &mv)                  Basedef.cpp:5367
     (declared but not called anywhere in the project)
4. AI mobs: CMob.cpp:896/905/948/957/1009/1018/1204/1213 call BASE_GetRoute
5. Step cost/ETA: BASE_GetSpeed(&CurrentScore) -> AttackRun & 15 clamped 1..6
6. Distance: BASE_GetDistance(x1,y1,x2,y2) -> g_pDistanceTable for dx,dy<=6 else max+1
```

### 2.5 Inventory / equip / trade flow

```
1. _MSG_UseItem / _MSG_TradingItem / _MSG_GetItem arrive
2. BASE_GetItemAbility(item, EF_POS / EF_CLASS / EF_LEVEL / EF_REQ_*)  -> eligibility inputs
3. BASE_CanEquip(item, score, Pos, Class, pBaseEquip, extra)            Basedef.cpp:4293
4. BASE_CanCarry(item, carry, DestX, DestY, &error)                     Basedef.cpp:4413
   or BASE_CanCargo(item, cargo, DestX, DestY)                          Basedef.cpp:4480
5. BASE_SetItemAmount / BASE_GetItemAmount for stackables
6. trade window: BASE_CanTrade(Dest, Carry, MyTrade, OpponentTrade, MaxCarry)
   -> BASE_SortTradeItem(OpponentTemp, 33) then first-fit packing        Basedef.cpp:4588
7. refinement: BASE_GetItemSanc -> BASE_GetSuccessRate -> BASE_SetItemSanc
8. audit: BASE_NeedLog(item, money) decides whether the operation is logged
```

### 2.6 State ownership

The component owns no entity objects. It owns only: (a) the content tables listed in Section 4.3, and (b) four mutable scalars `g_HeightWidth`, `g_HeightHeight`, `g_HeightPosX`, `g_HeightPosY` (all initialised to 4096/4096/0/0 at `Basedef.cpp:759-762`), `CurrentClientGuild` (`:59`) and `g_dwInitItem` (`:757`). Every `STRUCT_MOB`, `STRUCT_ITEM`, `STRUCT_AFFECT` and height grid it manipulates is caller-owned memory passed by pointer/reference, and several "getters" mutate their arguments (`BASE_GetCurrentScore`, `BASE_GetMobCheckSum`, `BASE_SetItemSanc`, `BASE_SetItemDate`, `BASE_CheckFairyDate`, `BASE_GetBonusScorePoint`, `BASE_GetBonusSkillPoint`, `BASE_GetHpMp`, `BASE_SortTradeItem`, `BASE_UpdateItem`).

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| ID | Rule Type | Rule Description | Location |
|----|-----------|------------------|----------|
| BR-01 | Baseline data | Four classes × (Str, Int, Dex, Con, HP, MP) baselines; per-level HP/MP increments per class | `Basedef.cpp:39-57`, `1174-1208` |
| BR-02 | Accounting | Ability-point budget per progression tier (Mortal/Arch/Celestial/CelestialCS/SubCelestial) and deficit claw-back | `Basedef.cpp:858-1172` |
| BR-03 | Accounting | Skill-point budget = 3/level (+tier bonuses) − Σ cost of learned skill bits | `Basedef.cpp:810-856` |
| BR-04 | Derivation | Item ability resolution precedence: static (12) + instance (3) + requirement fields + mount tables, then sanc scaling | `Basedef.cpp:1497-2093` |
| BR-05 | Encoding | Refinement ("sanc") packed into effect byte: tier 0-15 and success counter; decode/encode/gem/success-rate/growth-rate | `Basedef.cpp:2095-2285` |
| BR-06 | Aggregation | Mob ability = Σ over 16 equip slots, with dual-wield merge, full-set AC bonus, RANGE max | `Basedef.cpp:2374-2501` |
| BR-07 | Derivation | Master score pipeline: item adds → soul → affect loop → refined slots → class skills → clamps | `Basedef.cpp:2973-4112` |
| BR-08 | Validation | Equip eligibility: slot bitmask, class bit, progression tier, two-hand pairing, right-hand 130 % requirement penalty | `Basedef.cpp:4293-4411` |
| BR-09 | Validation | Carry (9×7) and Cargo (9×14) grid occupancy using 8 item shapes | `Basedef.cpp:4413-4537` |
| BR-10 | Validation | Trade compaction: clear traded slots, sort opponent items by grid size, first-fit pack | `Basedef.cpp:4539-4635` |
| BR-11 | Combat formula | Physical damage: `(dam - ac/2) × rnd/100` with 3-regime curve, floor 1 | `Basedef.cpp:1225-1256` |
| BR-12 | Combat formula | Skill damage by skill id: instance-type switch, class formulas, weather multipliers, Magic scaling | `Basedef.cpp:6342-6442` |
| BR-13 | Resource | Mana cost = spell base × (Special/2+100)/100 × (100−SaveMana)/100 | `Basedef.cpp:5415-5421` |
| BR-14 | Combat formula | Hit rate from Dex/Level ratio, clamped 40-95; accuracy and damage-rate helpers | `Basedef.cpp:5423-5464` |
| BR-15 | RNG state machine | Critical/double-critical progress ring over `g_pHitRate[1024]`, 255-bucket critical roll | `Basedef.cpp:5466-5536`, `5244-5290` |
| BR-16 | Pathfinding | Greedy 8-neighbour route with ±`MH`(=8) slope constraint, ≤23 steps, target clamping | `Basedef.cpp:5538-5826` |
| BR-17 | Decoding | Route digit → coordinate delta | `Basedef.cpp:5367-5413` |
| BR-18 | Geometry | Chebyshev-with-correction distance table for dx,dy≤6, else max+1 | `Basedef.cpp:5828-5849` |
| BR-19 | Grid height | Item place/remove height state machine with 6 ground masks in 4 rotations | `Basedef.cpp:5292-5365` |
| BR-20 | Terrain | Attribute stamping: `g_pAttribute[y>>2][x>>2] & 2` ⇒ height 127 | `Basedef.cpp:2583-2606` |
| BR-21 | Content loading | Nine loaders for CSV/text/binary content + XOR-0x5A/0xFF obfuscation + checksum + C-header emission | `Basedef.cpp:2696-2906`, `4637-5242`, `5851-6335` |
| BR-22 | Integrity | Mob checksum `Σ equip[i].index*i + Σ carry[i].index*i`; character-name syntax rules | `Basedef.cpp:2503-2562` |
| BR-23 | Audit policy | Which item operations must be written to the audit log | `Basedef.cpp:766-808` |
| BR-24 | Spatial lookup | Guild-zone / arena / village rectangle membership; guild id from item bytes; sub-guild from item index | `Basedef.cpp:4164-4267`, `4207-4245`, `4175-4204` |
| BR-25 | Time-bounded items | Expiry stamping and comparison (day/month/year); fairy countdown (day/hour/min) | `Basedef.cpp:6732-6814`, `6462-6473` |
| BR-26 | Wire projection | Visual item/anct code composition for client display; textual item code | `Basedef.cpp:6598-6731`, `1479-1495` |
| BR-27 | Packet validation | Per-message-type size table — **disabled** | `Basedef.cpp:6475-6597` |
| BR-28 | Localisation | Message-catalog lookup (7 overloads), mob-name translation, Korean initial derivation | `Basedef.cpp:1258-1312`, `6444-6460`, `4114-4162`, `4712-4901` |
| BR-29 | Presentation | Guild name formatting from 16-bit id (`group-server-guild`) with Hangul fallback glyphs | `Basedef.cpp:1314-1444` |
| BR-30 | Utility | Week number from epoch days; HTTP helper stub | `Basedef.cpp:6462-6473`, `6337-6341` |

---

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Class baselines and per-level HP/MP growth

**Overview**:
Every character's HP/MP pool is derived from four per-class baseline sextuples plus per-class per-level increments. The rule fixes the starting stat line of each of the four playable classes and the growth curve that `BASE_GetHpMp` applies when recalculating maximum HP and MP.

**Detailed description**:
`BaseSIDCHM[4][6]` (`Basedef.cpp:39-45`) is the master baseline: class 0 (TK) `{Str 8, Int 4, Dex 7, Con 6, HP 80, MP 45}`, class 1 (FM) `{5, 8, 5, 5, 60, 65}`, class 2 (BM) `{6, 6, 9, 5, 70, 55}`, class 3 (HT) `{8, 9, 13, 6, 75, 60}`. These six numbers are used for three distinct purposes throughout the component: as the HP/MP base, as the "zero point" from which spent ability points are measured in `BASE_GetBonusScorePoint` (`:865-868`), and as the reference against which stat gains from gear are converted to HP/MP in `BASE_GetCurrentScore` (`:2984-2995`). The array is also read directly by `_MSG_Quest.cpp` and `_MSG_UseItem.cpp`, so a change to it propagates to quest and consumption logic outside this component.

`BASE_GetHpMp` (`:1174-1208`) implements the growth curve. `MaxHp = BaseSIDCHM[cls][4] + (mob->BaseScore.Con − BaseSIDCHM[cls][3]) × 2 + effectiveLevel × g_pIncrementHp[cls]`, with `g_pIncrementHp = {3, 1, 1, 2}` (`:56`) and `g_pIncrementMp = {1, 3, 2, 1}` (`:57`). The `effectiveLevel` term is the single most consequential branch: for `ClassMaster == ARCH (1)` or `MORTAL (2)` it is `CurrentScore.Level`, but for every higher tier it is `CurrentScore.Level + MAX_LEVEL` (i.e. +399, `Basedef.h:117`), which is what makes Celestial-tier characters carry a much larger pool for the same displayed level. Both results are then clamped to `MAX_HP`/`MAX_MP` (2,000,000,000, `Basedef.h:203-204`).

The function writes both `BaseScore.MaxHp/MaxMp` and `CurrentScore.MaxHp/MaxMp`, so it resets any previously applied item or buff HP bonus. It returns `FALSE` — without writing anything — when `Class` is outside `[0, MAX_CLASS)`. Callers are `ProcessDBMessage.cpp:773`, `_MSG_UseItem.cpp:2113`, `_MSG_UseItem.cpp:2147`, `_MSG_UseItem.cpp:3156` and `_MSG_Quest.cpp:1882`; all of them then rely on a later `BASE_GetCurrentScore` pass to re-add item HP.

**Rule workflow**:
1. Reject `cls < 0 || cls >= 4` → `return FALSE` (`:1178-1179`).
2. `basehp = BaseSIDCHM[cls][4]`; `stathp = (BaseScore.Con − BaseSIDCHM[cls][3]) * 2` (`:1182-1183`).
3. `levelhp = (tier <= ARCH ? CurrentLevel : CurrentLevel + 399) * g_pIncrementHp[cls]` (`:1184`).
4. `t_hp = basehp + stathp + levelhp`; clamp to `MAX_HP` (`:1186-1189`).
5. Write `BaseScore.MaxHp` and `CurrentScore.MaxHp` (`:1191-1192`).
6. Repeat 2-5 for MP using `BaseSIDCHM[cls][5]`, `Con→Int`, `g_pIncrementMp` (`:1194-1205`).
7. `return TRUE`.

---

### Business Rule: BR-02 — Ability-point budgeting across progression tiers

**Overview**:
The server does not trust the client's stat values. `BASE_GetBonusScorePoint` recomputes, from first principles, how many ability points a character of a given tier and level is *allowed* to have spent, and rewrites `ScoreBonus` accordingly, clawing back any excess.

**Detailed description**:
The budget (`leveluse`) is computed per `ClassMaster` constant (`Basedef.h:178-182`: `ARCH = 1`, `MORTAL = 2`, `CELESTIAL = 3`, `CELESTIALCS = 4`, `SCELESTIAL = 5`). Mortal: `5 × level`, plus `5 × (level − 254)` from level 254, plus `10 × (level − 299)` from level 299, plus a **negative** `−8 × (level − 354)` from level 354 (`:876-885`). Arch: `6 × level + 6 × QuestInfo.Arch.MortalLevel + 6 × (level − 354)` from 354 (`:913-918`). Celestial, CelestialCS and SubCelestial share one formula — `6 × level + 100 × Arch.Cristal + 200 × Celestial.Reset + 1001`, plus a step bonus from `Celestial.ArchLevel` (1→+100, 2→+300, 3→+600, 4→+900, 5→+1200), plus five level brackets (`+6` per level ≥120, `+2` per level ≥150, ≥170, ≥180, ≥190) (`:942-982`, `:1006-1046`, `:1090-1130`). CelestialCS additionally credits half of the sub-celestial level progression (`(lvl*6)/2` plus the same five brackets) (`:1048-1065`), and `SCELESTIAL` does the same using `CelestialLevel` (`:1132-1148`).

Usage is measured relative to the class baseline: `usestr = BaseScore.Str − BaseSIDCHM[cls][0]`, and likewise for Int/Dex/Con (`:865-868`). Over-budget handling is deliberately crude: `dif = totaluse − leveluse`, and the whole deficit is subtracted from the **first** attribute able to absorb it, scanning `Str → Int → Dex → Con` with a strict `else if` chain (`:887-904`). A character who over-invested 40 points in Dex but has 40 spare Str will lose the Str, not the Dex. After the claw-back, `totaluse` is recomputed from scratch and `ScoreBonus = leveluse − totaluse` (`:903-905`).

Two behaviours are easy to miss. First, an unrecognised `ClassMaster` (0, or anything above 5) matches none of the five branches, so `ScoreBonus` is left at whatever value it already held and the function still returns `TRUE` (`:1171`). Second, the Mortal budget changes slope at every threshold: `+5` per level up to 253, `+10` in `[254,299)`, `+15` in `[299,354)`, then `+7` from 354 onwards (`5 + 10 − 8`), because the `−8 × (level − 354)` term is cumulative. The curve therefore keeps rising but flattens sharply above 354 — the highest-level characters earn the fewest new ability points per level. No test file exists to pin any of these thresholds (Section 10).

**Rule workflow**:
1. Validate class range; `return FALSE` if invalid (`:860-863`).
2. Compute `usestr/useint/usedex/usecon` against the class baseline (`:865-868`).
3. Select branch on `extra->ClassMaster` (`:870`, `907`, `940`, `1004`, `1088`).
4. Accumulate `leveluse` from level, quest fields, reset count, ArchLevel step and level brackets.
5. If `totaluse > leveluse`, subtract the deficit from the first attribute that can absorb it (`:887-903`).
6. Recompute `totaluse` and set `mob->ScoreBonus = leveluse − totaluse` (`:905`).
7. `return TRUE`.

---

### Business Rule: BR-03 — Skill-point budgeting

**Overview**:
Mirror of BR-02 for skill points: the allowed total is `3 × level` plus tier/quest bonuses, minus the summed cost of every bit set in the 24-bit `LearnedSkill` mask, resolved through the class's slice of the spell table.

**Detailed description**:
`spellperlevel` starts at `CurrentScore.Level × 3` (`:812`). Arch characters add 112; every tier that is neither Arch nor Mortal (Celestial and above) adds 1500 (`:814-818`). Levels above 199 add one point per level (`:820-823`), and completing the Orc-pill quest (`extra->QuestInfo.Mortal.PilulaOrc != 0`) adds 9 (`:825-828`).

Consumption walks the low 24 bits of the `long LearnedSkill` (`MAX_SKILL = 24`, `Basedef.h:111`): for each set bit `j`, it adds `g_pSpell[(cls × 24) + j].SkillPoint` (`:838-844`). The skill index space is therefore class-major: class 0 owns indices 0-23, class 1 owns 24-47, and so on, which matches the `kind = skillnum % MAX_SKILL / 8 + 1` arithmetic used by BR-12. A class outside `[0,4)` aborts with `FALSE` before any consumption is computed (`:830-833`).

The remainder is written to `mob->SkillBonus` only when it is non-zero, and the function returns `FALSE` in that case and `TRUE` when the remainder is exactly zero (`:846-855`). Callers therefore receive an inverted-looking success signal: `TRUE` means "the budget is exactly balanced", `FALSE` means "a bonus was written". Any negative remainder is written as a negative `SkillBonus` (the field is `unsigned short` in `Basedef.h:467`, so it wraps), which is a latent corruption path for a character with more learned-skill cost than budget.

**Rule workflow**:
1. Validate class; `return FALSE` if invalid (`:830-833`).
2. `spellperlevel = Level × 3` (`:812`).
3. Add tier bonus: Arch +112; non-Arch/non-Mortal +1500 (`:814-818`).
4. Add `Level − 199` when positive; add 9 when the Orc-pill quest flag is set (`:820-828`).
5. For each set bit `j` in `LearnedSkill` (24 bits), accumulate `g_pSpell[cls*24+j].SkillPoint` (`:838-844`).
6. `rest = spellperlevel − spelluse`; if non-zero write `mob->SkillBonus = rest` and `return FALSE` (`:846-853`).
7. Otherwise `return TRUE`.

---

### Business Rule: BR-04 — Item ability resolution and refinement scaling

**Overview**:
`BASE_GetItemAbility`, `BASE_GetStaticItemAbility`, `BASE_GetBonusItemAbility` and their `Nosanc` variants are the four (five, counting both bonus forms) readers through which every other subsystem asks "what does this item give for effect type `Type`?". They define precedence between the item *definition* (item list) and the item *instance* (the three `stEffect` bytes), plus the refinement multiplier.

**Detailed description**:
Resolution order inside `BASE_GetItemAbility` (`:1647-1842`) is: (1) derived requirement fields — `EF_LEVEL` from `ReqLvl` (or from `stEffect[1].cEffect − 1` for mounts 2330-2359), `EF_REQ_STR/INT/DEX/CON` from the list, `EF_POS` from `nPos`; (2) all 12 static list effects matching `Type` (skipped entirely for `EF_INCUBATE`); (3) the five mount accessors (`EF_MOUNTHP/MOUNTSANC/MOUNTLIFE/MOUNTFEED/MOUNTKILL`) which return raw effect bytes; (4) mount-relative combat stats from `g_pMountBonus[idx−2360]` — `EF_DAMAGE = (lv+20) × bonus[0] / 100`, `EF_MAGIC = (lv+15) × bonus[1] / 100`, `EF_PARRY = bonus[2]`, any `EF_RESIST1..4 = bonus[3]` — gated on `idx ≥ 2362 && stEffect[0].sValue > 0`; (5) temporary mounts 3980-3994 from `g_pMountTempBonus[idx−3980]`; (6) the three instance effects; (7) `EF_RESISTALL` folded into every `EF_RESIST1..4` from both static and instance slots.

Several redirect rules exist so that "hidden" refinement effects are readable under their public code. `EF_CRITICAL (42)` is redirected to `EF_CRITICAL2 (71)` if the item carries 71 anywhere in slots 1-2 (`:1665-1669`); `EF_DAMAGE (2)` is redirected to `EF_DAMAGE2 (73)` only when `nPos == 32` (a two-handed weapon slot) and 73 is present (`:1671-1675`); `EF_ACADD (53)` → `EF_ACADD2 (72)` (`:1677-1681`); `EF_MPADD`/`EF_HPADD` → their `2` forms in the `Nosanc` variant (`:1516-1520`). `EF_DAMAGEADD (67)` and `EF_MAGICADD (68)` are hard-gated: they return 0 unless the item's `nUnique` is in `41..50` (`:1659-1663`).

The refinement multiplier is the last transformation: `sanc = BASE_GetItemSanc(item)`; if `sanc >= 9` **and** the `nPos` high nibble is non-zero (`nPos & 0xF00`), `sanc` is incremented (`:1809-1811`); then, for every `Type` outside a 19-entry exclusion list (grid, class, pos, wtype, range, level, the four requirements, volatile, incubate, incudelay, mobtype, itemtype, itemlevel, notrade, nosanc, donate), `value = value × (sanc + 10) / 10` (`:1813-1817`). Two post-rules follow: `EF_RUNSPEED` is hard-capped at 2 and given +1 when `sanc == 9` (`:1819-1826`), and `EF_GRID` is clamped to `0..7` (`:1835-1839`).

The variants differ in exactly three ways, and the differences are behaviourally significant. `*Nosanc` (`:1497-1645`) omits step (7)'s sanc multiplier entirely. `*Static` (`:1844-1991`) uses a **shorter** exclusion list (it omits `EF_VOLATILE`, `EF_MOBTYPE`, `EF_ITEMTYPE`, `EF_ITEMLEVEL`, `EF_NOTRADE`, `EF_NOSANC`, `EF_DONATE`), so boolean flag effects such as `EF_NOSANC (126)`/`EF_NOTRADE (127)` are multiplied by the refine factor in that path. `*Bonus` (`:1993-2050`) reads **only** instance effects plus `EF_RESISTALL`, returns early when `sanc == 0`, and returns `FALSE` for any mount index 2330-2389.

**Rule workflow**:
1. Range-check `sIndex` (guard form differs per variant — see F-05).
2. Resolve derived fields (`EF_LEVEL/EF_REQ_*/EF_POS`) from `g_pItemList[idx]`.
3. If `Type != EF_INCUBATE`, accumulate all 12 static list effects equal to `Type`, converting `EF_ATTSPEED` value 1 to 10.
4. If mount (2330-2389): evaluate mount accessors, then mount/temp-mount combat tables; return.
5. Accumulate the 3 instance effects equal to `Type` (again `EF_ATTSPEED` 1→10).
6. If `Type` is a resistance, add every `EF_RESISTALL` from list and instance.
7. Compute `sanc`; `sanc++` when `sanc >= 9 && (nPos & 0xF00)`.
8. If `Type` not in the exclusion list: `value = value * (sanc+10)/10`.
9. Apply `EF_RUNSPEED` cap / `EF_GRID` clamp; return `value`.

---

### Business Rule: BR-05 — Refinement ("sanc") encoding, decoding and success rate

**Overview**:
Item refinement level and the number of accumulated refine successes are packed into a single effect byte (`cValue`) on the item instance. Five functions in the component implement the codec and the two probability tables that consume it.

**Detailed description**:
Decoding starts by finding the carrier effect: any effect code in `116..125` (the per-tier refine markers) or `EF_SANC (43)`, scanning slot 0, then 1, then 2, first match wins (`:2102-2119`). The raw byte is then normalised: `9` stays `9`; `230-233 → 10`, `234-237 → 12`, `238-241 → 15`, `242-245 → 18`, `246-249 → 22`, `250-253 → 27` (the `REF_10..REF_15` constants, `Basedef.h:196-201`); anything else is `value % 10` (`:2121-2140`). So a legal sanc is one of `0..9`, `10`, `12`, `15`, `18`, `22`, `27` — the "reference" values are display-scale numbers, not contiguous levels.

Encoding is the inverse and is delegated to `BASE_SetItemSanc(dest, sanc, success)` (`:2239-2285`): `sanc < 0 → 0`; **`sanc > 15 → 0`** (a reset, not a clamp); `success` clamped to `0..20`; base packing `sc = sanc + 10 × success`; then tiers 10-15 are re-mapped to `230/234/238/242/246/250 + success`. The encoder writes into the first of the three slots whose effect is `EF_SANC` or `116..125`; if no such slot exists and slot 2 is also unsuitable, it returns `FALSE` without writing. Because the tier bands are only 4 values wide while `success` is allowed up to 20, `sanc = 10, success = 4` encodes as 234 and decodes back as tier 12 — the success counter and the tier share the same byte space and collide above 3 successes.

Two consumers read that byte differently. `BASE_GetItemSancSuccess` (`:2177-2200`) returns `value / 10` (integer), which for the 230+ encodings yields 23-25 rather than 0-3. `BASE_GetItemGem` (`:2143-2175`) returns `(value − 230) % 4` when `value >= 230`, i.e. the gem slot index, and `-1` otherwise. `BASE_GetSuccessRate(item, OriLacto)` (`:2202-2225`) early-returns 15 for `sanc == REF_10` with lacto ≠ 2, 100 for `sanc == REF_10` with lacto 2, then computes `g_pSancRate[OriLacto][sanc % 10 + 1] + successes × g_pSuccessRate[sanc % 10 + 1]`, returning 100 unconditionally for lacto 2. The rate tables are `g_pSancRate[3][12]` = `{100,100,100,85,70,40,10,0,0,0,0,0}` (PO), `{100,100,100,100,100,80,70,70,50,60,0,0}` (PL) and `{100,80,60,40,20,10,10,10,5,5,5,5}` (Amagos) (`:61-66`), combined with `g_pSuccessRate[11] = {5,5,5,5,4,4,3,3,2,1,0}` (`:104`). `OriLacto` outside `0..2` returns `FALSE`. Mounts (2330-2389) are excluded from every refine function. `BASE_GetGrowthRate` (`:2227-2237`) is the mount-only variant: it requires `2360 <= idx < 2390` and reads `g_pSancRate[2][stEffect[1].cEffect / 10]`.

**Rule workflow**:
1. Reject mounts 2330-2389 (`:2097`).
2. Find carrier effect (116-125, then 43) in slots 0→1→2.
3. Normalise: 230-253 bands → `REF_10..REF_15`; else `raw % 10`.
4. *Encoding path*: clamp sanc/success; `sc = sanc + 10*success`; override for tiers 10-15; write to carrier slot or `FALSE`.
5. *Rate path*: `sanc==10` shortcuts; else index `g_pSancRate[lacto][sanc%10+1]` and add `successes × g_pSuccessRate[...]`; lacto 2 forces 100.
6. *Gem path*: `value >= 230` → `(value-230)%4`; else `-1`.

---

### Business Rule: BR-06 — Mob ability aggregation across equipment

**Overview**:
`BASE_GetMobAbility` sums one effect type across all 16 equipment slots, with slot-specific exclusion, a dual-wield merge model and a full-set armour bonus. `BASE_GetMaxAbility` takes the maximum instead of the sum.

**Detailed description**:
The loop walks `mob->Equip[0..15]` (`MAX_EQUIP = 16`). Empty slots are skipped **except slot 7** (`:2400-2401`), which is the right-hand weapon slot and must still participate in the dual-wield branch even when empty. Slots 1-5 (armour pieces) cache their `nUnique` into `nUnique[]` for the full-set test. `EF_DAMAGE` skips slot 6 and `EF_MAGIC` skips slot 7 (`:2406-2410`), i.e. the left hand contributes physical damage only through the pair logic and the right hand contributes magic only as a stat carrier.

The dual-wield rule (`:2412-2461`) fires when the *right* hand (slot 7) is reached for `EF_DAMAGE`. It reads the physical damage of slots 6 and 7 (each as `EF_DAMAGE + EF_DAMAGE2`), reads both `nUnique` types, and if **both** types are non-zero it takes the larger weapon at 100 % and the smaller at `multi` percent: 50 % when the two weapons share a unique type, 30 % when they differ, and **100 %** when the character is class 3 (Huntress) with skill bit `1<<10` ("Pericia do caçador") or class 0 (TK) with skill bit `1<<9` ("Mestre das Armas"). When either type is zero it simply adds the larger single-weapon damage. Once the pair is processed the loop `continue`s, so no per-slot accumulation happens for `EF_DAMAGE` on slot 7.

The full-set rule (`:2473-2477`) applies only to `EF_AC`: if slot 1 has a non-zero unique type and slots 1-5 all share the same type, the accumulated AC is multiplied by 105/100. `EF_ATTSPEED` values of 1 are converted to 10 during accumulation (`:2467-2468`). `EF_RANGE` short-circuits the entire loop and instead takes the maximum across slots, then applies a floor of 2 for bow-class characters (class derived as `Equip[0].sIndex / 10 == 3`) that have learned skill bit `0x100000` (`:2379-2392`). There is no bounds check on `mob->Equip[i].sIndex` before `g_pItemList[item]` is indexed at line 2404, so a corrupted or hostile item index above `MAX_ITEMLIST` reads out of bounds.

**Rule workflow**:
1. If `Type == EF_RANGE`: `value = BASE_GetMaxAbility`; apply bow floor 2; return (`:2379-2392`).
2. For each of 16 slots: skip empty unless slot 7; cache `nUnique` for slots 1-5.
3. If slot 7 and `Type == EF_DAMAGE`: dual-wield merge (50/30/100 %) and `continue`.
4. Otherwise `value += BASE_GetItemAbility(&Equip[i], Type)` with `EF_ATTSPEED` 1→10.
5. If `Type == EF_AC` and slots 1-5 uniques are all equal and non-zero: `value = value*105/100`.
6. Return `value`.

---

### Business Rule: BR-07 — Current-score derivation pipeline

**Overview**:
`BASE_GetCurrentScore` is the authoritative rule that turns `BaseScore` + equipment + affects + progression tier into the `CurrentScore` that the client is shown and that combat uses. It is 1,141 lines long, has 204 decision points, and mutates eleven different fields of the mob plus four of the five out-parameters.

**Detailed description**:
The function starts by resetting `Rsv` and re-deriving `CurrentScore` from `BaseScore` while preserving current `Hp`/`Mp` (`:2975-2982`). It then applies item contributions one effect type at a time through `BASE_GetMobAbility`: `EF_AC + EF_ACADD` into AC, `EF_DAMAGE` into Damage, `EF_HP`/`EF_MP` into maxima, and `EF_STR/INT/DEX/CON` into stats (`:2984-2995`). Because these are added to `BaseScore` values which already include base attributes, the ordering matters: item stat gains are present before the soul multipliers of the next block and before every affect multiplier, so soul and buff percentages compound on top of gear.

The soul block (`:2999-3124`) only runs when `isSummon == 0`. It scans the 32 affect slots for `Type == 29`; for a Mortal below level 369 it merely sets `isKibitaSoul` (`:3058-3059`); for Mortal at 369+ it applies `1.8f` single-stat or `1.6f/1.4f` dual-stat multipliers; for all higher tiers it applies `2.2f` single or `1.8f/1.4f` dual (`:3011-3107`). The multipliers are applied to local copies `bInt`/`bCon` only — Str/Dex multipliers are applied later, inside the affect loop (`:3579-3751`) — which is why the HP/MP consequence is computed here as `ItemInt = bInt − BaseScore.Int`, `MPDelta = ItemInt*2`, `HPDelta = ItemCon*2`, and then **added to the maxima twice**: `mhp = MaxHp + HPDelta; MaxHp += mhp` (`:3111-3122`). Every non-summon mob of every class therefore ends up with `MaxHp = 2 × MaxHp + 2 × ItemCon` and `MaxMp = 2 × MaxMp + 2 × ItemInt`, with or without any soul affect.

Specials are computed as `Special[0] + EF_SPECIAL1` and `Special[1..3] + EF_SPECIALn + EF_SPECIALALL` (the "all" effect is not applied to `Special[0]`), each capped at 320 (`:3126-3149`), but the affect loop type 15 later re-caps them at 400 (`:3394-3425`) — the two caps disagree. `Magic` is `(EF_MAGIC + EF_MAGICADD + 1) / 4` (`:3153-3154`); `Run` is `AttackRun & 15 + EF_RUNSPEED` (`:3156-3157`); `Att` is `(BaseAttackRun / 16) × 10 + EF_ATTSPEED` (`:3159-3160`); `Critical` is `EF_CRITICAL / 4` (`:3168`). Face and clan normalisation follows (`:3170-3205`): a face index below 4 (i.e. no real head item) overwrites `Equip[0].stEffect[0]` with `EF_SANC`/0 and clears clan ids 7/8/9; specific mantle indices in slot 15 force clan 7 or 8 unless the mob is clan 4.

The main affect loop (`:3255-3777`) implements 25 distinct affect types, including `Slow`, `Haste`, `Drag Reduction` (resistances reduced), `Power-up` (+4 DAMAGEMULTI, +30 damage, +5 magic), Dex percentage changes, attack-speed reduction, `Demijieop` (damage + `((Level*5/20)+Value)*3/2`, tripled for class 1 with skill `0x80000`), `Assault` (damage −, DAMAGEMULTI +), `Magic Shield` (AC + `Level/3+Value`), `Reduction in defense` (AC × `(100−Value)/100`), `Gross` (MaxHp ×9/10, DAMAGEMULTI += `Level/10+Value`), `Increase in skill` (all four specials + `Level/10+Value`, cap 400), `Polymorph` (the transformation block, `:3426-3528`), `Magic Stamina` (`:3529-3535`), block/parry/frost/drain/hide flags, the two soul blocks, `ForceMobDamage`, `ForceDamage`, `Protectors`, `Avoid falling`, MP→HP conversions and `+100 ExpBonus`. The polymorph block is a sub-system in itself: face values 22-25 and 32 encode wolf/bear/asta/final forms, each granting a specific tuple of damage/AC/HP/attack-speed bonuses; the item's displayed sanc is recomputed from `special3` and level against `pTransBonus[value].Unk17` (`:3476-3486`); damage, AC and MaxHp are then scaled by `min + (max−min) × Level / 200` ratios taken from `pTransBonus` (`:3488-3519`).

A second affect loop (`:3780-3900`) handles effects whose semantics need the *final* maxima: type 8 is a bitmask (bit1 resist +25 all, bit3 `AbsHp +20`, bit4 `+10 % HP and +10 % AC`, bit5 `+10 % HP and +10 % damage`, bit6 `Accuracy +50`, bit7 half of MaxMp moved to MaxHp), type 34 (`Divina`) adds 20 % to HP/MP/damage and 10 % to `Magic`, type 35 (`Vigor`) adds 10 % to HP/MP. After that, a refinement block grants `+25 AC` for `nPos` 4/8/128, `RSV_CAST` for `nPos` 16, and for two-handed weapon positions `64/192` either `+8 magic` (unique 47) or `+40 damage` (`:3902-3934`). Class-specific skill bits then apply (`:3936-3977`), the Kibita soul forces `Str = Int = 2000`, `MaxHp = 10000` and a final MP add (`:3979-3992`), and a "naked face" bonus adds `Str/2 + Dex/3 + Special[0] + effectiveLevel` to damage (`:3995-3996`).

The tail normalises everything: `DAMAGEMULTI` is applied to damage (`:3998-3999`); regen is clamped to `0..255`; `Merchant` high nibble is preserved and low nibble replaced with the mob's merchant id; `Att += AttackSpeedBonus + Dex/5`; `Run += RunSpeedBonus`; a mount in slot 14 can only *raise* `Run` to the mount table value when `face <= 4` (`:4022-4041`); `Run` is clamped `0..6` and `Att` `0..150` then divided by 10; the packed `AttackRun = Att*16 + Run` is written (`:4043-4055`); and finally Hp/Mp are clamped to their maxima, `Magic ≤ 254`, `Critical ≤ 255`, and the four resistances are clamped to `0..100` (`:4058-4111`).

**Rule workflow**:
1. `Rsv = 0`; preserve `Hp`/`Mp`; `CurrentScore = BaseScore`; restore `Hp`/`Mp` (`:2975-2982`).
2. Add item AC/ACADD/DAMAGE/HP/MP/Str/Int/Dex/Con (`:2984-2995`).
3. If `isSummon == 0`: apply soul Int/Con multipliers per tier; double MaxHp/MaxMp and add `2 ×` soul deltas (`:2999-3124`).
4. Compute specials (cap 320), SaveMana, Magic, Run, Att, Regen, Critical (`:3126-3168`).
5. Normalise face/clan; apply class-0 armour skill (`:3170-3216`).
6. Scale MaxHp/MaxMp by `EF_HPADD + EF_HPADD2 + 100` and `EF_MPADD + EF_MPADD2 + 100` (`:3219-3239`).
7. Run affect loop #1 (types 1,2,3,4,5,6,7,9,10,11,12,13,14,15,16,19,21,24,25,26,27,28,29,30,31,36,37,38,39,42) (`:3255-3777`).
8. Run affect loop #2 (types 8, 34, 35) (`:3780-3900`).
9. Apply sanc ≥ 9 slot bonuses (`:3902-3934`); class-3 skill bits (`:3936-3977`); Kibita override (`:3979-3992`).
10. Add naked-face damage; apply `DAMAGEMULTI` (`:3995-3999`).
11. Regen clamps, Merchant merge, Att/Run assembly, mount speed floor (`:4001-4041`).
12. Clamp Run 0-6, Att 0-150, pack `AttackRun` (`:4043-4055`).
13. Clamp Hp/Mp/Magic/Critical/Resist and write back (`:4058-4111`).

---

### Business Rule: BR-08 — Equipment eligibility

**Overview**:
`BASE_CanEquip` decides whether an item may occupy a given slot for a given character, combining a slot bitmask, a class bitmask, a progression-tier gate, a two-hand pairing rule and a right-hand requirement penalty.

**Detailed description**:
The first gate is the item's own slot mask: `tpos = BASE_GetItemAbility(item, EF_POS)`; the requested slot is legal only if bit `Pos` is set (`:4305-4309`). Slot 15 is unconditionally rejected (`:4300-4301`), and `Pos == -1` skips the mask test entirely (used for "is this item equippable anywhere" queries). For weapon slots 6 and 7 the function inspects the *other* hand: if either hand's `EF_POS` is 64 (one-handed marker), it enforces a pairing rule — an item with `nUnique == 46` may only be equipped if the other hand's position is exactly 128, and any other combination returns `FALSE` (`:4311-4337`). This is the shield/off-hand exclusivity rule.

The class gate reads the item's `EF_CLASS` bitmask. For non-Mortal tiers the class used for the bit test is **re-derived from the face item** (`Class = extra->MortalFace / 10`, `:4342-4343`) rather than from `mob->Class`, and a class-bit miss is fatal for Mortal characters but tolerated for slots 6/7 on higher tiers (`:4345-4351`) — i.e. a Celestial character may hold a weapon whose class bits do not match the face class, but may not wear armour that fails the test. Progression gating follows from `EF_MOBTYPE`: `ARCH` items are rejected for Mortal, `MORTAL` items are rejected for non-Mortal, and `CELESTIAL` items are rejected for both Mortal and Arch (`:4353-4363`). Slot 1 on any non-Mortal character only accepts index 747 or the 3500-3507 range (`:4365-4366`).

Stat requirements are evaluated **only for Mortal characters** (`:4368-4372`): level, Str, Int, Dex and Con are read from the item and compared to the passed `STRUCT_SCORE`; a shortfall in any one returns `FALSE` (`:4395-4408`). For the right hand with a non-zero weapon type, all five requirements are inflated by 130 % when the *last digit* of `EF_WTYPE` is greater than 1 (`:4379-4393`). The intended second tier of that penalty (150 % for weapon family 6) is unreachable because `weapontype %= 10` executes before `divweapon = weapontype / 10` (F-16).

**Rule workflow**:
1. Reject `sIndex <= 0 || >= MAX_ITEMLIST` (`:4295-4296`).
2. Reject `Pos == 15` (`:4300-4301`).
3. If `Pos != -1`: test `(EF_POS >> Pos) & 1`; reject on 0 (`:4303-4309`).
4. If `Pos` is 6 or 7 and `EF_POS` is 64: apply the unique-46 pairing rule (`:4311-4337`).
5. Class bit test; for non-Mortal derive class from `MortalFace/10`; reject Mortal on miss, reject non-Mortal outside slots 6/7 (`:4340-4351`).
6. `EF_MOBTYPE` gate for ARCH/MORTAL/CELESTIAL (`:4353-4363`).
7. Non-Mortal slot 1 whitelist 747 / 3500-3507 (`:4365-4366`).
8. For Mortal only: read level/Str/Int/Dex/Con; apply 130 % right-hand penalty when applicable; compare with `score` (`:4368-4408`).
9. `return TRUE`.

---

### Business Rule: BR-09 — Carry and Cargo grid occupancy

**Overview**:
Backpack space is modelled as a fixed 9×7 grid (`CARRYGRIDX/CARRYGRIDY`, `Basedef.h:92-93`) and warehouse space as 9×14 (`CARGOGRIDX/CARGOGRIDY`, `:90-91`). Each item occupies a shape taken from `g_pItemGrid[8][4][2]`, and placement fails if any covered cell is out of bounds or already occupied.

**Detailed description**:
The shape table (`Basedef.cpp:106-160`) defines eight footprints as up to eight (dx,dy) cells in a 2-column, 4-row mask: 1×1, 1×2, 1×3, 1×4, 2×1, 2×2, 2×3 and 2×4. The shape selector is `BASE_GetItemAbility(item, EF_GRID)`, which is clamped to `0..7` by BR-04. Occupied cells are keyed by linear index `i` of the existing item in the `carry[]`/`cargo[]` array; the slot grid origin is `tx = i % 9`, `ty = i / 9`, so the model assumes a dense 9-wide layout whose length is the array length (64 for carry, 128 for cargo).

The algorithm builds an occupancy map from every non-empty slot and then tests the candidate shape at the destination origin. Three failure modes exist: any covered cell outside the grid sets `*error = -1` and returns `FALSE` (`:4461-4466`); any covered cell already occupied sets `*error` to the occupant's index + 1 and returns `FALSE` (`:4468-4473`); otherwise `TRUE`. `BASE_CanCargo` is the same algorithm without an error channel.

The implementation contains a stride defect that materially changes collision results: the map is *written* with stride `CARRYGRIDX` (9) at line 4449 but *read* with stride `CARRYGRIDY` (7) at line 4468 (F-09); cargo does the same with 9 vs 14 at 4516/4531. The buffers themselves are adequate (63 cells in a 64-byte array and 126 in a 128-byte array), and their `memset` calls happen to zero exactly the used region (`MAX_CARRY−1`, `MAX_CARGO−2`), so the only behavioural error is the row-stride mismatch.

**Rule workflow**:
1. `grid = EF_GRID(item)`; copy shape into `SourGrid[8]` (`:4415-4419`).
2. Zero the occupancy map (`:4421-4423`).
3. For each non-empty existing item: resolve its shape, compute `tx/ty`, and stamp every covered cell with `i+1` (`:4425-4452`).
4. For each cell of the candidate shape: bounds-test (`error = -1` on failure); occupancy-test (`error = occupant` on failure) (`:4454-4475`).
5. `return TRUE`.

---

### Business Rule: BR-10 — Trade compaction

**Overview**:
When a trade completes, the server must merge the opponent's items into the receiver's carried inventory. `BASE_CanTrade` performs the merge against a copy and reports whether it fits; `BASE_SortTradeItem` orders the source items first.

**Detailed description**:
`BASE_CanTrade(Dest, Carry, MyTrade, OpponentTrade, MaxCarry)` (`:4588-4635`) first snapshots the opponent's trade window into a local `STRUCT_ITEM[15]`, then copies the whole `Carry` array over `Dest` (writing `sizeof(STRUCT_ITEM) × MAX_CARRY` bytes into the caller's destination buffer), then clears every slot named in `MyTrade[]` — treating `-1` and `255` as "no slot" (`:4596-4605`). Sorting is then applied to the opponent's items with ability type 33 (`EF_GRID`), i.e. descending grid size: the largest items are placed first, which is the standard descending-size first-fit heuristic.

The placement loop is a first-fit scan over `0..MaxCarry`, taking the first slot whose `sIndex == 0` and `memcpy`ing the item into it; if no slot is found the function returns `FALSE` (`:4609-4632`). The loop computes `x = j % 9`, `y = j / 9` and an `error` local but never uses any of them (`:4618-4621`) — grid shape and adjacency are not considered at all, so two 2×4 items can be placed into adjacent single slots that could never hold them in the real grid. `BASE_SortTradeItem` (`:4539-4586`) is a selection sort over `MAX_TRADE` entries using `BASE_GetItemAbility(item, Type)` as the key, compacting non-empty items to the front and leaving the tail zeroed; it moves items with two 4-byte copies rather than `memcpy` of the whole struct (`:4581-4582`).

**Rule workflow**:
1. Snapshot opponent window; copy carry array into `Dest` (`:4592-4594`).
2. Clear each slot listed in `MyTrade` (skip `-1`/`255`) (`:4596-4605`).
3. Sort opponent items descending by `EF_GRID` (`:4607`).
4. For each opponent item: find the first empty `Dest` slot below `MaxCarry`; `memcpy`; else `return FALSE` (`:4609-4631`).
5. `return TRUE`.

---

### Business Rule: BR-11 — Physical damage formula

**Overview**:
`BASE_GetDamage(dam, ac, combat)` converts an attack value and a defence value into a final damage number, using a bounded random band and a three-regime curve that converts small/negative results into bounded positive damage.

**Detailed description**:
The raw term is `dam − ac/2` (`:1227`), so armour halves effectiveness point-for-point. The random multiplier is `rnd = rand() % (12 − combat/2) + combat/2 + 99` (`:1229-1235`), i.e. a band of `99..110` percent that *narrows* as the caller's combat modifier grows: at `combat/2 = 0` the roll spans 99-110 (12 values), at `combat/2 = 7` (the clamp) it spans 106-110 (5 values). Higher combat therefore means both a higher floor and less variance. `tdam = rnd × tdam / 100` is computed in `long long` to avoid intermediate overflow (`:1237`).

The three-regime curve (`:1239-1250`) is the most game-specific part: `tdam < −50 → 0`; `−50 ≤ tdam < 0 → (tdam + 50) / 7`; `0 ≤ tdam ≤ 50 → 5 × tdam / 4 + 7`. Values above 50 pass through unchanged (only scaled by the roll). The effect is that even a fully-defended hit yields a small non-zero chip (0-7) and that low-end damage is boosted 1.25× plus a flat 7, which is the classic "minimum damage" curve of this engine family. The final `if (tdam <= 0) tdam = 1` guarantees at least 1 damage whenever the function is reached (`:1252-1253`).

Callers pass `master` as the third argument (`_MSG_Attack.cpp:457`, `:465`, `:601`; `GetFunc.cpp:1506`, `:1805`), and one caller halves the result afterwards (`_MSG_Attack.cpp:601-603`), showing that the curve is intentionally shared between player and reflected/mob damage paths.

**Rule workflow**:
1. `tdam = dam − ac/2` (`:1227`).
2. `combat = min(combat/2, 7)`; `delta = 12 − combat` (`:1228-1233`).
3. `rnd = rand() % delta + combat + 99` (`:1235`).
4. `tdam = rnd × tdam / 100` in 64-bit (`:1237`).
5. Apply regime: `<−50 → 0`; `[−50,0) → (tdam+50)/7`; `[0,50] → 5*tdam/4 + 7` (`:1239-1250`).
6. Floor at 1 (`:1252-1253`); return.

---

### Business Rule: BR-12 — Skill damage and mana cost (BR-13)

**Overview**:
`BASE_GetSkillDamage(skillnum, mob, weather, weapondamage)` is the per-skill damage model. It selects a formula by the spell's `InstanceType`, then by class, then modifies by weather and by the caster's `Magic` stat. `BASE_GetManaSpent` (BR-13) prices the cast.

**Detailed description**:
`InstanceType` drives the top-level switch (`:6344`, `:6363-6435`). For `InstanceType == 0` six hard-coded skill ids get bespoke formulas (11: `special/10 + affectbase`; 13: `3*special/4 + affectbase`; 41: `special/25 + 2`; 43: `special/3 + affectbase`; 44: `2*(3*special/20 + affectbase)`; 45: `special/10 + affectbase`), and every other skill in that family yields `dam = 0`.

For `InstanceType` 1-5 the model is class-based. Skill 97 is a special case (`15 × level + base`). Class 0 (TK) splits on skill group `skind = skillnum / 8`: group 1 uses `3 × weapondamage + 3 × Str + level + special + base`, all other groups use `special + base + weapondamage + level + Int/4 + Int/40`. Class 1 (FM) and class 2 (BM) share `Int/30 + Int/3 + level + base + 2 × special`. Class 3 (HT) uses `3 × weapondamage + 3 × Str + level/2 + special + base` (`:6386-6404`). Weather then multiplies: weather 1 (rain?) reduces `InstanceType == 2` to 90 % and raises `InstanceType == 5` to 130 %; weather 2 raises `InstanceType == 3` to 120 % (`:6406-6416`). Finally, everything except class-3 and TK-group-1 damage is scaled by `(4 × Magic + 100) / 100` and then all paths get `× 5 / 4` (`:6419-6425`). `InstanceType == 6` yields `3 × special / 2 + base`; `InstanceType == 11` yields `base`; any other instance type yields `mob->Magic` (`:6428-6435`). Skill 79 overrides everything with the mob's melee damage (`:6438-6439`).

The `special` input is `mob->CurrentScore.Special[kind]` where `kind = skillnum % MAX_SKILL / 8 + 1`, i.e. always 1, 2 or 3 — the engine never reads `Special[0]` for skills even though `Special[0]` is the melee special (`:6346`). Level is clamped to `0..MAX_LEVEL` (399) before use (`:6348-6354`).

`BASE_GetManaSpent` (`:5415-5421`) is two chained percentage scalings: `base × (Special/2 + 100) / 100`, then `× (100 − SaveMana) / 100`. Both divisions truncate, and there is no floor, so a spell with `SaveMana >= 100` costs 0.

**Rule workflow**:
1. Read `InstanceType`, `InstanceValue` (base), `AffectValue` (affectbase), level (clamped), `Special[kind]` (`:6344-6359`).
2. Branch on `InstanceType` (0 / 1-5 / 6 / 11 / other) and, inside 1-5, on class and skill group.
3. Apply weather multiplier for the affected instance types.
4. Apply the `Magic` scaling where the class/group allows it, then `×5/4`.
5. Override for skill 79; return `dam`.
6. Mana: `base × (Special/2+100)/100 × (100−SaveMana)/100`.

---

### Business Rule: BR-14 — Hit rate, accuracy and damage rate helpers

**Overview**:
Three small closed-form helpers compute hit probability from a Dex/Level contest, an accuracy ceiling from Dex, and a damage percentage from skill level plus `Special[0]`. Only the first is used indirectly (through nothing in this project); all three are exported but unused.

**Detailed description**:
`BASE_GetHitRate(att, def)` (`:5423-5441`) returns 95 immediately if the defender has zero Dex or zero level — the "cannot be missed" default. Negative inputs on either side return 0. Otherwise `parta = 60 × attDex / (defDex/2 + attDex)` (a saturating ratio that asymptotes to 60), `partb = 100 × (defLevel + attLevel) / defLevel` (a level-ratio term that is always ≥ 100), and `rate = partb × parta / 100`, clamped to `40..95`. The composition means a 10-level attacker against a 100-level defender gets `partb = 110`, and a Dex parity matchup gets `parta = 40`, i.e. 44 % — the 40 % floor then applies.

`BASE_GetDamageRate(att, Skill)` (`:5443-5453`) is `Skill + Special[0]` clamped to `0..200`. `BASE_GetAccuracyRate(att)` (`:5455-5464`) is `Dex/2 + 50` capped at 100. Both are declared in `Basedef.h:2408-2409` and have zero call sites in the project; `BASE_GetHitRate` likewise has zero call sites. The hit model actually in force in `_MSG_Attack` is the progress-ring model of BR-15, so these three formulas describe a hit model that is no longer wired in.

**Rule workflow**:
1. `def->Dex == 0 || def->Level == 0` → 95 (`:5425-5426`).
2. Any negative input → 0 (`:5428-5429`).
3. `parta = 60*attDex/(defDex/2+attDex)`; `partb = 100*(defLevel+attLevel)/defLevel` (`:5431-5432`).
4. `rate = partb*parta/100`; clamp high 95, low 40 (`:5433-5438`).

---

### Business Rule: BR-15 — Critical and double-critical progress machine

**Overview**:
Hits and criticals are not rolled independently per attack. The engine keeps two monotonic 10-bit progress counters per attacker (`cProgress` current, `sProgress` snapshot) and compares a pre-computed, deliberately non-uniform lookup table `g_pHitRate[1024]` against an attack-speed-derived threshold and against the `Critical` stat.

**Detailed description**:
`g_pHitRate` is generated once by `BASE_InitializeHitRate` (`:5244-5290`): the table is filled in four interleaved quadrants (values `Start`, `512−Start`, `Start+512`, `1024−Start` in round-robin) with `Start` incrementing after every full cycle, and every entry is capped at 999; entry 0 is then forced to 512. The resulting sequence is a deterministic sawtooth that makes the *probability of a hit* vary with the phase of the counter, which is what produces the engine's characteristic streakiness. If `BASE_InitializeHitRate` is never called, the table is all zeros and `g_pHitRate[cProgress]` is 0 for every counter except 0.

`BASE_GetDoubleCritical(mob, sProgress, cProgress, bDoubleCritical)` (`:5466-5536`) first masks each counter back into range when it exceeds 1024 (`:5473-5487`), computes `hitvalue[0] = 100 × ((AttackRun >> 4) − 5)` (the attack-speed threshold: `AttackRun >> 4` is the `Att` value packed by BR-07, so a slow attacker has a negative threshold and a fast one up to +1000) and `hitvalue[1] = mob->Critical` (`:5491-5494`), then fills a two-bit result: bit 0 is set when `g_pHitRate[cProgress] < hitvalue[0]` (a hit), bit 1 is set when `rand() % 255 < Critical` (a critical) (`:5509-5528`). Both counters are incremented at the end (`:5530-5534`), and the function returns 1.

The counter-comparison block at `:5496-5507` is dead: it assigns `*sProgress = *cProgress` and then compares `tProgress` against the freshly written `*sProgress`, so `tProgress < *sProgress` can only be true via the `+256` adjustment which itself requires the first comparison to be true. The intended 256-step wraparound protection therefore never applies, and the only effect of the block is that `sProgress` is overwritten with `cProgress` on every call over two NULL-checked pointers.

**Rule workflow**:
1. `*bDoubleCritical = 0`; reject null `cProgress` (`:5468-5471`).
2. Mask `cProgress` and `sProgress` into `0..1023` when out of range (`:5473-5487`).
3. `hitvalue[0] = 100 * ((AttackRun>>4) − 5)`; `hitvalue[1] = Critical` (`:5493-5494`).
4. Bit 0 = `g_pHitRate[cProgress] < hitvalue[0]`; bit 1 = `rand()%255 < hitvalue[1]` (`:5509-5528`).
5. Increment `sProgress` and `cProgress` (`:5530-5534`); return 1.

---

### Business Rule: BR-16 — Server-side route generation

**Overview**:
`BASE_GetRoute` validates and rewrites a client-supplied movement path. It walks at most `min(distance, MAX_ROUTE−1)` = 23 steps toward the requested target, accepting a step only if the destination cell's height is within ±`MH` (8) of the current cell's height, and it rewrites `*targetx`/`*targety` to the last cell actually reachable.

**Detailed description**:
Each iteration reads the current cell height `cul` and the eight neighbour heights `n, ne, e, se, s, sw, w, nw` (`:5556-5568`). It then runs a long ordered catalog of `(relative direction of target, neighbour passable)` tests, emitting the corresponding route digit and moving. The ordering encodes a preference for the primary axis: straight-toward-target moves are tested before diagonal fallbacks, so a target directly north produces `'2'` rather than `'3'`/`'1'`. The passability predicate is identical in every branch: `neighbour < cul + MH && neighbour > cul - MH`, i.e. a **slope** constraint of ±8 height units, not a "blocked" test — impassable terrain is expressed in the heightmap as a large discontinuity, not as a flag.

Direction encoding is the engine's own, and it is *not* the numeric keypad: `'2'` = `y−1`, `'3'` = `(x+1, y−1)`, `'6'` = `x+1`, `'9'` = `(x+1, y+1)`, `'8'` = `y+1`, `'7'` = `(x−1, y+1)`, `'4'` = `x−1`, `'1'` = `(x−1, y−1)` (`:5576-5808`). Termination happens on: reaching the target (`tx == x && ty == y`), leaving the grid (`<1` or `> dim−2`), the "adjacent but not reachable" short-circuit at `:5723-5728`, or exhaustion of the catalog (`Route[i] = 0; break`). The function returns 1 if at least one cell of movement occurred and 0 otherwise, and it writes the final position back through `targetx`/`targety` (`:5814-5825`).

Structural defects: `if (cul > MH) int kk = 0;` is a statement with no effect (`:5558-5559`); the same `int kk = 0` is repeated at `:5817`; the identical `(tx > x && ty < y)` diagonal test appears twice with different neighbours (`ne` at `:5584`, `e`/`n` at `:5651`/`:5660`), and after the early `if (tx == x+1 || ty == y+1 || tx == x−1 || ty == y−1)` break at `:5723`, the eight subsequent branches (`:5730-5808`) can only be reached when the *sum* of coordinate deltas is not exactly ±1 — which is most of the time but makes the catalog's intent ambiguous. Practically the function is a heuristic that always moves *something* toward the target if any neighbour is within the slope band.

**Rule workflow**:
1. Zero `Route[0..23]`; snapshot start position (`:5540-5545`).
2. Loop `i` from 0 to `min(distance, MAX_ROUTE−1) − 1`.
3. Bounds-check current cell; break if outside (`:5549-5554`).
4. Read `cul` and eight neighbours (`:5556-5568`).
5. If already at target: terminate (`:5570-5574`).
6. Test the ordered direction catalog; on match emit digit, move, `continue`.
7. If no branch matches: `Route[i] = 0`, break (`:5810-5811`).
8. If position unchanged return 0; else write back target and return 1 (`:5814-5825`).

---

### Business Rule: BR-17 — Route decoding (and its invertibility failure)

**Overview**:
`BASE_GetDestByAction` is the inverse operation: given a start position and a `MSG_Action` route string, it computes the destination. It is declared in the contract (`Basedef.h:2405`) but has no caller in the project, and it does not actually move the coordinates.

**Detailed description**:
The function copies `PosX/PosY` into `*x`/`*y` and then iterates the route string until a NUL byte, applying a delta per digit (`:5369-5412`). Every mutation is written as `*x++` or `*x--`, which in C parses as `*(x++)` / `*(x--)`: the *pointer* is advanced or retreated, the pointee is never modified. After the first digit the function is reading and writing through a pointer that no longer refers to the caller's `x`, so the caller-visible coordinate is unchanged for non-empty routes and the function additionally performs an out-of-bounds pointer walk of up to 23 elements.

Even if the pointer arithmetic were correct, the digit semantics disagree with the encoder for the vertical axis (F-03): `'8'` decrements `y` here and increments it in `BASE_GetRoute`; `'2'` increments here and decrements there; `'3'` maps to `(x+1,y+1)` here and `(x+1,y−1)` there. `'4'`, `'6'`, `'9'`, `'7'`, `'1'` agree. Consequently the encoder and decoder are not inverses on four of eight directions, and no round-trip test would pass.

**Rule workflow**:
1. `*x = mv->PosX; *y = mv->PosY` (`:5369-5370`).
2. For `i` in `0..23`: stop at `Route[i] == 0`.
3. Apply the digit→delta table (`:5377-5411`).
4. Return (void).

---

### Business Rule: BR-18 — Distance metric

**Overview**:
`BASE_GetDistance` provides the canonical in-game distance between two map cells. It is a table lookup for close ranges and a `max+1` formula beyond.

**Detailed description**:
For `dx <= 6 && dy <= 6` the function returns `g_pDistanceTable[dy][dx]` (`:162-171`, `:5842-5843`). The table is a hand-tuned metric that is *not* Chebyshev and *not* Euclidean: `t[1][1] = 1`, `t[2][2] = 3`, `t[3][3] = 4`, `t[4][4] = 5`, `t[5][5] = 5`, `t[6][6] = 6`, and the diagonal is cheaper than the L1 sum but at least the Chebyshev max. Beyond 6 in either axis the function returns `max(dx,dy) + 1` (`:5845-5848`), which is *larger* than the tabulated diagonal at the boundary (e.g. `t[6][6] = 6` but `max+1 = 7`), producing a small discontinuity at range 7.

Callers are spread across 4 files and use it for attack range, view range and NPC interaction range, so the discontinuity is observable at the exact boundary of a 6-cell radius. All callers pass signed coordinates through `int`, and the function takes absolute differences, so negative or wrapped coordinates are handled.

**Rule workflow**:
1. `dx = |x1−x2|`, `dy = |y1−y2|` (`:5832-5840`).
2. If both `<= 6`: return `g_pDistanceTable[dy][dx]` (`:5842-5843`).
3. Else return `max(dx,dy) + 1` (`:5845-5848`).

---

### Business Rule: BR-19 — Ground item placement height/state machine

**Overview**:
Dropped items occupy a 6×6 footprint taken from `g_pGroundMask[mask][rotation]` and raise/lower the cell heights accordingly. `BASE_UpdateItem` applies one state transition; `BASE_UpdateItem2` force-sets the height.

**Detailed description**:
Legality is decided by a state transition table: `1 → 2` or `1 → 3` yields `delta = +1` (item placed / locked), `2|3 → 1` yields `delta = −1` (item removed), and any other pair returns `FALSE` without touching the map (`:5300-5306`). The state constants are `STATE_NOTHING 0`, `STATE_OPEN 1`, `STATE_CLOSED 2`, `STATE_LOCKED 3` (`Basedef.h:1834-1837`). `maskidx` outside `0..MAX_GROUNDMASK−1` (0-5) is rejected (`:5297-5298`), but the `rotate` parameter is **not** range-checked before indexing `g_pGroundMask[maskidx][rotate][y][x]` (`:5312`) even though the second dimension is only 4 — a `rotate` of 4 or more reads into the next mask.

For each non-zero mask cell the function computes `xp = xx + x − 2`, `yp = yy + y − 2` (the mask is centred), multiplies the mask value by `delta`, adds it to the current height, clamps to `0..255`, and writes it back (`:5308-5334`). The bounds test is `xp − g_HeightPosX < 1 || > g_HeightWidth − 2` (and the `yp` equivalent), so the border row/column is protected; a failing test `break`s only the inner loop, leaving the rest of the mask row unprocessed. The last processed cell's height is reported through `*height` (`:5331-5332`). `BASE_UpdateItem2` (`:5341-5365`) skips the transition logic entirely and stamps a caller-supplied absolute `height` into every covered cell, with the same border-protection `break`.

**Rule workflow**:
1. Reject `maskidx` out of range (`:5297-5298`).
2. Derive `delta` from `(CurrentState, NextState)`; reject unmodelled pairs (`:5300-5306`).
3. For each `y,x` in the 6×6 mask: skip zero cells; compute `xp/yp`.
4. Border check → `break` (`:5321-5322`).
5. `Last = clamp(pHeight[...] + mask*delta, 0, 255)`; write back; track `*height` (`:5324-5334`).
6. `return TRUE`.

---

### Business Rule: BR-20 — Terrain attribute stamping

**Overview**:
`BASE_ApplyAttribute` converts the coarse 1024×1024 attribute map into the fine 4096×4096 height grid by writing the sentinel height 127 wherever the attribute byte has bit 1 set.

**Detailed description**:
The attribute map `g_pAttribute[1024][1024]` is loaded once by `BASE_InitializeAttribute` (BR-21). `BASE_ApplyAttribute(pHeight, size)` iterates a square window of side `size` starting at `g_HeightPosX/Y` (both 0 at start-up) and for each `(x,y)` samples `g_pAttribute[(y>>2) & 0x3FF][(x>>2) & 0x3FF]` — a 4:1 downscale in both axes, matching the 4096:1024 ratio (`:2595-2599`). If `att & 2`, the height grid cell is set to **127**, the value that `MH`-based route and placement checks treat as an obstruction step (`:2601-2602`).

Only bit 1 is consumed here; the other seven bits of the attribute byte are not interpreted in this component, but the same array is read directly by `GetFunc.cpp` (`g_pAttribute` is `extern` at `Basedef.h:2469`), so the remaining bits carry meaning outside the component boundary. The index arithmetic uses `x + g_HeightWidth * (y − g_HeightPosY) − g_HeightPosX`, which is correct only when the destination grid is exactly `g_HeightWidth` wide and the window origin is `g_HeightPosX/Y`; both invariants are caller-managed (the host passes `MAX_GRIDX` = 4096, `Server.cpp:7159`).

Because the stamp is one-way (127 overwrites whatever was there), calling `BASE_ApplyAttribute` twice is idempotent, but calling it after `BASE_UpdateItem` has placed items will flatten those heights back to 127 on attributed cells.

**Rule workflow**:
1. `endx = g_HeightPosX + size`, `endy = g_HeightPosY + size` (`:2585-2586`).
2. For each `(x,y)` in the window: `xx = (x>>2)&0x3FF`, `yy = (y>>2)&0x3FF` (`:2595-2597`).
3. If `g_pAttribute[yy][xx] & 2`: `pHeight[x + Width*(y−PosY) − PosX] = 127` (`:2601-2602`).

---

### Business Rule: BR-21 — Content loading, obfuscation and checksums

**Overview**:
Nine loaders ingest the game's content plane at start-up, three writers persist binary caches, and two checksum functions seal them. Six of the nine loaders have a two-location search order; all of them fail "softly" (log, return, leave the table partly populated).

**Detailed description**:
The loaders and their sources are: `BASE_InitializeSkill` ← `../../Common/SkillData.csv` or `../../TMSRV/Run/SkillData.csv`, comma-to-space normalised, 21-field `sscanf` into `STRUCT_SPELL`, `AffectTime` divided by 4, `Act[]` parsed from a dotted sub-string, index range-checked against `MAX_SKILLINDEX` (103) (`:6001-6071`); `BASE_InitializeEffectName` ← `../../Common/ItemEffect.h` or `../../TMSRV/Run/ItemEffect.h`, **parsed as a header at runtime** — every `#define NAME VALUE` line populates `EffectNameTable[VALUE]` and `#include` lines trigger a recursive call that restarts the whole parse (`:5163-5242`); `BASE_InitializeItemList` ← `itemlist.csv` then `extraitem.csv`, via `BASE_ReadItemListFile`, which also writes `extraitem.bin`, `ItemList.bin` and `../ItemCSum.h` (`:4963-5161`); `BASE_InitializeInitItem` ← `InitItem.csv` (`:5947-5999`); `BASE_InitializeGuildName` / `BASE_InitializeClientGuildName` ← `Guilds.txt` in three candidate locations (`:6153-6216`, `:6086-6151`); `BASE_InitializeServerList` ← `serverlist.txt` in two locations, then writes `./serverlist.bin` (`:6218-6305`); `BASE_InitializeAttribute` ← `./AttributeMap.dat` or `../../TMSRV/Run/AttributeMap.dat`, a raw `fread` of 1024×1024 bytes (`:6307-6335`); `BASE_InitializeMessage` ← caller-supplied `Language.txt` (`:4637-4710`); `BASE_InitializeMobname` ← `MobName.txt` with two modes — offset 0 loads `(key, value)` pairs into `g_pEnglish[i][0..1]`, offset 1 matches an existing key and overwrites `[2]` (`:4712-4901`).

Obfuscation: `ItemList.bin`, `SkillData.bin` and `UI/strdef.bin` are XOR-ed with `0x5A` on write and XOR-ed back in memory to restore the live table (`:2696-2732`, `2734-2790`, `4903-4930`, `2792-2826`, `2828-2906`, `4932-4961`); `InitItem.bin` uses `0xFF` (`:5851-5878`); `serverlist.bin` uses an additive byte-wise offset (`:6268-6299`). Checksums are `BASE_GetSum` (7-period) for the message table and `BASE_GetSum2` (9-period) for item/skill data. Both are written into the file and into generated C headers (`ITEM_CHECKSUM`, `ITEM_CHECKSUM2`, `SKILL_CHECKSUM`, `Basedef.cpp:2722`, `2779-2781`).

Integrity is not actually enforced. `BASE_ReadSkillBin` computes `sum` and never compares it (`:2813-2818`, comparison commented out); `BASE_ReadItemList` does the same twice, once for the base file and once after `ExtraItem.bin` is overlaid (`:2849-2854`, `2893-2896`); `BASE_ReadMessageBin` compares `sum != tsum` inside a block comment (`:4950-4954`). The readers also leave `tsum` unused after reading it.

Several loaders have soft-failure semantics that produce a running server with an empty content table: `BASE_InitializeSkill` returns without populating `g_pSpell` on a missing file (but zeroes it first, `:6017`); `BASE_InitializeEffectName` returns with an all-zero name table, which then makes every item effect name lookup fail; `BASE_InitializeItemList` returns after zeroing `g_pItemList` (`:4967`-`4978`); `BASE_InitializeAttribute` leaves `g_pAttribute` untouched (not zeroed) so a failed load keeps whatever the previous map contained.

**Rule workflow**:
1. Open the primary path; fall back to the `../../TMSRV/Run/` path (`:5013`, `:5170-5173`, `:6005-6006`, `:5951-5958`, `:6090-6096`, `:6224-6225`, `:6320-6321`).
2. On failure: `MessageBox` diagnostic and `return` (see F-22).
3. Read line-by-line; normalise separators (`,` → space) and truncate at `\t`/`\n`.
4. Parse into a fixed field list (`sscanf`), range-check the index, clamp over-long names.
5. Resolve symbolic effect names through `EffectNameTable` (item list only).
6. If building: write the binary cache with the XOR/offset obfuscation and emit/replace the checksum header.
7. Restore the in-memory table by re-applying the obfuscation XOR.

---

### Business Rule: BR-22 — Mob checksum and character-name validation

**Overview**:
Two integrity rules guard character data: a positional checksum over equipment and carried items, and a syntax policy for character names that blocks reserved GM/command words.

**Detailed description**:
`BASE_GetMobCheckSum` (`:2503-2517`) computes `Σ Equip[i].sIndex × i + Σ Carry[i].sIndex × i`. Because the slot index multiplies the item id, the checksum detects both substitution and slot swapping, and it is order-sensitive by construction. Before summing it **zeroes `mob->BaseScore`** (`:2508`), so the function destroys the caller's base stats — a destructive side effect from a `Get`-named function. There is no caller in the project (See Section 6.4), so the rule is latent; if it were wired in, invoking it before `BASE_GetCurrentScore` would silently zero the character's stats.

`BASE_CheckValidString` (`:2522-2551`) enforces: length 4 ≤ `len < NAME_LENGTH (16)`; rejection of 15 exact reserved words (`"Reino"`, `"subcreate"`, `"create"`, `"gritar"`, `"king"`, `"kingdom"`, `"getout"`, `"gfame"`, `"expulsar"`, `"summonguild"`, `"summon"`, `"time"`, `"relo"`, `"stopally"`, `"stopwar"`); and a character whitelist of ASCII letters, digits and `-`, with any byte ≥ 0x80 treated as a two-byte character and skipped (`:2534-2548`). The two-byte skip is positional, not table-driven, so a mis-paired lead byte will consume the following ASCII character and accept an otherwise illegal name. The reserved-word check is case-sensitive, so `"King"` and `"KING"` are accepted.

`BASE_CheckHangul` (`:2553-2562`) tests the KS-C-5601 (EUC-KR) Hangul syllable range: low byte `0xB0-0xC9`, high byte `0xA1-0xFF`. Only the first of the two range pairs (`0xB0-0xC8`) is covered — syllables in `0xC9A1-0xC9FE` are accepted while the stated upper band implies a wider range. `BASE_GetFirstKey` (`:4114-4161`) maps a name's first syllable to a grouping key: ASCII letters pass through, byte pairs are converted to an index `(b0−176)*94 + b1−161` and bucketed against the 19-entry `KorIndex[]` table to yield one of 18 two-byte initial glyphs, everything else becomes `"etc"`.

**Rule workflow**:
1. `BASE_GetMobCheckSum`: `memset(BaseScore, 0)`; `sum += Equip[i].sIndex*i` for 16 slots; `sum += Carry[i].sIndex*i` for 64 slots; return sum (`:2508-2516`).
2. `BASE_CheckValidString`: length test → reserved-word test → per-character whitelist with 2-byte skip → `true` (`:2524-2550`).
3. `BASE_CheckHangul`: range test on the two bytes (`:2555-2561`).
4. `BASE_GetFirstKey`: ASCII → 1 byte; 2-byte → index → `KorIndex` bucket → glyph; else `"etc"` (`:4116-4161`).

---

### Business Rule: BR-23 — Audit-logging policy

**Overview**:
`BASE_NeedLog(item, money)` decides whether an item movement must be written to the item audit log. The policy is "log anything valuable, refined, guild-bound, donation-bound, effect-bearing or above a price floor".

**Detailed description**:
The rule accumulates disjuncts (`:766-807`): invalid index → no log; a `money` argument ≥ 0 (currency movement) → always log; refine ≥ 3 → log; for weapon-slot items (`nPos == 64 || 192`) magic ≥ 12 or damage ≥ 27 → log, for all other items magic ≥ 8 or damage ≥ 12 → log; any item with a guild binding (`BASE_GetGuild(item) > 0`) → log; a hard-coded list of indices (`419`, `420`, `412`, `413`, `753`, `657`, `470-499`, `522`) → log; price ≥ 100 → log.

The final disjunct is defective: `((idx >= 551 || idx <= 562) || stEffect[0].cEffect != 0 || stEffect[0].cEffect != 59 || ...)` — both the index test and each effect test are tautologies (`x >= 551 || x <= 562` is true for every integer; `c != 0 || c != 59` is true for every byte). The function therefore returns `TRUE` for every in-range item that reaches line 799, so all the earlier selectivity is unreachable. In practice `BASE_NeedLog` logs everything except items whose `sIndex` is out of range. Three call sites (`MobKilled.cpp:2332`, `:2387`, and one more) rely on it for drop auditing, which means the audit log contains every dropped item rather than the intended subset.

**Rule workflow**:
1. Reject `idx < 0 || idx >= MAX_ITEMLIST` → `FALSE` (`:770-771`).
2. `money >= 0` → `TRUE` (`:773-774`).
3. `sanc >= 3` → `TRUE` (`:776-779`).
4. Weapon/non-weapon magic/damage thresholds → `TRUE` (`:783-794`).
5. Guild binding or hard-coded index list → `TRUE` (`:796-797`).
6. Tautological disjunct → `TRUE` (`:799`); price ≥ 100 → `TRUE` (`:802-805`).

---

### Business Rule: BR-24 — Spatial and identity lookups (zones, guilds, sub-guilds, item indices)

**Overview**:
Five small lookups resolve map zones, guild identity and "logical item index": guild-city membership, arena (war) membership, village membership, guild id from item bytes, sub-guild from item id, and 16-bit logical index from the two index effects.

**Detailed description**:
`g_pGuildZone[MAX_GUILDZONE=5]` (`Basedef.cpp:47-54`) hard-codes five rectangular regions (Armia, Azran, Erion, Nippleheim, Noatum) each with guild spawn, city spawn, city rectangle, war rectangle, alternate spawns, tax and clan fields. `BASE_GetGuild(x,y)` returns the first zone whose *city* rectangle contains the point, else 5 (`:4164-4173`); `BASE_GetArena(x,y)` does the same with the *war* rectangle (`:4247-4256`); `BASE_GetVillage(x,y)` is identical to `BASE_GetGuild` but iterates only the first five entries explicitly (`:4258-4267`). All three return the sentinel `5`, which is **out of range** for an array of 5 — a fact exploited by callers, but a trap for any caller that indexes `g_pGuildZone` with the result (a pattern that exists elsewhere in the codebase).

`BASE_GetGuild(STRUCT_ITEM*)` (`:4207-4235`) and `BASE_GetIndex(STRUCT_ITEM*)` (`:4175-4205`) recombine two bytes carried in effect slots into a 16-bit value as `hword*256 + lword`, using `EF_HWORDGUILD`/`EF_LWORDGUILD` (56/57) and `EF_HWORDINDEX`/`EF_LWORDINDEX` (62/63) respectively; `BASE_GetIndex` returns 0 for mounts 2330-2389 and casts each byte to `unsigned char` before combining, while `BASE_GetGuild` does not. `BASE_GetSubGuild(item)` (`:4237-4245`) maps item ids 526-537 to sub-guild 1-3 via `(item − 526) % 3 + 1`. `BASE_GetKorFirst` (`:4269-4279`) extracts bits 10-14 of a packed value minus 1 — a one-line helper with no caller.

**Rule workflow**:
1. `BASE_GetGuild(x,y)`: for each of 5 zones, test the city rectangle; return index, else 5 (`:4166-4172`).
2. `BASE_GetArena(x,y)`: same with the war rectangle (`:4249-4255`).
3. `BASE_GetVillage(x,y)`: same as (1) over indices 0-4 (`:4260-4266`).
4. `BASE_GetGuild(item)`: read `EF_HWORDGUILD`/`EF_LWORDGUILD` from the first matching slot; return `h*256 + l` (`:4209-4234`).
5. `BASE_GetIndex(item)`: same with the index effects, returning 0 for mounts (`:4177-4204`).
6. `BASE_GetSubGuild(id)`: `526 <= id <= 537` → `(id−526)%3+1`, else 0 (`:4241-4244`).

---

### Business Rule: BR-25 — Time-bounded items and the weekly clock

**Overview**:
Consumable/timed items carry an expiry (day/month/year) or a countdown (day/hour/minute) inside their three effect bytes. Two writers and two readers implement the policy; one helper derives the "week number" used for weekly resets.

**Detailed description**:
`BASE_SetItemDate(item, day)` (`:6732-6761`) stamps `EF_WDAY (106)`, `EF_WMONTH (110)` and `EF_YEAR (109)` from `localtime`, adding `day` to the current day-of-month and rolling the month/year forward when `day >= 30` or the month reaches 12. The calendar arithmetic is a coarse 29/30-day model, not a real calendar: `if (daynext >= 30) daynext -= 29;` and `if (month == 1 && daynext >= 27) daynext -= 26;` (`:6747-6751`) — so a 30-day duration starting on the 15th yields day 16 of the *next* month, and February is special-cased only for month index 1. `BASE_CheckItemDate(item)` (`:6763-6780`) returns 1 when the stored date is in the past, using three ordered disjuncts (same-month day comparison, later-month comparison, later-year comparison); the first disjunct compares `when.tm_mon >= (cValue−1)` *and* `tm_mday >= stored day`, which means an item expiring on the 20th is considered expired on the 1st of the same month if the stored month is earlier — an over-eager expiry at month boundaries.

`BASE_CheckFairyDate(item)` (`:6782-6814`) is a countdown decrementer limited to indices 3900-3913 (the fairies). It reads day/hour/min from the three effect bytes and applies one decrement per call: `0/0/≤1 → clear the item`; `day≥1,hour=0,min=1 → day−−, 23:59`; `hour≥1,min=1 → hour−−, 59`; `min≥2 → min−−`. The `min == 1` sentinel is what makes the decrement happen once per minute of wall-clock (the caller invokes it on a timer tick); `min` never reaches 1 through normal decrement (`min >= 2 → min−−` stops at 2), so the sentinel is written by the caller before the tick.

`BASE_GetWeekNumber` (`:6462-6473`) returns `time_t / 86400 − 3`, i.e. a *day* index offset by 3, not an ISO week. It is used by `ProcessSecMinTimer.cpp:2062` for weekly reset logic, so the reset boundary is a 24-hour boundary rather than a 7-day one; the `−3` shifts the boundary to a specific weekday.

**Rule workflow**:
1. `BASE_SetItemDate`: read `localtime`; `daynext = mday + day`; month rollover when `day >= 30`; year rollover when `month >= 12`; normalise `daynext` and February; write `EF_WDAY/EF_WMONTH/EF_YEAR` (`:6736-6760`).
2. `BASE_CheckItemDate`: three ordered comparisons against `localtime`; return 1 on expiry (`:6766-6779`).
3. `BASE_CheckFairyDate`: index gate 3900-3913; four-branch decrement; clear when exhausted (`:6784-6813`).
4. `BASE_GetWeekNumber`: `now/86400 − 3` (`:6466-6472`).

---

### Business Rule: BR-26 — Visual code projection and item text code

**Overview**:
The client receives an item's visual identity as a 16-bit `index | (code << 12)` word. Two functions build that word (one for refinement, one for ancient/enchant colour) and one function renders a debug text form.

**Detailed description**:
`BASE_VisualItemCode(item, mnt)` (`:6598-6661`) has two modes. For the mount slot (`mnt == 14`) it returns the raw index for temporary mounts 3980-3994; for permanent mounts 2360-2389 it packs `stEffect[1].cEffect / 10` (the mount level) into the high nibble, unless `stEffect[0].sValue <= 0`, in which case it returns 0. The permanent-mount branch is guarded by `if (Item->sIndex >= 2360 && Item->sIndex < 2360)` (F-17), which can never be true, so permanent mounts are returned through the generic path as plain indices. For non-mount items it looks for effect `43` (`EF_SANC`) or any `116-125` tier effect; a tier effect forces `12 * 0x1000`; otherwise the sanc byte is normalised to `0-9 / 10-16` and packed into the high nibble.

`BASE_VisualAnctCode(item)` (`:6662-6731`) produces the "ancient" colour code. For mounts with `stEffect[0].sValue > 0` it switches on `stEffect[2].cValue` (11-20 → `0x10B..0x114`, 35 → `0x115` only for indices 2363/2377, everything else `0x00`). For normal items it prefers tier effects 116-125 (`code = effect − 3`), then `EF_SANC`, and finally maps `value % 4` to `0x30/0x40/0x10/0x20` (with `value < 230 → 43`). The two functions therefore encode the same underlying byte differently — one into a 4-bit nibble, the other into a byte — and a single item can carry both encodings simultaneously.

`BASE_GetItemCode(item, str)` (`:1479-1495`) writes `" <Name> : <index>.<c0>.<v0>.<c1>.<v1>.<c2>.<v2>"` into the caller buffer with `sprintf` and no length parameter; it is called from 23 files (mostly logging), so the caller's buffer discipline is entirely external.

**Rule workflow**:
1. Mount slot: detached mounts → raw index; permanent mounts → high nibble = level (currently unreachable).
2. Otherwise: find effect 43 / tier 116-125; normalise; `return index | (code * 0x1000)`.
3. Anct path: mount switch table; else tier effect → `effect − 3`; else `EF_SANC` → `%4` mapping.

---

### Business Rule: BR-27 — Packet-size validation contract (disabled)

**Overview**:
`BASE_CheckPacket(MSG_STANDARD *m)` is the component's only declared protocol-validation rule. It is specified as a 100-entry table pairing each `_MSG_*` type with the exact `sizeof` of its payload struct; in the delivered source the entire table is commented out.

**Detailed description**:
The function signature returns `BOOL` and takes a single `MSG_STANDARD*` (`Basedef.h:2428`). Its body opens a block comment at `Basedef.cpp:6476` and closes it at `:6594`, followed by `return FALSE;` at `:6595`. Inside the comment are approximately 100 independent `if (m->Type == _MSG_X && m->Size != sizeof(MSG_X)) code = 1;` statements covering the client↔game, game↔DB and DB↔NP namespaces, plus a handful of deliberately commented-out entries for messages that legitimately vary in size (`_MSG_REQMobByID`, `_MSG_Stop`, `_MSG_RemoveItem`, `_MSG_SwapItem`, `_MSG_AddParty`, `_MSG_BillingPage`, `_MSG_DBCNFNewAccount`, `_MSG_DBAccountLoginFail`). If it were live, it would return 1 for the first size mismatch and 0 otherwise.

All five call sites follow the same idiom: `if (BASE_CheckPacket(debug)) { sprintf(...); Log(...); }` inside `#ifdef _PACKET_DEBUG` (`CPSock.cpp:544-552`, `TMSrv/Server.cpp:3895-3905`, `TMSrv/Server.cpp:3978-3988`, `DBSrv/Server.cpp:845-854`, `DBSrv/Server.cpp:929-938`). Because the function always returns `FALSE`, the log line never executes and no packet is ever rejected. Note also that even in the commented design the function *reports* rather than rejects: the call sites log and continue, so the table never had enforcement semantics.

Message framing is still bounded elsewhere — `CPSock` validates `Size >= sizeof(HEADER)` and `Size <= MAX_MESSAGE_SIZE` before processing (`CPSock.cpp:386-416`) — so the gap is specifically per-message-type structural validation: a message of the correct framing size but the wrong payload size for its type reaches the handler.

**Rule workflow** (as specified, not as executed):
1. `m == NULL` → 0.
2. For each of ~100 `(type, sizeof)` pairs, set `code = 1` on mismatch.
3. Return `code`.
Actual behaviour: `return FALSE` unconditionally.

---

### Business Rule: BR-28 — Localisation and name translation

**Overview**:
User-visible text is looked up by numeric id from a runtime-loaded catalogue; mob names are additionally translated through a three-column English table and grouped by first syllable.

**Detailed description**:
`g_pMessageStringTable[MAX_STRING=2000][128]` is loaded from `Language.txt` by `BASE_InitializeMessage` in `"<index><tab><text>"` form; indices outside `1..2000` are skipped, lines without a tab raise a `MessageBox` and are skipped, and text longer than 127 bytes is rejected (`:4650-4707`). The seven `BASE_GetLanguage` overloads then `sprintf` the entry into a caller buffer, substituting 0, 1 or 2 parameters of `char*` or `int` type — the *entry itself* is the format string, so any `%` sequence in the language file is interpreted. Out-of-range indices write the literal `"[UNHANDLED]"`.

`g_pEnglish[MAX_ENGLISH=600][3][NAME_LENGTH]` holds `[key, value, translated]` triples. Mode 0 of `BASE_InitializeMobname` fills columns 0 and 1 (and copies the key into column 2); mode 1 searches existing rows for the key in column 0 or 1 and overwrites column 2 (`:4881-4897`). `BASE_GetEnglish(name)` scans all 600 rows for an exact `strcmp` match on column 0 and replaces the caller's buffer with column 2 (`:6444-6460`); it declares a temporary `char temp[256]` and `strncpy(temp, name, sizeof(name))` — `sizeof` of a pointer, i.e. 4 bytes — but never uses `temp`, so the truncation is harmless dead code. The lookup has no bounds check on the replacement length.

Because the table is only 600 rows and indices are assigned by file order, a `MobName.txt` longer than 600 valid rows stops loading with a diagnostic (`:4793-4798`) and leaves the remainder untranslated.

**Rule workflow**:
1. Loader: `memset` the table; per line `sscanf("%d %s")`; locate the tab; truncate the text; validate 1..127 bytes; `strcpy` into `[index]` (`:4637-4710`).
2. Lookup: bounds-test the index; `sprintf(buffer, table[idx], ...)`; else `strcpy(buffer, "[UNHANDLED]")` (`:1258-1312`).
3. Mob names: exact-match scan over 600 rows; replace with column 2 (`:6449-6457`).

---

### Business Rule: BR-29 — Guild-name formatting

**Overview**:
Guilds are addressed by a 16-bit id whose high bits select the server and whose low 12 bits select the guild. The name is resolved through `g_pGuildName` when a real name exists, and synthesised from two Hangul glyph tables when it does not.

**Detailed description**:
`BASE_GetGuildName(group, usGuild, out)` splits `usGuild` into `Server = usGuild / 4096` and `Guild = usGuild − Server*4096` (`:1317-1318`). If `g_pGuildName[group][server][guild]` is non-empty it is copied (12 bytes, unconditionally) and underscores are converted to spaces (`:1330-1337`). Otherwise the fallback composes `<server glyph>‑<glyph1><glyph2>` from `g_pGuildIndex[server*2]`, `g_pGuildMark[2*((usGuild/64)&63)]` and `g_pGuildMark[2*(usGuild&63)]` (`:1340-1353`) — a two-level 6-bit decomposition of the low 12 bits, i.e. a 64×64 mark space. An out-of-range group or server yields an empty string.

`BASE_GetClientGuildName(group, usGuild, out, sub)` (`:1357-1444`) adds the sub-guild suffix (1-3) and, importantly, refreshes a per-group cache via `BASE_InitializeClientGuildName(group)` whenever `CurrentClientGuild != group` (`:1366-1367`). Its real-name branch indexes `g_pGuildName` with two subscripts instead of three (F-08), so it reads `g_pGuildName[server][guild][0]` — a different row than the one the loader populated — and can therefore append a sub-guild to a name that came from an unrelated group/guild. Its fallback path has extra defensive range checks on the glyph indices (`> 20`, `> 128`) that the non-client variant lacks. Sub-guild glyphs come from `g_pGuildSub`, addressed with `((int)&g_pGuildSub − 2) + 2*Sub`, i.e. pointer arithmetic starting one element *before* the array (`:1395`, `:1441`) — for `Sub == 0` that reads the two bytes immediately preceding the array.

`BASE_InitializeClientGuildName(group)` re-reads `Guilds.txt`, keeps only rows whose `Group` matches, truncates each of the four names to 11 bytes, and applies `IsClearString3` to drop a dangling lead byte at the end of the string (`:6086-6151`). `IsClearString3` uses `CharNext` and returns `FALSE` when advancing one byte lands inside a two-byte character (`:6073-6084`).

**Rule workflow**:
1. Split `usGuild` into server (÷4096) and guild (remainder) (`:1317-1318`).
2. Validate group/server windows; empty string on failure (`:1323-1328`).
3. If a stored name exists: copy 12 bytes, `_` → space (`:1330-1337`).
4. Else synthesise from `g_pGuildIndex` + two `g_pGuildMark` bytes (`:1343-1353`).
5. Client variant: refresh cache per group, append `-<sub>` from `g_pClientGuildName` or `g_pGuildSub` (`:1385-1398`, `:1439-1443`).

---

### Business Rule: BR-30 — Week clock and HTTP stub

**Overview**:
Two utility rules exist for the host: a "week number" used for periodic resets, and an HTTP request helper that is a stub.

**Detailed description**:
`BASE_GetWeekNumber` (`:6462-6473`) returns `(int)(time(NULL) / 86400 − 3)`. Despite the name it is a *day counter* offset by three days; consumers treat it as a period identifier, so any reset keyed on it fires daily at a fixed UTC-midnight-plus-offset boundary. The `−3` constant is undocumented and there is no week modulo anywhere in the component, so the "weekly" semantics must be implemented by the caller or not at all.

`BASE_GetHttpRequest(httpname, Request, MaxBuffer)` (`:6337-6341`) takes three parameters, ignores all of them, and returns `1`. It is declared in the public contract but has no call site anywhere in the project, so a feature that was expected to perform an outbound HTTP call is present only as a shape.

**Rule workflow**:
1. `BASE_GetWeekNumber`: `time()` → seconds; `÷86400`; `−3`; cast to `int`.
2. `BASE_GetHttpRequest`: `return 1`.

---

## 4. Component Structure

### 4.1 File and boundary structure

```
Code/
├── Basedef.cpp                  # THIS COMPONENT — 6,814 lines, 101 functions, 30 tables
├── Basedef.h                    # companion contract (separate component) — types, constants, 101 declarations, 47 externs
├── ItemEffect.h                 # EF_* dictionary — #included for the EF_ constants AND parsed at runtime
├── CPSock.cpp / CPSock.h        # transport; calls BASE_CheckPacket (debug only)
├── ItemEffect.h                 # (listed above)
├── TMSrv/                       # game server — 47 of its TUs consume BASE_* symbols
│   ├── Server.cpp               # boot sequence: InitModuleDir, InitializeBaseDef, ApplyAttribute, InitializeMessage
│   ├── imple.cpp                # Windows-mode boot sequence
│   ├── CMob.cpp                 # sole caller of BASE_GetCurrentScore; AI route calls
│   ├── _MSG_*.cpp               # 60 handlers; UseItem is the heaviest consumer (221 calls)
│   ├── GetFunc.cpp / SendFunc.cpp
│   └── CReadFiles.cpp           # reads and also writes several Basedef tables
├── DBSrv/
│   ├── Server.cpp               # debug packet logging sites
│   ├── CFileDB.cpp              # BASE_* usage for persistence validation
│   └── CReadFiles.cpp           # bulk I/O
├── ClientPatch_v7662/           # .vcxproj.user only — no source
└── release/TMSrv/run/           # prebuilt TMSrv.exe + .pdb (binary artefact, not used for analysis)
```

### 4.2 Internal region map of `Code/Basedef.cpp`

| Region | Lines | % of file | Contents |
|--------|-------|-----------|----------|
| Header includes | 1-35 | 0.5 % | `windows.h`, `windowsx.h`, CRT file/string/time headers, `Basedef.h`, `ItemEffect.h` |
| Tuning tables and globals | 37-764 | 10.7 % | 30 tables (class baselines, drop/sanc/refine/mount tables, item shapes, distance table, formation, clan matrix, dungeon positions, XP curves, beast/transform bonuses, Hangul glyph tables) and the 14 content arrays |
| Progression accounting | 766-1256 | 7.2 % | `BASE_NeedLog`, bonus skill/score points, HP/MP, speed, physical damage, 7 language overloads, guild names, legacy skill damage |
| Item ability system | 1479-2338 | 12.6 % | 5 ability readers, refine codec (7 functions), amount codec, `GetItemPointer` |
| Mob ability and identity | 2374-2971 | 8.8 % | ability aggregation, max ability, checksum, name validation, boot orchestrator, attribute stamp, checksum helpers, bin writers/readers, string helpers |
| Score engine | 2973-4112 | 16.7 % | `BASE_GetCurrentScore` and `BASE_GetFirstKey` |
| Spatial / guild / index lookups | 4114-4279 | 2.4 % | guild/arena/village rectangles, item byte recombination, sub-guild, `GetKorFirst` |
| Inventory, equip, trade | 4281-4635 | 5.2 % | clear, CanEquip, CanCarry, CanCargo, sort, CanTrade |
| Content loaders (text, in-memory) | 4637-5242 | 8.9 % | message catalogue, mob names, item list CSV, effect-name header parser |
| Grid / heightmap | 5244-5365 | 1.8 % | hit-rate table builder, item placement state machine |
| Combat maths | 5367-5536 | 2.5 % | dest-by-action, mana, hit/accuracy/damage rate, double-critical |
| Routing | 5538-5849 | 4.6 % | `BASE_GetRoute`, `BASE_GetDistance` |
| Loaders (binary/init/guild/server/attribute) | 5851-6335 | 7.1 % | init-item, skill, guild-name, server-list, attribute-map |
| Skill damage and utility | 6337-6473 | 2.0 % | HTTP stub, per-skill damage, English lookup, week number |
| Packet validation (disabled) | 6475-6597 | 1.8 % | 118-line commented table + `return FALSE` |
| Visual and date codecs | 6598-6814 | 3.2 % | visual item/anct codes, item date, expiry, fairy countdown |

### 4.3 Owned content tables (file-scope globals)

| Symbol | Type / dimensions | Approx. size | External readers |
|--------|-------------------|--------------|------------------|
| `g_pMessageStringTable` | `char[2000][128]` | 256 KB | **46 files** |
| `g_pGuildName` | `char[10][16][4096][12]` | 7.5 MB | 2 files |
| `g_pClientGuildName` | `char[16][4096][4][12]` | 3.0 MB | 0 (internal only) |
| `g_pAttribute` | `uchar[1024][1024]` | 1.0 MB | `GetFunc.cpp` |
| `g_pItemList` | `STRUCT_ITEMLIST[6500]` | ~0.9 MB | 16 files |
| `g_pSpell` | `STRUCT_SPELL[103]` | ~9 KB | 5 files |
| `g_pInitItem` | `STRUCT_INITITEM[256]` | 2 KB | 2 files |
| `g_pServerList` | `char[10][11][64]` | 7 KB | 2 files |
| `g_pEnglish` | `char[600][3][16]` | 28.8 KB | 0 (internal) |
| `EffectNameTable` | `char[128][24]` | 3 KB | 0 (internal) |
| `g_pGuildZone` | `STRUCT_GUILDZONE[5]` | small | 15 files |
| `g_pItemGrid` | `char[8][4][2]` | 64 B | 0 (internal) |
| `g_pDistanceTable` | `int[7][7]` | 196 B | 0 (internal) |
| `g_pGroundMask` | `int[6][4][6][6]` | 3.5 KB | `Server.cpp` |
| `g_pHitRate` | `int[1024]` | 4 KB | 0 (internal; **not** declared in the header) |
| `g_pMountBonus` / `g_pMountTempBonus` | `int[30][5]` / `int[20][5]` | 1 KB | 0 (internal) |
| `g_pSancRate`, `g_pEhreRate`, `g_pOdinRate`, `g_pCelestialRate`, `g_pItemSancRate12[Minus]`, `g_pSancGrade`, `g_pAnctChance`, `g_pTinyBase`/`Shany`/`Ailyn`/`Agatha` | various | <1 KB | 1-3 files each |
| `g_pBonusValue[2..5]`, `g_pBonusType` | tables | few KB | 2 files |
| `g_pNextLevel` / `g_pNextLevel_2` | `long long[401]` | 3.2 KB each | 4 files |
| `pSummonBonus` / `pTransBonus` | `STRUCT_BEASTBONUS[50]` / `[5]` | 3.7 KB | `Server.cpp` / 0 |
| `DungeonPos` / `DungeonItem` | `int[30][2]` / `int[10]` | 256 B | `ProcessSecMinTimer.cpp` |
| `g_pFormation`, `g_pClanTable` | `char[5][12][2]`, `int[9][9]` | small | `Server.cpp`, `CMob.cpp` |
| `g_pGuildSub`, `g_pGuildIndex`, `g_pGuildMark`, `KorFirst`, `KorIndex` | byte tables | <1 KB | 0 (internal) |
| `ChargedGuildList` | `int[10][5]` | 200 B | 4 files |
| `BaseSIDCHM`, `g_pIncrementHp/Mp` | small int tables | tiny | 2 files |
| `g_HeightWidth/Height/PosX/PosY`, `g_dwInitItem`, `CurrentClientGuild` | scalars | tiny | host |

Total writable footprint of this translation unit is approximately **12.6 MiB (~13.2 MB)**, replicated in both `TMSrv.exe` and `DBSrv.exe`.

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (compile-time)

```
Basedef.cpp
├── "Basedef.h"        (project)  -> STRUCT_*, MSG_*, EF_* message/domain contract, 101 declarations, 47 externs
├── "ItemEffect.h"     (project)  -> EF_* numeric effect codes (compile-time constants)
└── Win32/CRT          (system)   -> windows.h, windowsx.h, stdlib, stdio, fcntl, io, sys/types, sys/stat,
                                    sys/timeb, string, time, mbstring
```

`Basedef.cpp` includes **no** other subsystem: no `Server.h`, no `CMob.h`, no `CPSock.h`, no `GetFunc.h`. It is the bottom of the dependency graph. It reaches world state only through pointer/reference parameters supplied by callers (`STRUCT_MOB&`, `char *pHeight`, `STRUCT_AFFECT*`), which is why the architecture report records efferent coupling of 3 (two headers + the content data plane) versus afferent ~49.

### 5.2 Internal dependencies (runtime, inbound)

| Consumer | Uses | Notes |
|----------|------|-------|
| `TMSrv/_MSG_UseItem.cpp` | 221 `BASE_*` calls | heaviest consumer: refinement, amounts, dates, visual codes, ability reads |
| `TMSrv/Server.cpp` | 52 | boot sequence, init-item creation, table reads |
| `TMSrv/GetFunc.cpp` | 47 | DTO projection reads item/mob abilities |
| `TMSrv/_MSG_CombineItemOdin.cpp` | 43 | refine odds (`g_pOdinRate`, `g_pItemSancRate12`) |
| `TMSrv/CMob.cpp` | 41 | score recomputation, routing, speed |
| `TMSrv/_MSG_Quest.cpp` | 28 | point budgets, class baselines |
| `TMSrv/_MSG_TradingItem.cpp` | 22 | `CanCarry`/`CanCargo`, sanc |
| `TMSrv/ProcessSecMinTimer.cpp` | 18 | week number, init items, fairy dates |
| `DBSrv/CFileDB.cpp` | 15 | cargo placement validation during save/load |
| `TMSrv/_MSG_Attack.cpp` | 13 | damage, skill damage, mana, double-critical |
| 37 further TUs | 1-11 each | handlers, SendFunc, MobKilled, CNPCGene, event modules |

Measured totals: **639 `BASE_*` call sites plus 24 `GetItemPointer` sites (663) across 47 translation units**, touching 59 of the 93 unique exported `BASE_*` names. `GetItemPointer` (declared `Basedef.h:2361`, defined `Basedef.cpp:2340`) is not `BASE_`-prefixed and is therefore absent from the per-function tables; its 24 call sites live in `_MSG_UseItem.cpp` (15), `_MSG_TradingItem.cpp` (4), `_MSG_DropItem.cpp` (2), `_MSG_Sell.cpp` (1), `_MSG_PutoutSeal.cpp` (1) and `DBSrv/CFileDB.cpp` (1).

### 5.3 External dependencies

| Dependency | Type | Purpose | Interface | Error handling in component |
|------------|------|---------|-----------|-----------------------------|
| Win32 `MessageBox`/`MessageBoxA` | OS UI | report missing/corrupt content files | blocking modal | blocks the process; 31 sites |
| Win32 `GetModuleFileName`, `SetCurrentDirectory` | OS | resolve working directory | silent failure | none checked (`:2608-2629`) |
| CRT file I/O (`fopen`/`fread`/`fwrite`/`fgets`/`_open`/`_read`/`_close`) | OS | read CSV/text/binary content, write caches | stdio / low-level | `NULL`/`-1` checks, mostly soft-fail |
| CRT `localtime`/`time` | OS | item expiry, fairy countdown, week clock | struct `tm` | none |
| CRT `rand` | OS | damage band, critical roll, `nRand` salt in binary caches | int | none |
| CRT `sprintf`/`strcpy`/`strcat`/`sscanf`/`strncpy`/`memcpy`/`memset` | OS | all string handling | — | **no destination-size parameter at any call site** |
| CRT `CharNext`, `_mbschr` | OS | DBCS-aware string walking | — | `CharNext` only in `IsClearString3` |
| Content files (`itemlist.csv`, `extraitem.csv`, `SkillData.csv`, `InitItem.csv`, `Guilds.txt`, `serverlist.txt`, `AttributeMap.dat`, `Language.txt`, `MobName.txt`, `UI/strdef.bin`, `ItemEffect.h`) | Data plane | all game content | CSV / TSV / raw binary | absent-file → `MessageBox` + partial table |
| `../../TMSRV/Run/` alternate directory | Data plane | fallback search path | same formats | same |

Notably absent: no socket, no database, no HTTP client (BR-30 is a stub), no registry, no environment-variable configuration, no logging API of its own (it writes to `MessageBox`, while the rest of the system uses `Log()`).

### 5.4 Content-file dependency matrix

| Loader | Primary path | Fallback path | Output side effects |
|--------|--------------|---------------|---------------------|
| `BASE_InitializeSkill` | `../../Common/SkillData.csv` | `../../TMSRV/Run/SkillData.csv` | none |
| `BASE_InitializeEffectName` | `../../Common/ItemEffect.h` | `../../TMSRV/Run/ItemEffect.h` | recursive re-parse on `#include` |
| `BASE_InitializeItemList` | `../../Common/itemlist.csv` | `../../TMSRV/Run/…` | `../../Common/extraitem.bin`, `../../Common/ItemList.bin`, `../ItemCSum.h` |
| `BASE_InitializeInitItem` | `../../Common/InitItem.csv` | `../../TMSRV/Run/InitItem.csv` | none |
| `BASE_InitializeGuildName` | `../../Common/Guilds.txt` | `./Guilds.txt`, `../../TMSRV/Run/Guilds.txt` | none |
| `BASE_InitializeClientGuildName` | `../../Common/Guilds.txt` | `./Guilds.txt`, `../../TMSRV/Run/Guilds.txt` | none |
| `BASE_InitializeServerList` | `./serverlist.txt` | `../../Common/serverlist.txt` | `./serverlist.bin` |
| `BASE_InitializeAttribute` | `./AttributeMap.dat` | `../../TMSRV/Run/AttributeMap.dat` | none |
| `BASE_InitializeMessage` | caller-supplied (`Language.txt`) | none | none |
| `BASE_InitializeMobname` | caller-supplied (`MobName.txt`) | none | none |
| `BASE_ReadItemList` / `BASE_ReadSkillBin` / `BASE_ReadMessageBin` | `../../Common/ItemList.bin`, `SkillData.bin`, `UI/strdef.bin` | — | `ExtraItem.bin` overlay |
| `BASE_ReadInitItem` | tail of the running executable | — | none |

---

## 6. Afferent and Efferent Coupling

### 6.1 Component-level aggregate

| Metric | Value | Derivation |
|--------|-------|------------|
| Ca (afferent, files) | 47 | distinct `.cpp`/`.h` TUs referencing `BASE_*` (excluding `Basedef.cpp` **and** `Basedef.h`) |
| Ca (afferent, call sites) | 639 `BASE_*` + 24 `GetItemPointer` = 663 | total `name(` occurrences in those files |
| Ca (distinct names used) | 59 / 93 | names with ≥ 1 external call site; 34 names are unused outside the file |
| Ce (efferent, components) | 3 | `Basedef.h`, `ItemEffect.h`, content data plane (system headers excluded per the architecture report's method) |
| Instability I = Ce/(Ca+Ce) | 0.06 | 3 / 50 — a maximally stable, dependency-sink module |
| Abstractness A | 0.00 | no virtual interfaces, no function pointers exposed |
| Distance from main sequence D = \|A + I − 1\| | 0.94 | concrete and stable — the classic "library of record" quadrant |
| Globals exported via header | 47 `extern` declarations | `Basedef.h:2441-2501` |
| Domain struct types exported | 25 `STRUCT_*` (§4.3 table lists the data-bearing ones) | `Basedef.h:398-920` |
| Message payload structs exported | 100 `MSG_*` | `Basedef.h:943-2300` |
| `_MSG_*` type constants exported | 191 | `Basedef.h:976-2300` |

### 6.2 Function-level coupling (top consumers / providers)

| Function | Ca (call sites) | Ca (files) | Ce (internal calls) | Criticality |
|----------|-----------------|------------|---------------------|-------------|
| `BASE_GetItemAbility` | 93 | 16 | 3 (`GetItemSanc`) | **Critical** — every item effect in the game |
| `BASE_SetItemAmount` | 93 | 4 | 0 | High — stack mutation |
| `BASE_GetItemSanc` | 79 | 10 | 0 | **Critical** — refine state of every item |
| `BASE_SetItemSanc` | 54 | 9 | 1 (`GetItemSanc` indirect) | High — writes refine byte |
| `BASE_GetItemCode` | 43 | 22 | 0 | Low — logging only |
| `BASE_ClearItem` | 37 | 11 | 0 | High — inventory reset |
| `BASE_GetGuildName` | 25 | 9 | 2 (`UnderBarToSpace`, `InitializeClientGuildName`) | Medium — display |
| `BASE_GetVillage` | 19 | 15 | 0 | Medium — zone identity |
| `BASE_GetMobAbility` | 13 | 1 | 3 | **Critical** — score pipeline |
| `BASE_GetItemAmount` | 12 | 6 | 0 | Medium — stacking |
| `BASE_GetStaticItemAbility` | 10 | 2 | 1 | High — requirement checks |
| `BASE_GetSpeed` | 9 | 2 | 0 | High — AI pacing |
| `BASE_GetBonusScorePoint` | 9 | 5 | 1 | High — stat legality |
| `BASE_GetRoute` | 9 | 2 | 0 | **Critical** — movement authority |
| `BASE_CheckItemDate` | 8 | 2 | 0 | Medium — expiry |
| `BASE_GetFirstKey` | 7 | 1 | 0 | Low — grouping |
| `BASE_GetItemAbilityNosanc` | 7 | 3 | 0 | High |
| `BASE_GetDistance` | 7 | 3 | 0 | High — range authority |
| `BASE_GetHpMp` | 5 | 3 | 0 | High — pool recomputation |
| `BASE_GetDamage` | 5 | 2 | 0 | **Critical** — every physical hit |
| `BASE_CheckPacket` | 5 | 2 | 0 | High by intent, zero by effect |
| `BASE_GetLanguage` | 4 | 2 | 0 | Medium — user text |
| `GetItemPointer` | 24 | 6 | 0 | High — inventory slot dereference |
| `BASE_GetCurrentScore` | 1 | 1 | 9 | **Critical, lowest Ca, highest Ce** — the engine's centre |

### 6.3 Data-level coupling (afferent reads of exported tables)

| Symbol | Files reading it | Coupling assessment |
|--------|------------------|---------------------|
| `g_pMessageStringTable` | 46 | **Pathological.** Every handler reaches into a 2,000×128 char array directly. Any layout change breaks 46 TUs; no accessor exists. |
| `g_pItemList` | 16 | **High.** Readers duplicate `Basedef`'s own lookup logic (`nUnique`, `nPos`, `Price`) instead of calling the ability readers. |
| `g_pGuildZone` | 15 | High — spatial constants consumed by 8 client handlers plus loading, service and header TUs |
| `g_pSpell` | 5 | High — skill metadata read directly by handlers |
| `g_pNextLevel` | 4 | Medium — XP curve read directly |
| `ChargedGuildList` | 4 | Medium — cross-server state written by `CFileDB` |
| `g_pServerList`, `g_pDropBonus` | 3 each | Medium — topology / drop modifiers re-read outside the component |
| `g_pSancRate`, `g_pEhreRate`, `g_pOdinRate`, `g_pSancGrade`, `g_pItemSancRate12[Minus]` | 1-3 each | Medium — refine odds re-read outside the codec |
| `g_pInitItem`, `BaseSIDCHM` | 2 each | Medium — initial-item and class-baseline tables shared |
| `g_pAttribute`, `g_pGroundMask`, `g_pFormation`, `g_pClanTable`, `g_pDropRate` | 1 each | Medium — table semantics split across the boundary |
| `g_pClientGuildName`, `g_pEnglish`, `g_pHitRate`, `g_pItemGrid`, `g_pDistanceTable`, `g_pMountBonus`, `g_pMountTempBonus`, `g_pGuildIndex`, `g_pGuildMark`, `g_pGuildSub`, `KorFirst`, `KorIndex`, `pTransBonus` | 0 | **Data that should be private is declared (or not) with external linkage** — `g_pHitRate` and `g_pEnglish` are defined without a header declaration, so they are reachable by `extern` from any TU but invisible to the compiler's contract. |

### 6.4 Unused exported surface (efferent dead weight)

34 of the 93 unique exported `BASE_*` names have **zero** call sites outside `Basedef.cpp`, i.e. they are declared in the public contract and reachable by any translation unit but never used. Full list, with the reason they remain:

| # | Symbol | Declared | Defined | Note |
|---|--------|----------|---------|------|
| 1 | `BASE_GetHitRate` | `Basedef.h:2407` | `:5423` | superseded by the progress-ring model (BR-15) |
| 2 | `BASE_GetDamageRate` | `:2408` | `:5443` | unused helper |
| 3 | `BASE_GetAccuracyRate` | `:2409` | `:5455` | unused helper |
| 4 | `BASE_GetMobCheckSum` | `:2364` | `:2503` | destructive; see TD-23 |
| 5 | `BASE_GetDestByAction` | `:2405` | `:5367` | defective decoder; see TD-02 |
| 6 | `BASE_UpdateItem2` | `:2404` | `:5341` | alternative placement path, never wired |
| 7 | `BASE_GetKorFirst` | `:2388` | `:4269` | one-line helper |
| 8 | `BASE_SortTradeItem` | `:2393` | `:4539` | internal-only (called by `BASE_CanTrade`) |
| 9 | `BASE_GetMaxAbility` | `:2363` | `:2482` | internal-only (called by `BASE_GetMobAbility`) |
| 10 | `BASE_GetIndex` | `:2383` | `:4175` | logical item-index decode, unused |
| 11 | `BASE_CheckHangul` | `:2366` | `:2553` | unused name validator |
| 12 | `BASE_GetBonusItemAbilityNosanc` | `:2352` | `:2052` | unused variant |
| 13 | `BASE_GetClientGuildName` | `:2345` | `:1357` | declared alternative to `BASE_GetGuildName` |
| 14 | `BASE_SpaceToUnderBar` | `:2376` | `:2908` | paired with the next entry |
| 15 | `BASE_UnderBarToSpace` | `:2377` | `:2917` | internal-only (used by both guild-name readers) |
| 16 | `BASE_GetSum` | `:2370` | `:2631` | internal-only (message checksum) |
| 17 | `BASE_GetSum2` | `:2371` | `:2663` | internal-only (item/skill checksum) |
| 18 | `BASE_WriteSkillBin` | `:2372` | `:2696` | internal-only build tool |
| 19 | `BASE_WriteItemList` | `:2373` | `:2734` | internal-only build tool |
| 20 | `BASE_ReadSkillBin` | `:2374` | `:2792` | internal-only reader |
| 21 | `BASE_ReadItemList` | `:2375` | `:2828` | internal-only reader |
| 22 | `BASE_WriteMessageBin` | `:2397` | `:4903` | internal-only build tool |
| 23 | `BASE_ReadMessageBin` | `:2398` | `:4932` | internal-only reader |
| 24 | `BASE_InitializeItemList` | `:2399` | `:4963` | internal-only (reached via `InitializeBaseDef`) |
| 25 | `BASE_ReadItemListFile` | `:2400` | `:4987` | internal-only |
| 26 | `BASE_InitializeEffectName` | `:2401` | `:5163` | internal-only |
| 27 | `BASE_InitializeSkill` | `:2418` | `:6001` | internal-only |
| 28 | `BASE_InitializeInitItem` | `:2417` | `:5947` | internal-only |
| 29 | `BASE_WriteInitItem` | `:2415` | `:5851` | internal-only build tool |
| 30 | `BASE_ReadInitItem` | `:2416` | `:5880` | internal-only reader |
| 31 | `BASE_InitializeAttribute` | `:2423` | `:6307` | internal-only |
| 32 | `BASE_InitializeClientGuildName` | `:2420` | `:6086` | internal-only (called by `BASE_GetClientGuildName`) |
| 33 | `IsClearString3` | `:2419` | `:6073` | internal-only DBCS helper |
| 34 | `BASE_GetHttpRequest` | `:2424` | `:6337` | no caller; body is `return 1;` |

Note the pattern: 21 of the 34 are loader/checksum/build helpers that only `BASE_InitializeBaseDef` (or another internal function) needs, yet all of them are published in the shared header. The remaining 13 are either superseded algorithms (`BASE_GetHitRate`, `BASE_GetDamageRate`, `BASE_GetAccuracyRate`), defective code (`BASE_GetDestByAction`), or defensive duplicates (`BASE_GetClientGuildName`, `BASE_GetBonusItemAbilityNosanc`). One further name is *partially* dead: `BASE_GetSkillDamage` is called 3 times, but every caller uses the 4-argument overload, so the 3-argument variant at `:1446` never executes.

---

## 7. Integration Points

### 7.1 No protocol endpoints

This component exposes **no** REST, GraphQL, gRPC, socket or IPC endpoint. It is a statically linked translation unit with C-linkage functions; its entire callable surface is the 101 functions and 47 globals declared in `Code/Basedef.h`. The only wire-protocol artefact it constructs is the commented-out message-type/size table in `BASE_CheckPacket`. Endpoint enumeration for this component is therefore not applicable; the protocol surface lives in `CPSock` and the `_MSG_*` handlers.

### 7.2 Integration surface (what other components actually bind to)

| Integration | Type | Purpose | Contract | Data format | Error handling |
|-------------|------|---------|----------|-------------|----------------|
| `TMSrv` process (static link) | In-process | game rules | 101 functions + 47 externs | C ABI, POD structs | return codes (`TRUE`/`FALSE`), `MessageBox` |
| `DBSrv` process (static link) | In-process | same rules for persistence validation | same | same | same |
| `itemlist.csv` / `extraitem.csv` | File input | item definitions | 12 static effects + 15 scalar fields | CSV, comma→space | `MessageBox` + partial load |
| `SkillData.csv` | File input | spell metadata (103 slots) | 21 fields + dotted `Act` | CSV | `MessageBox` + zeroed table |
| `InitItem.csv` | File input | world initial ground items | index, x, y, rotate | CSV | `MessageBox` |
| `Guilds.txt` | File input | guild names per group/server/guild | `group server guild name [s1 s2 s3]` | space-separated | `MessageBox` + zeroed table |
| `serverlist.txt` | File input | shard topology | `group number address` | space-separated | `MessageBox`, returns FALSE |
| `AttributeMap.dat` | File input | 1024×1024 terrain attributes | raw bytes, bit1 = blocked | binary | `MessageBox`, returns FALSE |
| `Language.txt` | File input | message catalogue | `<index>\t<text>` | TSV | per-line `MessageBox`, continues |
| `MobName.txt` | File input | mob name translation | `<key>\t<value>` | TSV | per-line `MessageBox`, continues |
| `ItemEffect.h` | File input (header parsed at runtime) | effect-name ↔ code map | `#define NAME VALUE` | C preprocessor text | `MessageBox` per bad line |
| `ItemList.bin` / `extraitem.bin` / `SkillData.bin` / `InitItem.bin` / `UI/strdef.bin` / `serverlist.bin` | File I/O (read + write) | binary caches | XOR 0x5A / XOR 0xFF / additive offset | binary | absent file → `MessageBox`; checksums not verified |
| `../ItemCSum.h`, `../SkillCSum.h` | File output | build-time checksum constants | `#define ITEM_CHECKSUM n` | C header | `NULL` check on `fopen` only |
| Running executable tail (`InitItem`) | File input | prebuilt initial-item block | XOR 0xFF `STRUCT_INITITEM[256]` | embedded binary | `MessageBox`, returns FALSE |
| Win32 current directory | OS state | all relative paths | `SetCurrentDirectory(exe dir)` | — | not checked |

### 7.3 Host-process integration points (call order)

| Host site | Call | Consequence if skipped or reordered |
|-----------|------|--------------------------------------|
| `TMSrv/imple.cpp:252`, `TMSrv/Server.cpp:444`, `:7158` | `BASE_InitializeBaseDef()` | all content tables empty; every item/ability lookup returns 0 |
| `TMSrv/Server.cpp:443`, `:3475`, `CNPCGene.cpp:682` | `BASE_InitModuleDir()` | relative data paths resolve against the launch directory |
| `TMSrv/Server.cpp:529` | `BASE_InitializeServerList()` | shard addresses empty |
| `TMSrv/Server.cpp:7159` | `BASE_ApplyAttribute(pHeightGrid, MAX_GRIDX)` | no terrain obstruction; mobs/items walk through blocked cells |
| `TMSrv/imple.cpp:253`, `TMSrv/Server.cpp:7160` | `BASE_InitializeMessage("Language.txt")` | `BASE_GetLanguage` returns garbage from an uninitialised 256 KB table |
| `TMSrv/Server.cpp:3476` | `BASE_InitializeHitRate()` | `g_pHitRate` stays all-zero; `BASE_GetDoubleCritical` hit bit degenerates |
| `TMSrv/Server.cpp:7142` | `BASE_InitializeMobname("MobName.txt", 0)` | `BASE_GetEnglish` never translates |

---

## 8. Design Patterns & Architecture

| Pattern / decision | Implementation | Location | Purpose / effect |
|--------------------|----------------|----------|------------------|
| **Table-driven rules** | 30 constant tables consulted instead of branches (`BaseSIDCHM`, `g_pSancRate`, `g_pMountBonus`, `g_pDistanceTable`, `g_pItemGrid`, `g_pGroundMask`, `g_pNextLevel`, `pTransBonus`, `KorIndex`, drop/sanc/refine rates) | `:39-736` | Data-driven tuning without recompilation; also means gameplay constants are scattered across table rows with no documentation |
| **Data-driven content plane** | Nine loaders + six binary caches + two generated C headers | `:2696-2906`, `4637-5242`, `5851-6335` | Content lives outside the binary; the server generates its own build artefacts |
| **Runtime header parsing** | `BASE_InitializeEffectName` parses `ItemEffect.h` line by line and recurses on `#include` | `:5163-5242` | The compile-time constant table and the runtime name table are kept in sync by re-parsing the header |
| **Fail-soft / fail-loud hybrid** | Missing item/skill/guild data → `MessageBox` + partial table; missing attribute map → `FALSE` | throughout loaders | Inconsistent failure policy: some subsystems boot with empty content |
| **Stacked multiplicative pipeline** | `BASE_GetCurrentScore` applies 13 ordered stages, each mutating the same struct in place | `:2973-4112` | One-pass evaluation; stage order is semantically significant and undocumented |
| **Encode-in-a-byte** | Refinement (tier+success), item date (3 effect bytes), fairy countdown, visual codes all pack multiple fields into `stEffect[].cValue` | `:2095-2285`, `6598-6814` | Saves persistence space; creates the band-collision and dual-decoder problems of BR-05/BR-26 |
| **Sentinel return values** | `BASE_GetGuild/GetArena/GetVillage` return `5` (= `MAX_GUILDZONE`) for "no zone"; `BASE_GetItemGem` returns `-1`; `BASE_GetItemAbility` returns `FALSE` for invalid input | `:4164-4267`, `:2143-2175` | Callers must special-case; one caller mistakes `5` for an index |
| **Obfuscation-at-rest** | XOR `0x5A` (item/skill/message), XOR `0xFF` (init item), additive offset (server list) | `:2696-2906`, `4903-4961`, `5851-5878`, `6268-6299` | Deters casual editing of binary caches; reversible in one loop, so not a security control |
| **Trailing-checksum seal** | Checksum appended after the payload; verification code present but disabled | `:2712-2715`, `2779`, `4919-4920`, `2815-2818`, `4951-4954` | Intended tamper detection, currently inert |
| **Global mutable registry** | 30 file-scope arrays with external linkage | `:39-755` | Zero-allocation content access from anywhere; no ownership, no lifetime, no thread safety |
| **Single-implementation library** | One TU compiled into two executables | `TMSrv.vcxproj:91`, `DBSrv.vcxproj:94` | Guarantees the game and DB servers share identical rules without a shared DLL |
| **Overload families instead of variadics** | 7 `BASE_GetLanguage` overloads, 2 `BASE_GetSkillDamage`, 2 `BASE_GetGuild` | `:1258-1312`, `1446/6342`, `4164/4207` | C++ overloads used to type-safely pass 0-2 `char*`/`int` parameters |
| **In-place output parameters** | 5 out-params on `BASE_GetCurrentScore`; `*error` on `BASE_CanCarry`; `*height` on `BASE_UpdateItem` | `:2973`, `:4413`, `:5292` | Avoids allocating result structs; makes the mutation contract implicit |
| **Anti-pattern: getter with side effects** | `GetCurrentScore`, `GetMobCheckSum`, `GetBonusScorePoint`, `GetHpMp`, `SortTradeItem`, `CheckFairyDate` all mutate their inputs | see Section 2.6 | "Get" verbs do not imply purity; `GetMobCheckSum` even zeroes `BaseScore` |
| **Anti-pattern: dead branch catalogs** | `BASE_GetRoute` 24-branch catalog with duplicated predicates; `BASE_CheckPacket` 100-branch catalog commented out; `BASE_VisualItemCode` impossible range | `:5576-5808`, `6476-6594`, `6607` | Logic preserved as documentation rather than deleted |

### 8.1 Architectural boundary assessment

The component is a clean **bottom layer** in the compile-time graph and a very dirty **data layer** at runtime. Function-level coupling is moderate and directional (handlers → rules, never rules → handlers). Data-level coupling is severe: 47 TUs read a 2,000-row message array directly, 16 read the item list directly, 15 read the guild-zone table directly. Cohesion inside the file is low: it groups at least nine unrelated concerns (progression accounting, item codecs, score computation, spatial queries, inventory geometry, content loading, combat maths, routing, presentation codecs) behind a single `BASE_` prefix and no sub-modules. The file is best characterised as a *stable, concrete, god-module*: everything depends on it, it depends on nothing, and it is internally unstructured.

---

## 9. Technical Debt & Risks

| ID | Risk | Area | Issue | Impact | Evidence |
|----|------|------|-------|--------|----------|
| TD-01 | **High** | Packet validation | `BASE_CheckPacket` body is one block comment; returns `FALSE` unconditionally. The 100-entry per-type size table never runs and callers only log when it returns non-zero. | Type-confused packets of valid framing size reach handlers; structural validation must exist elsewhere or not at all. | `:6475-6597`; callers `CPSock.cpp:547`, `TMSrv/Server.cpp:3898`, `:3983`, `DBSrv/Server.cpp:848`, `:931` |
| TD-02 | **High** | Route decoding | `*x++`/`*x--` increment pointers, not values (`*(x++)`), for all eight digits. | Any route decode returns the start position and walks the pointer out of bounds; the function is uncallable safely. | `:5378`, `5381`, `5384`, `5387`, `5391-5392`, `5397-5398`, `5403-5404`, `5409-5410` |
| TD-03 | **High** | Movement authority | Encoder and decoder disagree on `'2'`/`'8'`/`'3'` semantics (vertical axis mirrored). | Server-validated routes and any consumer of `BASE_GetDestByAction` cannot round-trip; client/server position divergence if both paths are ever used. | `:5576-5591` vs `5383-5399` |
| TD-04 | **High** | Score engine | `MaxHp`/`MaxMp` are doubled on every non-summon evaluation (`MaxHp += MaxHp + HPDelta`), independent of soul state. | Displayed and effective HP is 2× the class/gear model; any balance figure derived from `BaseSIDCHM` is off by the same factor; a second call in the same tick would compound. | `:3117-3122` |
| TD-05 | **High** | Inventory geometry | Occupancy maps are written with stride `CARRYGRIDX`/`CARGOGRIDX` and read with stride `CARRYGRIDY`/`CARGOGRIDY`. | Placement collision tests examine the wrong rows: items can be placed on top of others, or valid placements rejected. | `:4449` vs `:4468`; `:4516` vs `:4531` |
| TD-06 | **High** | Out-of-bounds read | Four ability readers accept `idx == MAX_ITEMLIST` (`idx > MAX_ITEMLIST` guard) into a `[6500]` array. | One-element OOB read of a ~140-byte struct for crafted item indices; potential crash or leaked adjacent-memory-derived ability values. | `:1502`, `1849`, `2002`, `2061` |
| TD-07 | **High** | Data integrity | All checksum verification is disabled (comparisons commented out or inside `#ifndef _DEBUG` blocks that no longer compare). | Corrupted or replaced `ItemList.bin`/`SkillData.bin`/`strdef.bin` load silently; the generated `ITEM_CHECKSUM*` headers are decorative. | `:2815-2818`, `:2849-2854`, `:2893-2896`, `:4951-4954` |
| TD-08 | **High** | Boot reliability | `BASE_InitializeServerList` gates the `serverlist.bin` write on `if (fp != NULL)` (the input handle already closed) instead of `fpBin`. | If `fpBin == NULL` the code still dereferences it via `fwrite`; if it is valid, the guard is simply wrong. Silent topology-cache loss at best. | `:6264-6288` |
| TD-09 | **High** | Global state | ~12.6 MiB of writable process globals, no accessors, no const discipline, no thread safety. `g_pHitRate` and `g_pEnglish` are defined without a header declaration. | Any future concurrency, multi-zone or reload feature races on these arrays; 46 TUs can mutate the message table directly. | `:739-755`; `g_pGuildName` 7.5 MiB |
| TD-10 | **High** | Progression integrity | `BASE_GetBonusSkillPoint` writes a negative remainder into `mob->SkillBonus`, declared `unsigned short`. | Over-budget characters wrap to a large positive bonus instead of being rejected. | `:846-853`; `Basedef.h:467` |
| TD-11 | **Medium** | Audit policy | `BASE_NeedLog` contains two tautologies, so it always returns `TRUE` for in-range items. | Audit log records every item operation; log volume and forensic signal-to-noise both degraded. | `:799` |
| TD-12 | **Medium** | Refine encoding | Tier bands are 4 wide while `success` is clamped to 20, so `success >= 4` writes a byte that decodes as the next tier; `BASE_GetItemSancSuccess` (`value/10`) and `BASE_GetItemGem` (`(value-230)%4`) read the same byte with different, mutually inconsistent arithmetic. | Refinement history and gem slot can be misread after enough successes; two decode functions disagree on the same item. | `:2239-2285`, `:2177-2200`, `:2143-2175` |
| TD-13 | **Medium** | Localisation storage | `memset(g_pMessageStringTable, MAX_STRING, 128)` passes the wrong fill value (`0xD0`) and a 128-byte length, so row 0 is corrupted and the other 1,999 rows are never reset on re-load. | Message id 0 always returns a `0xD0` string; a hot reload keeps stale text for every id present in the old table but absent from the new one. | `:4639`, `:4934` |
| TD-14 | **Medium** | Content loader | `BASE_InitializeItemList` checks `j == MAX_EFFECTINDEX` instead of `v == MAX_EFFECTINDEX`, so an unregistered effect name is stored as effect code 128 and no diagnostic is raised. | Item rows with a typo silently acquire effect 128; downstream `switch` statements ignore it, producing invisible stat loss. | `:5128-5143` |
| TD-15 | **Medium** | Equip rules | `weapontype %= 10` executes before `divweapon = weapontype / 10`, so `divweapon` is always 0 and the 150 % right-hand requirement tier is unreachable. | Two-handed/off-hand requirements are systematically under-penalised for weapon family 6. | `:4375-4386` |
| TD-16 | **Medium** | Guild display | `BASE_GetClientGuildName` indexes `g_pGuildName` with two subscripts instead of three. | Reads the wrong row; sub-guild suffixes can attach to an unrelated guild name. | `:1379-1381` |
| TD-17 | **Medium** | Waypoint decode | `BASE_GetDoubleCritical`'s wraparound-protection block is self-cancelling (`*sProgress = *cProgress` then compare against `*sProgress`); the `(*cProgress & 0x3FF) < 0` test is unsatisfiable. | Progress counters can drift past 1024 without normalisation, indexing `g_pHitRate[1024]` out of bounds over long sessions. | `:5473-5507` |
| TD-18 | **Medium** | Terrain | `BASE_UpdateItem` does not range-check `rotate` (dimension is 4) before indexing `g_pGroundMask[maskidx][rotate]`. | Rotated placements read an adjacent mask, stamping wrong heights; no crash because the array is contiguous. | `:5312` |
| TD-19 | **Medium** | Null handling | `BASE_GetCurrentScore`'s first affect loop dereferences `Affect[i]` without a NULL guard, unlike the two later loops. | Any caller passing a NULL affect array on the non-summon path crashes immediately. | `:3005-3007` vs `3258`, `3783` |
| TD-20 | **Medium** | Output safety | `BASE_GetLanguage` (7 overloads) and `BASE_GetItemCode` `sprintf` an unbounded table entry into caller buffers with no size parameter; the `"[UNHANDLED]"` path uses `strcpy`. | Buffer overflow whenever a language entry grows beyond the caller's stack buffer; caller buffers vary in size across the 47 consumers. | `:1258-1312`, `:1494` |
| TD-21 | **Medium** | Failure mode | 31 blocking `MessageBox`/`MessageBoxA` calls on server-path error handling; `BASE_InitializeBaseDef` ignores every loader's return value and always returns 1. | A missing CSV in an unattended/service context stalls the process behind a modal dialog; partial content is indistinguishable from complete content. | `:2570-2580` and the 31 sites listed in F-22 |
| TD-22 | **Medium** | Checksum helpers | The dangling `else` in `BASE_GetSum`/`BASE_GetSum2` attaches the extra term to `if (mod == 5)`, producing a mixed algorithm. | The "7-period" and "9-period" checksums are in fact 7- and 9-period *plus* an unconditional division term on all non-5 residues; both writers and readers agree, so they "work" but are not the documented function. | `:2654-2657`, `:2686-2690` |
| TD-23 | **Medium** | Destructive getter | `BASE_GetMobCheckSum` zeroes `mob->BaseScore` before summing. | Any future wiring of this already-declared API silently destroys a character's base stats. | `:2508` |
| TD-24 | **Low** | Dead capability | 34 exported names have no external caller (Section 6.4); `BASE_GetHttpRequest` returns constant 1; the 3-argument `BASE_GetSkillDamage(dam,ac,combat)` is shadowed by the 4-argument overload. | Misleading contract surface; a caller that wires `BASE_GetHitRate` expecting the live model gets a model the engine no longer uses. | Section 6.4 |
| TD-25 | **Low** | Dead code | `if (cul > MH) int kk = 0;`, duplicate `int kk = 0`, duplicated route predicates, `if (sIndex >= 2360 && sIndex < 2360)`, `if (isanc == 9) isanc = 9;`, unused `x/y/error` in `BASE_CanTrade`, unused `temp` in `BASE_GetEnglish`. | Reader confusion; competing intent signals for maintainers. | `:5558`, `:5817`, `:6607`, `:2123-2124`, `:4618-4621`, `:6446-6447` |
| TD-26 | **Low** | Magic numbers | `63`, `447`, `3980-3994`, `2360-2390`, `0x100000`, `1<<22`, `127`, `MH = 8`, `399`, `1001`, `1.6f/1.8f/2.2f` appear inline with no named constants. | Any balance change requires editing code, not data; audit of a single rule requires reading the whole function. | throughout `:2973-4113`, `:5538-5826` |
| TD-27 | **Low** | Header coupling | `Basedef.h` declares 191 message constants, 26 struct types and 101 functions in one file; `ItemEffect.h` is both a compile-time include and a runtime data file. | The runtime parser makes the header a data-plane artefact: adding a comment or reformatting it changes server behaviour at boot. | `:5163-5242` |

---

## 10. Test Coverage Analysis

### 10.1 Test inventory

No automated tests exist for this component, or for the project as a whole. Evidence, gathered project-wide with the ignore list applied:

| Search | Scope | Result |
|--------|-------|--------|
| Files/directories matching `test`/`spec`/`fixture`/`mock` | entire project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec` | **0** matches (`openspec/` matched only on the word "spec" in its name and is excluded by the parameter) |
| Files matching `*test*`, `*Test*`, `*_test*`, `*.spec.*` | entire project root, same exclusions | **0** files |
| `.vcxproj` project inventory | `W2PP Code Project.sln` | 2 projects only — `TMSrv`, `DBSrv`. No test project, no GoogleTest/Catch2/CPPUnit/MSpec/CppUTest reference in either `.vcxproj` |
| `*.vcxproj` test-related `ClCompile` entries | both projects | **0** |
| Test framework packages / fixture directories | whole tree | **0** |
| Build targets producing a test binary | `Code/release/TMSrv/run/` | only `TMSrv.exe`, `TMSrv.pdb` |
| `assert()` / `static_assert` usage in `Basedef.cpp` | this component | **0** |

### 10.2 Coverage assessment

| Component area | Functions | Unit tests | Integration tests | Coverage | Test quality |
|----------------|-----------|------------|-------------------|----------|--------------|
| Progression accounting (BR-01..03) | 5 | 0 | 0 | 0 % | No verification of the tier budgets, the deficit claw-back priority, or the negative-remainder wrap |
| Item ability resolution (BR-04) | 5 | 0 | 0 | 0 % | The four variants' differing exclusion lists are entirely unverified |
| Refine codec (BR-05) | 7 | 0 | 0 | 0 % | The tier/success band collision and the two divergent decoders have no test |
| Mob ability aggregation (BR-06) | 2 | 0 | 0 | 0 % | Dual-wield percentages and the full-set bonus are unverified |
| Score pipeline (BR-07) | 1 | 0 | 0 | 0 % | 204 decisions, 13 ordered stages, 32 affect types, 5 out-params — zero assertions |
| Equip / inventory / trade (BR-08..10) | 5 | 0 | 0 | 0 % | Grid-shape geometry has no test; the stride defect would be caught by a two-item placement test |
| Combat maths (BR-11..15) | 6 | 0 | 0 | 0 % | RNG-dependent curves have no seeded harness; the disabled hit-rate helpers are untested and unused |
| Routing / distance (BR-16..18) | 3 | 0 | 0 | 0 % | No round-trip test exists between encoder and decoder (which would immediately expose TD-02/TD-03) |
| Grid height / terrain (BR-19..20) | 3 | 0 | 0 | 0 % | Mask rotations and the `rotate` bound have no test |
| Content loaders (BR-21) | 12 | 0 | 0 | 0 % | No fixture files, no malformed-input tests, no checksum round-trip test |
| Integrity / naming (BR-22, BR-23) | 4 | 0 | 0 | 0 % | Reserved-word list and DBCS handling unverified; `BASE_NeedLog`'s tautology would be caught by one negative test |
| Spatial / identity lookups (BR-24) | 6 | 0 | 0 | 0 % | Sentinels (`5`, `-1`, `0`) have no boundary test |
| Time-bounded items (BR-25) | 3 | 0 | 0 | 0 % | Calendar arithmetic and month-boundary expiry have no test; `localtime` makes them non-deterministic without injection |
| Presentation codecs (BR-26, BR-28, BR-29) | 6 | 0 | 0 | 0 % | Visual-code range mappings unverified; the 4-subscript/2-subscript mismatch would be caught by one lookup test |
| Packet validation (BR-27) | 1 | 0 | 0 | 0 % | The table is commented out; nothing tests it because nothing executes it |
| Utility (BR-30) | 2 | 0 | 0 | 0 % | Stub, no assertions |

Aggregate: **0 % automated coverage** over 101 functions and ~4,929 lines of code. No integration, smoke, fixture or regression suite exists.

### 10.3 Verification substitutes actually available

Because no test automation exists, the only currently available verification surfaces are:

1. **In-process diagnostics** — 31 `MessageBox`/`MessageBoxA` calls and the item-load "Unregisterd effect" / "Too much items in mobname.txt" dialogs act as manual assertions on content validity (and block the process when they fire).
2. **Generated artefacts as oracles** — `ItemCSum.h`, `SkillCSum.h`, `ItemList.bin`, `serverlist.bin` let a human diff content-build outputs between runs, but the comparison values are not checked programmatically.
3. **Client-observable behaviour** — the score pipeline's outputs are streamed to the client (`MSG_UpdateScore`, `MSG_UpdateEtc`), so an in-game character sheet is the de facto regression test for BR-01..07. It cannot distinguish a 2× HP defect from intended design (TD-04) without an independent oracle.
4. **Prebuilt binary** — `Code/release/TMSrv/run/TMSrv.exe` + `.pdb` (509 KB / 2.8 MB) is a shipped artefact that could serve as a differential oracle, but it is a build output, not a test.

### 10.4 Highest-value untested behaviours (risk ranking)

| Rank | Behaviour | Why untested testing matters | Related debt |
|------|-----------|------------------------------|--------------|
| 1 | Score pipeline stage ordering | 13 stages mutate one struct; a single ordering error changes every character's stats and is invisible without an oracle | TD-04, TD-19 |
| 2 | Route encode/decode round-trip | Would deterministically expose the pointer bug and the mirrored axis | TD-02, TD-03 |
| 3 | Grid occupancy with two adjacent multi-cell items | Would deterministically expose the stride defect | TD-05 |
| 4 | Ability readers at index boundaries (0, 6499, 6500) | Would expose the guard inconsistency | TD-06 |
| 5 | Refine encode→decode→success-rate round-trip for all `sanc × success` pairs | Would expose the band collision and the twin decoders | TD-12 |
| 6 | Budget arithmetic at tier/level boundaries (199/254/299/354/399) | Would expose the non-monotonic Mortal budget and the negative wrap | TD-10 |
| 7 | Content load with malformed/missing files | Would expose fail-soft vs fail-loud inconsistency and the `memset` fill value | TD-08, TD-13, TD-21 |
| 8 | Packet size table (if re-enabled) | Would define the structural validation contract | TD-01 |

---

## Appendix A — Function inventory

Counts are per exported *name*: the seven `BASE_GetLanguage` overloads and the two `BASE_GetSkillDamage` / `BASE_GetGuild` overloads share their external counts. "Ext. calls" = call sites in other translation units  -  `Basedef.cpp` and `Basedef.h` are both excluded, so header declarations are not counted. "Ext. files" = distinct translation units holding those sites. Name matches inside comments are counted, so the figures are upper bounds. Names with 0 external call sites are exported but never used outside the component.

| # | Function | Lines | LOC | Decisions | Ext. calls | Ext. files |
|---|---|---|---|---|---|---|
| 1 | `BASE_NeedLog` | 766-809 | 44 | 9 | 2 | 1 |
| 2 | `BASE_GetBonusSkillPoint` | 810-857 | 48 | 8 | 3 | 3 |
| 3 | `BASE_GetBonusScorePoint` | 858-1173 | 316 | 75 | 9 | 5 |
| 4 | `BASE_GetHpMp` | 1174-1209 | 36 | 3 | 5 | 3 |
| 5 | `BASE_GetSpeed` | 1210-1224 | 15 | 2 | 9 | 2 |
| 6 | `BASE_GetDamage` | 1225-1257 | 33 | 5 | 5 | 2 |
| 7 | `BASE_GetLanguage` | 1258-1265 | 8 | 1 | 4 | 2 |
| 8 | `BASE_GetLanguage` | 1266-1273 | 8 | 1 | 4 | 2 |
| 9 | `BASE_GetLanguage` | 1274-1281 | 8 | 1 | 4 | 2 |
| 10 | `BASE_GetLanguage` | 1282-1289 | 8 | 1 | 4 | 2 |
| 11 | `BASE_GetLanguage` | 1290-1297 | 8 | 1 | 4 | 2 |
| 12 | `BASE_GetLanguage` | 1298-1305 | 8 | 1 | 4 | 2 |
| 13 | `BASE_GetLanguage` | 1306-1313 | 8 | 1 | 4 | 2 |
| 14 | `BASE_GetGuildName` | 1314-1356 | 43 | 3 | 25 | 9 |
| 15 | `BASE_GetClientGuildName` | 1357-1445 | 89 | 10 | 0 | 0 |
| 16 | `BASE_GetSkillDamage` | 1446-1478 | 33 | 5 | 3 | 1 |
| 17 | `BASE_GetItemCode` | 1479-1496 | 18 | 1 | 43 | 22 |
| 18 | `BASE_GetItemAbilityNosanc` | 1497-1646 | 150 | 41 | 7 | 3 |
| 19 | `BASE_GetItemAbility` | 1647-1843 | 197 | 52 | 93 | 16 |
| 20 | `BASE_GetStaticItemAbility` | 1844-1992 | 149 | 39 | 10 | 2 |
| 21 | `BASE_GetBonusItemAbility` | 1993-2051 | 59 | 13 | 3 | 2 |
| 22 | `BASE_GetBonusItemAbilityNosanc` | 2052-2094 | 43 | 10 | 0 | 0 |
| 23 | `BASE_GetItemSanc` | 2095-2142 | 48 | 14 | 79 | 10 |
| 24 | `BASE_GetItemGem` | 2143-2176 | 34 | 8 | 4 | 3 |
| 25 | `BASE_GetItemSancSuccess` | 2177-2201 | 25 | 4 | 1 | 1 |
| 26 | `BASE_GetSuccessRate` | 2202-2226 | 25 | 4 | 2 | 1 |
| 27 | `BASE_GetGrowthRate` | 2227-2238 | 12 | 1 | 1 | 1 |
| 28 | `BASE_SetItemSanc` | 2239-2286 | 48 | 13 | 54 | 9 |
| 29 | `BASE_GetItemAmount` | 2287-2303 | 17 | 3 | 12 | 6 |
| 30 | `BASE_SetItemAmount` | 2304-2373 | 70 | 13 | 93 | 4 |
| 31 | `BASE_GetMobAbility` | 2374-2481 | 108 | 20 | 13 | 1 |
| 32 | `BASE_GetMaxAbility` | 2482-2502 | 21 | 3 | 0 | 0 |
| 33 | `BASE_GetMobCheckSum` | 2503-2521 | 19 | 2 | 0 | 0 |
| 34 | `BASE_CheckValidString` | 2522-2552 | 31 | 5 | 2 | 2 |
| 35 | `BASE_CheckHangul` | 2553-2563 | 11 | 1 | 0 | 0 |
| 36 | `BASE_InitializeBaseDef` | 2564-2582 | 19 | 0 | 3 | 2 |
| 37 | `BASE_ApplyAttribute` | 2583-2607 | 25 | 3 | 1 | 1 |
| 38 | `BASE_InitModuleDir` | 2608-2630 | 23 | 2 | 3 | 2 |
| 39 | `BASE_GetSum` | 2631-2662 | 32 | 7 | 0 | 0 |
| 40 | `BASE_GetSum2` | 2663-2695 | 33 | 7 | 0 | 0 |
| 41 | `BASE_WriteSkillBin` | 2696-2733 | 38 | 4 | 0 | 0 |
| 42 | `BASE_WriteItemList` | 2734-2791 | 58 | 8 | 0 | 0 |
| 43 | `BASE_ReadSkillBin` | 2792-2827 | 36 | 3 | 0 | 0 |
| 44 | `BASE_ReadItemList` | 2828-2907 | 80 | 10 | 0 | 0 |
| 45 | `BASE_SpaceToUnderBar` | 2908-2916 | 9 | 2 | 0 | 0 |
| 46 | `BASE_UnderBarToSpace` | 2917-2942 | 26 | 4 | 0 | 0 |
| 47 | `BASE_ClearMob` | 2943-2964 | 22 | 2 | 4 | 1 |
| 48 | `BASE_ClearMobExtra` | 2965-2972 | 8 | 0 | 4 | 1 |
| 49 | `BASE_GetCurrentScore` | 2973-4113 | 1141 | 204 | 1 | 1 |
| 50 | `BASE_GetFirstKey` | 4114-4163 | 50 | 6 | 7 | 1 |
| 51 | `BASE_GetGuild` | 4164-4174 | 11 | 2 | 4 | 4 |
| 52 | `BASE_GetIndex` | 4175-4206 | 32 | 7 | 0 | 0 |
| 53 | `BASE_GetGuild` | 4207-4236 | 30 | 8 | 4 | 4 |
| 54 | `BASE_GetSubGuild` | 4237-4246 | 10 | 1 | 2 | 2 |
| 55 | `BASE_GetArena` | 4247-4257 | 11 | 2 | 3 | 3 |
| 56 | `BASE_GetVillage` | 4258-4268 | 11 | 2 | 19 | 15 |
| 57 | `BASE_GetKorFirst` | 4269-4280 | 12 | 0 | 0 | 0 |
| 58 | `BASE_ClearItem` | 4281-4292 | 12 | 0 | 37 | 11 |
| 59 | `BASE_CanEquip` | 4293-4412 | 120 | 27 | 2 | 1 |
| 60 | `BASE_CanCarry` | 4413-4479 | 67 | 11 | 1 | 1 |
| 61 | `BASE_CanCargo` | 4480-4538 | 59 | 11 | 4 | 3 |
| 62 | `BASE_SortTradeItem` | 4539-4587 | 49 | 6 | 0 | 0 |
| 63 | `BASE_CanTrade` | 4588-4636 | 49 | 8 | 2 | 1 |
| 64 | `BASE_InitializeMessage` | 4637-4711 | 75 | 10 | 2 | 2 |
| 65 | `BASE_InitializeMobname` | 4712-4902 | 191 | 27 | 1 | 1 |
| 66 | `BASE_WriteMessageBin` | 4903-4931 | 29 | 3 | 0 | 0 |
| 67 | `BASE_ReadMessageBin` | 4932-4962 | 31 | 3 | 0 | 0 |
| 68 | `BASE_InitializeItemList` | 4963-4986 | 24 | 1 | 0 | 0 |
| 69 | `BASE_ReadItemListFile` | 4987-5162 | 176 | 20 | 0 | 0 |
| 70 | `BASE_InitializeEffectName` | 5163-5243 | 81 | 10 | 0 | 0 |
| 71 | `BASE_InitializeHitRate` | 5244-5291 | 48 | 10 | 1 | 1 |
| 72 | `BASE_UpdateItem` | 5292-5340 | 49 | 11 | 1 | 1 |
| 73 | `BASE_UpdateItem2` | 5341-5366 | 26 | 5 | 0 | 0 |
| 74 | `BASE_GetDestByAction` | 5367-5414 | 48 | 10 | 0 | 0 |
| 75 | `BASE_GetManaSpent` | 5415-5422 | 8 | 0 | 1 | 1 |
| 76 | `BASE_GetHitRate` | 5423-5442 | 20 | 4 | 0 | 0 |
| 77 | `BASE_GetDamageRate` | 5443-5454 | 12 | 2 | 0 | 0 |
| 78 | `BASE_GetAccuracyRate` | 5455-5465 | 11 | 1 | 0 | 0 |
| 79 | `BASE_GetDoubleCritical` | 5466-5537 | 72 | 14 | 1 | 1 |
| 80 | `BASE_GetRoute` | 5538-5827 | 290 | 31 | 9 | 2 |
| 81 | `BASE_GetDistance` | 5828-5850 | 23 | 4 | 7 | 3 |
| 82 | `BASE_WriteInitItem` | 5851-5879 | 29 | 3 | 0 | 0 |
| 83 | `BASE_ReadInitItem` | 5880-5946 | 67 | 6 | 0 | 0 |
| 84 | `BASE_InitializeInitItem` | 5947-6000 | 54 | 7 | 0 | 0 |
| 85 | `BASE_InitializeSkill` | 6001-6072 | 72 | 8 | 0 | 0 |
| 86 | `IsClearString3` | 6073-6085 | 13 | 2 | 0 | 0 |
| 87 | `BASE_InitializeClientGuildName` | 6086-6152 | 67 | 11 | 0 | 0 |
| 88 | `BASE_InitializeGuildName` | 6153-6217 | 65 | 9 | 4 | 3 |
| 89 | `BASE_InitializeServerList` | 6218-6306 | 89 | 12 | 1 | 1 |
| 90 | `BASE_InitializeAttribute` | 6307-6336 | 30 | 2 | 0 | 0 |
| 91 | `BASE_GetHttpRequest` | 6337-6341 | 5 | 0 | 0 | 0 |
| 92 | `BASE_GetSkillDamage` | 6342-6443 | 102 | 24 | 3 | 1 |
| 93 | `BASE_GetEnglish` | 6444-6461 | 18 | 2 | 1 | 1 |
| 94 | `BASE_GetWeekNumber` | 6462-6474 | 13 | 0 | 1 | 1 |
| 95 | `BASE_CheckPacket` | 6475-6597 | 123 | 111 | 5 | 2 |
| 96 | `BASE_VisualItemCode` | 6598-6661 | 64 | 17 | 5 | 2 |
| 97 | `BASE_VisualAnctCode` | 6662-6731 | 70 | 26 | 5 | 2 |
| 98 | `BASE_SetItemDate` | 6732-6762 | 31 | 3 | 5 | 3 |
| 99 | `BASE_CheckItemDate` | 6763-6781 | 19 | 3 | 8 | 2 |
| 100 | `BASE_CheckFairyDate` | 6782-6814 | 33 | 5 | 1 | 1 |
| 101 | `GetItemPointer` (no `BASE_` prefix; declared at `Basedef.h:2361`) | 2340-2372 | 33 | 5 | 24 | 6 |

Totals: 100 BASE_-prefixed definitions plus 1 helper = 101 functions; 639 BASE_* external call sites plus 24 GetItemPointer sites = 663 external call sites in total.
---

## Appendix B — Analysis notes, assumptions and ambiguities

| # | Note |
|---|------|
| A-01 | This analysis covers `Code/Basedef.cpp` only. `Code/Basedef.h` (types, constants, 101 declarations, 47 externs) is covered by the separate *Basedef Domain Contract* component; it is referenced here but not re-analysed. |
| A-02 | Call-site counts were produced by matching `BASE_<name>(` across all `.cpp`/`.h` in `Code/`, excluding **both** `Basedef.cpp` and `Basedef.h` (header declarations would otherwise add a spurious +1 per name, which was the defect corrected between the first and final revision of this report) plus the four ignored folders. Comments are included in the match (e.g. the two commented `BASE_GetHitPosition` references in `CMob.cpp:294` and `Basedef.h:2413`), so counts are upper bounds. `GetItemPointer` is counted separately because it has no `BASE_` prefix. |
| A-03 | "Decision points" counts every `if`/`for`/`while`/`switch`/`case` token, including those inside block comments (relevant for `BASE_CheckPacket`, whose 111 decisions are all commented out). |
| A-04 | Struct sizes used for the memory footprint estimate assume 4-byte `int`, 4-byte pointers, 8-byte `long long`, 1-byte `char` (MSVC Win32 defaults for the `Debug/Win32` configurations in the solution). |
| A-05 | The dimensions of `g_pGuildName` (`[10][16][4096][12]`) are taken verbatim from `Basedef.cpp:739`; `Basedef.h:2460` declares the same shape. The `[16]` middle dimension is never bounds-checked by the loader beyond `Server < 16`. |
| A-06 | Behaviour of `BASE_GetRoute`'s direction semantics is stated as *observed from the code*, not from protocol documentation — no protocol specification exists in the repository. |
| A-07 | Whether `BASE_GetCurrentScore`'s HP/MP doubling (TD-04) is intentional is **not determinable from the code**: no comment, no specification, and no test distinguishes it from a defect. It is reported as an observed behaviour with high impact. |
| A-08 | Whether the commented-out `BASE_CheckPacket` table reflects the current message set cannot be verified. All the types it names do exist in `Basedef.h` (`_MSG_Trade` at `:2046`, `_MSG_CNFCheck` at `:2060`, `_MSG_ReqTransper` at `:2169`, `_MSG_NPIDPASS` at `:2244`), so the table is structurally plausible — but it cannot be shown to be complete, and it has no effect either way. |
| A-09 | The `EF_*` semantics used throughout (e.g. `EF_SANC = 43`, `EF_AMOUNT = 61`, `EF_WDAY = 106`) come from `Code/ItemEffect.h`; the Korean comments there were not translated, so any *name* interpretation beyond the numeric value is inferred from usage. |
| A-10 | No runtime observation was performed (no code executed, no tests run); all findings are static. |

---

*End of report — Basedef Rules Engine (`Code/Basedef.cpp`, 6,814 lines).*
