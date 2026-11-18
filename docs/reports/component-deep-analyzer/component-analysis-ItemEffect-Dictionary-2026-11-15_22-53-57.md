# Component Deep Analysis Report — ItemEffect Dictionary

**Component:** ItemEffect Dictionary
**Primary location:** `Code/ItemEffect.h` (6,534 bytes, 153 lines)
**Project:** W2PP — decompiled C/C++ (Win32, MSVC v143) MMORPG server emulator
**Analysis date:** 2026-11-15
**Ignored folders:** `.agents`, `.codegraph`, `.opencode`, `openspec`

---

## 1. Executive Summary

The ItemEffect Dictionary is a single 153-line, preprocessor-only header that defines the numeric **effect ID contract** of the entire item system. It declares 101 active `#define` constants (`EF_LEVEL` = 1 through `EF_NOTRADE` = 127) that form a flat, hand-partitioned namespace of unsigned-byte effect identifiers. Each identifier names a slot semantics: the ID identifies **what** an item attribute means (damage bonus, refinement level, stack amount, use-script verb, trade prohibition, mount statistic, expiry date component, and so on). The header contains no types, no functions, no `#include` directives, and no include guard — it is pure textual macro substitution with **efferent coupling of exactly 0**.

The component plays a dual role, and this duality is its most architecturally significant property:

1. **Compile-time contract.** Fourteen translation units in `Code/` include the header directly (`#include "ItemEffect.h"` / `"..\ItemEffect.h"` / `"../ItemEffect.h"`), and 26 distinct source files reference `EF_*` tokens or consume the effect-ID space. Both shipped binaries (`TMSrv`, `DBSrv`) compile it, because `Code/Basedef.cpp` — the shared engine layer — is listed in both project files (`Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj`).
2. **Runtime data file.** `BASE_InitializeEffectName()` (`Code/Basedef.cpp:5163-5242`) does not rely on the compiled constants at all. It `fopen`s `../../Common/ItemEffect.h` (falling back to `../../TMSRV/Run/ItemEffect.h`, `Code/Basedef.cpp:5170-5173`), parses every `#define EF_name <integer>` line with `sscanf`/`atoi`, and populates `char EffectNameTable[128][24]` (`Code/Basedef.cpp:741`, `Code/Basedef.h:2462`). That table is then the name-to-ID lookup used when loading `itemlist.csv` / `extraitem.csv` (`Code/Basedef.cpp:5121-5143`), so the same text file must be present on disk at server start-up, next to the CSV content files.

In short: the header is simultaneously the compile-time contract and the runtime schema file for item content. It is the only artifact in the repository whose **revision is a data migration**.

Key findings:

* **The namespace is 0..127, not 0..255.** Storage is `unsigned char cEffect` (`Code/Basedef.h:407`), so 256 values are representable, but `MAX_EFFECTINDEX` is 128 (`Code/Basedef.h:142`) and the name table has exactly 128 rows. IDs 128..255 are unreachable by name and can only be produced by out-of-contract writes (see BR-02, and the loader bug in section 11).
* **The declared namespace is sparse.** 26 numeric slots are undefined holes (0, 28, 86, 88-90, 92-99, 111, 114-125); two definitions are commented out (`EF_NONE` = 0, line 1; `EF_PRICE` = 28, line 67); and 22 declared tokens have zero references anywhere in `Code/` (see BR-26).
* **Code uses effect IDs that the dictionary does not declare.** The literal 98 is written to a character's face-item effect slot (`Code/TMSrv/_MSG_UseItem.cpp:3045`, `Code/TMSrv/ProcessDBMessage.cpp:708,715,722`), and the entire range 116..125 is used as an *attribute/colour refinement* sub-namespace (`116 + color`, `Code/TMSrv/_MSG_UseItem.cpp:3711-3717`) although no `EF_*` name exists for any value in that range. The dictionary is therefore **incomplete with respect to its consumers**.
* **`EF_VOLATILE` (38) has silently changed meaning.** Its comment documents a 0..7 domain (general, potion, money, key, book 1-3, spell+15, level+1). Runtime dispatch in `Code/TMSrv/_MSG_UseItem.cpp` branches on **63 distinct values up to 243**, plus `_MSG_GetItem.cpp` and `_MSG_Sell.cpp`. The effect ID is now a **use-script opcode**, and the header comment is stale documentation.
* **`EF_SANC` (43) is overloaded as a packed bitfield.** Its `cValue` encodes refinement level *and* success-attempt count (`Code/Basedef.cpp:2239-2285`), with 230..253 as a second encoding space for levels 10..15 — while the *effect ID* range 116..125 encodes which attribute a refinement is bound to.
* **The loader has three defects** that directly affect the dictionary's runtime integrity: an off-by-one bound check (`val > MAX_EFFECTINDEX` at `Code/Basedef.cpp:5222`), a mis-compared loop variable (`if (j == MAX_EFFECTINDEX)` at `Code/Basedef.cpp:5134`), and an out-of-bounds write `str2[1 + strlen(str2)] = 0` (`Code/Basedef.cpp:5208`). See section 11.
* **Zero tests exist.** No test directory, no test source file, and no test framework reference anywhere in the repository. The dictionary and its loader are entirely unprotected (section 12).

---

## 2. Data Flow Analysis

The component has two completely separate flows: a compile-time flow (macro substitution) and a runtime flow (header-as-data). The runtime flow is the one with failure modes.

```
Compile-time flow (per translation unit):
 1. MSVC preprocessor opens Code/ItemEffect.h while compiling e.g. Code/Basedef.cpp:35
 2. Every EF_* token in that TU is replaced by its integer literal (EF_DAMAGE -> 2, EF_SANC -> 43)
 3. The literal is passed as `unsigned char Type` to resolvers, or compared against `stEffect[i].cEffect`
 4. No symbol, storage or runtime lookup is produced by this path (macros only)

Runtime flow (server start-up, before any content load):
 5. BASE_InitializeBaseDef() (Code/Basedef.cpp:2564) calls BASE_InitializeSkill(), then
    BASE_InitializeEffectName() (Code/Basedef.cpp:2574)
 6. BASE_InitializeEffectName() clears EffectNameTable[0..127][0] (Code/Basedef.cpp:5165-5166)
 7. fopen("../../Common/ItemEffect.h","rb"), fallback "../../TMSRV/Run/ItemEffect.h"
    (Code/Basedef.cpp:5170-5173); on failure MessageBoxA("There is no file") and return
    with an EMPTY table
 8. For each line: fgets -> first token must be "#define" (or "#include", see BR-17);
    sscanf extracts name (str2) and value (str3); atoi(str3) -> val
 9. Range check 0..MAX_EFFECTINDEX, duplicate check via EffectNameTable[val][0] != 0,
    then memcpy(EffectNameTable[val], str2, 24) (Code/Basedef.cpp:5218-5236)
10. BASE_InitializeItemList() (Code/Basedef.cpp:2575 -> 4963) calls
    BASE_ReadItemListFile("../../Common/itemlist.csv", 1) and "../../Common/extraitem.csv"
11. Each CSV row supplies up to MAX_STATICEFFECT (12) name/value pairs; every name is
    resolved by linear strcmp against EffectNameTable[0..127] (Code/Basedef.cpp:5128-5132)
12. Resolved IDs are stored as short sEffect in STRUCT_ITEMLIST.stEffect[12]
    (Code/Basedef.h:894-899) -> the static template of the item
13. At gameplay time, per-instance attributes live in STRUCT_ITEM.stEffect[3]
    (unsigned char cEffect / cValue, Code/Basedef.h:398-412)
14. Rules read them through BASE_GetItemAbility() (Code/Basedef.cpp:1647),
    BASE_GetStaticItemAbility() (1844), BASE_GetBonusItemAbility() (1993),
    BASE_GetBonusItemAbilityNosanc() (2052)
15. Handlers branch on specific IDs (EF_VOLATILE use-script dispatch, EF_KEYID door match,
    EF_NOTRADE trade gate, EF_NOSANC refine gate, ...)
16. Mutators write back: BASE_SetItemAmount (2304), BASE_SetItemSanc (2239),
    BASE_SetItemDate (6732), and ~150 direct `stEffect[n].cEffect = <literal>` assignments
```

The critical property of this flow is that step 7 can fail while step 5 reports success: `BASE_InitializeEffectName()` returns `void`, and its failure path is a modal `MessageBoxA` plus `return`. In a headless/service deployment the message box either blocks or is auto-dismissed, and the server continues with an all-empty `EffectNameTable`. Step 11 then fails to resolve every effect name, and step 12 stores garbage IDs. This is the highest-severity data-flow risk of the component.

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Data contract | Effect ID 0 means "no effect / empty slot"; the slot is reusable and is the only legal "absent" marker | `Code/ItemEffect.h:1` (comment), `Code/Basedef.cpp:2317-2336` |
| Data contract | Effect ID address space is 0..127 (`MAX_EFFECTINDEX`), stored in `unsigned char` (0..255 representable) | `Code/Basedef.h:142`, `Code/Basedef.h:407`, `Code/Basedef.h:2462` |
| Data contract | An item carries 12 static effect slots (content template) and 3 dynamic effect slots (per instance) | `Code/Basedef.h:894-899`, `Code/Basedef.h:398-412` |
| Namespace | IDs 1..16 = character score bonuses (level, damage, AC, HP, MP, EXP, STR/INT/DEX/CON, SPECIAL1-4, SCORE14/15) | `Code/ItemEffect.h:3-19` |
| Namespace | IDs 17..25 = equip requirements (POS, CLASS, R1SIDC, R2SIDC, REQ_STR/INT/DEX/CON) | `Code/ItemEffect.h:21-25,56-59` |
| Namespace | IDs 26..74 = stat/bonus effects (speed, range, spell, grid, volatile, resists, guild PvP, magic, amount, indices, add-effects) | `Code/ItemEffect.h:65-116` |
| Namespace | IDs 78..83 = mount statistics (life, HP, sanc/growth, feed, kill count) | `Code/ItemEffect.h:121-128`, `Code/Basedef.cpp:1720-1756` |
| Namespace | IDs 100..105 = contiguous item-grade index (GRADE0..GRADE5, 0..5) | `Code/ItemEffect.h:136-141`, `Code/TMSrv/Server.cpp:1406-1408` |
| Namespace | IDs 106..110 = expiry date stamp (day, hour, minute, year, month) | `Code/ItemEffect.h:143-147`, `Code/Basedef.cpp:6732-6761` |
| Namespace | IDs 112/113 = target classification (mob type, item type) | `Code/ItemEffect.h:149-150` |
| Namespace | IDs 126/127 = hard exclusion flags (no refinement, no trade) | `Code/ItemEffect.h:152-153` |
| Undeclared | IDs 116..125 = refinement attribute/colour sub-namespace, assigned as `116 + color` (color from item index 3407..3417) | `Code/TMSrv/_MSG_UseItem.cpp:3675,3711-3717`, `Code/Basedef.cpp:2102-2109` |
| Undeclared | ID 98 = class-master marker written to the face item, value 0 or 3 | `Code/TMSrv/_MSG_UseItem.cpp:3045`, `Code/TMSrv/ProcessDBMessage.cpp:708-726` |
| Validation | Refinement level multiplies any effect value as `value * (sanc + 10) / 10`, with an explicit exemption list | `Code/Basedef.cpp:1813-1817`, `2043-2047`, `1967-1972` |
| Validation | `sanc >= 9` on an armour slot (`nPos & 0xF00`) grants one extra effective level | `Code/Basedef.cpp:1809-1811`, `1962-1965`, `2035-2038` |
| Validation | `EF_SANC` cValue packing: level*10+success, with 230..253 for levels 10..15 | `Code/Basedef.cpp:2095-2141`, `2239-2285` |
| Validation | Attack speed value 1 is a sentinel meaning 10 | `Code/Basedef.cpp:1713-1714`, `1784-1785`, `2014-2015`, `2071-2072` |
| Validation | Run speed is clamped: `>= 3` becomes 2; `> 0 && sanc == 9` becomes +1 | `Code/Basedef.cpp:1819-1826`, `1974-1981` |
| Validation | `EF_RESISTALL` fans out into all four resist queries | `Code/Basedef.cpp:1790-1807`, `2020-2033` |
| Validation | Grid size is clamped to 0..7 | `Code/Basedef.cpp:1835-1839` |
| Validation | `EF_DAMAGEADD` / `EF_MAGICADD` are only honoured for unique 41..50 | `Code/Basedef.cpp:1659-1663` |
| Validation | `EF_DAMAGE` on a two-hand position (`nPos == 32`) is promoted to `EF_DAMAGE2` | `Code/Basedef.cpp:1671-1675` |
| Validation | Trade rejected when `EF_NOTRADE != 0` | `Code/TMSrv/_MSG_Trade.cpp:179`, `_MSG_SendAutoTrade.cpp:84` |
| Business logic | `EF_VOLATILE` is the use-item opcode (63 values observed, 1..243) | `Code/TMSrv/_MSG_UseItem.cpp:94-5710` |
| Business logic | `EF_AMOUNT` stores the stack count; absent slots are claimed on first write | `Code/Basedef.cpp:2287-2338` |
| Business logic | `EF_KEYID` must match the door/gate key; `EF_QUEST` is the quest key variant | `Code/TMSrv/Server.cpp:6092,7178,7497`, `CCastleZakum.cpp:77-89` |
| Business logic | Effect names in `itemlist.csv` are resolved through `EffectNameTable` built from this header | `Code/Basedef.cpp:5121-5143` |
| Integrity | Duplicate numeric values are rejected at load with a modal dialog | `Code/Basedef.cpp:5229-5234` |
| Integrity | Values outside 0..127 are rejected at load with a modal dialog | `Code/Basedef.cpp:5222-5227` |
| Lifecycle | Header is loaded before item content; failure leaves an empty name table but continues | `Code/Basedef.cpp:2574-2575`, `5175-5180` |

### Detailed breakdown of the business rules

---

### Business Rule: Effect slot model and the "0 means none" sentinel

**Overview**
This rule states that an effect slot with ID 0 carries no effect, that its value byte is meaningless, and that a zero slot is the only legitimate representation of "absent". It is the sentinel that every reader of `stEffect[]` depends on.

**Detailed description**
Every item in the game is a `STRUCT_ITEM` with exactly three effect slots (`Code/Basedef.h:398-412`). Each slot is a union of a `short sValue` and a byte pair `{ unsigned char cEffect; unsigned char cValue; }`. The effect ID therefore occupies the first byte of the slot and the payload the second. There is no separate "occupied" flag, no count, and no ordering guarantee: a slot is occupied if and only if `cEffect != 0`. Because of this, ID 0 can never be allocated to a real effect. The original authors documented this explicitly on line 1 of the header, and then commented the definition out — the constant `EF_NONE` no longer exists, so the rule is enforced only by convention and by the literal `0` appearing in roughly twenty places.

The sentinel is load-bearing in three distinct ways. First, writers use it to find a free slot: `BASE_SetItemAmount()` (`Code/Basedef.cpp:2317-2336`) looks for the first slot whose `cEffect == 0` and claims it by writing `EF_AMOUNT` plus the count. Second, readers use it to stop scanning: for example `Code/TMSrv/_MSG_PutoutSeal.cpp:74` and `Code/TMSrv/_MSG_UseItem.cpp:5631` test `stEffect[0].cEffect == 0` as a precondition. Third, display code treats a zero slot as a terminator, which is why the client contract can serialise an item as "index plus up to three pairs" without a length prefix.

The consequence for the dictionary is that value 0 is permanently reserved and cannot be reused for a real effect, yet it is not declared anywhere in the file. Any automated regeneration of the header from a schema would need to preserve this reservation, or the entire content corpus would be misread. It also means `EF_NONE` cannot be reintroduced with a different value — the two facts (0 = empty, 1 = `EF_LEVEL`) are coupled.

**Rule workflow**
```
item slot vector stEffect[0..2]
  -> for each slot: is cEffect == 0 ?
       yes -> slot is free; payload byte ignored; may be claimed by a writer
       no  -> slot holds effect cEffect with value cValue
  -> writers claim the first free slot (BASE_SetItemAmount)
  -> readers skip/stop at free slots and never interpret cValue of a free slot
```

---

### Business Rule: Effect ID address space is 0..127

**Overview**
The legal effect-ID range is 0 through 127 inclusive, set by `MAX_EFFECTINDEX` (128). Storage permits 0..255, but nothing above 127 has a name, a table row, or a documented meaning.

**Detailed description**
`MAX_EFFECTINDEX` is defined as 128 in `Code/Basedef.h:142`. The same constant sizes the name table (`char EffectNameTable[MAX_EFFECTINDEX][24]`, `Code/Basedef.h:2462`) and bounds the loader loop (`Code/Basedef.cpp:5128`, `5222`). However, the field itself is `unsigned char` (`Code/Basedef.h:407`), giving 256 possible values, and `STRUCT_ITEMLIST.stEffect[].sEffect` is a `short` (`Code/Basedef.h:896`), giving 65,536. Three different widths are therefore in play for the same concept, and only the narrowest — the name table — actually defines the namespace.

This mismatch is not theoretical. The highest declared IDs are `EF_NOSANC` = 126 and `EF_NOTRADE` = 127, which sit exactly at the ceiling, leaving no headroom. Any new effect added by a content author with value 128 or higher is silently unnameable in `itemlist.csv`: `atoi` accepts it, the loader's `val > MAX_EFFECTINDEX` check passes it through (128 is not greater than 128), and `memcpy(EffectNameTable[val], ...)` then writes one row past the end of a 128-row array. The name is destroyed or corrupts the adjacent global, and the effect ID stored in the item cannot round-trip through the CSV. This is the single most consequential coupling in the component, because it links a content-authoring convention directly to a fixed-size global buffer.

The practical consequence is that the item design space is full. `EF_NOTRADE` = 127 uses the last legal slot, and the undeclared attribute range 116..125 already occupies ten slots that no `EF_*` name claims. Any extension of the item system must either reclaim those ten undocumented IDs by naming them, or widen `MAX_EFFECTINDEX` — which in turn changes the size of `EffectNameTable` and, because `STRUCT_ITEMLIST` is serialised to `extraitem.bin` / `ItemList.bin`, is a data-format migration.

**Rule workflow**
```
#define value V in ItemEffect.h
  -> compile time: V substituted as integer literal, no range check at all
  -> runtime load: atoi(str3) = V
       V < 0 or V > 128  -> reject (dialog)   [note: 128 is NOT rejected, see defects]
       table[V][0] != 0  -> reject (duplicate dialog)
       else              -> table[V] = "EF_name"
  -> item CSV row references EF_name
       name found in table[0..127] -> sEffect = index (0..127)
       name not found              -> defect path, sEffect = 128 or stale value
```

---

### Business Rule: Two-tier effect storage (12 static slots vs 3 dynamic slots)

**Overview**
Content templates and live item instances use different slot budgets for the same effect ID space: 12 per item type in the loaded item list, 3 per concrete item in the world.

**Detailed description**
`STRUCT_ITEMLIST.stEffect` is an array of 12 `{ short sEffect; short sValue; }` pairs (`Code/Basedef.h:894-899`, sized by `MAX_STATICEFFECT` = 12 at `Code/Basedef.h:144`). This is the *template*: it is parsed once from `itemlist.csv`, cached in binary form, and conceptually immutable during play. `STRUCT_ITEM.stEffect` is an array of 3 pairs (`Code/Basedef.h:398-412`), and this is the *instance*: it travels with the item through inventories, trades, drops and the database.

The asymmetry has a precise purpose, visible in the resolvers. `BASE_GetStaticItemAbility()` (`Code/Basedef.cpp:1844-1991`) iterates all 12 template slots to sum intrinsic bonuses of the item type, then iterates the 3 instance slots to add per-instance bonuses, and finally applies the refinement multiplier. `BASE_GetBonusItemAbility()` (`1993-2050`) does the mirror image: it reads only the 3 instance slots plus the template's `EF_RESISTALL` fan-out, and returns 0 early when the item has no refinement. In other words, the 12-slot template is the item's "printed stats" and the 3-slot instance is its "rolled/refined stats".

There is a hard structural limit hiding here: with only three instance slots, at most three per-instance effects can coexist on one item. The code defends this implicitly rather than explicitly. If an item already has three occupied instance slots, `BASE_SetItemAmount()` (`Code/Basedef.cpp:2317-2336`) falls through all branches and silently does nothing — the count is lost, and the item behaves as if it had no `EF_AMOUNT`. Similarly, the three duplicated blocks in `_MSG_UseItem.cpp:240-261`, `400-421`, `813-834` and `3701-3717` are all hand-written three-way cascades implementing "find a free slot or report `_NN_Cant_Refine_More`". The rule "3 dynamic slots" is therefore enforced by copy-paste, not by a helper.

**Rule workflow**
```
itemlist.csv row
  -> up to 12 (name,value) pairs resolved via EffectNameTable -> STRUCT_ITEMLIST.stEffect[0..11]
live instance
  -> up to 3 (effect,value) pairs in STRUCT_ITEM.stEffect[0..2]
resolver reads:
  BASE_GetStaticItemAbility -> sum(stEffect[0..11]) + sum(instance[0..2]) * sancMultiplier
  BASE_GetItemAbility       -> sum(stEffect[0..11]) + sum(instance[0..2]) * sancMultiplier (guarded)
  BASE_GetBonusItemAbility  -> sum(instance[0..2]) * sancMultiplier, 0 if sanc == 0
```

---

### Business Rule: Score namespace (IDs 1..16)

**Overview**
IDs 1 through 16 are pure additive character-score effects: they add their value directly to the owning character's derived statistics.

**Detailed description**
The header groups these under two comments: "Score 시작" (score start) at line 2 and "Score 끝" (score end) at line 17. `EF_LEVEL` = 1, `EF_DAMAGE` = 2, `EF_AC` = 3, `EF_HP` = 4, `EF_MP` = 5, `EF_EXP` = 6, `EF_STR` = 7, `EF_INT` = 8, `EF_DEX` = 9, `EF_CON` = 10, `EF_SPECIAL1..4` = 11..14, `EF_SCORE14` = 15, `EF_SCORE15` = 16. The header comment on `EF_LEVEL` states the design intent plainly: "Score[1] - Score[15] Bonus Score값을 그대로 더해 버린다" — the values are added verbatim, with no transformation.

The resolvers honour this literally. `BASE_GetMobAbility()` composes the final scoreboard by calling `BASE_GetItemAbility(&item, EF_DAMAGE)` (`Code/Basedef.cpp:784`), `BASE_GetMobAbility(&MOB, EF_STR)` (`2992`), and equivalents for the other members. Each call sums all matching template slots plus all matching instance slots. `EF_LEVEL` is the notable special case: it is not a pure sum, because `BASE_GetItemAbility()` short-circuits it at `Code/Basedef.cpp:1683-1687` to either read a mount's stored level or read `g_pItemList[idx].ReqLvl`, i.e. the effect ID is used as an *accessor enum* for the item's own level rather than as a stored attribute.

Two entries are dead weight. `EF_SCORE14` (15) and `EF_SCORE15` (16) have no references anywhere in `Code/`, and `EF_EXP` (6) likewise has zero references. They occupy namespace slots that could otherwise be reclaimed. `EF_SPECIAL1..4` (11..14) are referenced exactly once each — inside the scoreboard composition — and are the "무마/특1/특2/특3" (mastery/special 1-3) stats that feed the client's extra stat panel.

**Rule workflow**
```
character scoreboard recomputation
  -> for each equipped/carried item
       BASE_GetItemAbility(item, EF_DAMAGE) -> sum of all matching slots
       BASE_GetItemAbility(item, EF_AC)     -> sum
       ...
       BASE_GetItemAbility(item, EF_LEVEL)  -> accessor, not a sum
  -> additive result is added to MOB.CurrentScore.<field>
  -> refinement multiplier has already been applied inside the resolver
```

---

### Business Rule: Equip requirement namespace (IDs 17..25)

**Overview**
IDs 17..20 and 22..25 are *not* bonuses: they are equipment requirements and slot-geometry descriptors. They are deliberately excluded from refinement multiplication.

**Detailed description**
`EF_POS` = 17 carries the equip-slot bitmask; `EF_CLASS` = 18 carries the class/gender bitmask; `EF_R1SIDC` = 19 and `EF_R2SIDC` = 20 carry the STR/INT/DEX/CON limits for one-hand and two-hand items respectively; `EF_REQ_STR/INT/DEX/CON` = 22..25 carry the explicit requirement values. The header's comments state their defaults (`EF_POS`: "디폴트 NOWHERE (0비트 가 EQUIP 0)"; `EF_CLASS`: "디폴트 YES"; `EF_R1SIDC`: "디폴트 0"; `EF_R2SIDC`: "디폴트 불가") and note at line 61 that requirements are "추가되는 값" — additive on top of the base SIDC.

The most important structural fact is that 17..20 and 22..25 are hard-coded into the refinement exemption list. `BASE_GetItemAbility()` at `Code/Basedef.cpp:1813` explicitly excludes `EF_GRID`, `EF_CLASS`, `EF_POS`, `EF_WTYPE`, `EF_RANGE`, `EF_LEVEL`, `EF_REQ_STR`, `EF_REQ_INT`, `EF_REQ_DEX`, `EF_REQ_CON`, `EF_VOLATILE`, `EF_INCUBATE`, `EF_INCUDELAY`, `EF_MOBTYPE`, `EF_ITEMTYPE`, `EF_ITEMLEVEL`, `EF_NOTRADE`, `EF_NOSANC` and `EF_DONATE` from the `value * (sanc + 10) / 10` multiplier. A level-15 item must not require 2.5x the STR, must not occupy 2.5x the grid cells, and must not be 2.5x less tradeable. The exemption list is the machine-readable statement of which IDs are *descriptors* rather than *quantities*.

This yields an implicit taxonomy that any new effect must respect: add a quantitative effect and it is automatically scaled by refinement; add a categorical effect and you must edit the exemption list in three separate places (`Code/Basedef.cpp:1813`, `1967`, and `2043`). Forgetting one of the three produces an effect that scales when bonus-applied but not when statically applied — a divergence that is extremely hard to detect without tests.

**Rule workflow**
```
BASE_GetItemAbility(item, Type)
  -> Type in {GRID, CLASS, POS, WTYPE, RANGE, LEVEL, REQ_*, VOLATILE, INCUBATE, INCUDELAY,
              MOBTYPE, ITEMTYPE, ITEMLEVEL, NOTRADE, NOSANC, DONATE}
       -> return raw summed value (no refinement scaling)
  -> otherwise
       -> value = value * (sanc + 10) / 10
```

---

### Business Rule: Refinement multiplier and the sanc>=9 armour bonus

**Overview**
For every quantitative effect, the resolved value is scaled by refinement: `value = value * (sanc + 10) / 10`. Armour at refinement 9 or above gains one further effective level.

**Detailed description**
The formula appears three times — `Code/Basedef.cpp:1815-1816` (`BASE_GetItemAbility`), `1970-1971` (`BASE_GetStaticItemAbility`), `2045-2046` (`BASE_GetBonusItemAbility`) — always as integer arithmetic in the order multiply-then-divide, which truncates rather than rounds. At sanc 0 the factor is 10/10 = 1, so unrefined items are unchanged; at sanc 9 the factor is 19/10 = 1.9; at sanc 15 it is 25/10 = 2.5. Because the division follows the multiplication, a base value of 1 at sanc 15 yields 2 (25/10 truncated), not 3.

The refinement value `sanc` itself is not a stored field: it is *derived from the effect slots*. `BASE_GetItemSanc()` (`Code/Basedef.cpp:2095-2141`) scans the three instance slots for `EF_SANC` or for any ID in 116..125, and decodes the payload. `BASE_GetItemSanc()` early-returns `FALSE` for mounts (`sIndex` in 2330..2389), so mounts never receive refinement scaling — their bonuses come from `g_pMountBonus` lookup tables instead. `BASE_GetBonusItemAbility()` adds a further early return at `2040-2041`: if `sanc == 0`, the bonus is returned unscaled and without consulting the template at all. That means an unrefined item provides only its instance-level bonuses through the "bonus" path and its template bonuses through the "static" path — a subtle split that callers must know about.

The armour bonus at `Code/Basedef.cpp:1810-1811` (`if (sanc >= 9 && (nPos & 0xF00) != 0) sanc++`) applies only when the item's position mask has bits in the 0x100..0xF00 range, i.e. it is worn rather than carried. Combined with the `>= 3 → 2` run-speed clamp at `1819-1826`, this creates a known corner: a run-speed item at sanc 9 is first clamped to 2 and then incremented to 3 by the `sanc == 9` clause, so the clamp is effectively bypassed for exactly that one refinement value.

**Rule workflow**
```
sanc = BASE_GetItemSanc(item)
  -> mounts (sIndex 2330..2389): return FALSE, no scaling
  -> scan instance slots 0..2 for EF_SANC or ID in 116..125
  -> decode payload (see BR: EF_SANC packing)
if sanc >= 9 and (nPos & 0xF00) != 0: sanc += 1
if Type not in exemption list:
   value = value * (sanc + 10) / 10
```

---

### Business Rule: EF_SANC payload packing and the 230..253 encoding space

**Overview**
`EF_SANC` (43) stores a *packed* refinement descriptor in its value byte: level and success-attempt count share one byte, with 230..253 forming a second encoding space for levels 10..15.

**Detailed description**
The write side, `BASE_SetItemSanc()` (`Code/Basedef.cpp:2239-2285`), clamps `sanc` to 0..15 (resetting to 0 if greater than 15) and `success` to 0..20, then computes `sc = sanc + 10 * success` for levels 0..9. For levels 10..15 it instead uses `sc = 230 + success` for level 10, `234 + success` for 11, `238 + success` for 12, `242 + success` for 13, `246 + success` for 14 and `250 + success` for 15. The read side, `BASE_GetItemSanc()` (`2095-2141`), inverts this: 230..233 maps to `REF_10` (10), 234..237 to `REF_11` (12), 238..241 to `REF_12` (15), 242..245 to `REF_13` (18), 246..249 to `REF_14` (22), 250..253 to `REF_15` (27); everything else is `sanc % 10`.

Note carefully that the *decoded* refinement level and the *reference-table* constant diverge above 9: `REF_10` is 10 but `REF_11` is 12, `REF_12` is 15, `REF_13` is 18, `REF_14` is 22 and `REF_15` is 27 (`Code/Basedef.h:196-201`). Those constants are success-rate tiers, not levels. Two further accessors read the same byte with different decoders: `BASE_GetItemGem()` (`2143-2175`) returns `(value - 230) % 4` and `-1` below 230, i.e. the low two bits of the success counter indicate which of four gem sockets was used; `BASE_GetItemSancSuccess()` (`2177-2200`) returns `value / 10`, the attempt count.

The most consequential interaction is with the undeclared 116..125 range. `BASE_GetItemSanc()` prefers 116..125 over `EF_SANC` in its if/else chain (`2102-2119`), which means a refinement performed with an attribute stone (see the 116..125 rule) stores its payload in the same byte and is indistinguishable from normal refinement to every caller. `BASE_SetItemSanc()` likewise writes to whichever slot already holds `EF_SANC` *or* an ID in 116..125 (`2270-2282`). This is a deliberate unification: attribute stones and normal refinement are the same numeric quantity with a different effect ID label.

**Rule workflow**
```
write: BASE_SetItemSanc(dest, level, success)
  level clamped 0..15, success clamped 0..20
  level 0..9  -> sc = level + 10*success
  level 10..15-> sc = 230 + 4*(level-10) + success
  store into the slot holding EF_SANC or 116..125
read:  BASE_GetItemSanc(item)
  116..125 -> payload is the packed byte
  EF_SANC  -> payload is the packed byte
  decode: 230..253 -> REF_10..REF_15 ; else payload % 10
```

---

### Business Rule: Refinement attribute sub-namespace 116..125 (undeclared)

**Overview**
IDs 116..125 are a ten-value sub-namespace written as `116 + color`, where `color` is derived from the refinement stone's item index. They have no `EF_*` name and are absent from the header entirely.

**Detailed description**
In `Code/TMSrv/_MSG_UseItem.cpp:3675` the handler computes `int color = item->sIndex - 3407;`, so the stone family is item indices 3407..3417. `color == 10` (index 3417) is the *remover* path: it resets the target slot to plain `EF_SANC` (43) with value 43's semantics (`3691-3697`). Every other colour writes `dest->stEffect[N].cEffect = 116 + color` into the first available slot (`3711-3717`). With colour 0..9 that produces exactly 116..125.

Every downstream consumer treats 116..125 as "has refinement, with an attribute". `BASE_GetItemSanc()` (`2102-2109`), `BASE_GetItemGem()` (`2150-2157`), `BASE_GetItemSancSuccess()` (`2184-2194`), `BASE_SetItemSanc()` (`2270-2282`), `BASE_VisualItemCode()` (`6628-6635`) and `BASE_VisualAnctCode()` (`6710-6715`) all special-case the range. Visual mapping is particularly instructive: `BASE_VisualItemCode()` returns `sIndex | (12 * 0x1000)` for any item carrying 116..125, i.e. the client is told "render this with visual tier 12", and `BASE_VisualAnctCode()` returns `cEffect - 3` — so an attribute refinement of colour 0 (ID 116) maps to the same visual code as `EF_SANC` at low refinement, and colour 9 (ID 125) maps to 122.

Because the range is unnameable, it cannot appear in `itemlist.csv` and cannot be validated by the loader. It is also invisible to the duplicate-detection logic, so a future `EF_*` definition at 116..125 would be accepted by the loader and then silently overwritten at runtime by `116 + color`. The ten IDs are, functionally, already spent.

**Rule workflow**
```
refinement stone used on item (Vol 4 / Vol 5)
  -> color = stone.sIndex - 3407
  -> color == 10 (remover):
        find slot holding 116..125 or EF_SANC; reset it to EF_SANC
  -> color 0..9:
        find first slot that is free OR holds EF_SANC OR holds 116..125
        write cEffect = 116 + color
  -> BASE_GetItemSanc / GetItemGem / VisualAnctCode interpret the range
```

---

### Business Rule: EF_VOLATILE is a use-script opcode, not a volatile flag

**Overview**
`EF_VOLATILE` (38) is documented as a small enum (0..7) describing consumable behaviour. In practice it is the **dispatch selector for the entire use-item script**, with 63 distinct values observed between 1 and 243.

**Detailed description**
The header comment at line 77 reads: "소모 먹으면 바로 사라지는 아이템. 0:일반장구 1:물약계통 2:돈계통 3:열쇠 4:축1 5:축2 6:축3" with a continuation line "6:스펠+15 7:레벨+1" — zero means ordinary gear, 1 potion, 2 money, 3 key, 4/5/6 scroll tiers, 7 level+1. The handler `Code/TMSrv/_MSG_UseItem.cpp:94` reads `int Vol = BASE_GetItemAbility(item, EF_VOLATILE);` and then branches: `Vol == 1` is the HP/MP potion path (`98-146`), `Vol == 4 || Vol == 5` is the refine-stone path (`150`), `Vol == 6` is commented "Pilula Orc" (`978`), `Vol == 7` "Poeira de Fada" (`999`), and thereafter a long sequence of `if (Vol == N)` blocks up to `Vol == 243` (`4228`). A full enumeration of the observed values is: 1, 4-17, 19, 20, 29, 52, 53, 55-58, 63-66, 93, 94, 139, 169, 173-175, 180-188, 190, 191, 194, 195, 198, 200-203, 206, 211, 212, 230, 235-238, 242, 243.

This is a semantic overload of the same 8-bit effect namespace for a completely different purpose. Effect IDs 1..127 name *item attributes*; the `EF_VOLATILE` **payload** selects a *server-side script branch*. The two encodings are unrelated: `Vol == 230` (`3612`) has nothing to do with `EF_SANC` payload 230, and `Vol == 63` (`2200`) has nothing to do with `EF_GRID` = 33 or with `EF_LWORDINDEX` = 63. Only the framing is shared: an item attribute slot with ID 38 and an unsigned byte payload.

The consequence is severe for the dictionary's role. Because the value is dispatched on integers rather than named constants, the header cannot document the actual contract, and the loader cannot validate it. There is no `EF_USE_*` enumeration, no `enum`, no bounds check. A content author who sets `EF_VOLATILE` to an unused value (say 240) gets an item that does nothing at all, with no diagnostic; a value that collides with an implemented branch silently receives that branch's behaviour. Additionally `EF_VOLATILE` appears in the refinement exemption list at `Code/Basedef.cpp:1813`, `2043` and `1967` but **not** in `BASE_GetItemAbility`'s earlier guard ordering, which is why it is explicitly listed — a volatile item must never be scaled by refinement, or a +15 potion would heal 2.5x.

**Rule workflow**
```
player uses item
  -> Vol = BASE_GetItemAbility(item, EF_VOLATILE)   (raw, unscaled)
  -> if Vol == 0 : not a consumable use-script; falls through
  -> if Vol == 1 : HP/MP potion (respects pUser.PotionTime / PotionDelay)
  -> if Vol == 4 or 5 : refinement stone targeting another item
  -> if Vol == N (N in observed set) : specialised handler block
  -> consume source item unless amount > 1
```

---

### Business Rule: EF_AMOUNT stack-count ownership

**Overview**
`EF_AMOUNT` (61) is the item's stack counter. The count lives in the *instance* effect slots, not in a dedicated field, and the first write claims a free slot.

**Detailed description**
`BASE_GetItemAmount()` (`Code/Basedef.cpp:2287-2302`) scans the three instance slots for `EF_AMOUNT` and returns the payload of the first match, or 0 if none. It uses a strict three-way cascade: slot 0, else slot 1, else slot 2. `BASE_SetItemAmount()` (`2304-2338`) first tries to overwrite an existing `EF_AMOUNT` slot; only if none exists does it scan for the first `cEffect == 0` slot and write `EF_AMOUNT` plus the value there. If all three slots are occupied by other effects, the function returns silently without writing.

This produces three observable behaviours. First, a stacked item consumes one of only three instance slots forever, which is why stackable consumables must not also need instance effects. Second, the amount is a single byte, so a stack is capped at 255 even though the accessor returns `int`. Third, the item-list template may also declare `EF_AMOUNT` in its 12 static slots, and `BASE_GetItemAbility(item, EF_AMOUNT)` would then return template + instance values summed — a subtle double-count that callers avoid by using `BASE_GetItemAmount()` instead.

`EF_AMOUNT` participates in several high-traffic flows. `Code/TMSrv/_MSG_UseItem.cpp:139-143` decrements it after a potion, `memset`ting the whole item when the count reaches 1. `Code/TMSrv/Server.cpp:8521,8542,8544` reads it to count quest collectibles, and even inspects `cValue > 10` to gate a reward. `Code/TMSrv/_MSG_Quest.cpp:565,568` treats a slot-0 `EF_AMOUNT` as the quantity when accumulating quest items. Because the count is stored as an effect, it is included in every serialisation of `STRUCT_ITEM` and therefore must remain byte-compatible with the client.

**Rule workflow**
```
read:  scan slots 0,1,2 for cEffect == EF_AMOUNT -> return first payload
write: if any slot has EF_AMOUNT -> overwrite its payload
       else if slot 0 free -> claim it
       else if slot 1 free -> claim it
       else if slot 2 free -> claim it
       else -> silent no-op
```

---

### Business Rule: EF_KEYID / EF_QUEST door and gate matching

**Overview**
`EF_KEYID` (39) is the numeric identifier a key item presents to a door or gate; `EF_QUEST` (58) is the quest-scoped variant that must also be consumed.

**Detailed description**
Three sites implement the same handshake. `Code/TMSrv/Server.cpp:6092` reads `BASE_GetItemAbility(&pItem[DoorId].ITEM, EF_KEYID)` from the door object; `7178` reads it from a candidate key; `7497` reads it from a gate. `Code/TMSrv/CCastleZakum.cpp:77-89` does the same for castle gates and additionally reads `EF_QUEST` from carried items to drive quest progression. `Code/TMSrv/_MSG_UpdateItem.cpp:57,71,76` repeats the pattern. The match is a plain integer equality: the key's `EF_KEYID` value must equal the door's.

`EF_QUEST` is documented at line 98 as "임무 반드시 1부터 시작, 대부분 Volatile과 함께 세팅될 것 임" — quest IDs always start at 1, and are usually set together with `EF_VOLATILE`. That co-setting is what makes a quest item both consumable and identified: `EF_VOLATILE` says "using this runs script N", `EF_QUEST` says "this is quest item N". `CCastleZakum.cpp:301-305` demonstrates the write side, copying `EF_KEYID` from a key into a new item and setting `EF_QUEST` on it.

Both IDs are excluded from refinement scaling, which is essential: a +15 key must still open the door whose ID matches, and a +15 quest item must still be quest item N. Note that neither is validated against any range: a key ID and a door ID are just bytes, so collisions across content are resolved only by authoring discipline.

**Rule workflow**
```
door interaction
  -> doorKey = BASE_GetItemAbility(door, EF_KEYID)
  -> for each carried item: playerKey = BASE_GetItemAbility(item, EF_KEYID)
       doorKey == playerKey -> open / advance
  -> quest gate: quest = BASE_GetItemAbility(item, EF_QUEST); quest != 0 required
```

---

### Business Rule: Mount statistics namespace (IDs 78..83) and index-range dispatch

**Overview**
IDs 78..83 do not describe the rider: they describe a mount entity whose entire state is packed into the mount item's own effect slots. `EF_MOUNTLIFE` = 79, `EF_MOUNTHP` = 80, `EF_MOUNTSANC` = 81, `EF_MOUNTFEED` = 82, `EF_MOUNTKILL` = 83.

**Detailed description**
The resolvers do not read mount statistics from the item type; they read them from fixed *positions* inside the instance slots, gated by the item index. `BASE_GetItemAbility()` at `Code/Basedef.cpp:1720-1756` and `BASE_GetStaticItemAbility()` at `1905-1941` both begin with `if(idx >= 2330 && idx < 2390)` and then return `stEffect[0].sValue` for `EF_MOUNTHP`, `stEffect[1].cEffect` for `EF_MOUNTSANC`, `stEffect[1].cValue` for `EF_MOUNTLIFE`, `stEffect[2].cEffect` for `EF_MOUNTFEED` and `stEffect[2].cValue` for `EF_MOUNTKILL`. Note the asymmetry: the two *payload halves* of a single slot are exposed under two different effect IDs, so `EF_MOUNTLIFE` and `EF_MOUNTSANC` share `stEffect[1]`.

Above the mount's own stats, the resolved combat bonuses come from lookup tables. For indices 2362..2389 with a positive `stEffect[0].sValue`, the function computes `lv = stEffect[1].cEffect` and `cd = sIndex - 2360`, then returns `(lv + 20) * g_pMountBonus[cd][0] / 100` for `EF_DAMAGE`, `(lv + 15) * g_pMountBonus[cd][1] / 100` for `EF_MAGIC`, and raw table values for `EF_PARRY` and the resists. A second range, 3980..3994, uses `g_pMountTempBonus[cd]` the same way. This makes mount bonuses entirely index-driven: the effect IDs are only *selectors* into a table indexed by item number, and the mount's "level" is whatever byte happens to be in `stEffect[1].cEffect`.

The consequence is a hard coupling between the dictionary and item numbering. `BASE_GetItemSanc()` early-returns `FALSE` for 2330..2389, so mounts can never be refined; `BASE_GetBonusItemAbility()` and `BASE_GetBonusItemAbilityNosanc()` return `FALSE` immediately for the same range (`1995-1996`, `2054-2055`), so mounts never contribute instance bonuses. A future mount outside those index ranges would silently be treated as ordinary equipment.

**Rule workflow**
```
resolve effect Type for item idx
  -> idx in 2330..2389 ?
        MOUNTHP / MOUNTSANC / MOUNTLIFE / MOUNTFEED / MOUNTKILL -> positional byte access
        idx >= 2362 and stEffect[0].sValue > 0:
            DAMAGE / MAGIC / PARRY / RESIST* -> g_pMountBonus[idx-2360][*]
  -> idx in 3980..3994 ?
        DAMAGE / MAGIC / PARRY / RESIST* -> g_pMountTempBonus[idx-3980][*]
  -> otherwise: normal item handling
```

---

### Business Rule: Item grade is a contiguous ID range (100..105)

**Overview**
`EF_GRADE0`..`EF_GRADE5` (100..105) are not independent flags: they form a contiguous range whose *offset from 100* is the item's grade index.

**Detailed description**
The only consumer, `Code/TMSrv/Server.cpp:1406-1408`, is explicit:

```
if (Dest->stEffect[0].cEffect >= EF_GRADE0 && Dest->stEffect[0].cEffect <= EF_GRADE5)
    add1 = Dest->stEffect[0].cEffect - EF_GRADE0;
```

Grade is therefore encoded as the ID itself, not in the payload. The comments label them A (100), B (101), C (102), D (103), E (104) and "E Arch" (105) — six grades. This is the only place in the dictionary where an arithmetic relationship between adjacent IDs is part of the contract. Any insertion in the middle of the range would change every grade above it; any gap would produce a value that passes the range test but maps to an undefined tier.

The range is also an example of declared-but-unused namespace: `EF_GRADE1` through `EF_GRADE4` (101..104) have **zero** references in `Code/`; only `EF_GRADE0` (used once in `_MSG_Quest.cpp:33` as an NPC grade lookup) and `EF_GRADE5` (used in the range test above) are named. The intermediate values exist purely so that the arithmetic `cEffect - EF_GRADE0` yields 1..4.

`EF_GRADE0` is also read through `BASE_GetItemAbilityNosanc()` in `Code/TMSrv/_MSG_Quest.cpp:33` to determine an NPC's grade from its face item — the only "Nosanc" read in the codebase, and a reminder that grade must never be refinement-scaled. The `EF_GRADE` range is not in the refinement exemption list, but it is also never passed to a scaling resolver, so the omission is latent rather than active.

**Rule workflow**
```
grade assignment: set stEffect[0].cEffect = EF_GRADE0 + gradeIndex
grade read:       if 100 <= cEffect <= 105 -> grade = cEffect - 100
                  else -> no grade
```

---

### Business Rule: Expiry date stamps (IDs 106..110)

**Overview**
`EF_WDAY` = 106, `EF_HOUR` = 107, `EF_MIN` = 108, `EF_YEAR` = 109, `EF_WMONTH` = 110 encode an expiry timestamp directly in the three instance slots.

**Detailed description**
`BASE_SetItemDate()` (`Code/Basedef.cpp:6732-6761`) is the only writer. It computes `daynext`, `month` and `year` from `localtime`, applies three hand-rolled rollover corrections (`if(daynext >= 30) daynext -= 29;`, `if(month == 1 && daynext >= 27) daynext -= 26;`, `if(month >= 12) month = 0;`), then writes `EF_WDAY` with the day into slot 0, `EF_WMONTH` with `month + 1` into slot 1, and `EF_YEAR` with `year - 100` into slot 2. `BASE_CheckItemDate()` (`6763-6774`) compares those three bytes against the current clock with a three-clause OR.

Only three of the five declared IDs are actually used. `EF_HOUR` (107) and `EF_MIN` (108) have zero references anywhere, so expiry granularity is day-level despite the namespace advertising hour and minute. The date encoding also consumes all three instance slots, which means a dated item cannot carry refinement or a stack amount — the three-slot budget is exhausted. Both date effects are absent from the refinement exemption list, but they are written directly rather than through a resolver, so they escape scaling by luck rather than design.

**Rule workflow**
```
BASE_SetItemDate(item, day)
  slot0 = (EF_WDAY, day)      slot1 = (EF_WMONTH, month)     slot2 = (EF_YEAR, year-100)
BASE_CheckItemDate(item)
  now.day >= slot0.value AND now.mon >= slot1.value-1 AND now.year-100 >= slot2.value -> expired
  OR now.mon > slot1.value AND now.year-100 >= slot2.value -> expired
```

---

### Business Rule: Target classification (IDs 112/113)

**Overview**
`EF_MOBTYPE` (112) classifies which monster family an item targets; `EF_ITEMTYPE` (113) classifies which item class it applies to. Both are gating predicates, not quantities.

**Detailed description**
`EF_MOBTYPE` is referenced 29 times, making it one of the most heavily consumed IDs. `Code/TMSrv/_MSG_UseItem.cpp:372` reads `int itemtype = BASE_GetItemAbility(dest, EF_MOBTYPE);` before refinement, `4286` and `4416` gate refinement with `sanc < REF_10 || BASE_GetItemAbility(dest, EF_MOBTYPE) != 1`, and `Code/TMSrv/_MSG_CombineItemOdin.cpp:73,270` gates combination on `== 3` and `== 1`. `Code/TMSrv/_MSG_CombineItemExtracao.cpp:42` requires `EF_MOBTYPE == 0` before extraction. The values in use are small integers (0, 1, 3 observed), but the vocabulary is undocumented in the header — line 149 defines the ID with no comment at all.

`EF_ITEMTYPE` (113) appears in `Code/TMSrv/GetFunc.cpp:190,196` with the comparisons `!= 4 || != 5` and at `406` with `!= MORTAL && != 0`, where `MORTAL` is a class-master constant reused as an item type. This is a second, unrelated classification axis sharing the same ID space, and it is compared against constants defined elsewhere in the codebase rather than in this header, which means the dictionary cannot describe it.

Both IDs appear in the refinement exemption list in `BASE_GetItemAbility()` (`1813`), so a tagged item's classification never drifts with refinement. `EF_ITEMTYPE` is *not* in the two `BASE_GetStaticItemAbility`/`BASE_GetBonusItemAbility` lists, an inconsistency that is currently harmless only because those functions are not called with `EF_ITEMTYPE`.

**Rule workflow**
```
combination / refinement / extraction handler
  -> tag = BASE_GetItemAbility(item, EF_MOBTYPE)     (0 = any, 1 / 3 = families)
  -> gated action if tag matches the recipe's requirement
  -> separately: itype = BASE_GetItemAbility(item, EF_ITEMTYPE) (0 / MORTAL / 4 / 5)
```

---

### Business Rule: Hard exclusion flags (IDs 126/127)

**Overview**
`EF_NOSANC` (126) forbids further refinement; `EF_NOTRADE` (127) forbids trading. They occupy the final two legal IDs of the namespace.

**Detailed description**
The refinement gate is implemented symmetrically in four places in `Code/TMSrv/_MSG_UseItem.cpp`: `185` and `362` read `int isSanc = BASE_GetItemAbility(dest, EF_NOSANC);` and bail with `_NN_Cant_Refine_More` when non-zero. The trade gate is implemented in `Code/TMSrv/_MSG_Trade.cpp:179` and `Code/TMSrv/_MSG_SendAutoTrade.cpp:84` as `if (m->Item[i].sIndex && BASE_GetItemAbility(&m->Item[i], EF_NOTRADE) != 0)`. Both are zero/non-zero predicates; the payload is irrelevant.

Their placement at 126 and 127 is architecturally significant rather than cosmetic. Because the name table has exactly 128 rows, these two definitions consume the last usable IDs. Any new attribute added above them collides with the table bound (see the loader defect in section 11), and the `116..125` attribute range immediately below them means IDs 114..125 plus 126..127 form a contiguous "modern extension" band that the header only half documents. `EF_NOSANC` = 126 also happens to sit one past the remover colour (10 → 116+10 = 126) in `_MSG_UseItem.cpp:3675`, which is consistent: the remover conceptually *is* the "no sanc" state, although the code writes literal `43` there rather than `EF_NOSANC`.

Both flags are correctly listed in the broadest refinement exemption list (`Code/Basedef.cpp:1813`), and both are absent from the two narrower lists, again a latent inconsistency only.

**Rule workflow**
```
refine attempt: if BASE_GetItemAbility(dest, EF_NOSANC) != 0 -> reject
trade attempt:  if any item has BASE_GetItemAbility(item, EF_NOTRADE) != 0 -> reject
```

---

### Business Rule: Dual representation of stackable/multiplicative bonuses

**Overview**
Six effects exist in matched pairs or triplets where a "base" ID and a "secondary" ID carry the same semantic: `EF_HPADD`/`EF_HPADD2` (45/69), `EF_MPADD`/`EF_MPADD2` (46/70), `EF_CRITICAL`/`EF_CRITICAL2` (42/71), `EF_ACADD`/`EF_ACADD2` (53/72), `EF_DAMAGE`/`EF_DAMAGE2` (2/73), plus `EF_DAMAGEADD` (67) and `EF_MAGICADD` (68).

**Detailed description**
The resolver silently promotes a base query to its secondary twin when the item carries the twin. `BASE_GetItemAbility()` at `Code/Basedef.cpp:1665-1681` contains three such promotions: `EF_CRITICAL` becomes `EF_CRITICAL2` if either instance slot holds `EF_CRITICAL2`; `EF_DAMAGE` becomes `EF_DAMAGE2` when `nPos == 32` and a twin is present; `EF_ACADD` becomes `EF_ACADD2` if a twin is present. The header explains the intent at lines 109-112: the `*2` variants are "자동 체킹" (auto-checked) on the unique option, and the client displays them as part of the base option rather than as a separate line, so "클라이언트 표기시는 이것을 먼저 던저서 이것이 나오면 베이스는 던지지 않는다" — the client asks for the `2` variant first and skips the base if it answers.

The pairing is not uniform, which is the risk. `EF_HPADD2`/`EF_MPADD2` have no promotion in `BASE_GetItemAbility()`; consumers must query them explicitly. `EF_DAMAGEADD` (67) and `EF_MAGICADD` (68) are not twins but are gated identically: `Code/Basedef.cpp:1659-1663` returns 0 unless the item's `nUnique` is in 41..50, so these two effects are *only* meaningful on a narrow band of unique items. Meanwhile `Code/TMSrv/ProcessDBMessage.cpp:412-419` and `664-693` rewrite `EF_DAMAGEADD` and `EF_DAMAGE2` down to `EF_DAMAGE` on load, i.e. the persistence layer normalises the variants for older data.

The net rule is that the "true" damage of an item is not `BASE_GetItemAbility(item, EF_DAMAGE)` alone unless the caller also adds `EF_DAMAGE2`, which `Code/Basedef.cpp:2412-2415` does explicitly for dual-wield equipment. Misreading this produces a weapon that reports half its damage on one code path.

**Rule workflow**
```
query EF_DAMAGE
  -> if nPos == 32 and instance slot 1 or 2 holds EF_DAMAGE2 -> query becomes EF_DAMAGE2
  -> sum matching template slots + matching instance slots
  -> apply refinement multiplier (EF_DAMAGE is not exempt)
query EF_CRITICAL  -> may become EF_CRITICAL2
query EF_ACADD     -> may become EF_ACADD2
```

---

### Business Rule: Two-hand position rule (nPos == 32)

**Overview**
An item whose position mask equals 32 is a two-handed weapon, and any `EF_DAMAGE` on it is automatically reinterpreted via `EF_DAMAGE2`.

**Detailed description**
The check appears twice, at `Code/Basedef.cpp:1671-1675` (`BASE_GetItemAbility`) and `1671-1674` in the mirrored static resolver (`1671` region), plus at `1513-1514` in a bonus-aggregation path. The rule is `if(Type == EF_DAMAGE && nPos == 32)` and then `if(stEffect[1].cEffect == EF_DAMAGE2 || stEffect[2].cEffect == EF_DAMAGE2) Type = EF_DAMAGE2;`. It is a strict equality on `nPos`, not a bit test, so it applies only when 32 is the entire mask.

The practical effect is that two-handed weapons report their damage through the `*2` channel, which is what makes the client display them on the primary damage line while still letting the server sum both channels for dual-wield comparison at `2412-2415`. `Code/TMSrv/Server.cpp:2463-2472` exploits the same convention when it decays over-powered weapons: it reads `stEffect[1]` and `stEffect[2]` for `EF_DAMAGE` and subtracts the excess above 30.

Because `nPos` lives in the item-list template (`STRUCT_ITEMLIST.nPos`, `Code/Basedef.h:903`) and not in the instance, this rule depends on content data rather than on the dictionary. A content edit that changes a weapon's `nPos` silently changes which damage channel it uses. The dictionary provides the two IDs but neither document nor enforce the mapping between position 32 and `EF_DAMAGE2`.

**Rule workflow**
```
nPos = g_pItemList[item->sIndex].nPos
if Type == EF_DAMAGE and nPos == 32:
    if instance slot 1 or 2 holds EF_DAMAGE2: Type = EF_DAMAGE2
sum slots matching the (possibly rewritten) Type, then scale by refinement
```

---

### Business Rule: Attack-speed sentinel value 1 means 10

**Overview**
For `EF_ATTSPEED` (26) only, a payload of exactly 1 is interpreted as 10.

**Detailed description**
The rule appears in all three resolvers — `Code/Basedef.cpp:1713-1714`, `1784-1785`, `2014-2015` and `2071-2072` — always as `if(Type == EF_ATTSPEED && tvalue == 1) tvalue = 10;` immediately before accumulating. It is a legacy encoding compatibility shim: content authored before attack speed used percentage points stored 1 to mean "one step" (10%), and the resolvers keep honouring it.

The rule is an exception to the general principle that effect payloads are literal. It means `EF_ATTSPEED` cannot express a genuine 1% bonus, and it means the sentinel is applied *before* the refinement multiplier, so an item with `EF_ATTSPEED` = 1 at sanc 15 yields `10 * 25 / 10 = 25`, not `1 * 25 / 10 = 2`. `EF_ATTSPEED` is not in the exemption list, so it is scaled.

None of the four sites is factored into a helper, so the sentinel is quadruplicated. A fifth reader added later would silently lack the shim.

**Rule workflow**
```
for each matching slot: tvalue = slot value
  if Type == EF_ATTSPEED and tvalue == 1 -> tvalue = 10
  value += tvalue
value = value * (sanc + 10) / 10
```

---

### Business Rule: Run-speed clamp

**Overview**
`EF_RUNSPEED` (29) is clamped so that its resolved value never exceeds 2, except that a value which is positive at refinement exactly 9 becomes 3.

**Detailed description**
The clamp is in `BASE_GetItemAbility()` (`Code/Basedef.cpp:1819-1826`) and `BASE_GetStaticItemAbility()` (`1974-1981`). The order is: `if(value >= 3) value = 2;` then `if(value > 0 && sanc == 9) value++;`. Because the second clause tests the already-clamped value, the only way to reach 3 is `value > 0 && sanc == 9` after clamping. Note the strict `sanc == 9`: refinement 10 and above (which decode to `REF_10` = 10) do *not* get the increment, so a run-speed bonus is paradoxically higher at sanc 9 than at sanc 15.

The clamp is a hard movement-speed guard against content that would otherwise allow players to outrun the server's movement validation. It is not listed in the refinement exemption list, so the scaling happens first and the clamp second — meaning the clamp is applied to a scaling-inflated value, which is the correct order for a speed cap.

`EF_RUNSPEED` is referenced only three times in the codebase, so the rule is narrow but absolute; there is no configuration switch.

**Rule workflow**
```
value = sum(slots) scaled by refinement
if value >= 3 -> value = 2
if value > 0 and sanc == 9 -> value += 1
```

---

### Business Rule: Resist-all fan-out

**Overview**
`EF_RESISTALL` (54) is not read directly by callers. Instead, any query for `EF_RESIST1..4` (49..52) additionally sums every `EF_RESISTALL` slot.

**Detailed description**
`BASE_GetItemAbility()` at `Code/Basedef.cpp:1790-1807`, `BASE_GetStaticItemAbility()` at `1890-1903`, `BASE_GetBonusItemAbility()` at `2020-2033` and `BASE_GetBonusItemAbilityNosanc()` at `2077-2090` all implement the same two-step fan-out: first sum template slots whose `sEffect == EF_RESISTALL`, then sum instance slots whose `cEffect == EF_RESISTALL`, adding both into the resist being queried. The four elemental resists (fire, ice, lightning, "번저"/poison) therefore each report `specific + all`.

This is the only effect in the dictionary with a *derived read*: reading ID 49 also reads ID 54. It has no symmetric behaviour — reading `EF_RESISTALL` directly returns only the literal `EF_RESISTALL` slots and nothing from the specific resists. Callers that want a total resist must not double-count by adding `EF_RESISTALL` themselves.

The fan-out is duplicated four times rather than factored, and the duplication is not perfectly consistent: `BASE_GetItemAbility` and `BASE_GetStaticItemAbility` both include the template loop, while `BASE_GetBonusItemAbility` and the Nosanc variant also include it. All four currently agree.

**Rule workflow**
```
query EF_RESISTn (n = 1..4)
  -> value = sum(slots with EF_RESISTn)              [template + instance]
  -> value += sum(template slots with EF_RESISTALL)
  -> value += sum(instance slots with EF_RESISTALL)
  -> scale by refinement (EF_RESISTn is not exempt)
```

---

### Business Rule: Grid size clamp

**Overview**
`EF_GRID` (33) is clamped to the range 0..7 after refinement scaling.

**Detailed description**
`BASE_GetItemAbility()` at `Code/Basedef.cpp:1835-1839` contains `if(Type == EF_GRID) { if(value < 0 || value > 7) value = 0; }`. `EF_GRID` is also in the refinement exemption list at `1813`, so the value is *not* scaled — the clamp therefore normally sees the raw authored value. The clamp is an inventory-grid safety net: the carry grid is byte-addressed and a value above 7 would overflow the packing used by the client's inventory layout.

The fact that out-of-range values collapse to 0 rather than to 7 means an invalid grid declaration produces an item that occupies no cells, which is a silent content error rather than a visible one. `EF_GRID` is referenced 8 times, all in inventory placement paths.

**Rule workflow**
```
value = sum(slots with EF_GRID)      [not refinement-scaled]
if value < 0 or value > 7 -> value = 0
```

---

### Business Rule: EF_DAMAGEADD / EF_MAGICADD unique gate

**Overview**
`EF_DAMAGEADD` (67) and `EF_MAGICADD` (68) are only honoured on items whose `nUnique` is between 41 and 50 inclusive.

**Detailed description**
`Code/Basedef.cpp:1659-1663`: `if(Type == EF_DAMAGEADD || Type == EF_MAGICADD) { if(nUnique < 41 || nUnique > 50) return 0; }`. The check is the very first thing `BASE_GetItemAbility()` does after the index bounds check, so it short-circuits before any slot scan or refinement scaling. `nUnique` comes from `g_pItemList[idx].nUnique` (`Code/Basedef.h:902`), parsed from the item-list CSV.

The rule exists because `EF_DAMAGEADD`/`EF_MAGICADD` are "additive percentage" effects that would be overpowered if available on generic items; restricting them to a ten-value unique band keeps them on endgame uniques. The band is hard-coded, not configured, and the same gate is *absent* from `BASE_GetStaticItemAbility()`, `BASE_GetBonusItemAbility()` and `BASE_GetBonusItemAbilityNosanc()` — a caller using those functions can read `EF_DAMAGEADD` from any item.

`EF_DAMAGEADD` is referenced 11 times and `EF_MAGICADD` 5 times, and `Code/TMSrv/ProcessDBMessage.cpp:412-419,664-693` normalises both to `EF_DAMAGE` for persisted legacy items, which means the gate can only be relied upon for items created by current code.

**Rule workflow**
```
query EF_DAMAGEADD or EF_MAGICADD
  -> nUnique = g_pItemList[idx].nUnique
  -> nUnique < 41 or nUnique > 50 -> return 0 immediately
  -> otherwise normal slot summation + refinement scaling
```

---

### Business Rule: Runtime name-table contract (header parsed as data)

**Overview**
The header must remain parseable by a naive line scanner: one `#define EF_name <decimal integer>` per line, no expressions, no continuation lines, no duplicate values, and values within 0..127.

**Detailed description**
`BASE_InitializeEffectName()` (`Code/Basedef.cpp:5163-5242`) clears the table, opens the file, and for each line: skips lines starting with `\r`; reads the first whitespace-delimited token; if it is `#include`, re-invokes itself recursively after a `str2[1 + strlen(str2)] = 0` write; if it is not `#define`, skips the line; otherwise parses up to five whitespace-delimited tokens with `sscanf(temp, "%s %s %s %s %s", ...)`, converts the third token with `atoi`, range-checks it, duplicate-checks `EffectNameTable[val][0] != 0`, and finally `memcpy(EffectNameTable[val], str2, 24)`.

This parser imposes constraints the current file happens to satisfy but does not document. Comments are tolerated only because `sscanf` stops at the first token of the comment and `atoi` on the third token ignores trailing text — so `#define EF_DAMAGE                 2 // 증뎀` parses correctly. Continuation lines (`//` comments spanning lines 78, 109-110) are harmless because they start with `//` and fail the `#define` test. Blank lines are harmless. But an expression value (`#define EF_X (1<<3)`) would `atoi` to 0 and collide with the reserved sentinel; a hex value (`0x10`) would `atoi` to 0; a duplicate would be rejected with a modal dialog and the *second* definition silently dropped, leaving the item CSV resolving to whichever definition came first.

`memcpy(..., 24)` copies 24 bytes from `str2` regardless of the token's length, so bytes past the token's NUL are uninitialised stack residue copied into the table. It is benign today because every comparison stops at the NUL, but it means the table contents are not deterministic beyond the name length.

**Rule workflow**
```
clear table[0..127][0]
open ../../Common/ItemEffect.h else ../../TMSRV/Run/ItemEffect.h
for each line:
   first token == "#define" ?
      val = atoi(third token)
      val < 0 or val > 128 -> dialog, skip
      table[val] already named -> dialog, skip
      table[val] = second token (24 bytes)
   first token == "#include" ? -> recurse (clears table again)
   else skip
close
```

---

### Business Rule: EF_ITEMLEVEL progression accessor

**Overview**
`EF_ITEMLEVEL` (87) reports an item's intrinsic level, used for grading, extraction and combination gating.

**Detailed description**
`EF_ITEMLEVEL` is referenced 23 times across five files, making it one of the most consumed IDs, yet it is defined with no comment at all (line 133). Consumers include `Code/TMSrv/_MSG_UseItem.cpp:850` (`int ItemGrade = BASE_GetItemAbility(dest, EF_ITEMLEVEL);`), `4832` (an equality gate against `replation`), `5030` (`int extra = ...`), and `Code/TMSrv/_MSG_CombineItemExtracao.cpp:34` where it drives extraction eligibility. Writers set it directly, e.g. `_MSG_CombineItemExtracao.cpp:69` and `_MSG_CombineItemOdin.cpp:287` both write `stEffect[0].cEffect = EF_ITEMLEVEL`.

The ID is in the refinement exemption list (`Code/Basedef.cpp:1813`), which is essential — an item's level must not rise when it is refined, or level-gated content would open up through refinement. Note the value 87 sits in a gap: 86 is undefined and 88..90 are undefined, so `EF_ITEMLEVEL` is an isolated island between the `EF_INCUDELAY`/`EF_SUBGUILD` block (84/85) and `EF_DONATE` (91).

**Rule workflow**
```
read:  lvl = BASE_GetItemAbility(item, EF_ITEMLEVEL)   (raw, unscaled)
write: slot0 = (EF_ITEMLEVEL, value)
use:   compare against recipe requirement, or pass to grade logic
```

---

### Business Rule: EF_DONATE currency marker

**Overview**
`EF_DONATE` (91) marks an item as premium/donation currency and supplies its donate value.

**Detailed description**
Three consumers: `Code/TMSrv/_MSG_Buy.cpp:73` (`int Donate = BASE_GetItemAbility(ItemMob, EF_DONATE);`) to price a shop purchase, `Code/TMSrv/_MSG_UseItem.cpp:5710` (`int donate = BASE_GetItemAbility(item, EF_DONATE);`) to credit the account, and the exemption list at `Code/Basedef.cpp:1813`. The ID occupies the only defined slot between 91 and 100, so eight values (92..99) are unused on either side.

The value 98 — inside that unused band — is already in service for an unrelated purpose (see the class-master marker rule), which demonstrates how the gaps in this namespace get consumed by undeclared conventions. `EF_DONATE` itself is exempt from refinement scaling, which is mandatory: a +15 donation item must not credit 2.5x currency.

**Rule workflow**
```
buy:   price += BASE_GetItemAbility(shopItem, EF_DONATE)
use:   account.Donate += BASE_GetItemAbility(item, EF_DONATE)
both paths read the value raw (no sanc multiplier)
```

---

### Business Rule: Undeclared ID 98 as class-master marker

**Overview**
Effect ID 98 is written to a character's face item to encode class-master rank (0 = base, 3 = celestial), but no `EF_*` constant exists for it.

**Detailed description**
Four write sites agree on the convention: `Code/TMSrv/_MSG_UseItem.cpp:3045-3048` sets `Equip[0].stEffect[1].cEffect = 98` and `cValue = 3` together with `stEffect[2].cEffect = 106` (`EF_WDAY`) and `cValue = (unsigned char)Equip[0].sIndex`; `Code/TMSrv/ProcessDBMessage.cpp:708-726` sets the same pair with `cValue = 0` for `MORTAL` and `ARCH`, and `cValue = 3` for `CELESTIAL`, `SCELESTIAL` and `CELESTIALCS`. So 98 carries the class-master tier and 106 piggybacks the face index — the `EF_WDAY` ID is being reused as a *generic "store the face index here"* slot, which is a direct collision with its documented date meaning.

ID 98 falls in the undefined band 92..99. Because it has no name, it cannot appear in `itemlist.csv`, cannot be validated, and cannot be found by any search of the dictionary. It is also outside the refinement exemption list (`98` is not in the `1813` list), but since it is written directly to the slot rather than passed through a resolver, it escapes scaling by construction.

This is the clearest evidence that the dictionary is no longer the authoritative schema: consumers invented values outside it, and one of them reused `EF_WDAY` for a meaning that contradicts line 143.

**Rule workflow**
```
class-master state change
  -> Equip[0].stEffect[1] = (98, rank)      rank 0 = mortal/arch, 3 = celestial
  -> Equip[0].stEffect[2] = (EF_WDAY, faceIndex)
  -> readers reconstruct rank from slot 1 and face from slot 2
```

---

### Business Rule: Unused, reserved and undefined slots

**Overview**
The namespace contains 26 undefined holes and 22 declared-but-unreferenced tokens. Neither set is documented as reserved, so both are effectively ambiguous.

**Detailed description**
Undefined numeric slots (no `#define` at all): **0** (`EF_NONE` commented out at line 1), **28** (`EF_PRICE` commented out at line 67), **86**, **88**, **89**, **90**, **92-99**, **111**, **114-125**. Declared but never referenced in `Code/`: `EF_EXP` (6), `EF_SCORE14` (15), `EF_SCORE15` (16), `EF_R1SIDC` (19), `EF_R2SIDC` (20), `EF_SPELL` (30), `EF_DURATION` (31), `EF_PARM2` (32), `EF_CLAN` (35), `EF_HITRATE` (41), `EF_BONUS` (55), `EF_UNIQUE` (59), `EF_INIT1` (64), `EF_INIT2` (65), `EF_INIT3` (66), `EF_SUBGUILD` (85), `EF_GRADE1` (101), `EF_GRADE2` (102), `EF_GRADE3` (103), `EF_GRADE4` (104), `EF_HOUR` (107), `EF_MIN` (108).

`EF_CURKILL` (75), `EF_LTOTKILL` (76) and `EF_HTOTKILL` (77) are explicitly annotated `// not used` yet are each referenced once — a reference count of 1 that consists of a stale or commented path, so the annotation is more accurate than the counter. `EF_UNIQUE` (59) is unreferenced as a *token* but the literal 59 is written in nine places (`Server.cpp:1914,2079,2138,2300,2305,2310,2319,2324,2329`, `GetFunc.cpp:1773`, `MobKilled.cpp:1773`), which is the same effect under a different spelling.

The undefined band 114..125 is not truly free: 116..125 are the attribute refinement range. Only 111 and 114..115 are genuinely unclaimed. Because no reservation is documented, a future author cannot distinguish "unused" from "deliberately skipped".

**Rule workflow**
```
value V has no #define
  -> cannot be authored in itemlist.csv (no name to resolve)
  -> may still appear at runtime from literal writes (98, 116..125 observed)
  -> the loader never validates instance-slot IDs, only template names
```

---

### Business Rule: Name-table load ordering and failure tolerance

**Overview**
The effect dictionary must be loaded *before* the item list, and a load failure is non-fatal: the server continues with an empty name table.

**Detailed description**
`BASE_InitializeBaseDef()` (`Code/Basedef.cpp:2564-2581`) calls `BASE_InitializeSkill()`, then `BASE_InitializeEffectName()`, then `BASE_InitializeItemList()`, then `BASE_InitializeInitItem()`, `BASE_InitializeGuildName()` and `BASE_InitializeAttribute()`. The ordering is mandatory because `BASE_ReadItemListFile()` resolves every effect name through `EffectNameTable` (`5128-5132`), which is populated only by the preceding call.

Failure handling is inconsistent with that mandate. If the header cannot be opened, the loader shows `MessageBoxA(NULL, "There is no file", "ItemEffect", MB_OK)` and returns with the table zeroed (`5175-5180`). It does not abort initialisation, does not set an error flag, and returns `void`, so `BASE_InitializeBaseDef()` cannot detect the failure and proceeds to load the item list. Every effect name then fails `strcmp` against an empty table, and the defect at `5134` (comparing `j` instead of `v`) means those failures are *not* reported: the code proceeds to store `v == MAX_EFFECTINDEX` (128) into `sEffect`. The result is a content corpus where every item has effect ID 128 — silent, unbounded, and unrecoverable.

This rule is therefore less a rule than a hazard: the component's only integrity guarantee (the load-must-succeed precondition) is encoded in ordering, not in control flow.

**Rule workflow**
```
BASE_InitializeBaseDef
  -> InitializeSkill
  -> InitializeEffectName   [may fail silently, table empty]
  -> InitializeItemList     [resolves names against the table]
       table empty -> every name unresolved -> sEffect = 128 (see defect)
  -> ...
```

---

## 4. Component Structure

```
Code/
├── ItemEffect.h                     # THE COMPONENT (153 lines, EUC-KR, CRLF, no guard)
│   ├── lines 1-2    commented-out EF_NONE (0) + "Score start" banner
│   ├── lines 3-16   Score namespace            1..14   (EF_LEVEL .. EF_SPECIAL4)
│   ├── lines 17-19  "Score end" + EF_SCORE14/15 15..16
│   ├── lines 20-25  REQUIREMENT banner         17..20   (POS, CLASS, R1SIDC, R2SIDC)
│   ├── lines 26-55  EF_WTYPE (21) + 29-line inline weapon-class table (1-104)
│   ├── lines 56-63  EF_REQ_STR..CON (22..25) + "additive requirement" note
│   ├── lines 65-74  speed / range / spell / grid / clan / coin (26..37)
│   ├── lines 77-78  EF_VOLATILE (38) + its documented 0..7 value domain
│   ├── lines 79-116 bonus block (39..74) incl. EF_SANC 43, EF_VOLATILE peers, EF_*2 twins
│   ├── lines 118-131 dead + mount block (75..85)
│   ├── lines 133-134 EF_ITEMLEVEL (87), EF_DONATE (91)
│   ├── lines 136-141 grade range 100..105
│   ├── lines 143-147 date stamp 106..110
│   ├── lines 149-150 classification 112..113
│   └── lines 152-153 terminal flags 126..127
│
├── Basedef.h                        # EFFECT CONSUMER CONTRACT (types + constants)
│   ├── Basedef.h:142   MAX_EFFECTINDEX  128
│   ├── Basedef.h:144   MAX_STATICEFFECT 12
│   ├── Basedef.h:196-201 REF_10..REF_15  (10,12,15,18,22,27) refinement/success tiers
│   ├── Basedef.h:398-412 STRUCT_ITEM.stEffect[3]  { cEffect (u8), cValue (u8) }
│   ├── Basedef.h:882-906 STRUCT_ITEMLIST.stEffect[12] { sEffect (s16), sValue (s16) }
│   └── Basedef.h:2462  extern char EffectNameTable[128][24]
│
├── Basedef.cpp                      # DICTIONARY LOADER + ID INTERPRETERS (shared by both EXEs)
│   ├── Basedef.cpp:741     char EffectNameTable[MAX_EFFECTINDEX][24]
│   ├── Basedef.cpp:1647    BASE_GetItemAbility()            primary interpreter
│   ├── Basedef.cpp:1844    BASE_GetStaticItemAbility()      template interpreter
│   ├── Basedef.cpp:1993    BASE_GetBonusItemAbility()       instance interpreter
│   ├── Basedef.cpp:2052    BASE_GetBonusItemAbilityNosanc() instance interpreter, no scaling
│   ├── Basedef.cpp:2095    BASE_GetItemSanc()               43 / 116-125 decoder
│   ├── Basedef.cpp:2143    BASE_GetItemGem()                230..253 gem decoder
│   ├── Basedef.cpp:2177    BASE_GetItemSancSuccess()        attempt decoder
│   ├── Basedef.cpp:2239    BASE_SetItemSanc()               43 / 116-125 encoder
│   ├── Basedef.cpp:2287    BASE_GetItemAmount()             EF_AMOUNT reader
│   ├── Basedef.cpp:2304    BASE_SetItemAmount()             EF_AMOUNT writer
│   ├── Basedef.cpp:2574    BASE_InitializeBaseDef()         load ordering
│   ├── Basedef.cpp:4963    BASE_InitializeItemList()        CSV entry point
│   ├── Basedef.cpp:4987    BASE_ReadItemListFile()          name -> ID resolution
│   ├── Basedef.cpp:5163    BASE_InitializeEffectName()      HEADER PARSER
│   ├── Basedef.cpp:6598    BASE_VisualItemCode()            43 / 116-125 visual mapping
│   ├── Basedef.cpp:6662    BASE_VisualAnctCode()            43 / 116-125 anct mapping
│   └── Basedef.cpp:6732    BASE_SetItemDate()               EF_WDAY/WMONTH/YEAR writer
│
├── TMSrv/  (14 TUs including ItemEffect.h; 20 TUs using EF_* tokens)
│   ├── _MSG_UseItem.cpp     # largest consumer: 131 EF_ references, 63 EF_VOLATILE branches
│   ├── Server.cpp           # 31 EF_ references; grade range, guild PvP, PvP damage decay
│   ├── ProcessDBMessage.cpp # 21 EF_ references; variant normalisation + ID 98 class marker
│   ├── CMob.cpp (18), _MSG_CombineItemOdin.cpp (16), GetFunc.cpp (14), CReadFiles.cpp (10)
│   ├── _MSG_CombineItemExtracao.cpp (7), imple.cpp (8), CCastleZakum.cpp (5)
│   └── _MSG_UpdateItem/GetItem/CombineItemEhre/Quest/TradingItem/Trade/Sell/Buy/SendAutoTrade
│
├── TMSrv/ProcessClientMessage.h  # includes ItemEffect.h transitively for all 61 handlers
└── DBSrv/Server.cpp              # 2 EF_ references; shares Basedef.cpp

(excluded from analysis: .agents, .codegraph, .opencode, openspec, **/Debug, **/Release)
```

Notes on structure:

* The header has **no include guard and no `#pragma once`**. It is idempotent (only `#define`s), so that is safe, but it also means the file can be included twice in a TU without diagnostic — which is exactly what the runtime parser must tolerate.
* The file is **EUC-KR / CP949 encoded with CRLF line endings**. Comments are invisible in UTF-8 tooling; the file appears as mojibake in any editor not configured for code page 949. This is a real maintenance hazard: 60 of 153 lines are Korean comments, and 13 of them carry the only specification of value semantics.
* The header is **not self-describing**: it has no group end-markers for the bonus block, no table of contents, and no statement of the 0..127 bound. The bound lives in `Basedef.h:142`, a different file in a different directory.

---

## 5. Dependency Analysis

### Internal Dependencies

```
Compile-time (textual, one direction only):

  ItemEffect.h ──(no outgoing edges; 0 includes)──> ∅

  Code/Basedef.cpp:35              ──includes──> ItemEffect.h
  Code/DBSrv/Server.cpp:37         ──includes──> ItemEffect.h
  Code/TMSrv/CCastleZakum.cpp:29   ──includes──> ItemEffect.h
  Code/TMSrv/CMob.cpp:21           ──includes──> ItemEffect.h
  Code/TMSrv/CReadFiles.cpp:29     ──includes──> ItemEffect.h
  Code/TMSrv/CWarTower.cpp:29      ──includes──> ItemEffect.h
  Code/TMSrv/GetFunc.cpp:22        ──includes──> ItemEffect.h
  Code/TMSrv/imple.cpp:22          ──includes──> ItemEffect.h
  Code/TMSrv/MobKilled.cpp:29      ──includes──> ItemEffect.h
  Code/TMSrv/ProcessClientMessage.cpp:29 ──includes──> ItemEffect.h
  Code/TMSrv/ProcessClientMessage.h:31   ──includes──> ItemEffect.h  (transitive for 61 handlers)
  Code/TMSrv/ProcessDBMessage.cpp:29     ──includes──> ItemEffect.h
  Code/TMSrv/ProcessSecMinTimer.cpp:29   ──includes──> ItemEffect.h
  Code/TMSrv/Server.cpp:29               ──includes──> ItemEffect.h

Runtime (file-system, not compiler-visible):

  BASE_InitializeEffectName()  ── reads ──> ../../Common/ItemEffect.h
                                            (fallback ../../TMSRV/Run/ItemEffect.h)
  BASE_InitializeEffectName()  ── writes ─> EffectNameTable[128][24]
  BASE_ReadItemListFile()      ── reads ──> EffectNameTable  (name -> ID)
  BASE_ReadItemListFile()      ── reads ──> ../../Common/itemlist.csv
                                            ../../Common/extraitem.csv
  BASE_ReadItemListFile()      ── writes ─> g_pItemList[6500].stEffect[12]
  BASE_ReadItemListFile()      ── writes ─> ../../Common/extraitem.bin (build mode)

Build-system dependencies:

  Code/TMSrv/TMSrv.vcxproj  ── <ClInclude Include="..\ItemEffect.h" />
  Code/DBSrv/DBSrv.vcxproj  ── <ClInclude Include="..\ItemEffect.h" />
  (neither project lists it as a build input beyond inclusion; no custom build step exists)
```

### External Dependencies

| Dependency | Version | Purpose | Notes |
|-----------|---------|---------|-------|
| MSVC C preprocessor | v143 (VS 2022) | Substitutes `EF_*` tokens | The header's only real "engine" |
| C standard library (`stdio.h`, `string.h`, `stdlib.h`) | MSVC CRT | `fopen`/`fgets`/`sscanf`/`atoi`/`strcmp`/`memcpy` for the loader | Used by `Basedef.cpp`, not by the header |
| Win32 API (`MessageBoxA`) | Windows SDK | Loader error reporting | Modal dialog on failure — not headless safe |
| `../../Common/ItemEffect.h` (runtime copy) | external to repo | Runtime schema | **Not present in this repository** (`Common/` does not exist) |
| `../../Common/itemlist.csv`, `extraitem.csv` | external to repo | Effect-name consumer data | **Not present in this repository** |
| Client build (not in repo) | unknown | Shares the same effect ID contract | `Code/ClientPatch_v7662/` contains only `ClientPatch_v7662.vcxproj.user`; no client source or binary is available to verify the contract |

Third-party libraries: **none**. The component has no functional dependency on any external package; the dependency-auditor report for this repository therefore has nothing to say about it.

---

## 6. Afferent and Efferent Coupling

The component's "units" for coupling purposes are its macro definitions (the C analogue of constants/symbols), plus the loader functions in `Basedef.cpp` that give them runtime meaning. Afferent coupling (Ca) = number of distinct source files that reference the symbol. Efferent coupling (Ce) for every macro is **0** by construction (no includes, no types, no dependencies).

**Aggregate:** Ca = **26 files** (union of 14 include-directives and 20 `EF_*`-using files), Ce = **0**. This matches the architecture report's estimate of ~27/0.

| Component (symbol / unit) | Afferent Coupling (files) | Efferent Coupling | Criticality |
|---------------------------|---------------------------|-------------------|-------------|
| `EF_DAMAGE` (2) | 6 | 0 | High — core combat math, twin-promoted via `EF_DAMAGE2` |
| `EF_ITEMLEVEL` (87) | 5 | 0 | High — 23 references, gates extraction/combination/grade |
| `EF_MOBTYPE` (112) | 5 | 0 | High — 29 references, gates refinement and recipes |
| `EF_VOLATILE` (38) | 4 | 0 | Critical — dispatch selector for 63 use-script branches |
| `EF_AC` (3) | 4 | 0 | Medium |
| `EF_HP` (4) | 3 | 0 | Medium — potion path |
| `EF_POS` (17) | 3 | 0 | High — equip mask, exemption from scaling |
| `EF_KEYID` (39) | 3 | 0 | High — door/gate access control |
| `EF_SANC` (43) as token | 3 | 0 | Critical (token undercounts: literal `43` appears in 8+ files) |
| `EF_HWORDGUILD` (56) / `EF_LWORDGUILD` (57) | 3 / 3 | 0 | Medium — PvP attack/defence |
| `EF_CRITICAL2` (71) / `EF_DAMAGE2` (73) | 3 / 3 | 0 | High — twin channels |
| `EF_INCUDELAY` (84) | 3 | 0 | Medium — incubation timing |
| `EF_DONATE` (91) | 3 | 0 | High — currency value |
| `EF_NOTRADE` (127) | 3 | 0 | High — economy integrity |
| `EF_NOSANC` (126) | 2 | 0 | Medium — refine gate |
| `EF_GRID` (33), `EF_RESISTALL` (54), `EF_AMOUNT` (61) | 2 each | 0 | Medium |
| `EF_VOLATILE` **payload domain** 1..243 | 1 file, 63 branches | 0 | Critical — no named constants, no validation |
| Numeric literals `116..125` | 4 files | 0 | Critical — undeclared namespace |
| Numeric literal `98` | 2 files | 0 | Medium — undeclared class-master marker |
| `BASE_InitializeEffectName()` | 2 call sites (`Basedef.cpp:2574`, recursive `5210`) | 3 (EffectNameTable, stdio, Win32) | Critical — single point of schema load |
| `EffectNameTable[128][24]` | 2 writers/readers (`5163`, `5128`) | 1 | Critical — fixed-size global, bound is the namespace |
| 22 unreferenced tokens | 0 | 0 | Low — dead weight |

**Interpretation.** The instability of this component is not numerical but *semantic*: Ce is zero, so it cannot break by dependency change, but Ca is high and its public surface is 101 flat constants with no encapsulation. Any change to a value is a breaking change to compiled binaries, to the on-disk `ItemList.bin`/`extraitem.bin` caches, and to the client. That is the definition of a stable-but-rigid dependency, and it is why the two stale items (`EF_VOLATILE` comment, undeclared 116..125) have survived: fixing them requires coordinated content and client changes.

**Cohesion assessment.** The header is cohesive only at the level of "everything an item can carry". Internally it is 11 loosely-related sub-namespaces with no separators beyond comment banners, and at least three of them (grade range, date stamp, attribute refinement) have arithmetic contracts invisible to a reader. Cohesion is **low (logical cohesion)** despite the file's small size — the constants are grouped by authoring convenience, not by behaviour.

---

## 7. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| `EffectNameTable` global | In-process shared state | Name-to-ID lookup built from this header, consumed by the CSV loader | Direct array index (`char[128][24]`) | Fixed-width char rows | Bounds check off-by-one (`val > 128`); duplicate check via `[val][0] != 0`; no length check on `memcpy` 24 bytes |
| `../../Common/ItemEffect.h` | External file (deployment artifact) | Runtime schema for effect names | Win32 `fopen`/`fgets` | Text, EUC-KR, `#define` lines | `MessageBoxA` + early return; caller cannot detect failure (`void`) |
| `../../TMSRV/Run/ItemEffect.h` | External file (fallback path) | Secondary schema location | Win32 `fopen` | Text | Same; second failure aborts load silently |
| `../../Common/itemlist.csv` | External content file | Primary item definitions with effect names | CSV, parsed via `sscanf` after comma→space rewrite | 33-field CSV row | `MessageBox` "There is no file." and abort of item init; per-row guards on `Index >= MAX_ITEMLIST` and name length |
| `../../Common/extraitem.csv` | External content overlay | Additional item definitions | Same | Same | No error handling on failure (`ret` ignored at `Basedef.cpp:4984`) |
| `../../Common/extraitem.bin` / `ItemList.bin` | Binary cache (written then read) | Serialised `STRUCT_ITEMLIST` including numeric `sEffect` | `fwrite`/`fread` of `sizeof(STRUCT_ITEMLIST)` | Native struct (no versioning, no endianness marker) | `MessageBox` "Can't write extraitem.bin" on open failure |
| Client build (out of repo) | Compile-time contract | Shares effect IDs for display and prediction | Binary protocol (effect pairs `{u8,u8}` in `STRUCT_ITEM`) | 3 × 2 bytes per item | None in this repository; `Code/ClientPatch_v7662/` holds only a `.vcxproj.user` |
| `BASE_InitializeEffectName` → `BASE_InitializeEffectName` | Recursive self-call | Handles `#include` lines in the header | In-process recursion | n/a | Unbounded recursion (no depth limit) and re-clears the table on each unwinding path |
| `MessageBoxA` / `MessageBox` | Win32 UI | Surface schema defects to an operator | Win32 API | Text | Blocking modal; unsuitable for service/headless start-up |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Constant namespace (flat macro enum) | 101 `#define EF_* <int>` | `Code/ItemEffect.h:3-153` | Compile-time effect ID contract; zero-cost, preprocessor-only |
| Header-as-schema / self-describing data file | `BASE_InitializeEffectName` parses the same header text at runtime | `Code/Basedef.cpp:5163-5242` | Keeps one authoritative text for both compiler and content loader |
| Table-driven interpretation | `EffectNameTable[128][24]` maps name↔ID; `g_pItemList[6500]` maps item→effects | `Code/Basedef.cpp:741`, `5128-5143` | Content is data, not code |
| Accessor / disguised accessor | Effect IDs used as field selectors (`EF_LEVEL` returns `ReqLvl`, `EF_POS` returns `nPos`) | `Code/Basedef.cpp:1683-1702`, `1854-1872` | One uniform `GetAbility(item, Type)` API over heterogeneous storage |
| Strategy by sentinel (integer dispatch) | `EF_VOLATILE` payload selects 1 of 63 handler blocks | `Code/TMSrv/_MSG_UseItem.cpp:94-5710` | Item-specific behaviour without subclassing; classic "switch on type code" |
| Null Object (implicit) | Effect ID 0 = empty slot; resolvers return 0 for unmatched types | `Code/ItemEffect.h:1`, `Code/Basedef.cpp:1649` | Absence without an explicit flag |
| Sentinel-value encoding | Attack speed 1 → 10; `sanc >= 9` → +1 | `Code/Basedef.cpp:1713`, `1810` | Legacy content compatibility |
| Packed bitfield-in-byte | `EF_SANC` cValue = `level + 10*success`, plus 230..253 space | `Code/Basedef.cpp:2239-2285`, `2095-2141` | Two quantities in one byte = save-file and packet compactness |
| Open/Closed violation (enumeration in `if`-chains) | Every new effect requires editing 3 exemption lists + N handlers | `Code/Basedef.cpp:1813`, `1967`, `2043` | Historical; no registry/dispatch table was introduced |
| Two-tier model (template vs instance) | `STRUCT_ITEMLIST.stEffect[12]` vs `STRUCT_ITEM.stEffect[3]` | `Code/Basedef.h:882-906`, `398-412` | Static content vs mutable per-item state |
| Facade over storage layout | `BASE_GetItemAbility` hides whether a value comes from template, instance, mount table, or item index | `Code/Basedef.cpp:1647-1842` | Callers never touch `g_pItemList` directly for bonuses |
| Copy-paste reuse (anti-pattern) | 4 near-identical refinement cascades; 4 near-identical resist fan-outs; 4 near-identical attack-speed shims | `_MSG_UseItem.cpp:240-261,400-421,813-834,3701-3717`; `Basedef.cpp:1790,1890,2020,2077` | Cheap to write, expensive to keep consistent |

**Architectural position.** In the architecture report's layering, this is a *shared data* artifact sitting beneath both `Basedef` (shared engine) and the two servers. It has no layer of its own: it is a vocabulary, not a module. Its architectural significance is that it is the only place where the item domain's ontology is written down — and, as sections 3 and 11 show, it is an incomplete account of that ontology.

**Notable absence of patterns.** There is no `enum`/`enum class`, no namespace, no compile-time assertion on the ID range, no registry, no validation schema, and no version constant. For a 128-slot contract shared with an external client, that is the main architectural decision *not* taken; the consequences are enumerated in section 11.

---

## 9. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| Critical | Loader bound check (`Code/Basedef.cpp:5222`) | `if(val < 0 \|\| val > MAX_EFFECTINDEX)` allows `val == 128`, then `memcpy(EffectNameTable[128], ...)` writes one row past a `[128][24]` array | Out-of-bounds write corrupting an adjacent global at start-up; triggered by any content author adding `EF_* 128` (no headroom remains, see next row) |
| Critical | Namespace exhaustion (`Code/ItemEffect.h:153`) | Highest IDs are 126/127, i.e. the last two legal values; no headroom for new attributes | Any new effect forces either reclaiming undeclared 116..125 or a `MAX_EFFECTINDEX` change (which resizes `EffectNameTable` and the serialised item caches) |
| Critical | Unresolved-name handling (`Code/Basedef.cpp:5134`) | `if (j == MAX_EFFECTINDEX)` compares the slot index (0..11) instead of the search result `v`, so unresolved names are never reported | Every typo in `itemlist.csv` silently stores `sEffect = 128`; with an empty name table (failed header load) every item is corrupted the same way |
| Critical | `EF_VOLATILE` contract drift (`Code/ItemEffect.h:77`) | Comment documents 0..7; code dispatches on 63 values up to 243 (`_MSG_UseItem.cpp`) | No named constants, no validation, no way to tell an unimplemented value from a typo; content errors are invisible |
| High | Undeclared 116..125 (`_MSG_UseItem.cpp:3711-3717`) | Attribute refinement range has no `EF_*` names and is absent from the header | Cannot be authored in CSV; invisible to duplicate detection; a future `#define` in that range would be silently shadowed at runtime |
| High | Undeclared ID 98 (`_MSG_UseItem.cpp:3045`, `ProcessDBMessage.cpp:708`) | Class-master marker with no constant; reuses `EF_WDAY` (106) to carry the face index | Direct collision with the documented date-stamp meaning of 106; unsearchable, unvalidated |
| High | Off-by-one buffer write (`Code/Basedef.cpp:5208`) | `str2[1 + strlen(str2)] = 0` writes one byte past the string terminator on every `#include` line | Benign today (256-byte buffer, short token) but undefined behaviour and a latent overflow |
| High | Silent load failure (`Code/Basedef.cpp:5175-5180`, `2574`) | `void` return + modal `MessageBoxA`; `BASE_InitializeBaseDef` continues to load items with an empty table | Corrupt item corpus on any deployment missing `Common/ItemEffect.h`; modal dialog blocks headless/service start-up |
| High | Client contract unverifiable in-repo | Architecture claims the dictionary is shared with the client; `Code/ClientPatch_v7662/` contains only a `.vcxproj.user`, no client source or binary | Any ID change is a protocol change with no in-repo check; the 116..125 and 98 conventions exist only because the client already understands them |
| High | No tests, no CI validation | 0 test files, 0 test framework references, no schema validation script | Loader defects and namespace collisions are found only at runtime by players |
| Medium | 26 undefined namespace holes (`0,28,86,88-90,92-99,111,114-125`) | No reservation markers; only comments (and only for 0 and 28) | Authors cannot distinguish "free" from "deliberately skipped"; 98 already leaked into a hole |
| Medium | 22 unreferenced tokens | `EF_EXP, EF_SCORE14/15, EF_R1SIDC, EF_R2SIDC, EF_SPELL, EF_DURATION, EF_PARM2, EF_CLAN, EF_HITRATE, EF_BONUS, EF_UNIQUE, EF_INIT1/2/3, EF_SUBGUILD, EF_GRADE1-4, EF_HOUR, EF_MIN` | Namespace waste at 100% capacity; misleading to content authors who assume they work |
| Medium | Same effect written as token and literal | `EF_SANC`/`43`, `EF_UNIQUE`/`59`, `EF_AMOUNT`/`61`, `EF_HWORDGUILD`/`56`, `EF_ITEMLEVEL` present, `EF_DAMAGE2`/`73` | A global rename would silently miss literal sites; `EF_SANC` token appears in 3 files but literal 43 in 8+ |
| Medium | Refinement exemption list duplicated 3× and inconsistent | `Code/Basedef.cpp:1813` (19 IDs) vs `1967` (12 IDs) vs `2043` (13 IDs) | Adding an effect to two of three lists yields an effect that scales on one code path but not another |
| Medium | `EF_VOLATILE` missing from `BASE_GetStaticItemAbility` exemption list | `Code/Basedef.cpp:1967` omits `EF_VOLATILE`, `EF_MOBTYPE`, `EF_ITEMTYPE`, `EF_ITEMLEVEL`, `EF_NOTRADE`, `EF_NOSANC`, `EF_DONATE` (present only in `1813`) | If any caller uses the static resolver for those IDs, the value is refinement-scaled — currently masked by call-site discipline |
| Medium | Non-deterministic table bytes (`Code/Basedef.cpp:5236`) | `memcpy(EffectNameTable[val], str2, 24)` copies 24 bytes from a partially-initialised `char[256]` | Table rows contain stack residue beyond the name; harmless today, breaks any future exact-compare or hash of the table |
| Medium | Unbounded recursion on `#include` (`Code/Basedef.cpp:5204-5213`) | Self-recursion with no depth limit; each recursive call re-clears the table, discarding everything parsed so far | A stray `#include` line (present in the original upstream file per `Basedef.cpp:5204` handling) wipes and re-reads; malformed chains could recurse deeply |
| Medium | `atoi`-only value parsing | Hex, expressions, or blank values all become 0 (the reserved sentinel) | A future author writing `#define EF_X 0x10` silently gets ID 0 = "no effect", with no diagnostic |
| Medium | Unguarded comma rewrite in CSV parser (`Code/Basedef.cpp:5044-5048`) | Loops the full 1024-byte buffer replacing `,` with ` ` without checking for NUL | Reads past the string terminator; works only because the buffer is fully on the stack |
| Medium | Mount handling keyed on hard-coded index ranges | `2330..2390`, `2362..2389`, `3980..3994` in four functions | New mounts outside those ranges are treated as ordinary equipment for scaling and bonus purposes |
| Low | Encoding and line endings | EUC-KR/CP949, CRLF; 60 of 153 lines are Korean comments | Unreadable in UTF-8 tooling; the only specification of value ranges is in these comments |
| Low | No include guard / `#pragma once` | Header is macro-only, so re-inclusion is safe | Fine today; forbids ever adding a type or a `static` helper |
| Low | `EF_WTYPE` inline table in comments (lines 26-55) | 29 lines of weapon-class codes live only as comments | Not machine-readable; cannot be validated, and duplicates knowledge held in item data |
| Low | `EF_HWORDGUILD`/`EF_LWORDGUILD` commented-out truncation | `Code/Basedef.cpp:1827-1834` retained as an `/* ... */` block | Dead code in the hot resolver path; obscures whether byte truncation is intended |

---

## 10. Test Coverage Analysis

**Result: there are no tests for this component, and no test infrastructure anywhere in the repository.**

Evidence of absence (all searches run over the full project root, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`, `.git`, `.vs`):

1. **No test directories.** A recursive directory scan for names matching `test|spec|unittest|fixture|mock` returned only `E:\wyd\test-setup\w2pp\openspec` (a specification-workflow folder, explicitly ignored, and not a test suite).
2. **No test source files.** A recursive file scan for `*test*`, `*spec*`, `*_t.*` over `*.cpp|*.c|*.h|*.py|*.js|*.md` returned zero matches.
3. **No test framework.** A scan of every `*.vcxproj`, `*.sln`, `CMakeLists.txt`, `*.cmake` and `package.json` for `gtest`, `catch2`, `CppUnit`, `vstest`, `pytest` or `jest` returned zero matches. The solution `W2PP Code Project.sln` contains only `TMSrv`, `DBSrv` and `ClientPatch_v7662`; `Code/TMSrv/TMSrv.vcxproj` and `Code/DBSrv/DBSrv.vcxproj` declare no test project or test target.
4. **No schema-validation script.** Nothing parses `Code/ItemEffect.h` except the C++ loader itself (`Basedef.cpp:5163`), and that parser has no test harness.
5. **No golden data.** The `Common/` directory that would hold `itemlist.csv` does not exist in the repository (`Test-Path Common` → `False`), so there is not even a fixture corpus to assert against.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| ItemEffect ID namespace (101 defines) | 0 | 0 | 0% | Not applicable — no test files exist |
| `BASE_InitializeEffectName` header parser (`Basedef.cpp:5163-5242`) | 0 | 0 | 0% | Not applicable; 3 known defects are untested |
| `EffectNameTable` name↔ID round-trip (`Basedef.cpp:5128-5143`) | 0 | 0 | 0% | Not applicable |
| `BASE_GetItemAbility` (`1647-1842`) | 0 | 0 | 0% | Not applicable; 33 callers, no assertions |
| `BASE_GetStaticItemAbility` (`1844-1991`) | 0 | 0 | 0% | Not applicable |
| `BASE_GetBonusItemAbility` / `...Nosanc` (`1993-2093`) | 0 | 0 | 0% | Not applicable |
| `BASE_GetItemSanc` / `GetItemGem` / `GetItemSancSuccess` / `SetItemSanc` (`2095-2285`) | 0 | 0 | 0% | Not applicable; packed-byte codec with 6 branches, untested |
| `BASE_GetItemAmount` / `SetItemAmount` (`2287-2338`) | 0 | 0 | 0% | Not applicable; silent no-op path untested |
| `BASE_VisualItemCode` / `VisualAnctCode` (`6598-6730`) | 0 | 0 | 0% | Not applicable |
| `BASE_SetItemDate` / `CheckItemDate` (`6732-6774`) | 0 | 0 | 0% | Not applicable; hand-rolled date rollover untested |
| `EF_VOLATILE` dispatch (63 branches in `_MSG_UseItem.cpp`) | 0 | 0 | 0% | Not applicable |

**Testability assessment.** The component is *cheap* to test in isolation and was never tested. All interpreters are pure functions of `(STRUCT_ITEM*, unsigned char Type)` plus two globals (`g_pItemList`, `EffectNameTable`); no sockets, no database, no timers. A table-driven test over the 101 IDs asserting `(id, slot layout) → expected value` would run in milliseconds. The loader is likewise testable by pointing it at a fixture header — but its file paths are hard-coded (`Basedef.cpp:5170-5173`), so it cannot be redirected without editing the source, which is itself a testability defect.

**Consequences of zero coverage.** Every defect listed in section 9 is currently undetectable by construction. In particular, the `5134` variable-confusion bug and the `5222` off-by-one would each be caught by a single negative test ("unknown effect name", "value 128"). Their survival is evidence that this component has never been exercised outside a live server start-up.

---

## 11. Appendix — Complete Effect ID Inventory

Values are from `Code/ItemEffect.h`. "Refs" is the count of token occurrences in `Code/` excluding the header itself (Debug/Release object directories excluded). "Files" is distinct-file fan-in.

| ID | Token | Refs | Files | Group | Header comment (translated) |
|----|-------|------|-------|-------|------------------------------|
| 0 | *(EF_NONE, commented out line 1)* | — | — | reserved | "effect type 0 = no effect; Score[0] cannot be added" |
| 1 | `EF_LEVEL` | 10 | 1 | score | level; Score[1]-Score[15] added verbatim |
| 2 | `EF_DAMAGE` | 52 | 6 | score | damage increase |
| 3 | `EF_AC` | 17 | 4 | score | defence |
| 4 | `EF_HP` | 6 | 3 | score | HP, MP, attack/move speed, MAX etc. |
| 5 | `EF_MP` | 2 | 1 | score | magic increase |
| 6 | `EF_EXP` | 0 | 0 | score | experience — **unused** |
| 7 | `EF_STR` | 1 | 1 | score | strength |
| 8 | `EF_INT` | 1 | 1 | score | spirit |
| 9 | `EF_DEX` | 1 | 1 | score | dexterity |
| 10 | `EF_CON` | 1 | 1 | score | constitution |
| 11 | `EF_SPECIAL1` | 1 | 1 | score | mastery |
| 12 | `EF_SPECIAL2` | 1 | 1 | score | special 1 |
| 13 | `EF_SPECIAL3` | 1 | 1 | score | special 2 |
| 14 | `EF_SPECIAL4` | 1 | 1 | score | special 3 |
| 15 | `EF_SCORE14` | 0 | 0 | score | — **unused** |
| 16 | `EF_SCORE15` | 0 | 0 | score | — **unused** |
| 17 | `EF_POS` | 10 | 3 | requirement | equip-slot bitmask; default NOWHERE |
| 18 | `EF_CLASS` | 4 | 1 | requirement | class/gender bitmask; default YES |
| 19 | `EF_R1SIDC` | 0 | 0 | requirement | one-hand STR/INT/DEC/CON; default 0 — **unused** |
| 20 | `EF_R2SIDC` | 0 | 0 | requirement | two-hand STR/INT/DEC/CON; default impossible — **unused** |
| 21 | `EF_WTYPE` | 6 | 1 | bonus | weapon class for animation; 29-line inline code table |
| 22 | `EF_REQ_STR` | 7 | 1 | requirement | strength requirement |
| 23 | `EF_REQ_INT` | 7 | 1 | requirement | spirit requirement |
| 24 | `EF_REQ_DEX` | 7 | 1 | requirement | dexterity requirement |
| 25 | `EF_REQ_CON` | 7 | 1 | requirement | constitution requirement |
| 26 | `EF_ATTSPEED` | 9 | 1 | bonus | attack speed; value 1 → 10 |
| 27 | `EF_RANGE` | 6 | 1 | bonus | range; maximum of all equipped applies |
| 28 | *(EF_PRICE, commented out line 67)* | — | — | reserved | price |
| 29 | `EF_RUNSPEED` | 3 | 1 | bonus | run speed; clamped to 2 |
| 30 | `EF_SPELL` | 0 | 0 | bonus | spell applied on equip/use — **unused** |
| 31 | `EF_DURATION` | 0 | 0 | bonus | parameter 1: effect duration — **unused** |
| 32 | `EF_PARM2` | 0 | 0 | bonus | parameter 2 — **unused** |
| 33 | `EF_GRID` | 8 | 2 | bonus | inventory grid size; clamped 0..7 |
| 34 | `EF_GROUND` | 2 | 1 | bonus | ground footprint |
| 35 | `EF_CLAN` | 0 | 0 | bonus | race/clan — **unused** |
| 36 | `EF_HWORDCOIN` | 1 | 1 | bonus | high word of coin value |
| 37 | `EF_LWORDCOIN` | 1 | 1 | bonus | low word of coin value |
| 38 | `EF_VOLATILE` | 7 | 4 | bonus | consumable; documented 0..7, **63 values in use up to 243** |
| 39 | `EF_KEYID` | 8 | 3 | bonus | key number |
| 40 | `EF_PARRY` | 7 | 1 | bonus | evasion bonus |
| 41 | `EF_HITRATE` | 0 | 0 | bonus | hit-rate bonus — **unused** |
| 42 | `EF_CRITICAL` | 5 | 1 | bonus | critical; promoted to `EF_CRITICAL2` |
| 43 | `EF_SANC` | 35 | 3 | bonus | refinement (Sanctuary); payload = level+success |
| 44 | `EF_SAVEMANA` | 1 | 1 | bonus | mana save 1 → 99% |
| 45 | `EF_HPADD` | 2 | 1 | bonus | HP boost 1 → 101% |
| 46 | `EF_MPADD` | 2 | 1 | bonus | MP boost 1 → 101% |
| 47 | `EF_REGENHP` | 1 | 1 | bonus | HP regeneration |
| 48 | `EF_REGENMP` | 1 | 1 | bonus | MP regeneration |
| 49 | `EF_RESIST1` | 13 | 2 | bonus | fire resist |
| 50 | `EF_RESIST2` | 13 | 2 | bonus | ice resist |
| 51 | `EF_RESIST3` | 13 | 2 | bonus | lightning resist |
| 52 | `EF_RESIST4` | 13 | 2 | bonus | poison resist |
| 53 | `EF_ACADD` | 2 | 1 | bonus | defence increase; promoted to `EF_ACADD2` |
| 54 | `EF_RESISTALL` | 10 | 2 | bonus | all resist; fans out to 49..52 |
| 55 | `EF_BONUS` | 0 | 0 | bonus | bonus — **unused** |
| 56 | `EF_HWORDGUILD` | 10 | 3 | bonus | PvP attack (Portuguese "Ataque PvP") |
| 57 | `EF_LWORDGUILD` | 10 | 3 | bonus | PvP defence ("Defesa PvP") |
| 58 | `EF_QUEST` | 3 | 2 | bonus | quest id; starts at 1; usually paired with `EF_VOLATILE` |
| 59 | `EF_UNIQUE` | 0 | 0 | bonus | uniqueness marker — **token unused** (literal 59 used in 9 sites) |
| 60 | `EF_MAGIC` | 14 | 1 | bonus | magic attack 1 → 1% amplification |
| 61 | `EF_AMOUNT` | 13 | 1 | bonus | stack count |
| 62 | `EF_HWORDINDEX` | 5 | 1 | bonus | high word of index |
| 63 | `EF_LWORDINDEX` | 5 | 1 | bonus | low word of index |
| 64 | `EF_INIT1` | 0 | 0 | bonus | initial value 1 — **unused** |
| 65 | `EF_INIT2` | 0 | 0 | bonus | initial value 2 — **unused** |
| 66 | `EF_INIT3` | 0 | 0 | bonus | initial value 3 — **unused** |
| 67 | `EF_DAMAGEADD` | 11 | 1 | bonus | damage add; gated to unique 41..50 |
| 68 | `EF_MAGICADD` | 5 | 1 | bonus | magic add; gated to unique 41..50 |
| 69 | `EF_HPADD2` | 5 | 1 | bonus | HP boost 2; shown inside base options |
| 70 | `EF_MPADD2` | 5 | 1 | bonus | MP boost 2; shown inside base options |
| 71 | `EF_CRITICAL2` | 12 | 3 | bonus | critical 2 |
| 72 | `EF_ACADD2` | 7 | 1 | bonus | defence increase 2 |
| 73 | `EF_DAMAGE2` | 19 | 3 | bonus | damage 2 (no comment in header) |
| 74 | `EF_SPECIALALL` | 3 | 1 | bonus | all masteries except mastery |
| 75 | `EF_CURKILL` | 1 | 1 | dead | "not used" |
| 76 | `EF_LTOTKILL` | 1 | 1 | dead | "not used" |
| 77 | `EF_HTOTKILL` | 1 | 1 | dead | "not used" |
| 78 | `EF_INCUBATE` | 7 | 1 | mount | incubation threshold |
| 79 | `EF_MOUNTLIFE` | 3 | 1 | mount | mount total life |
| 80 | `EF_MOUNTHP` | 4 | 1 | mount | mount current HP |
| 81 | `EF_MOUNTSANC` | 5 | 1 | mount | mount growth |
| 82 | `EF_MOUNTFEED` | 3 | 1 | mount | mount satiety |
| 83 | `EF_MOUNTKILL` | 3 | 1 | mount | monsters killed by mount? |
| 84 | `EF_INCUDELAY` | 5 | 3 | bonus | incubation delay |
| 85 | `EF_SUBGUILD` | 0 | 0 | bonus | sub-guild; 0 means max guild 1,2,3 — **unused** |
| 86 | *(undefined)* | — | — | hole | — |
| 87 | `EF_ITEMLEVEL` | 23 | 5 | bonus | item level (no comment) |
| 88-90 | *(undefined)* | — | — | holes | — |
| 91 | `EF_DONATE` | 3 | 3 | bonus | donation currency |
| 92-99 | *(undefined)* | — | — | holes | 98 is used at runtime as class-master marker |
| 100 | `EF_GRADE0` | 3 | 3 | grade | grade A |
| 101 | `EF_GRADE1` | 0 | 0 | grade | grade B — **unused token** (used via range arithmetic) |
| 102 | `EF_GRADE2` | 0 | 0 | grade | grade C — **unused token** |
| 103 | `EF_GRADE3` | 0 | 0 | grade | grade D — **unused token** |
| 104 | `EF_GRADE4` | 0 | 0 | grade | grade E — **unused token** |
| 105 | `EF_GRADE5` | 1 | 1 | grade | grade E Arch |
| 106 | `EF_WDAY` | 1 | 1 | date | day; also reused as face-index carrier |
| 107 | `EF_HOUR` | 0 | 0 | date | hour — **unused** |
| 108 | `EF_MIN` | 0 | 0 | date | minute — **unused** |
| 109 | `EF_YEAR` | 1 | 1 | date | year |
| 110 | `EF_WMONTH` | 1 | 1 | date | month |
| 111 | *(undefined)* | — | — | hole | — |
| 112 | `EF_MOBTYPE` | 29 | 5 | class | mob type (no comment) |
| 113 | `EF_ITEMTYPE` | 7 | 2 | class | item type (no comment) |
| 114-125 | *(undefined in header)* | — | — | hole / undeclared | 116..125 used at runtime as attribute refinement (`116 + color`) |
| 126 | `EF_NOSANC` | 3 | 2 | flag | no further refinement |
| 127 | `EF_NOTRADE` | 3 | 3 | flag | no trading |

Totals: **101 active defines**, 2 commented-out defines, 26 undefined slots (0, 28, 86, 88-90, 92-99, 111, 114-125), 22 declared-but-unreferenced tokens, 2 effect IDs used at runtime but undeclared (98, 116-125).

---

## 12. Analysis Notes and Ambiguities

* **Encoding.** `Code/ItemEffect.h` is EUC-KR (code page 949) with CRLF endings, 6,534 bytes on disk. It was decoded to UTF-8 in a temporary location for reading only; no project file was modified. All Korean comment translations above are from that decode and are marked as translations, not quotations of English source.
* **Not present in the repository:** `Common/ItemEffect.h`, `Common/itemlist.csv`, `Common/extraitem.csv`, `ItemList.bin`, `extraitem.bin`, and any client source or binary (other than `TMSrv.exe`/`TMSrv.pdb` under `Code/release/TMSrv/run/`). Statements about runtime resolution therefore describe the code path, not observed data. The `../../Common/ItemEffect.h` path could not be verified to exist at the deployment root.
* **"Shared with the client"** is asserted by the project context and by the architecture report; it cannot be verified from this repository. The header's comments do describe client display behaviour (`Basedef.cpp`-adjacent comment at lines 109-112 of the header: the client queries the `*2` variants first), which is consistent with a shared contract.
* **`EF_CURKILL`/`EF_LTOTKILL`/`EF_HTOTKILL`** are annotated "not used" yet report a reference count of 1 each. The single reference in each case is consistent with a vestigial or commented-out use; the annotation was treated as authoritative over the counter.
* **Confidence levels.** Namespace grouping and value ranges: high (directly readable). Semantics of `EF_VOLATILE` payloads above 7: high for the dispatch structure, low for the meaning of individual branches (they are unnamed integer literals; only four carry inline comments: "PO - PL" at 150, "Pilula Orc" at 978, "Poeira de Fada" at 999, "7DD/15D/30D" at 2098-2104). Semantics of `EF_MOBTYPE` values 1/3: medium (inferred from usage, no header documentation). Semantics of ID 98: high for the write pattern and the 0/3 correlation with `MORTAL`/`ARCH` vs `CELESTIAL`, medium for the interpretation "class-master rank".
* **No business rule in this report was inferred from undocumented behaviour without stating the evidence site.** Where the header comment and the code disagree (notably `EF_VOLATILE`), both were reported and the code was treated as the operative contract.
