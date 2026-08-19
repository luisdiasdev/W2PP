# Component Deep Analysis Report

**Component:** Basedef
**Project:** W2PP (legacy C/C++ game server)
**Scope analyzed:** `/home/luisdias/dev/github/luisdiasdev/w2pp/legacy/Code/Basedef.h` and `Basedef.cpp`
**Date:** 2026-08-19 17:13:23
**Folders ignored:** `.git`, `.opencode`

---

## 1. Executive Summary

`Basedef` is the **central foundational component** of the W2PP legacy game server. It is a
shared, header-driven contract compiled into **both** server executables — `TMSrv` (the
game/connection server) and `DBSrv` (the accounts/characters database server) — and is
implemented by exactly two translation units at the root of the shared `Code/` directory:

- `legacy/Code/Basedef.h` (2,505 lines) — the packet protocol (`_MSG_*` types), the domain
  data structures (`STRUCT_*`), and the game/network constants.
- `legacy/Code/Basedef.cpp` (6,814 lines) — the implementation of the `BASE_*` helper
  functions that encode the core gameplay business rules.

The component's role in the system is twofold:

1. **Wire protocol contract.** `Basedef.h` defines the exact byte layout of every message
   exchanged between the client and `TMSrv`, between `TMSrv` and `DBSrv`, and between
   `DBSrv` and the admin tool (`NPTool`). The two binaries are coupled purely by this
   source-level contract; there is no interface definition language or versioning.
2. **Domain model and shared game rules.** It defines the persistent and in-memory domain
   entities (`STRUCT_MOB`, `STRUCT_ITEM`, `STRUCT_ACCOUNTFILE`, `STRUCT_SCORE`, etc.) and
   implements the deterministic gameplay math used across both servers: item ability
   aggregation, sanctification (upgrade) success rates, equipment/carry/cargo placement
   rules, character-stat bonus computation, HP/MP growth, combat hit/damage rates, skill
   damage, movement routing, and name/string validation.

**Key findings:**

- **Extremely high afferent coupling.** `Basedef.h` is `#include`d by 34 files, and the
  `BASE_*` functions are called **639 times across 47 files** (excluding `Basedef` itself).
  It is the stability hub of the entire codebase; any change ripples project-wide and
  requires both binaries to be rebuilt in lockstep.
- **Fused protocol + domain model + game constants.** A single header mixes network framing
  (packed `MSG_*` structs), persistent domain structs, and dozens of `#define` game
  constants. There is no separation of concerns at the contract layer.
- **Implicit, dispersed business rules.** Nearly every gameplay rule (stat allocation,
  upgrade chances, combat formulas, placement restrictions) is hardcoded as magic numbers
  and branched logic inside `Basedef.cpp`, with heavy use of comments in Portuguese and
  Korean, and many unexplained fields.
- **No test coverage whatsoever.** No unit, integration, or test files exist for this
  component (or the project at large). All rules are untested and validated only through
  runtime gameplay.
- **Legacy robustness risks.** Raw pointer casting, `rand()`-based probabilistic rules,
  mutable global tables, and a packet-size validation function (`BASE_CheckPacket`) that is
  entirely commented out (returns `FALSE`) in the shipped implementation.

---

## 2. Data Flow Analysis

`Basedef` is not an endpoint or a service; it is a library of domain rules and the shared
data contract. Data flows *through* it as follows:

```
1. Incoming network bytes are read by CPSock and cast to a packed MSG_* struct
   (defined in Basedef.h) — e.g. MSG_AccountLogin, MSG_Attack, MSG_UseItem.
2. The dispatcher (ProcessClientMessage / ProcessDBMessage) reads msg->Type
   (defined in Basedef.h) and routes to a _MSG_* handler.
3. The handler mutates a STRUCT_MOB / STRUCT_ITEM / STRUCT_SCORE domain object
   (defined in Basedef.h) loaded from CUser/CMob.
4. The handler calls a BASE_* rule function in Basedef.cpp to validate or compute:
   - BASE_CanEquip / BASE_CanCarry / BASE_CanCargo  -> placement validation
   - BASE_GetItemAbility / BASE_GetItemSanc        -> item stat aggregation
   - BASE_GetBonusScorePoint / BASE_GetHpMp        -> character stat/HP/MP growth
   - BASE_GetHitRate / BASE_GetDamage / BASE_GetSkillDamage -> combat math
5. The computed result is written back into the STRUCT_* domain object and/or
   packed into an outgoing MSG_* struct (via SendFunc).
6. Persistent domain structs (STRUCT_ACCOUNTFILE) are serialized to/from disk by
   CFileDB (DBSrv) using the same STRUCT_* layout from Basedef.h.
```

Because the packet structs, the persistent account file structs, and the in-memory domain
structs all share the same definitions in `Basedef.h`, a single `STRUCT_MOB` instance
traverses the entire pipeline (client packet -> domain mutation -> save to file) without
any conversion layer — a design that keeps the code small but makes the wire format, disk
format, and memory format one and the same.

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Validation | Item indexes must be in `[0, MAX_ITEMLIST)` (0 < idx < 5000) | Basedef.cpp:770, 1502, 4295 |
| Validation | Character name must be 4..15 chars, alphanumeric/hyphen, not a reserved command | Basedef.cpp:2522 |
| Validation | Classes are limited to 4 (TK/FM/BM/HT); invalid class rejected | Basedef.cpp:832, 860, 1178 |
| Business Logic | ScoreBonus (Str/Int/Dex/Con) depends on class-master tier and level | Basedef.cpp:858 |
| Business Logic | SkillBonus = level-derived points minus spent skill points | Basedef.cpp:810 |
| Business Logic | HP/MP = base + Con/Int-derived + level-increment, capped at 2,000,000,000 | Basedef.cpp:1174 |
| Business Logic | Physical damage formula with AC mitigation and combat roll | Basedef.cpp:1225 |
| Business Logic | Skill damage formula with special/level/weather/class multipliers | Basedef.cpp:6342, 1446 |
| Business Logic | Item ability aggregation scaled by sanctification (sanc) | Basedef.cpp:1647, 2095 |
| Business Logic | Item sanctification success/growth rates by orient + sanc level | Basedef.cpp:2177, 2202, 2227 |
| Validation | Equipment placement: position bitmask, class bitmask, mob-type, level/stat requirements | Basedef.cpp:4293 |
| Validation | Carry/cargo grid collision detection (2x4 item footprint in 9x7 / 9x14 grid) | Basedef.cpp:4413, 4480 |
| Business Logic | Double-weapon damage uses 50%/30%/100% multiplier rules | Basedef.cpp:2412 |
| Business Logic | Hit rate clamped to [40, 95]; capped by Dex and Level ratio | Basedef.cpp:5423 |
| Business Logic | Soul effects multiply Int/Con by tier-specific factors | Basedef.cpp:2997 |
| Validation | Guild/server list parsing with index/guild bounds checks | Basedef.cpp:1314, 6218 |
| Business Logic | Item expiry date encode/decode (day/month/year) | Basedef.cpp:6732, 6763 |
| Business Logic | Fairy item countdown and expiry cleanup | Basedef.cpp:6782 |
| Business Logic | Packet checksum-style sums for save verification | Basedef.cpp:2631, 2663 |

### Detailed breakdown of the business rules

---

### Business Rule: Character Stat Allocation (ScoreBonus)

**Overview:**

The game allows a player to allocate "score bonus" points to four primary attributes —
Strength (Str), Intelligence (Int), Dexterity (Dex), and Constitution (Con). `BASE_GetBonusScorePoint` (Basedef.cpp:858) recomputes how many unspent bonus points a character has by comparing the sum of allocated attribute points (above the class base values) against the total the character should have earned by its level and class-master tier. This function is invoked whenever a character's effective bonus pool needs to be re-derived (login, level-up, class-master change, or after the `MSG_ApplyBonus` handler spends points).

**Detailed description:**

The function first validates the class (`cls < 0 || cls >= MAX_CLASS`), then computes `usestr`, `useint`, `usedex`, `usecon` as the difference between the character's base attributes and the class's starting values from the `BaseSIDCHM[4][6]` table (Basedef.cpp:39). The sum of these differences (`totaluse`) is the number of attribute points already spent. The allowed pool (`leveluse`) is then computed from a different formula depending on the character's class-master tier:

- **Mortal** (tier 2): `leveluse = level * 5`, plus `(level-254)*5` beyond level 254, plus `(level-299)*10` beyond 299, and a negative `(level-354)*-8` adjustment beyond 354 (Basedef.cpp:870-905).
- **Arch** (tier 1): `leveluse = level * 6`, plus `MortalLevel*6`, plus `(level-354)*6` beyond 354 (Basedef.cpp:907-939).
- **Celestial / CelestialCS / SCelestial** (tiers 3/4/5): `leveluse = level*6` plus `Cristal*100`, `Reset*200`, a flat `1001`, and an `ArchLevel`-dependent bonus (100/300/600/900/1200), plus per-level increments at 120/150/170/180/190 thresholds; the celestial sub-tiers add half the secondary level's contribution (Basedef.cpp:940-1170).

If `totaluse` exceeds `leveluse` (an over-allocation, e.g. due to a level reset), the function removes the excess (`dif`) from the first attribute that can absorb it, in the order Str, Int, Dex, Con. Finally, `mob->ScoreBonus = leveluse - totaluse` records the remaining spendable pool. This guarantees that a character can never have spent more stat points than its level/tier entitles it to, which is the core anti-exploit invariant of the character-progression system.

**Rule workflow:**

```
1. Validate class index (0..3).
2. totaluse = (Str-StrBase) + (Int-IntBase) + (Dex-DexBase) + (Con-ConBase)
3. Compute leveluse by tier-specific formula (Mortal/Arch/Celestial variants).
4. If totaluse > leveluse: remove excess from Str, else Int, else Dex, else Con.
5. Recompute totaluse after correction.
6. ScoreBonus = leveluse - totaluse; return TRUE.
```

---

### Business Rule: Skill Point Allocation (SkillBonus)

**Overview:**

`BASE_GetBonusSkillPoint` (Basedef.cpp:810) derives how many unspent skill points a character has. A character earns skill points from leveling and its class-master tier; it spends them by learning skills (bitfield `LearnedSkill`). The function compares the earned pool against the summed cost of all learned skills.

**Detailed description:**

The earned pool (`spellperlevel`) starts at `level * 3` points. An **Arch** character gains an additional `112` points; any non-Arch, non-Mortal character (i.e. Celestial tiers) gains `1500` extra points to account for the additional skill trees. Points beyond level 199 add `(level-199)` more. Completing the "PilulaOrc" Mortal quest adds `9` points (Basedef.cpp:825-828). The function then iterates over the 24 skill slots (`MAX_SKILL`) for the character's class, and for each learned skill (`LearnedSkill & skillbit`) sums the `SkillPoint` cost from the `g_pSpell[]` table at index `(cls * MAX_SKILL) + j`. The remaining pool is `spellperlevel - spelluse`; if nonzero it is written to `mob->SkillBonus` and the function returns `FALSE` (meaning "there are points remaining"). If exactly zero, it returns `TRUE` (all points spent). This invariant is used to prevent characters from learning more skills than their progression allows.

**Rule workflow:**

```
1. spellperlevel = Level*3 (+112 Arch, +1500 Celestial, +(Level-199) past 199, +9 if PilulaOrc quest).
2. Sum SkillPoint cost of all learned skills (bitfield).
3. rest = spellperlevel - spelluse.
4. If rest != 0: SkillBonus = rest; return FALSE.  Else return TRUE.
```

---

### Business Rule: HP/MP Growth Formula

**Overview:**

`BASE_GetHpMp` (Basedef.cpp:1174) computes a character's maximum HP and MP from its class, Constitution/Intelligence, and level. This is the fundamental stat-derivation rule that defines how robust a character is.

**Detailed description:**

For HP, `basehp = BaseSIDCHM[cls][4]` (the class's starting HP: 80/60/70/75 for TK/FM/BM/HT), `stathp = (Con - ConBase) * 2` (each Constitution point above base adds 2 HP), and `levelhp = effectiveLevel * g_pIncrementHp[cls]` where the increment is 3/1/1/2 HP per level. The effective level is the raw `CurrentScore.Level` for Mortal/Arch, but `Level + MAX_LEVEL` for the higher Celestial tiers, effectively granting them a large bonus pool. The sum is capped at `MAX_HP` (2,000,000,000). The same structure applies to MP using Intelligence and `g_pIncrementMp[]` (1/3/2/1). Both `BaseScore.MaxHp/MaxMp` and `CurrentScore.MaxHp/MaxMp` are set, keeping the base and the derived current values synchronized.

**Rule workflow:**

```
1. Validate class.
2. MaxHp = classBaseHp + (Con-ConBase)*2 + effectiveLevel * hpIncrement[cls].
3. Clamp MaxHp to MAX_HP.
4. MaxMp = classBaseMp + (Int-IntBase)*2 + effectiveLevel * mpIncrement[cls].
5. Clamp MaxMp to MAX_MP; write both base and current.
```

---

### Business Rule: Physical Damage Calculation

**Overview:**

`BASE_GetDamage` (Basedef.cpp:1225) and its skill variant `BASE_GetSkillDamage` (Basedef.cpp:1446) define how much damage an attack deals given raw damage, the target's armor class (AC), and a "combat" modifier. This is the core combat resolution formula shared by melee and many skill attacks.

**Detailed description:**

The physical variant computes `tdam = dam - ac/2` (armor halves its value as mitigation), then clamps `combat` to 7 and produces a random damage roll `rnd = rand() % (12 - combat) + combat + 99`, giving a percentage between roughly 99% and 111% of the raw value scaled by the combat level. The result is then shaped by three bands: if below -50 it is zeroed; if between -50 and 0 it is boosted by 50 and divided by 7 (a small-grace for low rolls); if between 0 and 50 it is scaled by 5/4 plus 7 (a small boost for weak hits). The final damage is floored at 1 so no valid hit deals zero damage. The skill variant uses a similar structure with a combat cap of 15, a `delta = 21 - combat` roll, and different band constants (`+=5`, `/=10`), reflecting that skills have a wider and slightly different damage distribution than basic attacks.

**Rule workflow:**

```
1. tdam = raw - AC/2; combat = min(combat, 7)  [15 for skills].
2. rnd = rand() % (12-combat) + combat + 99.
3. tdam = rnd * tdam / 100.
4. Shape result by bands (<-50 -> 0; -50..0 -> +50 then /7; 0..50 -> *5/4 then +7).
5. Clamp to >= 1; return.
```

---

### Business Rule: Item Ability Aggregation

**Overview:**

The family of functions `BASE_GetItemAbility` (Basedef.cpp:1647), `BASE_GetItemAbilityNosanc`, `BASE_GetStaticItemAbility`, `BASE_GetBonusItemAbility`, and `BASE_GetBonusItemAbilityNosanc` compute the effective value of a single item attribute (`Type`, an `EF_*` constant from `ItemEffect.h`). This is the canonical way the rest of the codebase reads "how much damage/AC/HP/MP/level/requirement does this item provide."

**Detailed description:**

Given an item index, the function aggregates three sources: (a) the item's base/static effects from the `g_pItemList[]` table (loaded from `itemlist.csv`), (b) the item instance's up-to-three `stEffect[]` slots, and (c) special-case handling for particular item ranges. Requirement-type queries (`EF_LEVEL`, `EF_REQ_STR/INT/DEX/CON`, `EF_POS`, `EF_CLASS`) read directly from the item-list table. The main aggregation sums matching effects and then, unless the attribute is excluded (`EF_GRID`, `EF_CLASS`, `EF_POS`, `EF_WTYPE`, `EF_RANGE`, `EF_LEVEL`, requirements, `EF_VOLATILE`, `EF_INCUBATE`, etc.), **scales the value by the sanctification level**: `value = value * (sanc + 10) / 10`. Resistance attributes (`EF_RESIST1..4`) additionally accumulate `EF_RESISTALL` contributions. Special item ranges are handled distinctly: mount items (index 2330-2390 and 3980-3994) return mount-specific values (`EF_MOUNTHP`, `EF_MOUNTSANC`, `EF_MOUNTLIFE`, `EF_MOUNTFEED`, `EF_MOUNTKILL`) and scaled damage/magic from the `g_pMountBonus`/`g_pMountTempBonus` tables. An `EF_RUNSPEED` value of 3+ is clamped to 2, and a sanc-9 item adds one more.

**Rule workflow:**

```
1. Validate item index bounds.
2. Handle mount-specific item ranges early (EF_MOUNT* and damage/magic/resist).
3. Sum static effects from g_pItemList plus instance stEffect[] slots for Type.
4. Add RESISTALL contributions for resistance attributes.
5. Determine sanc via BASE_GetItemSanc; for non-excluded types scale by (sanc+10)/10.
6. Apply EF_RUNSPEED clamp; return value.
```

---

### Business Rule: Item Sanctification (Upgrade) System

**Overview:**

Item sanctification (referred to as "sanc") is the game's upgrade/enhancement system. Several functions implement it: `BASE_GetItemSanc` (Basedef.cpp:2095) reads the current sanc level from an item's effect slots, `BASE_GetItemSancSuccess` (2177) reads the success counter, `BASE_GetSuccessRate` (2202) computes the chance to upgrade, `BASE_GetGrowthRate` (2227) computes mount growth chance, and `BASE_SetItemSanc` (2239) writes a new sanc/success value back into the item.

**Detailed description:**

The sanc value is stored as a special effect in one of the three `stEffect[]` slots, either as effect code `EF_SANC` (43) or within codes 116-125. The raw stored value encodes both the sanc tier and a success sub-counter: tiers 0-9 are stored directly (0-9), while "refined" tiers 10-15 are stored in the ranges 230-233, 234-237, 238-241, 242-245, 246-249, 250-253 (the `REF_10..REF_15` constants). `BASE_GetItemSanc` normalizes this back to a tier 0-15. `BASE_GetItemGem` (2143) extracts which gem slot (`(value-230) % 4`) is used for refined items. `BASE_GetSuccessRate` then computes the upgrade chance from the `g_pSancRate[3][12]` table indexed by upgrade "orient" (0=PO, 1=PL, 2=Amagos) and current sanc, plus a per-success bonus from `g_pSuccessRate[]`; orient 2 always yields 100%, and a REF_10 item with a non-Amagos orient returns 15% while orient 2 returns 100%. `BASE_SetItemSanc` validates that sanc is in [0,15] and success in [0,20], then encodes the new combined value, writing it back into whichever effect slot currently holds the sanc effect.

**Rule workflow:**

1. Read raw sanc from the item's `EF_SANC`/116-125 slot.
2. Normalize to tier 0-15 (with 230-253 mapping to refined tiers 10-15).
3. Compute success rate from orient + tier tables (orient 2 always 100%).
4. On upgrade, `BASE_SetItemSanc` validates bounds and encodes sanc + success back into the slot.

---

### Business Rule: Equipment Placement Validation

**Overview:**

`BASE_CanEquip` (Basedef.cpp:4293) determines whether a given item can be equipped by a character into a specific equipment slot (0-15). It enforces slot-appropriateness, class restrictions, mob-type restrictions, and level/stat requirements.

**Detailed description:**

The function rejects slot 15 outright and, for a non-default position, checks the item's `EF_POS` bitmask against the requested slot (`(tpos >> Pos) & 1`). For the two weapon hands (slots 6 and 7), it enforces a pairing rule: a two-handed weapon (pos 64) can only be paired with an empty/one-handed counterpart, and a unique-46 item on one hand requires the other hand to have pos 128 (and vice versa). It then checks the item's class bitmask (`EF_CLASS`) against the character's class, with special handling that non-Mortal characters use the `MortalFace`-derived class and only Mortal characters are strictly bound by class restrictions (non-Mortal can ignore class on weapon hands 6/7). Mob-type (`EF_MOBTYPE`) restrictions prevent Mortal items on non-Mortal characters and Celestial items on Mortal/Arch. Slot 1 for non-Mortal characters is restricted to specific item indexes (747 or 3500-3507). Finally, for Mortal characters, the item's level and Str/Int/Dex/Con requirements are checked against the character's score; for two-handed weapons (pos 7) the requirements are scaled up by 130% (light weapon) or 150% (heavy weapon) before comparison.

**Rule workflow:**

```
1. Validate item index; reject slot 15.
2. Check EF_POS bitmask vs slot; enforce two-handed weapon pairing on slots 6/7.
3. Check EF_CLASS bitmask vs character class (strict for Mortal, lenient on 6/7 for others).
4. Check EF_MOBTYPE tier restrictions.
5. Enforce slot-1 special restriction for non-Mortal.
6. For Mortal: enforce level + Str/Int/Dex/Con requirements (scaled for 2H).
```

---

### Business Rule: Carry and Cargo Grid Placement

**Overview:**

`BASE_CanCarry` (Basedef.cpp:4413) and `BASE_CanCargo` (Basedef.cpp:4480) validate whether an item fits into the character's inventory (carry) or storage (cargo) at a given grid position. The inventory is a 2D grid (carry 9x7, cargo 9x14) and each item occupies a 2x4 footprint defined by the `g_pItemGrid[8][4][2]` table (Basedef.cpp:106) selected by the item's `EF_GRID` attribute.

**Detailed description:**

For the carry case, the function builds a virtual occupancy grid (`CarryGrid`) by stamping every existing item's 2x4 footprint at its current slot position (converting linear slot index to `x = i % 9`, `y = i / 9`). It then stamps the source item's footprint at the requested `DestX/DestY`; if any occupied cell falls outside the grid bounds it returns `*error = -1`, and if it collides with an existing item it returns the colliding slot index in `*error`. The cargo variant is analogous but uses a single occupancy flag per cell (no per-slot reporting) and the 9x14 grid. These functions are the guards behind `MSG_TradingItem` (moving items between equip/carry/cargo), `MSG_GetItem`, and `MSG_DropItem` handlers, and are fundamental to inventory integrity.

**Rule workflow:**

1. Read the item's `EF_GRID` and look up its 2x4 footprint in `g_pItemGrid[]`.
2. Stamp all existing items' footprints into a virtual occupancy grid.
3. Stamp the source item at DestX/DestY.
4. If out-of-bounds -> error -1; if colliding -> return colliding slot (carry) or FALSE (cargo).
5. Otherwise TRUE.

---

### Business Rule: Character Name / String Validation

**Overview:**

`BASE_CheckValidString` (Basedef.cpp:2522) validates character names (and related user-supplied strings) at creation/login time, enforcing length, allowed character set, and a blocklist of reserved command words.

**Detailed description:**

The function rejects names shorter than 4 or at/above `NAME_LENGTH` (16). It then rejects a hardcoded blocklist of command/reserved names (e.g. `Reino`, `create`, `subcreate`, `gritar`, `king`, `kingdom`, `getout`, `gfame`, `expulsar`, `summonguild`, `summon`, `time`, `relo`, `stopally`, `stopwar`) to prevent command injection/name collisions with in-game commands. Each byte of the name must be a letter (A-Z, a-z), a digit, or a hyphen; multi-byte (Korean) characters, detected by a negative signed `char`, are skipped in pairs (the adjacent continuation byte). The companion `BASE_CheckHangul` (2553) specifically detects Korean syllables in the range 0xB0-0xC9 high byte / 0xA1-0xFF low byte so that hangul names are permitted. This is the security and integrity gate for all user-created character identifiers.

**Rule workflow:**

1. Length must be 4..15.
2. Reject reserved command names (blocklist).
3. Each byte must be alphanumeric or hyphen; skip Korean double-bytes.
4. Return TRUE if all pass.

---

### Business Rule: Combat Hit Rate and Accuracy

**Overview:**

`BASE_GetHitRate` (Basedef.cpp:5423), `BASE_GetDamageRate`, `BASE_GetAccuracyRate`, and `BASE_GetDoubleCritical` implement the probabilistic accuracy/critical subsystem of combat.

**Detailed description:**

`BASE_GetHitRate` computes the chance to land a hit from the attacker's and defender's Dexterity and Level: `parta = 60*attDex/(defDex/2 + attDex)` and `partb = 100*(defLvl+attLvl)/defLvl`, with `rate = partb*parta/100`, clamped to [40, 95]. If either side has zero Dex or Level it returns a 95% cap; negative values return 0%. `BASE_GetAccuracyRate` is a simpler `Dex/2 + 50` clamped to 100. `BASE_GetDoubleCritical` (5466) evaluates whether an attack is a double-strike and whether it is critical: it uses the `g_pHitRate[]` table (initialized in `BASE_InitializeHitRate`, 5244, to a deterministic pseudo-random distribution) indexed by a progress counter, and compares against `hitvalue[0]` (derived from attack-run/speed) for the double-hit bit and a `rand()%255 < mob->Critical` roll for the critical bit, OR-ing both into `*bDoubleCritical`. This is the per-attack randomness backbone used by the `MSG_Attack` handler.

**Rule workflow:**

1. Compute hit rate from Dex/Level ratio, clamp to [40,95].
2. Compute damage rate (special + skill) clamped to [0,200].
3. Compute accuracy rate as Dex/2+50 clamped to 100.
4. For double/critical: compare hit table value and a rand() roll to thresholds, set two flag bits.

---

### Business Rule: Soul and Affect Modifiers

**Overview:**

Within `BASE_GetCurrentScore` (Basedef.cpp:2973), the "Soul" system and per-affect status modifiers adjust a character's effective attributes during combat/inventory recomputation. A `Type == 29` affect activates the Soul bonus.

**Detailed description:**

Depending on the class-master tier, a Soul multiplies the character's Intelligence and/or Constitution by tier-specific factors: Celestial tiers use up to 2.2x, while Mortal uses up to 1.8x (and a sub-369 Mortal with the Kibita soul simply flags `isKibitaSoul`). These scaled attributes are then folded back into the current MaxHp/MaxMp via `HPDelta = ItemCon*2` and `MPDelta = ItemInt*2`. Separately, the function processes each of the 32 active affects (`Affect[i]`), applying dozens of typed effects: movement slow/haste, resistance drains, damage multipliers, Dex percentage changes, AC buffs/drains, HP sacrifices, special-point boosts, transformation (beast-master form) with its own stat/damage/sanc computation, HP/MP conversions, and reserved flags (`RSV_*`). Each effect type (1-42) has a bespoke formula, many scaling with `Level` and `Value`. This single function is the master aggregation point that turns base stats + equipment + affects + soul into the effective combat statistics.

**Rule workflow:**

1. Copy BaseScore to CurrentScore preserving current Hp/Mp.
2. Add equipment-derived abilities (AC, damage, HP/MP, Str/Int/Dex/Con).
3. If not a summon, apply Soul multipliers to Int/Con and fold into HP/MP deltas.
4. Clamp specials to 320 (and 400 under affect 15).
5. Iterate all 32 affects applying their typed formulas (move, damage, resist, transformation, etc.).
6. Finalize MaxHp/MaxMp (capped at MAX_HP/MAX_MP) and output derived bonus pointers.

---

### Business Rule: Skill Damage Computation

**Overview:**

`BASE_GetSkillDamage` (Basedef.cpp:6342) computes the damage dealt by a specific skill invocation given the skill number, the casting mob, the current weather, and weapon damage.

**Detailed description:**

The function reads the skill's `InstanceType` from `g_pSpell[]` and classifies it. For `InstanceType == 0` it uses per-skill hardcoded formulas (e.g. skill 11: `special/10 + affectbase`; skill 13: `3*special/4 + affectbase`; skill 41: `special/25 + 2`; skill 43: `special/3 + affectbase`; skill 44: `2*(3*special/20 + affectbase)`; skill 45: `special/10 + affectbase`). For `InstanceType 1..5` (weapon/skill damage) it branches by class: TK (`Class 0`) uses `3*weaponDamage + 3*Str + level + special + base` for the physical skill kind, and an Int-scaled formula otherwise; Foema/BeastMaster (1/2) use `Int/30 + Int/3 + level + base + 2*special`; Huntress (3) uses `3*weaponDamage + 3*Str + level/2 + special + base`. Weather modifies this: rain reduces instance-2 skills to 90% and boosts instance-5 to 130%; thunder boosts instance-3 to 120%. A `Magic`-scaled multiplier and a 5/4 factor are applied for non-physical classes. Other instance types use special-only or flat formulas, and skill 79 returns raw `CurrentScore.Damage`. This is the authoritative per-skill combat math used by `MSG_Attack`/`_MSG_CombineItem` contexts.

**Rule workflow:**

1. Read InstanceType and base/affect values from g_pSpell[skillnum].
2. Branch by InstanceType (0, 1-5, 6, 11, else).
3. Apply class-specific Str/Int/weaponDamage/special formulas.
4. Apply weather modifiers (rain/thunder).
5. Apply Magic multiplier and 5/4 factor for appropriate classes; special-case skill 79.

---

### Business Rule: Movement Routing

**Overview:**

`BASE_GetRoute` (Basedef.cpp:5538) computes a pathfinding route (an array of direction characters) from a starting cell to a target cell over a height-map, producing the `Route[]` used in `MSG_Action` movement packets.

**Detailed description:**

Given start `(x,y)` and target `(tx,ty)`, the function iterates up to `min(distance, MAX_ROUTE-1)` steps. At each step it reads the height map at the current cell and its 8 neighbors (north, north-east, east, ...) from the `pHeight` buffer, indexed relative to the global `g_HeightPosX/Y` and `g_HeightWidth/Height`. A move in a given direction is only allowed if the destination cell's height is within `MH` (8) units of the current cell's height, i.e. the terrain slope is traversable. The function greedily chooses the direction toward the target (preferring diagonal/straight moves: '1'=NW, '2'=N, '3'=NE, '4'=W, '6'=E, '7'=SW, '8'=S, '9'=SE), stopping when it reaches the target (Route[i]=0) or exits the map bounds. This produces a byte string of direction codes that the client replays to animate movement.

**Rule workflow:**

1. Validate bounds; zero the route buffer.
2. For each step up to distance: read 8 neighbor heights.
3. If at target or out of bounds, stop.
4. Else greedily pick the next direction whose neighbor height is within MH; emit its code.
5. Advance x/y; repeat.

---

### Business Rule: Item Expiry and Fairy Countdown

**Overview:**

`BASE_SetItemDate` (Basedef.cpp:6732), `BASE_CheckItemDate` (6763), and `BASE_CheckFairyDate` (6782) manage time-limited items: setting an expiry date on an item, checking whether it has expired, and decrementing a fairy companion's remaining life.

**Detailed description:**

`BASE_SetItemDate` computes a future date by adding `day` to the current local time, then encodes day/month/year into the item's three effect slots (`EF_WDAY`, `EF_WMONTH`, `EF_YEAR`) with a simplified 30-day month model (February treated as 27 days, `month>=12` wraps to 0, and the year stored as `year-100`). `BASE_CheckItemDate` compares the current date against the encoded expiry, returning 1 (expired) if the current day/month/year is at/after the stored date, using three cascading comparisons (day+month+year, month+year, year). `BASE_CheckFairyDate` operates only on fairy items (index 3900-3913), treating the three effect values as day/hour/minute of remaining life; it decrements the countdown, and when it reaches zero (`day==0 && hour==0 && min<=1`) clears the item entirely (the fairy expires). This enforces time-based item lifecycles.

**Rule workflow:**

1. Set: compute expiry day/month/year from now + duration; encode into slots.
2. Check: compare now vs encoded expiry; return expired flag.
3. Fairy: decrement day/hour/minute; clear item at zero.

---

## 4. Component Structure

```
legacy/Code/
├── Basedef.h            # 2,505 lines - Shared contract header
│   ├── #define constants  # Server ports, limits, dimensions, class/tier IDs,
│   │                       #   soul IDs, quest/NPC/secret-room IDs, sanc refs
│   ├── STRUCT_* structs   # 24 domain structs (ITEM, MOB, MOBEXTRA, SCORE, AFFECT,
│   │                       #   ACCOUNTFILE, ACCOUNTINFO, ITEMLIST, SPELL, RANKING, ...)
│   ├── MSG_* packet types # 98 _MSG_* type constants + 100 packed message structs
│   │                       #   (MSG_AccountLogin, MSG_Attack, MSG_DBSaveMob, ...)
│   ├── BASE_* prototypes  # 99 function declarations (the component's public API)
│   └── extern globals     # g_pItemList, g_pSpell, g_pGuildName, g_pNextLevel, ...
└── Basedef.cpp          # 6,814 lines - Implementation of the BASE_* rule functions
    ├── Global data tables  # BaseSIDCHM, g_pSancRate, g_pSuccessRate, g_pItemGrid,
    │                       #   g_pDistanceTable, g_pFormation, g_pClanTable, ...
    ├── Character rules     # GetBonusScorePoint, GetBonusSkillPoint, GetHpMp
    ├── Item rules          # GetItemAbility(+variants), GetItemSanc/Gem/Success,
    │                       #   SetItemSanc, CanEquip/CanCarry/CanCargo, GetItemCode
    ├── Mob rules           # GetMobAbility, GetMaxAbility, GetCurrentScore, GetMobCheckSum
    ├── Combat rules        # GetDamage, GetSkillDamage, GetHitRate, GetDamageRate,
    │                       #   GetAccuracyRate, GetDoubleCritical
    ├── Movement rules      # GetRoute, GetDistance, GetDestByAction
    ├── String rules        # CheckValidString, CheckHangul, SpaceToUnderBar, ...
    ├── Name/guild rules    # GetGuildName, GetClientGuildName, guild/server init
    ├── Data loading        # ReadItemList, ReadSkillBin, ReadInitItem, ReadMessageBin,
    │                       #   InitializeItemList/EffectName/HitRate/Attribute, ...
    ├── Time rules          # SetItemDate, CheckItemDate, CheckFairyDate
    └── Misc                # CheckPacket (disabled), GetSum/GetSum2, GetHttpRequest
```

The component is organized as a flat pair of files with no internal sub-modules or classes
(only the `STRUCT_*` POD structs and free `BASE_*` functions). It sits at the root of the
shared `Code/` directory, alongside `CPSock` and `ItemEffect.h`, and is compiled into both
the `TMSrv` and `DBSrv` projects (referenced via their `.vcxproj`/`.sln`).

---

## 5. Dependency Analysis

### Internal Dependencies

`Basedef` is a leaf/shared library — it depends on almost nothing internal and is depended
upon by nearly everything.

```
TMSrv: ProcessClientMessage -> _MSG_* handlers -> BASE_* (Basedef)
      Server.cpp / SendFunc / GetFunc / CMob / CUser / MobKilled / CNPCGene / ...
      ProcessSecMinTimer / imple / CCastleZakum / CWarTower / CReadFiles
DBSrv: Server.cpp / CFileDB / CRanking / CReadFiles -> STRUCT_* + BASE_* (Basedef)
CPSock.cpp -> MSG_STANDARD (packet header defined in Basedef.h)
```

Dependency chain within Basedef: the `BASE_*` functions call each other (e.g.
`BASE_GetCurrentScore` -> `BASE_GetMobAbility` -> `BASE_GetItemAbility` ->
`BASE_GetItemSanc`; `BASE_CanEquip` -> `BASE_GetItemAbility`), and all read the shared
global tables (`g_pItemList`, `g_pSpell`, `g_pMountBonus`, `BaseSIDCHM`, `g_pSancRate`).

### External Dependencies

| Dependency | Type | Purpose |
|------------|------|---------|
| Windows SDK (`windows.h`, `Windows.h`, `windowsx.h`) | Platform API | Message boxes, file I/O, module/current-dir, time |
| C runtime (`stdlib.h`, `stdio.h`, `fcntl.h`, `io.h`, `string.h`, `time.h`, `sys/*`) | Standard library | File reading, string ops, `rand()`, `time()`, `sscanf`/`fgets` |
| `ItemEffect.h` | Shared header | Defines all `EF_*` item-effect constants used throughout the item rules |
| Data files (runtime) | External files | `itemlist.csv`, `extraitem.csv`, `ItemEffect.h`, `Guilds.txt`, `serverlist.txt`, `AttributeMap.dat`, skill/init-item/message bins |

No third-party frameworks, databases, or network libraries are used by `Basedef` itself; it
is pure Windows + C runtime + filesystem.

---

## 6. Afferent and Efferent Coupling

The "components" here are the individual `BASE_*` functions and `STRUCT_*`/`MSG_*` types
(the C-style programming paradigm has no classes; free functions and POD structs are the
units of coupling). Metrics are based on call-site counts across the 47 files that invoke
`BASE_*` (639 total call sites) plus intra-component reuse.

| Component (function/type) | Afferent Coupling | Efferent Coupling | Critical |
|---------------------------|-------------------|-------------------|----------|
| `BASE_GetItemAbility` | 93 | 2 (`GetItemSanc`, table) | High |
| `BASE_SetItemAmount` | 93 | 1 | Medium |
| `BASE_GetItemSanc` | 79 | 1 | High |
| `BASE_SetItemSanc` | 54 | 1 | Medium |
| `BASE_GetItemCode` | 43 | 1 | Low |
| `BASE_ClearItem` | 37 | 0 | Medium |
| `BASE_GetGuildName` | 25 | 2 | Medium |
| `BASE_GetVillage` | 19 | 1 | Low |
| `BASE_GetMobAbility` | 13 | 3 | High |
| `STRUCT_MOB` / `STRUCT_ITEM` / `MSG_*` | 34 files include Basedef.h | 0 | Critical |
| `BASE_GetCurrentScore` | low (called by TMSrv) | very high (many sub-calls) | High |

**Observations:** The item-abstraction functions (`GetItemAbility`, `GetItemSanc`,
`SetItemSanc`, `SetItemAmount`) form a small, intensely reused hot cluster — together they
account for ~320 of the 639 external call sites, making them the highest-risk, most
central rules. The domain structs and message types have maximal afferent coupling (every
module) but zero efferent coupling. `BASE_GetCurrentScore` has the highest efferent
coupling within the component, aggregating many sub-rules.

---

## 7. Endpoints

This component **does not expose any network endpoints** (REST/GraphQL/gRPC/HTTP). It is a
shared definitions + rule library, not a service. All actual endpoints belong to the
`TMSrv` and `DBSrv` executables that consume `Basedef`. The only "interface" `Basedef`
exposes is its function API (`BASE_*`) and its wire-format message structs, which are not
served by this component directly. (Per the analysis guidelines, this section is omitted
because no endpoints exist on this component.)

---

## 8. Integration Points

`Basedef` integrates with other system parts through compile-time linkage (shared header)
and runtime data files, not through network protocols.

| Integration | Type | Purpose | Protocol / Format | Error Handling |
|-------------|------|---------|-------------------|----------------|
| `TMSrv` executable | Internal (compile-time) | Consumes `STRUCT_*`, `MSG_*`, `BASE_*` | C++ header + function calls | `BASE_*` return FALSE / 0 on invalid input |
| `DBSrv` executable | Internal (compile-time) | Persists `STRUCT_ACCOUNTFILE` layout; uses `BASE_*` for data rules | C++ header + function calls | Bounds checks before array access |
| `ItemEffect.h` | Internal (compile-time) | Provides `EF_*` effect constants | C header | N/A |
| `itemlist.csv` / `extraitem.csv` | External data | Populates `g_pItemList` item definitions | Comma-separated text | Missing file -> message box + abort init |
| `Guilds.txt` | External data | Populates guild name table | Space-delimited text | Missing/parse-error -> message box |
| `serverlist.txt` | External data | Populates server address list | Space-delimited text | Missing file -> message box + return FALSE |
| `AttributeMap.dat` | External data | Height/attribute map for movement | Binary (1024x1024) | Missing file -> message box + return FALSE |
| Skill/InitItem/Message bins | External data | Loads spells, starting items, localized strings | Binary | Read failure handled per function |
| Windows FS + time | Platform | Current directory, file I/O, `time()`/`localtime` | Win32 / CRT | Message boxes; `rand()` without seed control |

Error handling is uniformly primitive: invalid inputs return sentinel values (`FALSE`/0),
missing data files trigger blocking `MessageBox` dialogs, and there is no logging,
exception handling, or recovery path.

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Shared Contract (Header-Defined Protocol) | `MSG_*` structs + `FLAG_*` direction bits | Basedef.h:923-2240 | Wire protocol between client/TMSrv/DBSrv/NPTool |
| Free-Function Rule Library | `BASE_*` helper functions over POD structs | Basedef.cpp throughout | Shared deterministic game rules |
| Lookup-Table Driven Logic | `g_pSancRate`, `g_pSuccessRate`, `g_pItemGrid`, `g_pDistanceTable`, `BaseSIDCHM`, `g_pMountBonus` | Basedef.cpp:39-171 | Replace branching with data tables for game constants |
| Bitmask Encoding | `EF_POS`/`EF_CLASS` slot/class bitmasks, `LearnedSkill` bitfield, `RSV_*` flags | Basedef.h:184-192, ItemEffect.h | Compact representation of restrictions and learned skills |
| Inline Fixed-Layout Structs | `#pragma pack(push,1)` on wire structs; `_MSG` macro for the common header | Basedef.h:808, 925-930, 1212, 1465 | Match the exact network/persistence byte layout |
| Union/Packed Effects | `STRUCT_ITEM.stEffect[]` union of value/effect-pair | Basedef.h:398-412 | Compact item stat storage |
| Global Singleton State | `extern` global tables (`g_pItemList`, `g_pSpell`, ...) | Basedef.h:2441-2502 | Shared, implicitly available data across both servers |
| Constructor-Initialized Wire Structs | `MSG_SendExpRanking`/`MSG_UpdateExpRanking` C++ ctors set Type/ID/Size | Basedef.h:2196-2233 | Correctly frame outbound packets |

**Architectural decisions:** The defining decision is fusing the packet protocol, the
domain model, and the game constants into a single shared header compiled into both
binaries. This provides a single source of truth and avoids IDL/versioning overhead, at the
cost of lockstep rebuilds and a hard coupling between network format and memory format.
Probabilistic rules are driven by `rand()` and precomputed tables rather than
deterministic/config-driven engines. The single-threaded cooperative model (from the
broader architecture) means these pure functions are always called without locking.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | `BASE_CheckPacket` (Basedef.cpp:6475) | Entire packet-size validation body is commented out; function returns `FALSE` (0) unconditionally | No release-build validation of incoming packet sizes; malformed packets cast to structs can cause overreads/overwrites |
| High | Shared header contract (Basedef.h) | `STRUCT_*`/`MSG_*` fused into one header compiled into both binaries | Any layout change silently changes the wire/disk format for both servers; requires lockstep rebuild; no versioning |
| High | Character stat/skill rules (Basedef.cpp:858, 810) | Complex tier-specific formulas with many magic thresholds and over-allocation correction paths | High risk of divergence from intended progression; hard to audit; over-allocation logic is fragile |
| High | `BASE_GetCurrentScore` (Basedef.cpp:2973) | ~1,140-line monolith handling stats, souls, and dozens of affect types with bespoke formulas | Single point of failure for all effective-stat computation; very high complexity, hard to reason about |
| Medium | `rand()`-based probabilities | Sanctification, hit rate, critical, damage all use `rand()` with no seeding/control (Basedef.cpp:1235, 1454, 5523) | Non-reproducible behavior; potential exploitability of probabilistic outcomes |
| Medium | Global mutable tables | Large `extern` globals (`g_pItemList`, `g_pSpell`, `g_pGuildName`) shared implicitly | Hidden coupling; any mutation is global; no ownership/immutability |
| Medium | Hardcoded magic numbers | Item indexes (e.g. 2330-2390 mounts, 3900-3913 fairies, 3980-3994), clan IDs, secret-room IDs inline | Maintenance burden; fragile if data changes |
| Medium | Undocumented/unknown fields | Many `Unk*`, `EMPTY[]`, `Rsv`, `unk[]` padding fields in structs (STRUCT_BEASTBONUS, STRUCT_MOBEXTRA) | Opaque data; risk of format drift and misunderstanding |
| Low | Data-file load failures | Missing `itemlist.csv`/`Guilds.txt`/`serverlist.txt`/`AttributeMap.dat` cause blocking `MessageBox` + abort | Boot fragility; no graceful degradation |
| Low | Date arithmetic | Simplified 30-day month model and `year-100` encoding (Basedef.cpp:6732) | Date edge cases (Feb, month-end) may be miscalculated |
| Low | `BASE_GetLanguage` uses `sprintf` with data-driven format strings | Potential format-string misuse if table entries contain `%` | Possible buffer/format issues on malformed localization data |

---

## 11. Test Coverage Analysis

**No test files were found anywhere in the project.** A recursive search across
`/home/luisdias/dev/github/luisdiasdev/w2pp` (excluding `.git` and `.opencode`) for
`*test*` returned no results. Neither `TMSrv`, `DBSrv`, nor the `Basedef` component has any
unit, integration, or coverage tests.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `BASE_GetBonusScorePoint` | 0 | 0 | 0% | No tests; complex tier formulas untested |
| `BASE_GetBonusSkillPoint` | 0 | 0 | 0% | No tests |
| `BASE_GetHpMp` | 0 | 0 | 0% | No tests |
| `BASE_GetDamage` / `GetSkillDamage` | 0 | 0 | 0% | No tests; `rand()` makes deterministic testing hard |
| `BASE_GetItemAbility` / `GetItemSanc` / `SetItemSanc` | 0 | 0 | 0% | No tests; highest fan-in cluster untested |
| `BASE_CanEquip` / `CanCarry` / `CanCargo` | 0 | 0 | 0% | No tests; placement/requirement rules untested |
| `BASE_GetCurrentScore` | 0 | 0 | 0% | No tests; largest function untested |
| `BASE_GetRoute` | 0 | 0 | 0% | No tests |
| `BASE_CheckValidString` | 0 | 0 | 0% | No tests; security gate untested |

**Risk note:** The complete absence of tests for the most-reused, highest-complexity rules
in the codebase (item ability aggregation, sanctification, stat/skill allocation, combat
math) is a significant risk. Any behavioral change to these functions is verified only
through manual gameplay. This aligns with the architectural report's finding that the
codebase has no automated testing.

---

## 12. Conclusion

`Basedef` is the architectural keystone of the W2PP legacy server: it is the single shared
source for the network protocol, the persistent domain model, and the core gameplay rules,
compiled into both `TMSrv` and `DBSrv`. Its 99 `BASE_*` functions encode nearly all of the
game's deterministic logic (stat growth, item upgrades, equipment/inventory placement,
combat math, movement routing, name validation, and time-based item lifecycles), and it is
consumed via 639 call sites across 47 files.

The component is well-contained in terms of outward dependencies (pure Windows + C runtime
+ filesystem) but carries the highest afferent coupling in the system, making it the
primary stability point and the highest-risk file to change. Key concerns are the disabled
packet validation (`BASE_CheckPacket`), the monolithic `BASE_GetCurrentScore`, the
hardcoded/magic-number-heavy rules, `rand()`-based probabilities, and a complete absence of
test coverage. The analysis is read-only; no project files were modified.

---

## Limitations

- Coupling metrics are relative estimates derived from call-site counts, not an exhaustive
  static-coupling enumeration.
- Several business rules are implicit (documented only by variable names and partial
  Korean/Portuguese comments); confidence is noted per rule where applicable. The
  `Unk*`/`EMPTY[]` fields indicate that parts of the domain model are not fully understood.
- No test files exist to validate the documented rules against expected behavior.
- The analysis covered only the `Basedef` component boundary as specified; interactions
  with consuming components were noted but not deeply analyzed.
