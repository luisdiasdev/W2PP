# Component Deep Analysis Report

## Component: GetFunc

**Project scope:** `legacy/` (W2PP C/C++ codebase)
**Location:** `legacy/Code/TMSrv/GetFunc.cpp`, `legacy/Code/TMSrv/GetFunc.h`
**Analyzed:** 2026-08-19 17:13:23
**Folders ignored:** `.git`, `.opencode`

---

## 1. Executive Summary

`GetFunc` is a C/C++ translation-unit (implemented in the files `GetFunc.cpp` and declared in `GetFunc.h`) that serves as the **shared game-logic helper library** for the W2PP (Wild West Online private server) `TMSrv` (Trade Manager Server) process. It exposes a flat collection of 34 free functions that are invoked as utility/getter/formatting routines throughout the server. It is not a service, class, or module with its own state; rather, it is a procedural helper layer that reads global in-memory server state (`pMob`, `pUser`, `pItemGrid`, `pHeightGrid`, `pMobGrid`, `g_pItemList`, `g_pSpell`, `g_pAttribute`, `g_pGuildZone`) and produces values or network message payloads consumed by other TMSrv files.

The component fulfills five broad responsibilities:

1. **Item-combination recipe validation** — A family of `GetMatchCombine*` functions that validate whether a set of 8 items in a combination window matches a known crafting/refinement recipe and returns a success-rate threshold.
2. **Combat scoring helpers** — `GetParryRate`, `GetAttack`, `GetAttackArea`, `GetAttribute`, `GetExpApply` that compute parry rates, damage, area damage, terrain attributes, and experience scaling.
3. **World/positioning helpers** — `GetTeleportPosition`, `GetInView`, `GetInHalf`, `GetEmptyItemGrid`, `GetEmptyMobGrid`, `GetEmptyMobGridGreat` that handle teleport coordinate resolution and free-grid scanning.
4. **Server message packers** — `GetCreateMob`, `GetCreateMobTrade`, `GetCreateItem`, `GetAction`, `GetAffect`, `GetAttack`, `GetAttackArea` that populate outgoing network message structs (`MSG_CreateMob`, `MSG_CreateItem`, `MSG_Action`, `MSG_AttackOne`, `MSG_Attack`) for broadcast to clients.
5. **Player-meta persistence accessors** — `GetCurKill`, `GetTotKill`, `SetCurKill`, `SetTotKill`, `GetPKPoint`, `GetGuilty`, `SetGuilty`, `SetPKPoint` that read/write kill counts, PK points, and guilt state packed inside a `KILL_MARK` carry item's effect bytes.

**Key findings:**

- **No object-oriented structure.** The component is entirely free functions over global state (procedural C-style code compiled as C++). There are no classes, namespaces, or interfaces defined within `GetFunc` itself.
- **Heavy dependence on global mutable state.** Every function reads from module-level globals (`pMob`, `pUser`, grids). This is a monolithic server architecture with no dependency injection or isolation.
- **Item IDs and recipe tables are hard-coded** as magic constants (e.g., item `747`, `413`, `697`, `2441-2444`, `5334-5337`, rune sequences `5110-5133`). Recipe membership is not data-driven, which makes balance tuning a code-editing task.
- **Zero automated tests.** No unit, integration, or regression tests exist anywhere in the repository for this component (see Section 11).
- **Dead/incomplete code observed:** `GetGuild(int conn)` at `GetFunc.cpp:1862` declares local variables and performs no work; `GetHide` and `GetGuild(STRUCT_ITEM*)` appear to have no in-repo callers.
- **Several implicit, undocumented business rules** (e.g., experience caps at level 399, teleport charge of 700 coins, chaos/PK clamping at 150, guilt reset at 50) are embedded as magic numbers with only partial inline comments.
- **Windows-specific.** Includes `<Windows.h>` and uses `BOOL`, `FALSE`, `TRUE`; the server targets Win32/x64 via the Visual Studio solution.

The component is a **high-fan-out utility layer**: it is depended upon (afferent coupling) by nearly every major TMSrv message handler and timer, yet it itself depends (efferent coupling) almost entirely on the shared `Basedef.h`/`Server.h` global state and the `BASE_*` helper functions defined in `Basedef.cpp`.

---

## 2. Data Flow Analysis

`GetFunc` has no single entry point; data flows through it in several independent logical paths. The dominant flows are described below.

**Flow A — Item combination / crafting (GetMatchCombine*):**

```
1. Client sends MSG_CombineItem -> Exec_MSG_CombineItem (TMSrv/_MSG_CombineItem.cpp:21)
2. Per-slot inventory validation (index range, item immutability)  (_MSG_CombineItem.cpp:25)
3. GetMatchCombine(m->Item) validates recipe + returns success rate   (GetFunc.cpp:33)
4. rate == 0 -> "wrong combination" rejected                          (_MSG_CombineItem.cpp:48)
5. rate > 0 -> items consumed; rand()%115 transformed; compare to rate (_MSG_CombineItem.cpp:80)
6. success -> item transform + SetItemSanc; failure -> complete signal (GetFunc.cpp output consumed)
```

**Flow B — Combat damage/attack broadcast (GetAttack / GetAttackArea):**

```
1. Sec-timer / battle processor triggers attack for a mob     (TMSrv/ProcessSecMinTimer.cpp)
2. GetAttack(mob, target, &msg) builds MSG_AttackOne           (GetFunc.cpp:1275)
3. Mob skill selection (SkillBar + rand ranges) -> SkillIndex  (GetFunc.cpp:1307)
4. BASE_GetDamage -> base damage vs. AC                        (GetFunc.cpp:1505)
5. Resistance scaling, reflect, PvP/attribute, friendly-fire    (GetFunc.cpp:1508-1556)
6. Broadcast MSG_AttackOne to clients (via SendFunc elsewhere)
```

**Flow C — Teleport coordinate resolution (GetTeleportPosition):**

```
1. Client requests teleport -> Exec_MSG_ReqTeleport (TMSrv/_MSG_ReqTeleport.cpp)
2. GetTeleportPosition(conn, &x, &y) matches current coord mask  (GetFunc.cpp:658)
3. Sets new x/y (with rand()%3 scatter) and returns coin charge  (GetFunc.cpp:899)
4. Caller deducts coin and moves the player; on mismatch charge 0
```

**Flow D — Spawn/mob visibility packing (GetCreateMob / GetCreateItem / GetAction / GetAffect):**

```
1. Spawn or state change event in Server.cpp / MobKilled.cpp / imple.cpp
2. GetCreateMob(mob, &sm) reads pMob[mob] -> packs MSG_CreateMob  (GetFunc.cpp:947)
3. GetAffect(out, pMob[mob].Affect) encodes 32 affects into u16   (GetFunc.cpp:1174)
4. Broadcast to clients in view (SendFunc.cpp callers)
```

**Flow E — Player meta (kills/PK/guilt):**

```
1. Kill events in MobKilled.cpp -> SetCurKill / SetTotKill / SetPKPoint / SetGuilty
2. GetCreateMob reads them back and packs into MobName[12..15]    (GetFunc.cpp:955-976)
3. SendFunc.cpp / _MSG_MessageWhisper.cpp read GetPKPoint/GetGuilty for display
```

---

## 3. Business Rules & Logic

The business rules below are extracted from `GetFunc.cpp`. Rules that are implicit (no explicit comment) are marked with a confidence indicator: **(Implicit — high)** where the intent is unambiguous from the surrounding handler, **(Implicit — medium)** where the exact intent is inferred, and **(Explicit)** where a comment documents the rule.

### Overview of the business rules:

| # | Rule Type | Rule Description | Location |
|---|-----------|------------------|----------|
| BR-1 | Validation | Ancients combination rejects any recipe containing item 747 | GetFunc.cpp:37,112,171,221,265,381,431,502 |
| BR-2 | Validation | Ancients combination requires valid item indices (0 < idx < 6500) | GetFunc.cpp:43-49,116-138,175-182,225-245,269-288,385-401,435-457,505-527 |
| BR-3 | Business Logic | Ancients combination base item must be unique in [41,49] and have Extra > 0 | GetFunc.cpp:51-55 |
| BR-4 | Business Logic | Ancients combination forbidden for Mob-type (ARCH) base item | GetFunc.cpp:57 |
| BR-5 | Business Logic | Ancients combination rate accumulates from ancient-grade stones (sanc 7/8/9) | GetFunc.cpp:79-100 |
| BR-6 | Business Logic | Ancients combination requires stone item level >= base item level | GetFunc.cpp:73-77 |
| BR-7 | Business Logic | Ehre combination returns recipe ID (1..8) for 8 distinct craft recipes | GetFunc.cpp:140-163 |
| BR-8 | Business Logic | Tiny combination validates grade, item type, slot position, and sanc >= 9 | GetFunc.cpp:184-213 |
| BR-9 | Business Logic | Shany combination validates stone set (540/541/633 + four 413s) | GetFunc.cpp:246-256 |
| BR-10 | Business Logic | Ailyn combination validates grade-matched soul stones (2441-2444) | GetFunc.cpp:290-358 |
| BR-11 | Business Logic | Agatha combination validates arch + mortal item + four 3140 stones | GetFunc.cpp:403-423 |
| BR-12 | Business Logic | Odin combination returns recipe ID (1..11) for 11 distinct recipes | GetFunc.cpp:459-494 |
| BR-13 | Business Logic | Alquimia combination returns recipe ID (0..9) or -1 for 10 recipes | GetFunc.cpp:529-558 |
| BR-14 | Business Logic | Parry rate computed from Dex, add, and attacker dex with hard caps | GetFunc.cpp:562-607 |
| BR-15 | Business Logic | Empty affect slot: return existing type slot, else first Type==0 slot, else -1 | GetFunc.cpp:610-625 |
| BR-16 | Business Logic | Hide state = presence of affect type 28 | GetFunc.cpp:627-638 |
| BR-17 | Business Logic | In-view check uses symmetric VIEWGRID bounds (33x33) | GetFunc.cpp:640-647 |
| BR-18 | Business Logic | In-half check uses HALFGRID bounds (16x16) | GetFunc.cpp:649-656 |
| BR-19 | Business Logic | Teleport resolves ~30 fixed world-coordinate transitions; charge 700 for cross-realm | GetFunc.cpp:658-899 |
| BR-20 | Business Logic | Kefra teleport consumes a KefraTicket when entering desert | GetFunc.cpp:868-878 |
| BR-21 | Business Logic | RvR desert teleport splits spawn by clan (Blue=7 / Red=8) only when RvRState==2 | GetFunc.cpp:845-866 |
| BR-22 | Business Logic | Experience scaling caps per class; ARCH/CELESTIAL gate exp on quest flags | GetFunc.cpp:902-944 |
| BR-23 | Business Logic | Experience multiplier window: <80% doubles minus 100; >200% capped at 200 | GetFunc.cpp:932-942 |
| BR-24 | Business Logic | CELESTIAL classes are forced to attacker = MAX_LEVEL (399) | GetFunc.cpp:923-924 |
| BR-25 | Business Logic | Mount outfit (2360-2389) with no effect hides equipment and flags self-dead | GetFunc.cpp:1030-1037 |
| BR-26 | Business Logic | Mount outfit sanc is packed into high 12 bits, capped at 13 | GetFunc.cpp:1039-1055 |
| BR-27 | Business Logic | GuildLevel 9 sets 0x80; >=6 sets 0x40 create-type flag | GetFunc.cpp:1006-1011,1122-1126 |
| BR-28 | Business Logic | Hide mob name as "??????" in BrState (war) zone rectangle | GetFunc.cpp:1060-1067 |
| BR-29 | Business Logic | Guild zone flag item 3145 shows victor + charge; item 5700 hidden | GetFunc.cpp:1215-1230 |
| BR-30 | Business Logic | NPC skill selection by weighted rand ranges (0-49/50-84/85-99) | GetFunc.cpp:1443-1502 |
| BR-31 | Business Logic | Low-HP leader heal trigger (insttype 6) heals 10% of MaxHp | GetFunc.cpp:1462-1480 |
| BR-32 | Business Logic | Resistance scaling: damage * (200 - resist)/100 for resist 0-3 | GetFunc.cpp:1508-1509 |
| BR-33 | Business Logic | ReflectDamage reduces damage (min 1 in single, min 0 in area) | GetFunc.cpp:1511-1519,1810-1818 |
| BR-34 | Business Logic | PvP/attribute damage reduction: 30% and zeroed on non-combat attribute | GetFunc.cpp:1521-1531,1820-1830 |
| BR-35 | Business Logic | Friendly-fire prevention: same leader or same guild -> zero damage | GetFunc.cpp:1533-1556,1832-1855 |
| BR-36 | Business Logic | Empty item grid scans 3x3 neighborhood for free, walkable cell | GetFunc.cpp:1876-1899 |
| BR-37 | Business Logic | Empty mob grid scans expanding rings up to radius 4 (Great: radius 30) | GetFunc.cpp:1901-2044 |
| BR-38 | Business Logic | CurKill clamped to [0,200]; TotKill to [0,32767] | GetFunc.cpp:2076-2096 |
| BR-39 | Business Logic | Guilt reset to 0 when >50; PKPoint clamped to [1,150] | GetFunc.cpp:2120-2124,2145-2154 |
| BR-40 | Validation | Kill/PK accessors return 0 for invalid connection index | GetFunc.cpp:2048,2060,2131,2147 |

### Detailed breakdown of the business rules:

---

### Business Rule: BR-1 — Forbidden item 747 in combination

**Overview:** Every `GetMatchCombine*` variant rejects a combination that contains item index `747` in any of the `MAX_COMBINE` (8) slots. Item 747 is a special/cash-shop item that is not permitted as a crafting ingredient.

**Detailed description:**
This rule acts as the first-line guard in all eight combination validators (`GetMatchCombine`, `GetMatchCombineEhre`, `GetMatchCombineTiny`, `GetMatchCombineShany`, `GetMatchCombineAilyn`, `GetMatchCombineAgatha`, `GetMatchCombineOdin`, `GetMatchCombineAlquimia`). Each function iterates over all 8 slots of the input `STRUCT_ITEM Item[]` array and returns `0` (invalid) as soon as it finds `sIndex == 747`. The scan occurs before any other validation, so item 747 short-circuits the entire recipe check regardless of the other seven slots. Because all variants share the same loop pattern, any recipe that would otherwise consume an item 747 as an ingredient is silently rejected before reaching the recipe-specific logic. The constant is not named; the literal `747` is duplicated across all eight functions, which constitutes a maintenance risk if the forbidden item ever changes.

**Rule workflow:**
1. Iterate `i` from 0 to `MAX_COMBINE-1`.
2. If `Item[i].sIndex == 747`, return 0 (invalid).
3. Otherwise continue to the next validation stage.

---

### Business Rule: BR-5 — Ancients combination success-rate accumulation

**Overview:** `GetMatchCombine` (GetFunc.cpp:33) returns an integer "rate" that the caller (`_MSG_CombineItem.cpp:84`) compares against a random roll to determine craft success. The rate is accumulated based on the sanctification grade of each "ancient stone" ingredient.

**Detailed description:**
The base item must pass the unique/extras/type gates (BR-3, BR-4). Then, for each of the remaining slots `j` from 2 to `MAX_COMBINE-1`, the function requires the slot to hold a valid item index and a non-zero equipment position (`EF_POS`). The item level of each stone must be greater than or equal to the base item's level (BR-6). The stone's sanctification (`BASE_GetItemSanc`) determines the contribution: a sanc of 7 adds `g_pAnctChance[0]`, sanc 8 adds `g_pAnctChance[1]`, sanc 9 adds `g_pAnctChance[2]`, and any other sanc value invalidates the combination. `g_pAnctChance` is an `int[3]` global (declared `Basedef.h:2491`, default `{2,4,10}` in `Basedef.cpp` but overridable from data files). The returned rate starts at 1 and grows by the sum of the chance bonuses. The caller rolls `rand()%115` (adjusted by subtracting 15 if >=100, yielding an effective 0-99 range) and succeeds when the roll is `<= rate`, so a higher rate means a higher success probability. The interplay of per-stone bonuses is additive and linear.

**Rule workflow:**
1. Validate base item (index, unique 41-49, Extra>0, not ARCH).
2. For each slot j=2..7: require valid index, non-zero `EF_POS`, item level >= base level.
3. Require stone sanc to be exactly 7, 8, or 9; add the corresponding `g_pAnctChance` entry to `rate`.
4. Return `rate` (>=1). Any failed check returns 0.
5. Caller rolls random and compares to `rate` for success.

---

### Business Rule: BR-7 / BR-12 / BR-13 — Recipe-ID combination tables (Ehre/Odin/Alquimia)

**Overview:** Three of the combination validators (`GetMatchCombineEhre`, `GetMatchCombineOdin`, `GetMatchCombineAlquimia`) do not compute a probabilistic rate; instead they return a **recipe identifier** (0-based or 1-based) that identifies which crafting result was matched, or 0/-1 if no recipe matched.

**Detailed description:**
These functions are a sequence of `if/else-if` blocks, each encoding an exact recipe in terms of item indices, ranges, amounts, and sanctification requirements, returning a distinct integer constant for each match. `GetMatchCombineEhre` (GetFunc.cpp:106) returns values 1 through 8 for recipes such as the Oriharucon package (two `697` + sanc>=9 non-`3338`), Mysterious Stone (two `5110-5133` + `413` amount>=10), Spiritual stone, Amunra stone, purified blessed refinement, mount outfit (two `2360-2389` + `4190-4199`), mount restore (`4899`), and Soul (three `2441-2444`). `GetMatchCombineOdin` (GetFunc.cpp:427) returns 1 through 11 for celestial item, Ref+12+, rune track, celestial level 40, and several "secret" rune sequence recipes (e.g., Pedra da furia, Secreta da Agua/Terra/Sol/Vento), plus crystal seed and cape. `GetMatchCombineAlquimia` (GetFunc.cpp:498) returns 0 through 9 for alchemy recipes (Sagacidade, Resistencia, Revelacao, Recuperacao, Absorcao, Protecao, Poder, Armazenagem, Precisao, Magia) and returns -1 for invalid inputs. The returned recipe ID is consumed by the corresponding `_MSG_CombineItem*.cpp` handler to perform the actual item transformation. Note these functions return `0` for "no match" in Ehre/Odin but `-1` for "no match" in Alquimia, an inconsistency in the sentinel convention that the callers must handle differently.

**Rule workflow:**
1. Reject any slot containing item 747.
2. Validate all 8 slot indices are within `[0, MAX_ITEMLIST)`.
3. Evaluate recipe `if/else-if` chain; the first matching branch returns its recipe ID.
4. If no branch matches, return 0 (Ehre/Odin) or -1 (Alquimia).

---

### Business Rule: BR-14 — Parry rate computation

**Overview:** `GetParryRate` (GetFunc.cpp:562) computes the chance of a mob parrying an incoming attack from a piecewise-linear function of the defender's Dex, the attack add bonus, and the attacker's dex/accuracy, with hard clamps.

**Detailed description:**
The function clamps the `add` parameter to `[0,100]` and the defender's `Dex` to at most 1000. It then derives two surplus terms: `parryrate1 = Dex - 1000` clamped to `[0,2000]` and `parryrate2 = Dex - 3000` clamped to `>= 0`. The base parry rate is `Dex/2 + add + parryrate1/4 + parryrate2/8 - attackerdex`. The attacker's reserved-flag bitmask (`attackrsv`) further modifies the result: bits `0x20`, `0x80`, and `0x200` each add 100/50/50 respectively (these correspond to haste/block/imunidade flags). The final rate is clamped to `[1, 650]`, ensuring a minimum 1% and maximum 650 "parry weight". The function is used by `ProcessSecMinTimer.cpp`, `Server.cpp`, and `_MSG_Attack.cpp`. The `attackaccuracy` parameter is accepted but is not used in the computation, an observation of dead/misused input.

**Rule workflow:**
1. Clamp `add` to [0,100] and defender Dex to <=1000.
2. Compute surplus parry terms from Dex thresholds (1000, 3000).
3. Compute base parry rate = Dex/2 + add + surpluses - attacker dex.
4. Add flag bonuses from `attackrsv` bits.
5. Clamp result to [1,650] and return.

---

### Business Rule: BR-19 / BR-20 / BR-21 — Teleport coordinate resolution

**Overview:** `GetTeleportPosition` (GetFunc.cpp:658) maps the player's current grid coordinates (masked with `0xFFFC`) to a destination coordinate and a coin charge, encoding the game world's fixed teleporter network.

**Detailed description:**
The function begins by masking the input coordinates: `xv = (*x) & 0xFFFC`, `yv = (*y) & 0xFFFC`. It then evaluates a long chain of coordinate equality checks, each mapping a departure location to a destination plus a small `rand()%3` scatter. Cross-realm teleports (Armia/Azran/Erion to Noatum and reverse, and Karden) set `Charge = 700`, meaning the caller deducts 700 coins; intra-realm transitions set Charge to 0 (no cost). Several branches are conditional on world state: the Noatum-to-RvR-desert branch spawns the player on the Blue or Red side based on `pMob[conn].MOB.Clan == 7` (BLUE) or `== 8` (RED), but only when the global `RvRState == 2`; otherwise it falls back to a neutral spawn. The Kefra-desert branch (`GetFunc.cpp:868`) decrements `pMob[conn].extra.KefraTicket` when the player enters the desert and sends a client message with the remaining ticket count. The `KefraLive != 0` gate (GetFunc.cpp:881) controls desert-to-Kefra entry. Branches with no matching coordinate leave `*x`/`*y` and `Charge` untouched (Charge stays 0). Because coordinates are compared with `& 0xFFFC` masks, nearby tiles within the same 4-aligned cell are treated as the same teleport point.

**Rule workflow:**
1. Mask input coords with `0xFFFC`.
2. Match against teleporter coordinate table; if matched, set destination (with rand scatter) and charge.
3. Apply world-state branches (RvR clan split, KefraTicket decrement, KefraLive gate).
4. Return charge; caller moves player and deducts coins.

---

### Business Rule: BR-22 / BR-23 / BR-24 — Experience scaling

**Overview:** `GetExpApply` (GetFunc.cpp:902) scales raw experience based on class, level ratio between attacker and target, and quest-progression gates, then returns the final exp value.

**Detailed description:**
For an `ARCH` class master with positive exp, the function returns 0 (no exp) if the attacker level is >= 354 without the Level355 quest flag set, or >= 369 without the Level370 quest flag set, and otherwise halves the exp (`exp * 0.50`). For a `CELESTIAL` class master, it returns 0 if attacker >= 39 without the Celestial Lv40 flag, or >= 89 without the Lv90 flag. Any Celestial variant (`CELESTIAL`, `SCELESTIAL`, `CELESTIALCS`) forces `attacker = MAX_LEVEL` (399), meaning celestial players always experience the game against a level-399 reference. The function then guards against out-of-range levels and increments both attacker and target by 1. The multiplier is `multiexp = (target*100)/attacker`; if it is below 80 and attacker >= 50, it is transformed to `multiexp*2 - 100`; if above 200 it is capped at 200; and if below 0 it is zeroed. Final exp is `(exp * multiexp + 1) / 100`. The ceiling of 200 effectively caps a single kill's exp at double, and the low-level penalty can push the multiplier negative then to zero, granting no exp.

**Rule workflow:**
1. If ARCH and exp>0: gate on quest flags (354/369), halve exp.
2. If CELESTIAL and exp>0: gate on quest flags (39/89); force attacker = 399.
3. Reject out-of-range levels; increment attacker/target.
4. Compute level-ratio multiplier with the <80 and >200 corrections.
5. Scale exp and return.

---

### Business Rule: BR-25 / BR-26 / BR-27 / BR-28 — Mob spawn message packing rules

**Overview:** `GetCreateMob` (GetFunc.cpp:947) and `GetCreateMobTrade` (GetFunc.cpp:1072) build the outgoing `MSG_CreateMob`/`MSG_CreateMobTrade` spawn messages, applying several presentation rules.

**Detailed description:**
Both functions pack the player's kill counts into the `MobName` bytes: `MobName[13]` = current kill count, `MobName[14..15]` = total kills (split as `tk%256` and `tk/256`), and `MobName[12]` = chaos (PK point) value, which is forced to 0 when guilt (`GetGuilty`) is positive. The guild is cleared when `GuildDisable == 1`. For non-player mobs (`mob >= MAX_USER`), AC is forced to 0 for clan 4 or 1 otherwise. The `CreateType` flag is set to `0x80` for guild level 9 and `0x40` for guild level >= 6 (or != 0 in the trade variant). Equipment is converted via `BASE_VisualItemCode`/`BASE_VisualAnctCode`. For the mount outfit (equip slot 14, item `2360-2389`): if the first effect value is <= 0 the outfit is hidden (`Equip=0`) and the function returns a `selfdead=1` flag; otherwise the sanctification is read from `stEffect[1].cEffect`, divided by 10, clamped to `[0,13]`, shifted left 12 bits, and added to the visual item code. Finally, if the `BrState` (war) is active and the mob is inside the war rectangle (x in [2604,2648], y in [1708,1744]), the name is overwritten with `"??????"`, equipment slot 15 is cleared, and the guild is hidden to anonymize participants. The trade variant additionally copies `pUser[mob].AutoTrade.Title` into `sm->Desc`.

**Rule workflow:**
1. Zero the message; set Type/ID/Size/tick.
2. Pack kill/chaos/guilt into name bytes (trade variant checks mob < MAX_USER).
3. Clear guild if disabled; set AC by clan for NPCs.
4. Set CreateType by guild level.
5. Convert equipment; apply mount-outfit hide/sanc packing; compute selfdead.
6. Anonymize in war zone when BrState active; copy auto-trade title.

---

### Business Rule: BR-30 / BR-31 — NPC skill selection and low-HP heal

**Overview:** In `GetAttack`/`GetAttackArea`, non-player mobs (`mob >= MAX_USER`) select an attack skill from their `SkillBar` using weighted random ranges, and may trigger a group-heal when health is low.

**Detailed description:**
The distance to the target determines which skill bar entries are used: if the distance (`BASE_GetDistance`) is >= 3, entries 2/3 are used; otherwise entries 0/1. The primary `special` value selects a `SkillIndex`/`SkillParm` via a switch (121->103/5, 122->105/1, 123->101/1, 124->101/2, 125/126/127->40 with parm 1/2/3, 128->33 with -4), with `255` treated as "no skill". A secondary `special2` value modifies the motion animation by a random 0-3 offset with per-case probability. Then a global `rand_ = rand()%100` is used: if `SkillBar[3]` is not 255 and `rand_` is in `[25,64]`, and that skill's `InstanceType == 6` (heal), and either the mob or its leader is at `hp <= 8` (out of 10), the mob heals the more-damaged of itself/leader by `MaxHp/10` and returns early. Otherwise the main skill is chosen from `SkillBar[0]` (rand 0-49), `SkillBar[1]` (rand 50-84), or `SkillBar[2]` (rand 85-99), with `Resist = InstanceType - 2` when applicable. This encodes a deterministic AI behavior with hard-coded probability bands.

**Rule workflow:**
1. If mob is NPC, pick skill-bar pair by distance.
2. Map special -> skill index/parm; apply motion offsets from special2.
3. Roll rand 0-99; check heal trigger (insttype 6 + low HP) -> heal 10% and return.
4. Otherwise select skill by rand band (0-49/50-84/85-99); set Resist.

---

### Business Rule: BR-32 / BR-33 / BR-34 / BR-35 — Damage modification chain

**Overview:** In `GetAttack` and `GetAttackArea`, the base damage from `BASE_GetDamage` is modified by resistance, reflect, PvP/terrain attribute, and friendly-fire checks.

**Detailed description:**
Base damage `fisdam` is computed from the attacker's `Damage` vs. the target's `Ac` via `BASE_GetDamage(fisdam, Ac, 0)`. If a resist index `Resist` is in `[0,3]`, damage scales as `(200 - target.Resist[Resist]) * fisdam / 100`, so resist values above 100 can invert to healing and below produce amplified damage. If the target is a player and has `ReflectDamage > 0`, that amount is subtracted; in the single-target `GetAttack` the damage is floored at 1, whereas in `GetAttackArea` it is floored at 0 (an inconsistency). When both attacker and target are player-type (or clan 4 NPCs), damage is reduced to 30% (`3*fisdam/10`) and then zeroed unless both cells' `GetAttribute` values have bit `0x40` set and lack bit `0x01` — i.e., damage only applies in a PvP-allowable terrain region. Finally, friendly-fire is suppressed: if the two mobs share a leader (falling back to self when `Leader == 0`) or share a non-zero guild (with `mob_guild` coerced to -1 when both are 0, and `target_guild` to -2 when `GuildDisable`), the attacker's `CurrentTarget` is cleared and damage is zeroed.

**Rule workflow:**
1. Compute base damage from Damage vs. AC.
2. Apply resistance scaling for resist index 0-3.
3. Subtract reflect damage (floor 1 single / 0 area).
4. If PvP-context, reduce to 30% and zero unless terrain attribute permits.
5. Zero damage and clear target if friendly (same leader/guild).

---

### Business Rule: BR-36 / BR-37 — Empty grid scanning

**Overview:** `GetEmptyItemGrid`, `GetEmptyMobGrid`, and `GetEmptyMobGridGreat` locate a free, walkable grid cell near a requested coordinate.

**Detailed description:**
`GetEmptyItemGrid` (GetFunc.cpp:1876) first returns TRUE if the exact cell is free (`pItemGrid==0`) and walkable (`pHeightGrid != 127`), then scans a 3x3 neighborhood and returns the first free, walkable cell found. `GetEmptyMobGrid` (GetFunc.cpp:1901) validates bounds (logging `"GetEmptyMobGridOut of range"` on failure), accepts the cell if it already holds the mob or is free/walkable, then scans expanding rings of radius 1, 2, 3, and 4, skipping out-of-range and occupied cells and cells whose `pHeightGrid` is 127 (impassable). `GetEmptyMobGridGreat` (GetFunc.cpp:2003) performs the same ring expansion but iterates `k` from 0 to 30 (radius 30) for a much wider search. Note that in all three ring-scan loops the impassability check uses `pHeightGrid[*ty][*tx]` (the original cell) rather than the candidate cell `pHeightGrid[y][x]`, which appears to be a latent bug: it tests the height of the starting coordinate instead of the candidate being placed.

**Rule workflow:**
1. Validate requested cell bounds; return FALSE if out of range.
2. Return TRUE if the exact cell is free/walkable (or already owned by the mob).
3. Scan expanding ring neighborhoods for the first free, walkable cell.
4. Return FALSE if no cell is found within the search radius.

---

### Business Rule: BR-38 / BR-39 / BR-40 — Player meta (kills/PK/guilt) persistence

**Overview:** The `Get/Set` accessor pair for current/total kills, PK points, and guilt read and write packed byte fields inside the `KILL_MARK` carry item (`pMob[conn].MOB.Carry[KILL_MARK]`), applying clamping.

**Detailed description:**
Current kill count is stored in `stEffect[0].cValue` (one byte); total kills are stored split across `stEffect[1].cValue` (low byte) and `stEffect[2].cValue` (high byte), giving a 16-bit range. `SetCurKill` clamps to `[0,200]`; `SetTotKill` clamps to `[0,32767]`. PK point (`GetPKPoint`) is read from `stEffect[0].cEffect` and `SetPKPoint` clamps to `[1,150]` (note the lower bound is 1, so 0 cannot be set directly through this setter). Guilt (`GetGuilty`) is read from `stEffect[1].cEffect`; if the raw value exceeds 50 it is reset to 0 (both stored and returned), and `SetGuilty` clamps to `[0,50]`. All accessors guard against invalid connection indices (`conn <= 0 || conn >= MAX_USER`) and return 0 (or return early for setters) when out of range. These byte-packed values are also what `GetCreateMob` reads back into the `MobName` bytes for client display, so the persistence format and the network format are the same packed representation.

**Rule workflow:**
1. Validate connection index; return/ignore on invalid.
2. Read or write the appropriate `cValue`/`cEffect` bytes of the `KILL_MARK` carry item.
3. Apply clamping (CurKill [0,200], TotKill [0,32767], PK [1,150], Guilt [0,50] with >50 reset).
4. Return the decoded value (or 0 on invalid input).

---

## 4. Component Structure

The component consists of exactly two files within the TMSrv project directory. It has no subfolders or separate modules of its own; all logic lives in the single translation unit.

```
legacy/Code/TMSrv/
├── GetFunc.h                  # Declarations of all 34 public functions (62 lines)
└── GetFunc.cpp                # Implementations (2160 lines)
```

**GetFunc.h** (62 lines) declares the public interface. It is guarded by `#ifndef __GETFUNC__`, includes `..\Basedef.h` and `<Windows.h>`, and declares:

- 7 combination validators: `GetMatchCombine`, `GetMatchCombineEhre`, `GetMatchCombineTiny`, `GetMatchCombineShany`, `GetMatchCombineAilyn`, `GetMatchCombineAgatha`, `GetMatchCombineOdin`, `GetMatchCombineAlquimia` (`GetFunc.h:25-32`).
- Combat/scoring helpers: `GetParryRate`, `GetExpApply` (`GetFunc.h:33,39`).
- World/positioning helpers: `GetEmptyAffect`, `GetHide`, `GetInView`, `GetInHalf`, `GetTeleportPosition`, `GetAttribute`, `GetEmptyItemGrid`, `GetEmptyMobGrid`, `GetEmptyMobGridGreat` (`GetFunc.h:34-38,45,50-51,60`).
- Message packers: `GetCreateMob`, `GetCreateMobTrade`, `GetCreateItem`, `GetAction`, `GetAttack`, `GetAttackArea`, `GetAffect` (`GetFunc.h:40-44,46-47,42`).
- Player-meta accessors: `GetCurKill`, `GetTotKill`, `SetCurKill`, `SetTotKill`, `GetPKPoint`, `GetGuilty`, `SetGuilty`, `SetPKPoint` (`GetFunc.h:52-59`).
- Guild helpers: two overloads of `GetGuild` (`GetFunc.h:48-49`).

**GetFunc.cpp** (2160 lines) includes `<Windows.h>`, `..\Basedef.h`, `..\ItemEffect.h`, `Language.h`, `Server.h`, `GetFunc.h`, `CMob.h`, and `SendFunc.h`, and declares `extern STRUCT_ITEMLIST g_pItemList[MAX_ITEMLIST];` at `GetFunc.cpp:30`.

---

## 5. Dependency Analysis

### Internal Dependencies (within the W2PP codebase)

`GetFunc` depends on shared TMSrv/global declarations and the `BASE_*` helper functions:

```
GetFunc.h -> ..\Basedef.h  (STRUCT_ITEM, STRUCT_MOB, STRUCT_MOBEXTRA, constants, globals)
GetFunc.cpp -> Server.h    (pMob, pUser, grids, RvRState, KefraLive, ServerIndex, g_pGuildZone)
GetFunc.cpp -> CMob.h      (CMob class / pMob global definition)
GetFunc.cpp -> SendFunc.h  (SendClientMessage)
GetFunc.cpp -> Language.h  (g_pMessageStringTable / message constants)
GetFunc.cpp -> ..\ItemEffect.h (EF_* constants)
```

Runtime value dependencies (globals consumed): `pMob[MAX_MOB]` (`CMob`), `pUser[MAX_USER]` (`CUser`), `pItemGrid[MAX_GRIDY][MAX_GRIDX]`, `pHeightGrid[MAX_GRIDY][MAX_GRIDX]`, `pMobGrid[MAX_GRIDY][MAX_GRIDX]`, `pItem` (item world objects), `g_pItemList[MAX_ITEMLIST]`, `g_pSpell[MAX_SKILLINDEX]`, `g_pAttribute[1024][1024]`, `g_pGuildZone[MAX_GUILDZONE]`, `g_pAnctChance[3]`, `g_pTinyBase`, `g_pAilynBase`, `g_pAgathaBase`, `RvRState`, `KefraLive`, `ServerIndex`, `BrState`, `CurrentTime`, `temp` buffer.

Helper functions invoked from `Basedef.cpp`: `BASE_GetItemAbility`, `BASE_GetItemSanc`, `BASE_GetItemAmount`, `BASE_GetDistance`, `BASE_GetDamage`, `BASE_VisualItemCode`, `BASE_VisualAnctCode`, `BASE_GetGuild`, `BASE_GetVillage`. From `SendFunc.cpp`: `SendClientMessage`. From global utilities: `Log`, `ItemLog`, `sprintf`.

### External Dependencies

- **Windows API** (`<Windows.h>`): `BOOL`, `FALSE`, `TRUE`, `rand()`. This binds the component to Windows (Win32/x64), consistent with the Visual Studio solution.
- **C standard library**: `memset`, `memcpy`, `strncpy`, `sprintf`, `rand`, `sizeof`.

There are **no third-party libraries** or network/database dependencies within this component; all state is in-memory in the server process.

---

## 6. Afferent and Efferent Coupling

Because `GetFunc` is procedural (free functions), the "components" here are the individual public functions of the translation unit. Afferent coupling (CA) = number of distinct TMSrv files that call the function; Efferent coupling (CE) = number of distinct external modules/globals the function touches (approximated from `BASE_*` helpers, globals, and shared structs).

| Component (function) | Afferent Coupling | Efferent Coupling | Critical |
|----------------------|-------------------|-------------------|----------|
| GetCreateMob | 10 | 12 | High |
| GetAttribute | 8 | 3 | High |
| GetEmptyMobGrid | 7 | 4 | High |
| GetPKPoint | 7 | 3 | Medium |
| GetAttack | 1 | 11 | High |
| GetAttackArea | 1 | 11 | High |
| GetExpApply | 2 | 6 | High |
| GetAction | 4 | 4 | Medium |
| GetCreateItem | 3 | 6 | Medium |
| GetGuild (STRUCT_ITEM) | 5 | 2 | Low |
| GetTeleportPosition | 1 | 6 | Medium |
| GetMatchCombine | 1 | 6 | Medium |
| GetMatchCombineOdin | 1 | 4 | Medium |
| GetMatchCombineAilyn | 1 | 4 | Medium |
| GetEmptyItemGrid | 2 | 3 | Low |
| GetParryRate | 3 | 3 | Medium |
| GetEmptyAffect | 3 | 2 | Medium |
| GetInView | 5 | 2 | Medium |
| SetPKPoint | 7 | 2 | Medium |
| GetGuilty | 7 | 2 | Medium |
| GetCreateMobTrade | 3 | 8 | Medium |
| SetGuilty / SetCurKill / SetTotKill / GetCurKill / GetTotKill | 2-3 | 2 | Low |
| GetAffect | 1 | 2 | Low |
| GetInHalf / GetHide / GetEmptyMobGridGreat | 1 | 2 | Low |
| GetMatchCombineEhre/Tiny/Shany/Agatha/Alquimia | 1 | 3-4 | Low |

**Interpretation:** `GetCreateMob` is the highest fan-in (10 callers) and the most complex, making it the hottest shared utility. `GetAttack`/`GetAttackArea` have the highest fan-out (11 efferent dependencies) despite only one caller each, meaning their blast radius is wide relative to usage. The combination validators are each used by exactly one dedicated message handler (1-to-1), giving them low fan-in but concentrated business-logic risk.

---

## 7. Endpoints

`GetFunc` is a server-side helper library and does **not expose any network endpoints** (no REST, GraphQL, gRPC, or socket message handlers). Its functions are invoked internally by message handlers and timers; the network protocol messages it *packs* (`MSG_CreateMob`, `MSG_CreateItem`, `MSG_Action`, `MSG_AttackOne`, `MSG_Attack`) are produced for broadcast but are not endpoints owned by this component. This section is therefore omitted per the analysis guidelines.

---

## 8. Integration Points

`GetFunc` integrates only with in-process shared state and helper functions; it has no external services, databases, or network calls of its own.

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| Global state (pMob, pUser, grids, g_pItemList, g_pSpell, g_pAttribute, g_pGuildZone) | In-process shared memory | Read/write authoritative server state | Direct memory access | C structs | Bounds guards in accessors (e.g., `conn <= 0 || conn >= MAX_USER`); `Log` on grid out-of-range |
| BASE_* helpers (Basedef.cpp) | In-process function calls | Item abilities, damage, distance, visuals | Function call | C primitives / structs | Return sentinel values checked by callers |
| SendFunc (SendClientMessage) | In-process function call | Client message dispatch | Function call | String message | None within this component |
| Network message structs (MSG_*) | Data producers for client broadcast | Spawn/action/attack messages | In-memory struct packing | Packed protocol structs | Zeroing via memset; early-return guards |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Procedural Utility Library (static helpers) | All functions are free functions over global state | GetFunc.cpp | Reusable shared game-logic helpers with no own state |
| Registry / Recipe-Table pattern (data-in-code) | `if/else-if` chains returning recipe IDs | GetMatchCombineEhre/Odin/Alquimia (GetFunc.cpp:106-560) | Encode item-crafting recipes as code |
| Facade / packing layer | Message struct fillers | GetCreateMob, GetCreateItem, GetAction, GetAttack, GetAffect | Convert internal server state into network protocol structs |
| Accessor / Mutator (getter-setter) | Get/Set pairs | GetCurKill..SetPKPoint (GetFunc.cpp:2046-2159) | Abstract byte-packed persistence of player meta |
| Global singleton state | Shared globals accessed directly | pMob, pUser, grids | Server-wide authoritative state (anti-pattern by modern standards) |
| Sentinel-value error signaling | Return 0 / -1 / FALSE on failure | Throughout | Indicate invalid input to callers |

Architectural decisions embedded: (a) all combination logic is compile-time hard-coded rather than data-driven; (b) server state is globally accessible rather than encapsulated; (c) network message packing is centralized in `Get*` helpers to keep message handlers thin; (d) byte-packing of meta values (kills, PK, guilt) into item effect bytes is used to persist player data without a separate schema.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | GetEmptyMobGrid / GetEmptyMobGridGreat | Impassability check uses `pHeightGrid[*ty][*tx]` (original cell) instead of candidate `pHeightGrid[y][x]` (GetFunc.cpp:1929,1949,1969,1989,2033) | May place mobs on impassable cells; inconsistent placement behavior |
| High | GetMatchCombine* | Item IDs and recipe tables are hard-coded magic constants across 8 functions | Recipe changes require code edits; risk of drift; no data-driven balance |
| High | GetAttack / GetAttackArea | Large duplicated block (skill selection, damage, friendly-fire) copied between both functions | ~280 lines duplicated; divergence risk (e.g., reflect floor 1 vs 0) |
| High | GetParryRate | `attackaccuracy` parameter accepted but unused (GetFunc.cpp:562) | Misleading API; callers may pass a value that has no effect |
| Medium | GetGuild(int conn) | Function body declares locals and performs no work (GetFunc.cpp:1862-1867) | Dead code; misleading |
| Medium | GetHide / GetGuild(STRUCT_ITEM*) | No in-repo callers found | Dead/unused public API |
| Medium | GetTeleportPosition | ~30 hard-coded coordinate branches; some with missing else-destinations (e.g., Azran-to-vale requires item 3916, otherwise no teleport) | World teleporter network is rigid and error-prone |
| Medium | GetExpApply | Magic class/quest gates and 50% halving (GetFunc.cpp:912) implicit, undocumented | Balance changes difficult; behavior surprising |
| Medium | Global mutable state | Functions depend on many module-level globals with no isolation | Hard to test, reason about, or parallelize |
| Medium | Combination sentinel inconsistency | Alquimia returns -1, Ehre/Odin return 0 for "no match" | Callers must handle different sentinels |
| Medium | Item 747 literal | Forbidden item duplicated in 8 loops as magic number | Risk of missing a spot if it changes |
| Low | SetPKPoint | Clamp lower bound is 1, cannot set 0 via setter (GetFunc.cpp:2150) | Caller wanting 0 must use a workaround |
| Low | Windows-specific | `<Windows.h>` + `BOOL` | Not portable; locks to Windows toolchain |
| Low | Overflow assumptions | `sprintf` into shared `temp` buffer; `unsigned char` truncation of large values | Potential buffer/value truncation |

---

## 11. Test Coverage Analysis

**No automated tests exist for this component or anywhere in the project.** A search of the entire `legacy/` tree for test files (`*.test.*`, `test_*`, `*.spec.*`, `*test*` directories) returned no matches, and the Visual Studio solution (`W2PP Code Project.sln`) contains only the two build projects `DBSrv` and `TMSrv` — neither is a test project.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| GetFunc (all 34 functions) | 0 | 0 | 0% (no test harness) | N/A — no tests present |

The absence of tests is a significant risk given the density of business rules (recipe tables, damage math, teleport coordinates, byte-packed meta). The only "validation" of behavior is indirect, through live server operation and manual playtesting, since the functions are procedural over global state and would require substantial refactoring (dependency injection / state isolation) to unit-test in their current form. This is called out per the analysis guidelines as a coverage risk.

---

## 12. Report Metadata

- **Component analyzed:** GetFunc
- **Files:** `legacy/Code/TMSrv/GetFunc.cpp` (2160 lines), `legacy/Code/TMSrv/GetFunc.h` (62 lines)
- **Scope:** shared game-logic helpers of the W2PP TMSrv server process
- **Folders ignored:** `.git`, `.opencode`
- **Report generated:** 2026-08-19 17:13:23
- **Note:** This is an analysis-only report; no project files were modified.
