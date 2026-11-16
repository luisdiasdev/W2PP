# Component Deep Analysis Report — MobKilled Resolution Module

- **Component:** MobKilled Resolution Module
- **Primary location:** `Code/TMSrv/MobKilled.cpp` (2,469 lines, 74,521 bytes, single translation unit, single exported function)
- **Entry point:** `void MobKilled(int target, int conn, int PosX, int PosY)` — `Code/TMSrv/MobKilled.cpp:41`
- **Host subsystem:** TMSrv (game/world server), Win32 C/C++, MSVC, MultiByte character set, `/W3`
- **Analysis scope:** entire project root `E:\wyd\test-setup\w2pp`; `openspec`, `.agents`, `.codegraph`, `.opencode` excluded
- **Report date:** 2026-11-15

---

## 1. Executive Summary

`MobKilled` is the **single death-resolution choke point** of the TMSrv world server. Every entity death in the game — a monster killed by a player, a player killed by another player, a summon's kill, a script/GM-forced kill, a DoT tick kill, and event-boss destruction — funnels through this one 2,429-line function. It is a *domain service* masquerading as a procedure: it decides who is credited, how much experience is paid, which items are rolled, which global/seasonal state machines advance, and what the clients are told.

**Purpose.** Given a dying entity (`target`), the entity that produced the lethal damage (`conn`), and optional death coordinates (`PosX`, `PosY`), the function (a) recognizes and vetoes "unkillable" entities, (b) converts the kill into experience, currency and items for the killer plus the killer's party, (c) advances roughly a dozen unrelated mini-game / event state machines keyed on `GenerateIndex`, and (d) resolves player-versus-player consequences (experience loss, kill counters, PK points, war flags, item drop).

**Role in the system.** The component is a *hub*: it is called from the combat pipeline (`_MSG_Attack.cpp`), the monster-attack pipeline (`ProcessSecMinTimer.cpp`), the damage-application helper (`Server.cpp`), and the GM console (`imple.cpp`). In the opposite direction it drives experience, inventory, chat, teleport, guild, and database subsystems. It also delegates two sub-domains to dedicated classes (`CCastleZakum`, `CWarTower`), which makes it a *partial* mediator rather than a pure god-function.

**Key findings.**

1. **Two mutually exclusive top-level domains** share one body: PvE resolution (`Code/TMSrv/MobKilled.cpp:207-2056`) and PvP resolution (`2058-2468`). They have different validation rules, different caps and different reward semantics, but no separate functions.
2. **Experience is paid to party members before the kill packet is broadcast**, and the level-up cascade is *not* performed here — it is triggered as a side effect of `GridMulticast` of `_MSG_CNFMobKill` inside `SendFunc.cpp:920-947`. Exp granting and level-up are therefore coupled through packet dispatch, not through a call.
3. **A dead arithmetic expression** (`exp = (UNK_1 + myLevel) * isExp / (UNK_1 + myLevel)` at `255`, `357`, `451`, `543`) makes the per-member level scaling a mathematical identity: every eligible member receives exactly `isExp`. The intended `(30 + myLevel) / (30 + mobLevel)` weighting does not exist in the compiled behaviour.
4. **Experience capping is asymmetric.** The open-field branch caps a member's share at the killer's computed base exp (`569-570`), while the three "Pesa" (nightmare) room branches have that cap commented out (`287-288`, `382-383`, `476-477`).
5. **Loot randomness is deterministic per process.** `rand()` is used 38 times in this file and `srand()` is called nowhere in `Code/` (verified by full-tree search), so the drop/roll sequence restarts identically on every server start.
6. **A compile-time-dead PvP item-drop subsystem** (`#ifdef PKDrop`, `2305-2418`) is present but never compiled: `PKDrop` is commented out at `Code/Basedef.h:29` and is absent from the preprocessor definitions of `Code/TMSrv/TMSrv.vcxproj`. A second dead branch (the level-400..404 "heal instead of die" path at `2047-2054`) is unreachable given `MAX_LEVEL = 399`.
7. **No tests exist.** There is no test project, no test directory, no test file, no test framework, and no assertion harness anywhere in the repository (evidence in Section 10).

---

## 2. Data Flow Analysis

```
 0. Callers (afferent):
    _MSG_Attack.cpp:1743        (player skill/melee kill, batched over MAX_TARGET)
    ProcessSecMinTimer.cpp:1940 (monster/DoT damage tick kill)
    Server.cpp:9203             (generic damage application)
    imple.cpp:1661, 1718        (GM console: "killkefra", "kill")

 1. Entry guard ............................ MobKilled.cpp:43-44
    reject conn/target outside 1..MAX_MOB(25000), reject MOB_EMPTY target

 2. Selection of an equipped "fairy" item ... :46           (Equip[13] aliased as FairySlot)

 3. Unkillable vetoes (early return, no reward, no delete):
    a) Level >= 1000 -> restore HP, re-broadcast appearance, SendScore ..... :50-63
    b) Fairy sIndex 769 + no RvR/GTorre/Castle -> consume one sanc point ..... :65-104
    c) CastleState > 1 and standing on (1046,1690) -> DoTeleport(1066,1717) .. :105-109

 4. Summon-growth side payment (before any kill processing) ............... :111-169
    killer is a Clan==4 summon with face 315..345 -> credit summoner's
    mount (Equip[14] sIndex 2330..2389) with growth/exp, push SendItem

 5. Kill confirmation message assembly .................................... :173-183
    MSG_CNFMobKill{ Type=56, ID=ESCENE_FIELD, KilledMob=target, Killer=conn }
    target HP forced to 0

 6. Attribution fix-up:
    Leader = pMob[conn].Leader or conn ................................... :185-188
    summon killer -> conn := Summoner (or abort: broadcast + DeleteMob) .. :190-205

 7. Domain split ......................................................... :207
    PvE if target is an NPC (>= MAX_USER) or level > MAX_LEVEL+5
    PvP otherwise

 ---- PvE path ------------------------------------------------------------
 8. Eligibility: killer is a player and dead entity is not Clan 4 ....... :211
 9. Base exp: GetExpApply(killerExtra, mobExp, killerLvl, mobLvl) ....... :214  -> GetFunc.cpp:902
10. Party iteration i = 0..MAX_PARTY (12 = leader slot) ................. :243-250
11. Per-member zone gate (Pesa A / Pesa M / Pesa N / within 16 cells) ... :252,347,441,533
12. Per-member base exp via GetExpApply ................................. :255,350,444,536
13. Class-master level offset (+400) and divisor tiers .................. :257-284
14. Share multiplier 60% ................................................ :285
15. Bonus stack: ExpBonus, RvR clan, newbie, DOUBLEMODE, KefraLive, ±15% . :290-308
16. Daily exp log (reset by tm_yday) .................................... :311-315
17. Exp-debt (Hold) settlement .......................................... :318-333
18. Commit pMob[party].MOB.Exp += exp ................................... :335
19. Send MSG_UpdateExpRanking to DBServerSocket ......................... :338-341
20. Death chatter (DieAction[rand()%4]) ................................. :628-632
21. GridMulticast(_MSG_CNFMobKill) ..................................... :634
    -> SendFunc.cpp:920-947: per receiving user, fill Exp/Hold,
       CheckGetLevel(), level-up messages, SendScore, SetCircletSubGod,
       DoItemLevel  == the level-up cascade
22. Clan gate: clan not in {0,4,7,8} -> DeleteMob and stop ............... :636-637
23. Boss/event hooks keyed on GenerateIndex (Kefra, water rooms, castle
    gates, kings, towers, duel cards, rune quests, RvR) ................. :650-1726
24. Gold: coin roll, 2G ceiling, SendEtc ................................ :1728-1758
25. Global event item roll ............................................. :1759-1790
26. Loot loop over 64 carry slots: rate, level scaling, server mode,
    Rsv special, forced slots, cap, roll, item filter, SetItemBonus,
    key interception, audit log, DB day log ............................ :1792-1945
27. Quest amulet (TerraMistica) ........................................ :1947-1956
28. DeleteMob(target, 1) ............................................... :1990
29. Respawn loops (rune quests, Kefra minions) ......................... :1992-2025
30. CWarTower::MobKilled .............................................. :2027
    (terrain-ineligible path: DeleteMob without any reward) ........... :2029-2030

 ---- PvP path ------------------------------------------------------------
31. Level guard (0..MAX_LEVEL) ......................................... :2061-2064
32. Exp-loss amount from g_pNextLevel / g_pNextLevel_2 ................. :2067-2108
33. Counter snapshot (curKill, totKill, pkPoint, guilty, clans) ....... :2111-2120
34. PK-point multiplier (x3 / x5) and TESTSERVER divisor .............. :2122-2137
35. War/safe-zone classification (guild war, castle, RvR, GTorre,
    arena, village, zone 1/31) ....................................... :2130-2195
36. GridMulticast(_MSG_CNFMobKill) ................................... :2161 (same cascade)
37. Safe zone: "no exp loss" message + item 431 creation .............. :2170-2195
38. Exp debt credited to Hold, DEADPOINT / FREEEXP exceptions ......... :2197-2230
39. Same-clan/kingdom branch: kill counters, PK point loss, RvR respawn :2232-2276
40. Hostile branch: PK point loss, (disabled) item drop ............... :2278-2418
41. Appearance refresh for killer and victim, PK point gain, audit log  :2423-2443
42. Fairy (sIndex 753/1726) sanc decay ................................ :2446-2466
```

**Data ownership.** The function mutates, in place: `pMob[]` (HP, Exp, Coin, Carry, Equip, extra.Hold, extra.DayLog), `pUser[]` (`ReqHp`), `pMobGrid[][]`, `pItemGrid[][]` / `pItem[]` (castle gates), `pHeightGrid[][]` (read only), `mNPCGen.pList[]` (indirectly via `GenerateMob`/`DeleteMob`), the process-wide event state (`KefraLive`, `RvRBonus`, `RvRState`, `Kingdom1Clear/2Clear`, `LiveTower[]`, `WaterClear1[][]`, `CartaTime`, `CartaSala`, `Pista[]`, `evCurrentIndex`), and the database server socket (two message types).

**No transaction boundary.** There is no rollback: once `pMob[party].MOB.Exp` is incremented (`335`), the ranking packet is sent (`341`), items are placed (`PutItem`), and only afterwards the corpse is deleted (`1990`). A failure or early return between those points leaves partial state (see `721-724` and `2029-2030`, both of which `return`/delete after exp has already been paid).

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---|---|---|---|
| BR-01 | Validation | Reject invalid killer/target indices and empty mob slots | `MobKilled.cpp:43-44` |
| BR-02 | Invariant | Entities at level >= 1000 are immortal; HP is restored and death is cancelled | `MobKilled.cpp:50-63` |
| BR-03 | Invariant | "Nyerds" fairy (Equip[13] = 769) absorbs one death by consuming one sanc point while no war state is active | `MobKilled.cpp:65-104` |
| BR-04 | Invariant | A player standing on the castle throne block (1046,1690) during an active siege is teleported away instead of dying | `MobKilled.cpp:105-109` |
| BR-05 | Business Logic | Summoned creature kills feed the summoner's mount growth/EXP meters | `MobKilled.cpp:114-169` |
| BR-06 | Contract | Death broadcast uses `_MSG_CNFMobKill`; `Exp`/`Hold` are filled at send time, not at construction | `MobKilled.cpp:173-183`, `SendFunc.cpp:920-923` |
| BR-07 | Business Logic | Party leader resolution defaults to the killer when unset | `MobKilled.cpp:185-188` |
| BR-08 | Business Logic | Summon kills are re-attributed to the summoner; orphan summons are deleted without reward | `MobKilled.cpp:190-205` |
| BR-09 | Validation | PvE reward path requires a player killer and a dead entity whose clan is not 4 | `MobKilled.cpp:207-211` |
| BR-10 | Business Logic | Party members must be alive and inside the leader's party list to receive exp | `MobKilled.cpp:243-254` |
| BR-11 | Business Logic | Base exp = mob exp scaled by attacker/target level ratio | `MobKilled.cpp:214`, `GetFunc.cpp:902-945` |
| BR-12 | Business Logic | Non-mortal/arch classes receive a +400 level offset and progressive exp divisors | `MobKilled.cpp:259-284` |
| BR-13 | Business Logic | Each recipient receives 60% of base, then a fixed bonus stack is applied | `MobKilled.cpp:285-308` |
| BR-14 | Accounting | Daily exp is accumulated per character and reset on calendar-day change | `MobKilled.cpp:311-315` |
| BR-15 | Business Logic | Outstanding exp debt (`Hold`) is paid down before any exp is credited | `MobKilled.cpp:318-333` |
| BR-16 | Integration | Exp commit is followed by a ranking update to the DB server | `MobKilled.cpp:335-342` |
| BR-17 | Business Logic | Nightmare rooms force a fixed zone pair and disable the exp ceiling | `MobKilled.cpp:252-253,347-348,441-442,569-570` |
| BR-18 | Presentation | Mob-specific death chatter is emitted from `DieAction` | `MobKilled.cpp:628-632` |
| BR-19 | Business Logic | Killers with a clan other than 0/4/7/8 get no drops; corpse is deleted immediately | `MobKilled.cpp:636-637` |
| BR-20 | Business Logic | Kefra boss kill transfers ownership, guild fame, notice, and showers loot over a 60x59 cell rectangle | `MobKilled.cpp:650-713` |
| BR-21 | Validation | Loot is only produced on walkable terrain (-50 <= height <= 92) | `MobKilled.cpp:714`, `2029-2030` |
| BR-22 | Business Logic | Destruction of the castle gate mob opens the corresponding gate item | `MobKilled.cpp:719-755` |
| BR-23 | Business Logic | Water-dungeon (Agua M/N/A) last-monster kills grant room runes and a countdown | `MobKilled.cpp:762-979` |
| BR-24 | Business Logic | Castle Zakum quest bosses pay items, exp and coin to leader and party | `MobKilled.cpp:980`, `CCastleZakum.cpp:210-297` |
| BR-25 | Business Logic | Named mobs trigger recall, tower reset, or kingdom-clear flags | `MobKilled.cpp:983-999` |
| BR-26 | Business Logic | Boss loot tables by generate id (0/1/2, 5/6/7, 3) | `MobKilled.cpp:1001-1073` |
| BR-27 | Business Logic | Nightmare mobs respawn immediately on death | `MobKilled.cpp:1076-1084` |
| BR-28 | Business Logic | Duel-card rooms (N/M/A, 4 rooms each) advance on room clear with a 60s timer | `MobKilled.cpp:1087-1350` |
| BR-29 | Business Logic | Rune quest ladder LV0..LV6 (gated rolls, party-wide runes, tier items) | `MobKilled.cpp:1354-1693` |
| BR-30 | Business Logic | RvR tower destruction ends the war and awards a kingdom-wide exp bonus | `MobKilled.cpp:1697-1723` |
| BR-31 | Business Logic | Coin drop uses a level tier plus a random roll, capped at 2000 and 2,000,000,000 total | `MobKilled.cpp:1729-1757` |
| BR-32 | Business Logic | Global event item rolls once per kill against `evRate`, with optional serial engraving | `MobKilled.cpp:1760-1789` |
| BR-33 | Business Logic | Per-slot loot roll with 12 modifiers (bonus, level, server mode, Rsv, forced slots) | `MobKilled.cpp:1792-1945` |
| BR-34 | Business Logic | Treasure keys are intercepted before normal loot placement | `MobKilled.cpp:1904`, `CCastleZakum.cpp:299-312` |
| BR-35 | Audit | Every successful drop is logged and reported to the DB day-log | `MobKilled.cpp:1907-1920` |
| BR-36 | Business Logic | "Mystic land" quest amulet (mobs 239/241) completes at 5% per kill | `MobKilled.cpp:1948-1955` |
| BR-37 | Business Logic | Corpse removal plus respawn loops for quest/event generators | `MobKilled.cpp:1990-2027` |
| BR-38 | Validation | PvP resolution requires the victim level to be within 0..MAX_LEVEL | `MobKilled.cpp:2061-2064` |
| BR-39 | Business Logic | PvP exp loss = level-gap exp / divisor tier, multiplied by PK-point band | `MobKilled.cpp:2067-2125` |
| BR-40 | Business Logic | War/arena/safe-zone classification suppresses exp loss and may spawn item 431 | `MobKilled.cpp:2130-2195` |
| BR-41 | Business Logic | Exp loss is credited to `Hold` (exp debt) rather than removed immediately | `MobKilled.cpp:2197-2230` |
| BR-42 | Business Logic | Same-kingdom kills increment counters and move PK points; RvR deaths teleport to kingdom spawn | `MobKilled.cpp:2232-2276`, `2426-2435` |
| BR-43 | Business Logic | Hostile PvP kills reduce PK points; item drop disabled at compile time | `MobKilled.cpp:2278-2418` |
| BR-44 | Audit | Post-PvP appearance refresh and one audit line per PvP death | `MobKilled.cpp:2423-2443` |
| BR-45 | Invariant | Fairy items 753/1726 lose one sanc point when their owner dies in PvP | `MobKilled.cpp:2446-2466` |

---

### Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Entry validation and slot rejection

**Overview**: The function refuses to run for malformed indices or for an empty mob slot, which is the only thing preventing out-of-bounds access on `pMob[]` at the component boundary.

**Detailed description**:
`MobKilled` receives both `target` and `conn` as plain integers from four independent call sites, none of which validates them (`_MSG_Attack.cpp:1743` iterates a `TargetKilled[]` array; `ProcessSecMinTimer.cpp:1940` passes the current AI index; `Server.cpp:9203` passes the damage attacker; `imple.cpp` passes a GM-supplied index or loop counter). The guard at `43` therefore carries the entire defensive burden: it rejects `conn <= 0`, `conn >= MAX_MOB` (25,000), `target <= 0`, `target >= MAX_MOB`, and `target` whose `Mode == USER_EMPTY`. Index 0 is intentionally excluded because slot 0 is reserved (all downstream helpers such as `PutItem` and `GetCurKill` also reject `conn <= 0`).

There is **no** validation of `PosX`/`PosY`. Those coordinates are used only as drop-placement hints (`641-647`) and are dereferenced against `pHeightGrid[AlvoY][AlvoX]` at `714`. Every in-tree caller passes `0, 0`, so in practice `AlvoX`/`AlvoY` fall back to the dead entity's tile; any future caller passing a non-zero out-of-range pair would produce an out-of-bounds read of a `char[4096][4096]` grid with no bounds check.

The guard is silent: no log line, no counter, no error return value (the function returns `void`). A rejected call is therefore indistinguishable from a successful kill of a zero-reward mob, which complicates operational diagnosis.

**Rule workflow**:
1. `conn <= 0` → return.
2. `conn >= 25000` → return.
3. `target <= 0` → return.
4. `target >= 25000` → return.
5. `pMob[target].Mode == USER_EMPTY` → return.
6. Otherwise continue with `FairySlot = &pMob[target].MOB.Equip[13]`.

---

### Business Rule: BR-02 — Immortal entity rule (level >= 1000)

**Overview**: Any entity whose current level is 1000 or above cannot die; the kill attempt is converted into a full heal plus a visual refresh.

**Detailed description**:
At `50` the function inspects `pMob[target].MOB.CurrentScore.Level`. Levels of 1000+ are outside the legitimate player band (`MAX_LEVEL = 399`, `MAX_CLEVEL = 199`) and are used by the server as a marker for invulnerable NPCs, guards, and scripted set pieces. Rather than simply returning, the code performs a *resurrection*: HP is restored to `MaxHp` (`52`), `SetReqHp` is called for user targets (`55`), a fresh `MSG_CreateMob` is generated with `GetCreateMob` and multicast (`57-58`), and `SendScore(conn)` is issued (`60`).

Note that the message is built once (`48`) and reused by every early-return path, so the "immortal" branch re-sends the appearance of the *target*, not of the killer. Because `pMob[target].MOB.CurrentScore.Hp` was not previously zeroed at this point (the zeroing at `183` happens later), this branch is effectively a no-op heal that exists to force clients to resynchronise the health bar — the client had already applied lethal damage locally.

No experience, no loot, no death flag, and no `DeleteMob` occur. The target's `Mode` is untouched, so the AI keeps running. This rule is the first of three "damage must be undone" mechanisms (the other two being BR-03 and BR-04).

**Rule workflow**:
1. `Level >= 1000` → clamp HP to `MaxHp`.
2. If target is a user → `SetReqHp(target)`.
3. Build and multicast `MSG_CreateMob`.
4. `SendScore(conn)`.
5. Return (no rewards, no deletion).

---

### Business Rule: BR-03 — Nyerds fairy death absorption

**Overview**: When the dead entity carries item 769 in equipment slot 13 (the "fairy" slot) while no RvR, guild-tower or castle state is active, one point of its `sanc` counter is consumed and the entity survives.

**Detailed description**:
Lines `65-104` implement a consumable "extra life". The gate is a four-way conjunction: slot-13 index equals `769`, `RvRState == 0`, `GTorreState == 0`, and `CastleState == 0`. During any war the fairy is inert and the entity dies normally — this is what makes the item safe to hand out in PvE but useless as a war exploit.

`BASE_GetItemSanc(FairySlot)` reads the refinement/sanction counter, which is stored inside whichever of the three effects carries effect id `43` and is *not* the item's `sanc` field per se — the code explicitly rewrites `cValue` of the first matching `cEffect == 43` slot (`73-80`). If the counter is above zero it is decremented in place; when it reaches zero the item is destroyed via `BASE_ClearItem` (`83`). When the counter was already zero, the item is cleared without any decrement.

The entity is then resurrected: HP is clamped to maximum, `SetReqHp` for players, `SendItem` on `ITEM_PLACE_EQUIP, 13` plus `SendEmotion(target, 14, 2)` to play the "blocked" animation (`87-89`), a full `SendEquip(target, 0)` refresh (`91`), and the standard `MSG_CreateMob` broadcast (`98-99`). `SendScore(conn)` finishes the resync. Notably `SendEquip` and `SendEmotion` are called with `target` even when the target is not a player (both helpers are index-guarded internally). The rule never credits experience and never deletes the mob.

**Rule workflow**:
1. `Equip[13].sIndex == 769` AND all three war states are 0.
2. Read `sanc`; if `sanc > 0` → decrement and write back into the effect-43 slot.
3. If `sanc == 0` → destroy the item.
4. Restore HP; `SetReqHp` when target is a player.
5. `SendItem(equip 13)`, `SendEmotion(14, 2)`, `SendEquip(target, 0)`.
6. Multicast refreshed appearance; `SendScore(conn)`; return.

---

### Business Rule: BR-04 — Castle throne-blocker displacement

**Overview**: During an advanced siege state, a player standing on tile (1046, 1690) is teleported to (1066, 1717) instead of being killed.

**Detailed description**:
`CastleState > 1` means the castle siege has progressed beyond the initial phase. Tile (1046, 1690) is the castle's inner throne/stone position. Instead of allowing the occupant to die (which would drop loot inside the throne area and potentially break the siege flow), the function calls `DoTeleport(target, 1066, 1717)` and returns immediately at `107-108`.

The rule is applied **before** the HP-zeroing at `183`, before attribution (BR-07/BR-08) and before any experience accounting. This ordering is essential: a player killed on that tile receives no exp to the killer and loses nothing. Because the check requires `target > 0 && target < MAX_USER`, it never applies to monsters.

The hard-coded coordinate pair is not configurable; it is duplicated from the castle configuration loaded elsewhere and any change to the castle map requires a source change here.

**Rule workflow**:
1. `CastleState > 1`.
2. `TargetX == 1046 && TargetY == 1690`.
3. `0 < target < MAX_USER`.
4. `DoTeleport(target, 1066, 1717)`; return.

---

### Business Rule: BR-05 — Summoned-creature mount growth credit

**Overview**: When a summoned creature (clan 4, face 315..345) lands the killing blow on a non-summon monster, the *summoner's* mount equipment accumulates growth points and can gain a mount level.

**Detailed description**:
This is the only reward path evaluated *before* the kill message is built, i.e. it is deliberately independent of who "officially" killed the mob. Conditions: the killer is not a player (`conn >= MAX_USER`), its clan is 4 (summon), its face item is in 315..345, the victim is a monster (`target > MAX_USER`) and the victim's clan is not 4. The summoner is then validated as a live logged-in player (`116-118`).

The mount is `pMob[summoner].MOB.Equip[14]`, and only indices 2330..2389 qualify (`122`). Two bytes are read: `stEffect[1].cEffect` as the current mount EXP (`XP`) and `stEffect[2].cValue` as the current growth counter (`Crescimento`). The per-tick growth requirement depends on the mount tier: base `XP + 100`, but 2330 → +25, 2331 → +35, 2332 → +45, 2333 → +55, 2334 → +65, 2335 → +75 (`129-145`). Lower-tier mounts therefore level faster.

Growth is only credited when `XP < mobLevel && XP < 100` (`147`). The growth counter increments by one; when it would reach the requirement, it wraps to 1, `XP` increments by one, the client receives `_NN_Mount_Level`, the item is resent, and `MountProcess(summoner, 0)` re-applies the mount's stat bonus (`157-165`). Note the subtle asymmetry at `149-155`: the counter is written but `SendItem` is only issued on the non-level-up path once the cap is not exceeded — the item update is pushed only when the value actually changed.

The mount EXP is stored in effect *codes*, i.e. `stEffect[1].cEffect` is abused as a data byte, not as an effect id. This makes the encoding fragile: any code that re-normalises effect codes on the item will silently destroy mount progress.

**Rule workflow**:
1. Killer is a summon (`>= MAX_USER`, clan 4, face 315..345) and victim is a monster with clan != 4.
2. Resolve `summoner`; require a live player.
3. `Equip[14].sIndex` in 2330..2389 → read `XP` and `Crescimento`.
4. Choose the tier requirement (25/35/45/55/65/75/100).
5. If `XP < victimLevel && XP < 100`: increment counter; on reaching requirement wrap to 1, `XP++`, notify, resend item, re-apply mount stats; otherwise store the counter and resend.
6. Continue with normal kill resolution.

---

### Business Rule: BR-06 — Kill confirmation contract

**Overview**: The death message is always `_MSG_CNFMobKill` (`Type = 56`), addressed to the scene (`ID = ESCENE_FIELD = 30000`), and carries only the two mob indices at construction time.

**Detailed description**:
`MSG_CNFMobKill` (`Code/Basedef.h:1512-1520`) holds `Hold`, `KilledMob`, `Killer` and `Exp`, but `MobKilled` populates only `Type`, `Size`, `ID`, `KilledMob` and `Killer` (`176-181`). `Exp` and `Hold` are *per-recipient* values and are therefore filled inside the multicast loop in `SendFunc.cpp:920-923` from `pMob[tmob].MOB.Exp` and `pMob[tmob].extra.Hold`.

This is an unusual design: the packet is a template that `GridMulticast` mutates differently for each receiver. It exists so that one broadcast can refresh experience counters for every player who can see the corpse while simultaneously triggering the receiver-side level-up cascade (BR-16 note, Section 9 pattern "Implicit Observer"). `KilledMob` and `Killer` are `unsigned short`, which is adequate for indices below 25,000.

Because `Exp` is written per recipient *after* the party exp block executed at `243-626`, the ordering guarantees clients see post-award values. If the multicast were moved before the exp loop, the level-up cascade would be one kill behind.

**Rule workflow**:
1. Zero the structure.
2. Set `Type = _MSG_CNFMobKill`, `Size = sizeof`, `ID = ESCENE_FIELD`.
3. Set `KilledMob = target`, `Killer = conn` (the *original* killer, pre-summoner-remap).
4. Force `pMob[target].HP = 0`.
5. Broadcast later (`634`, `196`, `2036`, `2043`, `2161`), each time with `Exp`/`Hold` substituted per receiver.

---

### Business Rule: BR-07 — Party leader resolution

**Overview**: `Leader` is the killer's `Leader` field, defaulting to the killer itself when zero.

**Detailed description**:
`pMob[conn].Leader` is the group leader pointer used throughout TMSrv. A solo player has `Leader == 0`, so the function substitutes `conn` (`187-188`). This resolved `Leader` is the anchor for the party iteration (`248`), the water-dungeon reward recipient (`774-777`, `815-818`, …), the rune-quest reward recipient (`1371-1374`, `1438-1441`, …) and the castle-quest prize recipient (`CCastleZakum.cpp:224-227`).

Two independent locals exist for the same concept in different blocks (`Leader` at `185`, and repeatedly re-derived as `partyleader`/`party` inside each hook). The redundancy is total: each hook re-reads `pMob[conn].Leader` and re-applies the `<= 0 → conn` fallback. Because `conn` may have been rewritten to the summoner at `204`, all later hooks use the summoner's leader, which is the intended behaviour but is not obvious from the code.

The rule does not verify that the resolved leader is alive, is a player, or is actually in the same party as the killer. Downstream loops compensate with per-member `pUser[...].Mode != USER_PLAY` checks (e.g. `795`, `826`).

**Rule workflow**:
1. `Leader = pMob[conn].Leader`.
2. If `Leader == 0` → `Leader = conn`.
3. Use `Leader` for party iteration and reward delivery.

---

### Business Rule: BR-08 — Summon kill attribution

**Overview**: A clan-4 summon's kill is re-attributed to its summoner; if the summoner is missing or not playing, the kill is broadcast and the corpse deleted with no reward.

**Detailed description**:
At `190-205` the function checks whether the *attacker* is a summon. If `Summoner <= 0`, `Summoner >= MAX_USER`, or `pUser[Summoner].Mode != USER_PLAY`, the code multicasts `_MSG_CNFMobKill` (`196`), deletes the monster if it is one (`198-199`) and returns. The corpse is therefore cleaned up even in the degenerate case — a deliberate anti-leak measure, because otherwise a summon whose owner logged out mid-fight would leave an unkillable, unrewardable corpse.

When the summoner is valid, `conn = Summoner` (`204`) rewrites the attacker identity for **all** subsequent logic: experience is computed from the summoner's `extra`, bonuses come from the summoner's `ExpBonus`/`DropBonus`, drops are placed in the summoner's inventory (`PutItem(conn, …)`), and `Log` lines name the summoner. The original killer index survives only inside the packet's `Killer` field (`181`).

This rule is the reason BR-05 (mount growth) must run *before* the remap: after `204`, `conn` no longer satisfies `conn >= MAX_USER`, so the mount-growth block would never fire.

**Rule workflow**:
1. Attacker is `>= MAX_USER` and clan 4.
2. Read `Summoner`.
3. If invalid or not `USER_PLAY`: broadcast kill, delete monster if any, return (no rewards).
4. Else `conn = Summoner`.
5. Continue as if the summoner had landed the blow.

---

### Business Rule: BR-09 — PvE eligibility gate

**Overview**: The PvE reward block runs only when the dead entity is an NPC with level <= `MAX_LEVEL`, the killer is a player, and the victim's clan is not 4.

**Detailed description**:
The pair of conditions at `207` and `209` look like a range test but are not: because `target >= MAX_USER` satisfies both, every monster enters the inner block regardless of level, while players only enter the outer block when their level exceeds 404 and then fail the inner test unless their level is <= 399. The inner `else` (`2047-2054`) — heal the player to full instead of killing them — is therefore **unreachable** with `MAX_LEVEL = 399`; players at 400..404 take the PvP path and are then rejected by the PvP level guard at `2063`.

Inside the block, `211` imposes the real eligibility rule: `conn < MAX_USER` (a player must own the kill) **and** `pMob[target].MOB.Clan != 4` (summons cannot be looted). If either fails, the corpse is broadcast and deleted without any reward (`2034-2038`), which is the correct behaviour for a summon dying or for a monster killed by another monster.

Everything downstream — experience, gold, event items, loot, quest hooks, respawns — inherits this gate. Consequently a monster killed by a monster or by a decaying DoT with an invalid attacker produces no loot at all, not even the guaranteed slot 11 item.

**Rule workflow**:
1. `target >= MAX_USER` (NPC) or victim level > 404 → outer block.
2. Inner: NPC, or victim level <= 399.
3. `conn < MAX_USER` and victim clan != 4 → reward path.
4. Otherwise broadcast + `DeleteMob(target, 1)`.
5. Player with 400..404 level and NPC branch failing → heal-only path (dead code).

---

### Business Rule: BR-10 — Party membership and proximity eligibility

**Overview**: Experience is paid to each entry of the leader's `PartyList[0..11]` plus the leader itself, provided the member is a live player inside one of four accepted zones.

**Detailed description**:
The loop `for (int i = 0; i < MAX_PARTY + 1; ++i)` runs 13 times: indices 0..11 read `pMob[Leader].PartyList[i]`, and index 12 falls into the `else` branch which assigns `party = Leader` (`247-250`). The leader is therefore always processed last, and is processed twice if it also appears inside its own `PartyList`. When a member slot is empty (0) or out of range, all four zone branches fail and the iteration does nothing.

Membership alone is not sufficient: each candidate must also have `pMob[party].MOB.CurrentScore.Hp > 0` (the dead and the disconnected are excluded) and must satisfy one of the four position predicates. The first three predicates are hard-coded tower-room pairs expressed in 128-cell block coordinates: Pesa A requires both killer and member in block (9,1); Pesa M requires block (8,2); Pesa N requires block (10,2). The fourth predicate, the default, is a 33x33 Chebyshev box around the killer: `tx` within `memberX ± 16` and `ty` within `memberY ± 16` (`533-534`).

Note that the default predicate compares the *killer's* coordinates to the *member's* coordinates, so it is a true proximity test, while the Pesa predicates compare absolute blocks and therefore require both parties to be in the same room, at any distance inside it. There is no line-of-sight, no party-share percentage and no distance-based attenuation of the reward.

**Rule workflow**:
1. `i` from 0 to 11 → `party = PartyList[i]`; `i == 12` → `party = Leader`.
2. If `party <= 0` or `party >= MAX_USER` → skip.
3. If `pMob[party].HP <= 0` → skip.
4. If killer and member are both in Pesa A / M / N blocks → take that branch.
5. Else if member within `±16` cells of killer → default branch; otherwise skip.

---

### Business Rule: BR-11 — Base experience and level-ratio scaling

**Overview**: Base exp is the victim's stored `Exp` value passed through `GetExpApply`, which scales it by the attacker/victim level ratio and applies class-master quest gates.

**Detailed description**:
`GetExpApply(extra, exp, attackerLevel, targetLevel)` (`Code/TMSrv/GetFunc.cpp:902-945`) is called once for the killer (`214`) to produce `MobExp` (also stored as `eMob`), and again once per eligible party member (`255`, `350`, `444`, `536`). The function first applies class-master gates: ARCH characters receive no exp at all for mobs at or above level 354/369 until the corresponding quest flags are set, and receive 50% otherwise; CELESTIAL characters are blocked at 40/90 without quests. All celestial variants have their attacker level forced to `MAX_LEVEL`.

The level ratio is computed after incrementing both levels (`929-930`) to avoid division by zero: `multiexp = (target * 100) / attacker`. If the ratio is below 80 and the attacker is at least level 50, the ratio becomes `multiexp * 2 - 100`, a linear penalty that reaches zero when the attacker is four times the victim's level; ratios above 200 are clamped to 200, and negative results are floored at zero. The final value is `(exp * multiexp + 1) / 100`.

Two structural problems are visible at the call sites. First, `(int)pMob[target].MOB.Exp` truncates a `long long` to `int` (`214`, `255`, …), so any victim whose stored exp exceeds 2,147,483,647 overflows into a negative or wrapped value, which then trips the `exp > 0` guard and silently yields zero exp. Second, the parameter is declared `STRUCT_MOBEXTRA extra` **by value** (`GetFunc.h:39`), copying roughly 400 bytes per member per kill.

**Rule workflow**:
1. ARCH: block at 354/369 quest thresholds, else halve.
2. CELESTIAL: block at 40/90 quest thresholds.
3. Celestial variants: force attacker level to 399.
4. If victim level > 400 or either level negative → return raw exp.
5. `multiexp = (t+1)*100/(a+1)`; low-ratio penalty; clamp to [0, 200].
6. `exp = (exp * multiexp + 1) / 100`.

---

### Business Rule: BR-12 — Class-master level offset and divisor tiers

**Overview**: Non-mortal/non-arch characters are treated as 400 levels higher for the purposes of a divisor ladder that shrinks their exp per kill.

**Detailed description**:
`myLevel` starts as `CurrentScore.Level`. If `extra.ClassMaster` is neither `MORTAL` (2) nor `ARCH` (1) — i.e. any celestial class — then `MAX_LEVEL + 1` (400) is added (`259-260`). Since celestial base levels are capped at `MAX_CLEVEL = 199`, this yields an effective level of 400..599 and permanently pins them into the final divisor bucket.

The ladder is applied only to celestial characters: `< 120 → /10`, `< 150 → /20`, `< 170 → /40`, `< 180 → /80`, `< 190 → /160`, `else → /320` (`265-284`). Because the offset has already been added, no celestial can ever land in the first five buckets; the effective rule is "celestial exp is divided by 320". For mortal and arch characters `myLevel` remains the real level, but the whole block is skipped, so the ladder never applies to them at all. The rule therefore degenerates to a flat celestial penalty rather than a graded one.

There is a further inconsistency: the divisor is applied to the **member's** level, not to a comparison between member and victim level, so a level-40 celestial killing a level-15 mob receives the same `/320` treatment as a level-199 celestial killing a level-199 mob.

**Rule workflow**:
1. `myLevel = member level`.
2. If class is not MORTAL and not ARCH → `myLevel += 400`.
3. If class is not MORTAL and not ARCH → apply the divisor ladder to `exp`.
4. Otherwise leave `exp` unchanged.
5. Continue to BR-13.

---

### Business Rule: BR-13 — Share multiplier and bonus stack

**Overview**: After the divisor, every recipient's exp is multiplied by 0.6 and then subjected to five multiplicative/additive modifiers in a fixed order.

**Detailed description**:
`exp = 6 * exp / 10` (`285`) is applied unconditionally, including to a solo killer. This is the de-facto "kill share" — the design intent appears to be that a party member receives 60% of the base and the remaining 40% is the server's sink. Because it is applied before every bonus, all bonuses are computed on the reduced value.

The stack, in order: (1) `ExpBonus` — if the **killer's** `pMob[conn].ExpBonus` is in `1..499`, add that percentage (`290-291`); the killer's bonus is used for every member, not the member's own. (2) RvR kingdom bonus — if `RvRBonus` equals the member's clan, add 10% (`293-294`). (3) Newbie server bonus — if `NewbieEventServer` is on, the member is below level 100 and is not a celestial class, add 25% (`296-297`). (4) `DOUBLEMODE` — multiply by 2 (`299-300`). (5) `KefraLive == 0` — divide by 2, i.e. when the Kefra world boss is alive the whole server earns half exp (`302-303`). (6) Season modifier — `+15%` when `NewbieEventServer` is on, otherwise `-15%` (`305-308`).

The final modifier is a global −15% on production servers, which means the nominal 60% share is really 51%. The ordering matters: `DOUBLEMODE` doubles the already bonused value, and the `KefraLive` halving happens after doubling, so the two cancel (2 × 0.5 = 1) for every member while the boss is alive.

**Rule workflow**:
1. `exp = exp * 6 / 10`.
2. `+ExpBonus%` (killer's bonus, 1..499).
3. `+10%` if member's clan == `RvRBonus`.
4. `+25%` for newbie-server low-level non-celestials.
5. `×2` if `DOUBLEMODE`.
6. `÷2` if `KefraLive == 0`.
7. `+15%` if `NewbieEventServer`, else `−15%`.

---

### Business Rule: BR-14 — Daily experience accounting

**Overview**: Each character maintains a per-day exp total (`extra.DayLog`) that is reset when the calendar day changes, enabling daily ranking/reporting.

**Detailed description**:
`localtime` is called once per kill (`238-241`) and `tm_yday` (0..365) is used as the day key. If the stored `YearDay` differs from the current day, `DayLog.Exp` is zeroed first (`311-312`); the key is then written and the just-computed `exp` is accumulated (`314-315`).

Two consequences follow. First, the reset is lazy: it happens only when the character next receives exp, so a character that does not play for a month still carries the previous value until its next kill. Second, `tm_yday` is not a monotonic clock — a year boundary (`366 → 0`) would only reset because the values differ, which is coincidentally correct, but a leap-year change comparing 365 against 0 also resets, again correct. Using `tm_yday` instead of a date string makes the log ambiguous across years.

`localtime` returns a pointer to a shared static buffer. There is no `localtime_s` and no visible mutex in this file; if the server ever processes two kills from different threads, the structure can be torn. All observed callers are in the same single-threaded processing chain, so this is a latent rather than an active defect.

**Rule workflow**:
1. Obtain local time once per kill.
2. If `when.tm_yday != extra.DayLog.YearDay` → `DayLog.Exp = 0`.
3. `extra.DayLog.YearDay = when.tm_yday`.
4. `extra.DayLog.Exp += exp`.
5. Proceed to debt settlement.

---

### Business Rule: BR-15 — Experience-debt settlement

**Overview**: A character can carry an `extra.Hold` exp debt; it is paid off before any exp is credited, and a debt larger than the award consumes the whole award.

**Detailed description**:
`Hold` is an unsigned counter populated when a player dies in PvP (BR-41). Three cases exist in `318-333`. If `Hold > 0` and the award is at least the debt, the debt is zeroed (with an explicit negative guard at `322-323` because `nhold` is computed as a signed subtraction), the award is reduced by the debt, and normal crediting continues. If `Hold > 0` and the award is smaller, the debt is reduced by the award and the loop executes `continue` — the member receives **nothing** and the ranking packet is not sent for that member.

The third case — `Hold == 0` — bypasses both branches and credits the full award. Because the block sits after the daily-log accumulation, debt-serviced exp still counts toward the daily log (`315` runs before `318`), which means the daily log records the *nominal* award while the character's actual `MOB.Exp` records only the net. This is a deliberate-looking but undocumented divergence between the two accounting systems.

The comparison `(unsigned int)exp >= pMob[party].extra.Hold` mixes a cast-to-unsigned award with an unsigned debt; since `exp > 0` is guaranteed by the enclosing `if`, the cast is safe, but the resulting code is hard to audit.

**Rule workflow**:
1. If `Hold == 0` → credit the full award.
2. If `award >= Hold` → `award -= Hold`, `Hold = 0`, credit the remainder.
3. If `award < Hold` → `Hold -= award`, credit nothing, skip ranking.
4. On credit: `pMob[party].MOB.Exp += award`.

---

### Business Rule: BR-16 — Experience commit and ranking propagation

**Overview**: The awarded exp is added to the character's `MOB.Exp` and immediately reported to the DB server as a ranking update.

**Detailed description**:
`pMob[party].MOB.Exp += exp` (`335`) is the only place in the PvE path where experience becomes permanent. It is immediately followed by construction of a `STRUCT_RANKING` from the character's name, new exp, class master, current level and class, wrapping it in `MSG_UpdateExpRanking(party, rankInfo)` and pushing it to `DBServerSocket` (`338-341`).

The ranking packet is fire-and-forget: no acknowledgement, no retry and no error path. If the DB socket is down, the in-memory exp is still advanced and the ranking store diverges until the periodic `SaveUser`/logout path persists state. The packet is emitted per member per kill, so a 12-member party on a fast-clearing spawn generates 12 DB messages per monster.

Critically, this block does **not** call `CheckGetLevel`. Level advancement for the killer happens in `_MSG_Attack.cpp:1748` after the `MobKilled` loop, and for every other recipient it happens inside `GridMulticast`'s `_MSG_CNFMobKill` handling (`SendFunc.cpp:925`). A party member who is not inside the multicast view radius would therefore gain exp without a level-up check in this call — the resulting level-up is deferred to whatever later call happens to send them a `_MSG_CNFMobKill` or to `SendScore`.

**Rule workflow**:
1. Add the net award to `MOB.Exp`.
2. Build `STRUCT_RANKING(name, exp, classMaster, level, class)`.
3. Wrap in `MSG_UpdateExpRanking(party, …)`.
4. `DBServerSocket.SendOneMessage(...)`.
5. Level-up is *not* triggered here (see BR-18 / Section 9).

---

### Business Rule: BR-17 — Nightmare-zone override and exp-ceiling asymmetry

**Overview**: Three hard-coded room-block predicates (Pesa A/M/N) replace the proximity test and deliberately remove the per-member exp ceiling.

**Detailed description**:
The Pesa predicates at `252-253`, `347-348` and `441-442` compare the killer's and the member's coordinates divided by 128. Pesa A = block (9,1), Pesa M = block (8,2), Pesa N = block (10,2); each also requires the killer to be in the same block on both axes. These rooms are the "nightmare" instances, and the rule's purpose is to allow party members who are anywhere inside the room to share, not just those standing near the corpse.

The important business consequence is the ceiling. In the default proximity branch the code contains `if (exp > eMob) exp = eMob;` (`569-570`), capping each member at the killer's computed base exp. In all three Pesa branches those two lines are commented out (`287-288`, `382-383`, `476-477`). Since `eMob` is computed from the *killer's* level and the multiplier stack can push a celestial member's share above it, Pesa rooms permit individual shares larger than the killer's own base award, bounded only by the `exp <= 10000000` sanity check at `263`/`358`/`452`/`544`.

A second asymmetry: the `isExp` variable is initialised at `236` as `NumMob * MobExp / 100` (a party-size bonus based on `g_EmptyMob + ClassMaster` and `PARTYBONUS`), but every branch immediately overwrites it with a fresh `GetExpApply` result (`255`, `350`, `444`, `536`). The party-size term therefore never reaches the award; the only surviving effect of `g_EmptyMob`/`PARTYBONUS` is that `NumMob` is computed and discarded.

**Rule workflow**:
1. Evaluate Pesa A → Pesa M → Pesa N → proximity, in that order.
2. In Pesa branches: recompute `isExp` per member, skip the `exp > eMob` cap.
3. In the proximity branch: recompute `isExp` per member, apply the cap.
4. All branches converge on the divisor ladder (BR-12) and the bonus stack (BR-13).

---

### Business Rule: BR-18 — Death chatter

**Overview**: If the dead NPC's generator defines death actions, one of four is emitted at random as a chat line.

**Detailed description**:
`DieSay = rand() % 4` selects among `mNPCGen.pList[GenerateIndex].DieAction[DieSay][0]` and, when non-empty and when the victim is not a minion (`pMob[target].Leader == 0`), `SendChat(target, ...)` multicasts it from the corpse (`629-632`).

The bounds check on `GenerateIndex` is `>= 0` only — there is no upper bound against `MAX_NPCGENERATOR` (8,192). `DeleteMob` performs that check for its own access (`Server.cpp:7038`), so the omission here is inconsistent and constitutes an out-of-bounds read of `mNPCGen.pList` if `GenerateIndex` is ever corrupted or set by a non-generator path. Generator-produced mobs are always in range, so the defect is latent.

`rand()` with an unseeded generator means the "random" selection follows the same 4-value cycle on every process start; the chatter sequence is reproducible.

**Rule workflow**:
1. `DieSay = rand() % 4`.
2. If `GenerateIndex >= 0` and `DieAction[DieSay][0] != 0` and victim is not a minion → `SendChat(target, DieAction[DieSay])`.
3. Continue to broadcast.

---

### Business Rule: BR-19 — Kill broadcast and clan-gated deletion

**Overview**: After the exp block, the kill is broadcast; the corpse is deleted immediately unless the killer's clan is 0, 4, 7 or 8, in which case loot processing continues.

**Detailed description**:
`GridMulticast` of `sm` at `634` performs two jobs: it notifies nearby clients and (via `SendFunc.cpp:920`) triggers per-recipient level-up checks. Immediately afterwards (`636`) the function tests `pMob[conn].MOB.Clan`. A non-zero clan that is not 4 (summon), 7 (blue kingdom) or 8 (red kingdom) causes `DeleteMob(target, 1)` and the entire remaining loot path is skipped.

Because ordinary players have `Clan == 0`, this condition only excludes entities that carry an "other" clan value — typically event factions, hostile NPC factions, or corrupted state. Its practical effect is a safety valve: kills attributed to a faction that should not receive loot are resolved without any drop logic, avoiding the item duplication paths that follow.

Everything after `637` runs inside a nested `else` that ends at `2031`, so the drop logic, Kefra handling, castle/water/duel/rune/RvR hooks, gold, event items, treasure keys, audit logs and the final `DeleteMob` are all gated behind this clan test as well as the `ClassMaster` test of BR-10's enclosing `if` (see risk R-04).

**Rule workflow**:
1. Multicast `_MSG_CNFMobKill`.
2. If killer clan is non-zero and not 4, 7 or 8 → `DeleteMob(target, 1)`; skip everything else.
3. Else resolve drop coordinates (`PosX/PosY` or the corpse tile).
4. Continue with hook evaluation.

---

### Business Rule: BR-20 — Kefra world-boss resolution

**Overview**: Killing the Kefra boss assigns live-boss ownership to the killer's guild (or to "UoW"), awards 100 guild fame, announces server-wide, and distributes loot for every NPC standing inside a 60x59 rectangle.

**Detailed description**:
`pMob[target].GenerateIndex == KEFRA_BOSS` (396) triggers a self-contained sub-flow (`650-713`). If the killer has a guild, `BASE_GetGuildName` resolves the display name, `KefraLive` is set to the guild id (thereby ending the global exp halving of BR-13 for everyone), `GuildInfo[usGuild].Fame += 100` is applied and pushed to the DB with `MSG_GuildInfo`, a `_SN_End_Khepra` notice is broadcast, and `DrawConfig(TRUE)` refreshes the admin UI. Without a guild, `KefraLive = 1` and the notice names "UoW".

The loot distribution (`693-708`) is the anomalous part. It iterates `xx` from 2335 to 2394 and `yy` from 3896 to 3954, reads `pMobGrid[yy][xx]`, skips any entry below `MAX_USER`, picks `rand() % 60` and calls `PutItem(conn, &pMob[target].MOB.Carry[itemrand])` — one item per occupied cell, drawn from the boss's own carry array. The number of items awarded is therefore proportional to the number of NPCs left alive in that rectangle, not to a fixed prize table. `PutItem` fails silently once the killer's 64 carry slots are full (`Server.cpp:725-735`), so the practical ceiling is 64 items, but the rule can generate up to 64 items from a single boss kill and will spray the same item indices repeatedly.

The block ends with an `etc,kefra killed` audit line. No experience, coin or party distribution occurs here; those follow through the normal paths, but only if the `pHeightGrid` test at `714` passes.

**Rule workflow**:
1. Victim generator == 396.
2. Guild present → resolve name, `KefraLive = guild`, `Fame += 100`, DB guild update, notice.
3. Guild absent → `KefraLive = 1`, notice "UoW".
4. `DrawConfig(TRUE)`.
5. For each occupied cell in x∈[2335,2395), y∈[3896,3955): `PutItem(conn, boss.Carry[rand()%60])`.
6. Log `etc,kefra killed`.

---

### Business Rule: BR-21 — Terrain eligibility for loot

**Overview**: Loot generation is only allowed when the corpse's terrain height is within −50..92; otherwise the corpse is silently deleted.

**Detailed description**:
`pHeightGrid[AlvoY][AlvoX]` (`714`) is the walkable-surface height field (`char[4096][4096]`, `Server.h:56`). Values below −50 are "no floor" (void, water, map edge) and above 92 are blocked/unreachable terrain. The test gates *all* reward paths: castle gates, water rooms, boss tables, duel cards, rune quests, RvR, gold, event items, the loot loop and the quest amulet.

Failure takes the `else` at `2029`, which calls `DeleteMob(target, 1)` — but only after `GridMulticast` of the kill message at `634` and after the party exp block already ran. So a mob killed over a pit still pays experience (and mounts a level-up cascade) while producing no items and no gold, and the killer is never told why.

`AlvoX`/`AlvoY` come from `PosX`/`PosY` when non-zero (`641-647`), else from the corpse's tile. No range validation is applied to `PosX`/`PosY` before this indexing.

**Rule workflow**:
1. Determine `AlvoX`, `AlvoY`.
2. Read `pHeightGrid[AlvoY][AlvoX]`.
3. If `< -50` or `> 92` → `DeleteMob(target, 1)`; no rewards.
4. Else evaluate the specialised drop branches.

---

### Business Rule: BR-22 — Castle gate destruction

**Overview**: When the dying entity carries face item 220 during a castle siege, the gate item under it is opened instead of producing loot.

**Detailed description**:
`pMob[target].MOB.Equip[0].sIndex == 220 && CastleState` (`719`) identifies a castle gate proxy object. The corpse's own tile is bounds-checked (`721-725`) — the only place in the function where a grid index is validated — and `pItemGrid[y][x]` gives the gate item slot. If the slot is valid and occupied (`728`), `UpdateItem(ItemID, 1, &heigth)` attempts to switch the gate to its open state; on success a `MSG_UpdateItem` with `ItemID + 10000` (the standard "state change" encoding), the item's state, and a grid multicast notifies clients, and the item's `Delay` is reset to 0 (`731-750`). Failures are logged as `err,no castle gato to open`.

On any failure — out-of-bounds corpse, empty slot, failed update — a `Log` line is written (`723`, `754`) and the function continues into the shared tail (gold, drops, `DeleteMob`). The `return` at `724` is the only early exit in this region, and it aborts *after* experience has been granted.

The rule does not verify that the killer belongs to the attacking guild, so any killer of the gate proxy opens the gate.

**Rule workflow**:
1. Victim face item == 220 and `CastleState != 0`.
2. Validate corpse tile bounds; on failure log and return.
3. Resolve `pItemGrid[y][x]`; require `0 <= id < MAX_ITEM` and `pItem[id].Mode`.
4. `UpdateItem(id, 1, &height)`.
5. On success: build `MSG_UpdateItem`, multicast, set `Delay = 0`.
6. On failure: log `err,no castle gato to open`.
7. Continue with the shared tail.

---

### Business Rule: BR-23 — Water dungeon room-clear rewards

**Overview**: The last surviving monster of each "Agua M/N/A" room grants a room rune to the party leader (and to every party member for the first eight rooms) and starts a countdown sent to the whole party.

**Detailed description**:
Three parallel blocks handle the three water dungeons: `WATER_M_INITIAL` (10) + 0..7 and + 8..11 (`762-832`), `WATER_N_INITIAL` (171) + 0..7 and + 8..11 (`835-905`), `WATER_A_INITIAL` (183) + 0..7 and + 8..11 (`908-978`). In each, the trigger is `mNPCGen.pList[GenerateID].CurrentNumMob == 1` — evaluated *after* the mob has been marked dead, meaning "this was the last one". `CurrentNumMob` is decremented by `DeleteMob` (`Server.cpp:7040`), but that call happens later at `1990`, so the `== 1` test intentionally reads the pre-decrement count.

For rooms 0..7, `WaterClear1[roomGroup][Sala]` is clamped to a maximum of 15 seconds; for room 9 (the `+8..+11` group) it is clamped to 5. The clamp is a *write* to the global that then doubles as the countdown value (`... * 2`) sent through `_MSG_StartTime` to the leader and to every live party member. Rooms 0..7 additionally place a rune item in the leader's inventory: `778 + Sala` for M, `3174 + Sala` for N, `3183 + Sala` for A. Room 9 places no item.

The `slot_free` local is declared and never used (`779`, `852`, `925`) — a leftover from an earlier implementation that searched for a free carry slot. `PutItem` performs that search itself and fails silently if the leader's inventory is full, so on a full leader the rune is lost while the countdown still starts. The three blocks are 95% identical; only the initial constant, the row index and the item base differ.

**Rule workflow**:
1. Match `GenerateID` against the three dungeon ranges.
2. Require `CurrentNumMob == 1`.
3. Compute `Sala` (0..7) or force 9 for the boss group.
4. Clamp the clear time (15s or 5s) in `WaterClear1[][]`.
5. Resolve `partyleader` (default killer).
6. For rooms 0..7: `PutItem(leader, rune)`.
7. Send `_MSG_StartTime = time*2` to leader and all `USER_PLAY` party members.

---

### Business Rule: BR-24 — Castle Zakum quest boss prizes

**Overview**: If the dying entity is one of the castle quest bosses, the quest is marked cleared and leader plus party receive configured items, exp and coin.

**Detailed description**:
`CCastleZakum::MobKilled` (`CCastleZakum.cpp:210-297`) is invoked unconditionally at `980`, so every terrain-eligible kill pays the cost of a 64-iteration loop scanning `CastleQuest[0..63]` for a matching `BOSS[0]`/`BOSS[1]`. On a match it broadcasts `_SN_CastleQuest_Killed` to the castle rectangle, sets `CastleQuestClear = 1`, and pays the prize.

Payment is two-tiered. The leader first receives every non-empty `Prize[j]` (`229-233`) plus `ExpPrize[ClassMaster]` (indexed by class master, so the prize differs per class) plus `CoinPrize` subject to the 2,000,000,000 ceiling. Then, if `PartyPrize` is non-zero, every live party member receives the same item set, the same exp prize and the same coin prize (`258-294`). The party loop condition `i < MAX_PARTY && CastleQuest[k].PartyPrize` re-tests `PartyPrize` on each iteration but never changes it.

`PutItem` failing on a full inventory is silent, and the exp/coin are added regardless of whether the items were delivered. `ExpPrize` is an `int[6]` indexed by `ClassMaster`, which can legally be 0..5; values 6+ would be out of bounds, but no such class exists.

**Rule workflow**:
1. Scan 64 castle quests for `BOSS[0]`/`BOSS[1]` match.
2. On match: notice, `CastleQuestClear = 1`.
3. Pay items/exp/coin to leader.
4. If `PartyPrize`, pay the same to each live party member.
5. Return to the caller at `981`.

---

### Business Rule: BR-25 — Named event-mob hooks

**Overview**: Five specific generator ids produce single-purpose world effects: orc recall, Noatum tower reset, and two kingdom-clear flags.

**Detailed description**:
`ORC_GUERREIRO` (3) calls `DoRecall(conn)` (`983-984`), teleporting the killer back to town — a "you win the invasion" effect. `TORRE_NOATUM1..3` (23..25) zero `LiveTower[GenerateID - 23]` (`986-987`), marking one of the three siege towers destroyed so the corresponding wave logic stops.

`REI_HARABARD` (8) sets `Kingdom1Clear = 1` and broadcasts `_NN_King1_Killed` to the rectangle (1676,1556)-(1776,1636); `REI_GLANTUAR` (9) sets `Kingdom2Clear = 1` and broadcasts to (1676,1816)-(1776,1892) (`990-999`). These flags are one-way latches: nothing in this file resets them, so they are cleared only by whatever periodic content reset exists elsewhere.

The chain is an `else if` ladder in which the water-room blocks are tested first, so a generator id that matches a water range will never reach these hooks. Because the ids are mutually exclusive this is harmless, but it means the ordering is significant and undocumented.

**Rule workflow**:
1. Test water ranges (`if`/`else if` chain).
2. Test `ORC_GUERREIRO` → `DoRecall(conn)`.
3. Test `TORRE_NOATUM1..3` → `LiveTower[n] = 0`.
4. Test `REI_HARABARD` → `Kingdom1Clear = 1` + area notice.
5. Test `REI_GLANTUAR` → `Kingdom2Clear = 1` + area notice.

---

### Business Rule: BR-26 — Boss loot tables by generator id

**Overview**: Generators 0/1/2, 5/6/7 and 3 use small hard-coded loot tables with fixed probabilities.

**Detailed description**:
Generators 0, 1 and 2 (`1001-1020`) roll `rand() % 14`; a result of 0 yields item 419, a result of 1 yields item 420, anything else yields nothing (a 2-in-14 ≈ 14.3% chance of loot). Generators 5, 6 and 7 (`1022-1043`) roll `rand() % 14`; 0 yields `421 + rand()%7` (one of seven variants), 1 yields 419. Generator 3 (`1046-1073`) rolls `rand() % 7`; 0..3 yield items 1106, 1256, 1418 and 1568 respectively (4-in-7 ≈ 57%).

All three call `SetItemBonus` with different arguments: 0/1/2 and 5/6/7 pass `(item, 0, 0, 0)` (no level influence, `a3 = 0`), while generator 3 passes `(item, 75, 1, 0)` — level 75 with `a3 = 1`, which in `SetItemBonus` disables the `Level >= 210 → Level -= 47` adjustment and caps `lvdif` at 2 (`Server.cpp:1444-1445`). The generator 3 items are therefore always rolled at a fixed mid-tier refinement.

The 5/6/7 and 3 branches additionally require `pHeightGrid` in −40..36 (a narrower band than BR-21), while the 0/1/2 branch has no such check. The commented-out `CreateItem(AlvoX, AlvoY, &item, rand()%4, 1)` lines show that these items were once dropped on the ground and were later redirected straight into the killer's inventory.

**Rule workflow**:
1. `GenerateID in {0,1,2}`: roll 1/14 for 419, 1/14 for 420; `SetItemBonus(item,0,0,0)`; `PutItem(conn, item)`.
2. `GenerateID in {5,6,7}`: roll 1/14 for `421+rand()%7`, 1/14 for 419; height gate −40..36; `PutItem`.
3. `GenerateID == 3`: roll 4/7 for 1106/1256/1418/1568; `SetItemBonus(item,75,1,0)`; height gate; `PutItem`.

---

### Business Rule: BR-27 — Nightmare respawn rule

**Overview**: Any monster belonging to a nightmare generator range is immediately recreated at its configured spawn point.

**Detailed description**:
`GenerateIndex` in `NIGHTMARE_M` (2377..2384), `NIGHTMARE_N` (2368..2375) or `NIGHTMARE_A` (2385..2394) triggers `GenerateMob(GenerateIndex, 0, 0)` (`1076-1084`). `GenerateMob` with zero coordinates re-reads the generator's configured segment list (`Server.cpp:2995-3105`) and enforces `MaxNumMob`, so the respawn is bounded.

Because this runs before `DeleteMob(target, 1)` at `1990`, the generator's `CurrentNumMob` counter is momentarily one too high when the new mob is created; `GenerateMob` therefore has less headroom than intended, and the subsequent `DeleteMob` decrements back. At the boundary (`CurrentNumMob == MaxNumMob`) the respawn is silently refused, leaving the room one monster short until the periodic generator timer corrects it.

The three `else if` branches are mutually exclusive and produce identical behaviour; they exist only to document intent. All three are inside the same terrain/height gate, so a nightmare mob killed in the void is not respawned.

**Rule workflow**:
1. Match `GenerateIndex` against the three ranges.
2. `GenerateMob(GenerateIndex, 0, 0)`.
3. Continue to `DeleteMob`.

---

### Business Rule: BR-28 — Duel-card room progression (N/M/A)

**Overview**: Sixteen secret-room generators are grouped into three ladder sets of four rooms; clearing a room resets a 60-second timer, teleports surviving players out and increments the room counter.

**Detailed description**:
The three sets are `SECRET_ROOM_N_*` (2395..2404), `SECRET_ROOM_M_*` (2405..2414) and `SECRET_ROOM_A_*` (2415..2424), each with sala1 (2 mobs), sala2 (2), sala3 (2) and sala4 (4). The trigger for sala 1..3 is "the sum of `CurrentNumMob` over the room's generators equals 1", i.e. the last kill in the room (`1089-1092`, `1113-1116`, `1137-1140`). On trigger the code sets `CartaTime = 60`, teleports everyone still inside the room to the next `CartaPos[sala][x/y]` via `ClearAreaTeleport`, increments `CartaSala`, and multicasts `_MSG_StartTime` with `CartaTime * 2 = 120` to map (6,28) (`1094-1107`).

Sala 4 is different: it requires the sum over four generators to equal 1 and then only sets `CartaTime = 3` and `CartaSala = 4` (`1166-1171`), with the `ClearArea` call commented out. The three sets are byte-for-byte identical apart from constants — a 4x duplication of the same 264-line pattern.

The insertion is an `if` chain (not `else if`), so all 12 room blocks are evaluated on every kill; each performs four `mNPCGen.pList[]` reads and three additions. The teleports are unconditional for whoever is inside the rectangle, including players who did not participate.

**Rule workflow**:
1. For each of 12 room definitions: sum `CurrentNumMob` over the room's generators.
2. If sum == 1 and the room is sala 1..3 → `CartaTime = 60`, teleport occupants to `CartaPos`, `CartaSala++`, multicast 120s timer.
3. If sum == 1 and the room is sala 4 → `CartaTime = 3`, `CartaSala = 4`.
4. Continue to the next room definition.

---

### Business Rule: BR-29 — Rune quest ladder (LV0..LV6)

**Overview**: Seven independent quest tiers react to specific boss/minion kills, granting runes to the whole party and advancing the next tier, some with probabilistic gating and some with multi-party counters.

**Detailed description**:
**LV0 (Lich, 5653/5654)** — `rand() % 100 < 20` succeeds: all Lich mobs are deleted (`1360-1369`), the leader and each live party member receive `PistaRune[0][rand()%4]`, and the leader additionally receives item 5134 with effect 43 value 1 as the "next pista" pass (`1379-1402`). On failure (80%), all Lich mobs are deleted and four are regenerated (two of each id, `1409-1424`). The success log names the leader.

**LV1 (towers 5706..5708, mobs 5709..5764)** — killing a tower zeroes `Pista[1].Party[n].MobCount` (`1429-1434`). Killing a tier mob increments the matching party's `MobCount` only when the corresponding tower is still alive and the killer's leader matches the registered `LeaderID` (`1436-1451`).

**LV2 (Amon boss 5789)** — unconditional: party runes from `PistaRune[2][rand()%5]`, next-pista item with value 3 (`1454-1491`). **LV3 (Sulrang 5972..5975, bosses 5948..5955)** — the first Sulrang kill after registration sets `MobCount = 1` and spawns a random boss from the 8-id boss range (`1494-1499`); each boss kill spawns a replacement and increments the corresponding party counter (`2001-2020`). **LV4 (maze 5854..5898, boss 5849)** — a three-party lockstep: the kill is only counted when the killer's leader matches party *n* and the other two parties have `MobCount >= 1`; when a counter reaches zero the whole party is teleported to (3351/3352, 1334) and the boss is spawned (`1502-1568`). **LV5 (Balrog 5899)** — sets `MobCount = 1`, grants runes from `PistaRune[5][rand()%6]`, next item value 6 (`1610-1649`). **LV6 (rabbit 5775..5785, boss 5767)** — decrements a counter and spawns the boss when it hits the value 1 (`1652-1663`); boss kill grants `PistaRune[6][rand()%8]` and logs, with no next-pista item (`1665-1693`).

Cross-cutting properties: rune items are rolled **separately per member** inside the party loop, so members receive different runes; `PutItem` failures are silent; the leader is also inside the party list in some flows and may receive two runes; and all of it sits behind the terrain gate, so a boss killed in the void neither advances the ladder nor grants runes.

**Rule workflow**:
1. Match generator id against LV0..LV6 patterns (independent `if`s, not a ladder).
2. LV0: 20% → delete all Lich, grant runes + pass item; 80% → delete all Lich, regenerate four.
3. LV1: towers reset counters; tier mobs increment counters for the matching leader.
4. LV2/LV5/LV6-boss/LV4-boss: grant per-member runes and optional pass item.
5. LV3: first Sulrang kill spawns a boss; boss kills keep spawning and counting.
6. LV4: require the other two parties cleared; on last counter zero → teleport party + spawn boss.

---

### Business Rule: BR-30 — RvR tower resolution

**Overview**: Destroying either RvR tower clears the war arena, deletes all tower instances, announces the winning kingdom and applies a kingdom-wide exp bonus until the next war.

**Detailed description**:
`GenerateID == RVRTORRE_1 || RVRTORRE_2` (4236/4237) triggers a full war teardown (`1697-1723`): `ClearArea(1020,1916,1286,2178)` recalls every player inside the arena, every remaining tower instance is deleted (`1701-1705`), and if the killer has a clan the appropriate kingdom name is resolved (8 = red, 7 = blue) and broadcast through `_SN_KINGDOMWAR_DROP_`. `RvRBonus` is then set to the killer's clan, which is exactly the value tested in the experience bonus of BR-13, and `RvRState` is reset to 0.

The rule therefore converts a single kill into a persistent server-wide exp modifier for one kingdom. It is also the only place `RvRBonus` is assigned in this file, and there is no symmetric "clear" path: the bonus persists until the next war overwrites it.

`ClearArea` restores players whose HP is <= 0 to 2 HP before recalling them (`Server.cpp:5767-5773`), so a player who died at the moment the tower fell is revived rather than left at zero — an important safety interaction with the PvP path.

**Rule workflow**:
1. Victim generator is RvR tower 1 or 2.
2. `ClearArea` the war rectangle.
3. Delete every live RvR tower instance.
4. If killer has a clan: resolve kingdom message, broadcast, `RvRBonus = clan`.
5. `RvRState = 0`; log the winner.

---

### Business Rule: BR-31 — Coin drop rule

**Overview**: A victim's `Coin` field is converted into a coin payout using a level-dependent chance band, a randomised amount, a 2000 cap and a 2,000,000,000 wallet ceiling.

**Detailed description**:
`UNKGOLD` is the exclusion band: 2 for victims below level 10, 4 below 20, 6 below 30, 9 below 50, and 18 otherwise (`1732-1739`). It is then replaced by `rand() % (UNKGOLD + 1)` (`1741`), so the chance of a payout is `1/(band+1)` — roughly 33% at level < 10, 20% at < 20, 14% at < 30, 10% at < 50 and 5.3% above. A payout additionally requires the victim's `MOB.Coin` to be non-zero.

The amount is `4 * (rand() % (((MobCoin+1)/4)+1) + (MobCoin+1)/4 + MobCoin)` (`1745`). For a victim coin value `c` this is approximately `4 * (c + (c+1)/4 + uniform(0..(c+1)/4))`, i.e. roughly 5x to 6x the victim's base coin, quantised in fours. It is clamped to 2000. The wallet is protected by `MobCoin + pMob[conn].MOB.Coin > 2000000000` → `_NN_Cant_get_more_than_2G` message and no credit (`1750-1751`); note that the comparison is performed on a signed `int` sum which cannot overflow for the values involved (2000 + 2e9 is within `int` range) but would if the caller's coin were already above 2,147,481,647.

On success, `pMob[conn].MOB.Coin += MobCoin` and `SendEtc(conn)` refreshes the client's currency display. The payout goes exclusively to the killer, never to party members, regardless of the party exp split.

**Rule workflow**:
1. Read victim coin; if 0 → no payout.
2. Select band from victim level (2/4/6/9/18).
3. Roll `rand() % (band+1)`; non-zero → no payout.
4. Compute amount, clamp to 2000.
5. If the killer's wallet would exceed 2e9 → message only.
6. Else credit and `SendEtc(conn)`.

---

### Business Rule: BR-32 — Global event item drop

**Overview**: When the server-side event is armed, each kill has a `1/evRate` chance to award the configured event item, optionally serial-numbered, with an optional broadcast.

**Detailed description**:
The guard is a seven-way conjunction (`1760`): `evOn`, `evStartIndex`, `evEndIndex`, `evItem`, `evRate`, `evCurrentIndex < evEndIndex`, and `rand() % evRate == 0`. The `evStartIndex` test is dead in this context — the field is required to be non-zero but never compared with the kill — so any event with a start index set behaves identically to one without. `evRate` is the reciprocal chance: `evRate = 100` means 1%, `evRate = 1` means every kill.

Serialisation: if `evIndex` is set, effect 62 receives `evCurrentIndex / 256`, effect 63 receives `evCurrentIndex`, and effect 59 receives a random value (`1769-1774`); the notice then includes the serial. Otherwise a simpler notice is used. `evCurrentIndex` is incremented on every award (`1784`) and is **not** rolled back if `PutItem` fails, so a full inventory burns event serials.

`SetItemBonus(&item, victimLevel, 0, 0)` runs *after* the effects are written (`1786`), which means the bonus roll can overwrite the engraving: `SetItemBonus` clears/reassigns effect 0 when it holds a grade effect, and writes its own effects for grade/level. The serial is therefore not guaranteed to survive on all item types. `DrawConfig(1)` marks configuration as dirty on every drop.

**Rule workflow**:
1. Test the seven conditions.
2. Roll `rand() % evRate`; require 0.
3. Build the event item; add serial effects if `evIndex`.
4. Format and optionally broadcast the notice.
5. `evCurrentIndex++`.
6. `SetItemBonus(item, victimLevel, 0, 0)`; `PutItem(conn, item)`; `DrawConfig(1)`.

---

### Business Rule: BR-33 — Common loot roll (per carry slot)

**Overview**: Every non-empty carry slot of the dead monster is an independent loot roll, filtered through a drop-rate table, a drop-bonus divisor, a victim-level scaling curve, server-mode divisors, an optional `Rsv` penalty, two forced slots, a 32,000 cap and an item-eligibility filter.

**Detailed description**:
The loop iterates all 64 carry slots (`1793`). A slot with `sIndex == 0` is skipped. `droprate = g_pDropRate[i]` and `dropbonus = g_pDropBonus[i] + pMob[conn].DropBonus` (`1798-1799`). When the combined bonus differs from 100 it is inverted (`dropbonus = 10000 / (dropbonus + 1)`) and the rate is rescaled (`droprate = dropbonus * droprate / 100`), so a bonus of 0 gives a 100x divisor (rate / 100) and a bonus of 900 gives a 1/10.1 divisor (rate × 9.9).

Level scaling splits the table in two. For `i < 60` in the first three carry rows (`pos = i/8` in {0,1,2}), the rate is multiplied by 4/5/6/7/8/99 percent for victim levels < 10 / < 20 / < 30 / < 40 / < 60 / else (`1813-1829`) — note the last bucket is 99%, effectively no penalty. For every other slot the multipliers are 90/60/50/43/38/50 percent for < 170 / < 200 / < 230 / < 255 / < 320 / else (`1835-1851`). The two curves have opposite biases: low-row items are heavily suppressed on low-level victims, high-row items are mildly suppressed everywhere.

Then `TESTSERVER` halves the rate and `LOCALSERVER` divides it by 100 (`1854-1858`). If the killer has `Rsv & 4`, the rate is reduced by `100 - (Special[2]/10 + 10)` percent (`1860-1866`). Slots 8, 9 and 10 are forced to rate 4 and slot 11 to rate 1 (`1868-1872`), then the rate is capped at 32,000 and the roll is `rand() % droprate`; a zero result (or slot 11 unconditionally) means the item drops (`1874-1882`).

Eligibility filters follow: `sIndex <= 390` or `>= MAX_ITEMLIST` (6500) are rejected, item 454 is rejected (`1886-1890`). If the item's `ReqLvl` is below 140, or the post-roll `droprate` is odd, the item is granted after `SetItemBonus(item, victimLevel, 0, killerDropBonus)`; otherwise the item is silently discarded (`1892-1895`). Because `droprate` is zero at that point for normal drops, `0 % 2 != 1` always passes, so the `ReqLvl` gate is effectively "grant everything"; for slot 11 the parity test can discard the guaranteed item.

Slots 8, 9 and 10 set the loop variable to 11 after a successful drop (`1941-1942`), which skips slots 9..11 and therefore guarantees the slot-11 item is evaluated last.

**Rule workflow**:
1. For `i` in 0..63: skip empty slots.
2. Load `g_pDropRate[i]`, `g_pDropBonus[i] + killer.DropBonus`; rescale if bonus != 100.
3. Apply the low-row or high-row victim-level curve.
4. Apply `TESTSERVER`, `LOCALSERVER`, `Rsv & 4` divisors.
5. Force rates for slots 8/9/10 (=4) and 11 (=1).
6. Cap at 32,000; `rand() % droprate`; drop on 0 or slot 11.
7. Filter index (391..6499, not 454) and `ReqLvl`/parity.
8. `SetItemBonus`; intercept treasure keys (BR-34); `PutItem`.
9. Audit + DB day log; if slot was 8/9/10 → jump to 11.

---

### Business Rule: BR-34 — Treasure-key interception

**Overview**: Loot items that carry the key effect (`EF_KEYID` 11..14) are converted into castle quest keys and delivered directly, bypassing the normal `PutItem` path.

**Detailed description**:
`CCastleZakum::KeyDrop(target, conn, PosX, PosY, item)` (`CCastleZakum.cpp:299-312`) reads `BASE_GetItemAbility(Key, EF_KEYID)`. If the value is 11..14, the item's first effect is overwritten with `EF_QUEST` (58) and `CastleQuestLevel`, the item is placed with `PutItem(conn, Key)`, and the function returns `FALSE`. The caller then skips its own `PutItem` (`1904-1905`: `if (KeyDrop(...) == TRUE) PutItem(conn, item);`).

The inverted return convention is the rule's defining hazard: `KeyDrop` performs the placement itself and returns `FALSE` to mean "already handled", so a reader who assumes `BOOL` success semantics will misread the call as "only place the item when the key check fails". Any key whose `EF_KEYID` falls outside 11..14 returns `TRUE` and is placed normally, which means the key effect is left on the item and the client sees a non-quest key.

A second quirk: because the effect is overwritten *before* `PutItem`, the item is mutated in the monster's own carry array (the caller passed a pointer into `pMob[target].MOB.Carry[i]`), so the corpse data is modified as a side effect of looting.

**Rule workflow**:
1. Read `EF_KEYID` from the candidate item.
2. If 11..14: set effect 0 to `EF_QUEST`/`CastleQuestLevel`, `PutItem(conn, item)`, return `FALSE`.
3. Caller: `FALSE` → skip its own placement; `TRUE` → `PutItem(conn, item)`.
4. Audit log proceeds regardless.

---

### Business Rule: BR-35 — Loot audit trail

**Overview**: Every successful drop writes a human-readable log line, a per-account attribution file entry, and a DB day-log message.

**Detailed description**:
For each drop the code builds a `sprintf` line containing the killer's mob name, the item name and index, all six effect bytes, and the victim's name (`1907`). If the killer is a player, `Log(temp, pUser[conn].AccountName, pUser[conn].IP)` writes it with account and IP attribution (`1909-1910`). Then a `MSG_STANDARDPARM` with `Type = _MSG_DBItemDayLog`, `ID = 0` and `Parm = item->sIndex` is pushed to `DBServerSocket` (`1912-1920`), which increments the DB-side daily item counter.

When `LOCALSERVER` is set, an additional debug `SendSay` echoes the item name, victim level, required level and the three `g_dLevel*` globals left over from `SetItemBonus` (`1923-1939`); the condition `itempos & 0xFE && itempos != 128` restricts the echo to equipment-usable items.

This block runs *before* `PutItem` validation succeeds — `PutItem`'s boolean return is discarded at `1905` — so a drop into a full inventory is logged and counted in the DB as if it had been delivered. That inflates the day-log statistics and makes item-duplication investigations harder.

**Rule workflow**:
1. Format the drop line.
2. If killer is a player → `Log(line, account, ip)`.
3. Send `_MSG_DBItemDayLog` with the item index.
4. If `LOCALSERVER` and the item is equipment → `SendSay` debug echo.
5. Place the item (result ignored).

---

### Business Rule: BR-36 — Mystic-land quest amulet

**Overview**: Killing a monster whose face item is 239 or 241 has a 5% chance to complete the "Terra Mistica" quest step for a mortal character.

**Detailed description**:
The gate is `conn < MAX_USER && (Equip[0] == 239 || Equip[0] == 241) && !(rand() % 20)` (`1948`) — a 1-in-20 chance per kill of a specific monster family. On success, only characters whose `QuestInfo.Mortal.TerraMistica == 1` are affected: they receive `_NN_Watching_Town_Success` and the flag advances to 2 (`1950-1954`). Characters who never started the quest (`0`) or already completed it (`2`) are silently unaffected, so the roll is wasted.

`rand() % 20` is evaluated as part of a short-circuit conjunction, so the roll only happens when the killer is a player and the victim matches — correct, but it means the RNG stream position depends on how many such mobs are killed, which matters if deterministic drop reproduction is ever attempted.

The rule is the only quest interaction inside the loot loop and it runs *after* the 64-slot loop, so it cannot be blocked by a full inventory.

**Rule workflow**:
1. Killer is a player and victim face item is 239 or 241.
2. Roll 1-in-20.
3. If quest flag == 1 → message and set flag to 2.
4. Continue to corpse removal.

---

### Business Rule: BR-37 — Corpse removal and respawn loops

**Overview**: All terrain-eligible PvE paths end with `DeleteMob`, followed by five conditional respawn rules and the war-tower delegate.

**Detailed description**:
`DeleteMob(target, 1)` (`1990`) multicasts `_MSG_RemoveMob`, decrements the generator's `CurrentNumMob`, zeroes the mob slot, clears its grid cell and calls `RemoveParty` (`Server.cpp:7020-7053`). The `Type = 1` argument is the "normal death" removal type used by clients for the death animation.

After removal, four respawn rules fire: rune-quest LV1 mobs (`5709..5764`) and LV2 mobs (`5790..5848`) are regenerated at the same generator (`1993-1999`); LV3 boss-range kills regenerate a random boss and increment the matching party counter when the party is registered (`2001-2020`); and Kefra minions (`397..400`) are regenerated while `KefraLive == 0` (`2023-2024`). Finally `CWarTower::MobKilled` (`2027`) handles the guild tower (`GTORRE`) replacement and ownership.

Two of these rules (LV1, LV2, Kefra) are "endless" generators: they respawn unconditionally on every kill, so a single generator slot can be farmed indefinitely. LV3's respawn depends on `Pista[3].Party[0].MobCount != 0`, which is incremented rather than decremented on each boss kill (`2013-2019`) — the counter therefore never reaches zero and the boss chain runs until the quest state is reset externally.

**Rule workflow**:
1. `DeleteMob(target, 1)`.
2. LV1 mob kill → `GenerateMob(same id)`.
3. LV2 mob kill → `GenerateMob(same id)`.
4. LV3 boss kill and party registered → spawn random boss, increment party counter.
5. Kefra minion kill while `KefraLive == 0` → `GenerateMob(same id)`.
6. `CWarTower::MobKilled(target, conn, PosX, PosY)`.

---

### Business Rule: BR-38 — PvP branch entry and level guard

**Overview**: Any death that is not classified as PvE is treated as PvP, but only victims within level 0..399 are processed.

**Detailed description**:
The `else` at `2059` catches every player victim (and, in theory, a level 400..404 NPC that failed the inner PvE test). The first action is a level sanity check: `tlevel < 0 || tlevel > MAX_LEVEL` returns immediately (`2063-2064`), leaving the victim at zero HP and *without* a death broadcast, since `GridMulticast` happens later at `2161`. That is a genuine hole: a corrupted or out-of-band level leaves an invisible corpse that never gets removed.

`tlevel` is read from `BaseScore.Level` while the exp tables and the rest of the rule use `CurrentScore.Level` in several places (`2229`), which can diverge when temporary effects alter the current score. The PvP path also never calls `DeleteMob` on the victim — a dead player remains in the world as a corpse until their own client reconnects or a respawn path handles them.

**Rule workflow**:
1. Non-PvE victim enters the PvP block.
2. Read `tlevel` from `BaseScore.Level`.
3. If `< 0` or `> 399` → return (no broadcast, no removal).
4. Snapshot counters and continue.

---

### Business Rule: BR-39 — PvP experience-loss amount

**Overview**: The exp a victim loses is a fraction of the exp gap to their next level, determined by a level-tier divisor and then multiplied by a PK-point band and a server-mode divisor.

**Detailed description**:
`curexp` and `nextexp` come from `g_pNextLevel` for mortal/arch or `g_pNextLevel_2` for celestial classes; `alphaexp` is the gap and `deltaexp` starts as `alphaexp / 20` (`2067-2070`). The divisor then tightens with level: 22 at level 30+, 25 at 40+, 30 at 50+, 35 at 60+, 40 at 70+, 45 at 80+, 50 at 90+, 55 at 100+, 70 at 150+, 85 at 200+, 100 at 250+ (`2072-2103`). Because each tier overwrites the previous unconditionally, the effective divisor is the last matching tier, i.e. a level 260 mortal loses 1% of their level gap.

The amount is then doubled or tripled by the victim's guilt: `deltaexp *= 3` when `killed_pkpoint` is in 11..25, otherwise `*= 5` (`2122-2125`) — counter-intuitively, *clean* players (PK point 0..10) lose five times as much as moderately guilty ones. It is then divided by 6 on production, or by 3 on test servers (`2133-2137`), and hard-clamped to `[0, 150000]`. In the arena branch it is divided by 4 again on test servers, increased by up to 25% when the victim and killer are in opposing kingdoms (`2202-2203`), then clamped to 30000 (`2205-2209`).

The `unsigned int` arithmetic deserves note: `nextexp - curexp` cannot go negative for a well-formed table, and `deltaexp` is clamped after all multiplications, so no wrap occurs in practice.

**Rule workflow**:
1. Read table entries for `tlevel` and `tlevel + 1`.
2. `deltaexp = gap / 20`.
3. Overwrite with the last matching level-tier divisor.
4. `×3` or `×5` by PK-point band.
5. Divide by 6 (or 3 on TESTSERVER).
6. Clamp to 0..150,000; later clamp to 0..30,000 after war bonuses.

---

### Business Rule: BR-40 — War and safe-zone determination

**Overview**: The victim's location and the two players' affiliations decide whether the death is "at war" (no exp loss, no PK penalty) and whether the death happens in a no-loss arena.

**Detailed description**:
`AtWar` is set by any of four conditions (`2142-2157`): a mutual guild war (`g_pGuildWar[a] == b && g_pGuildWar[b] == a`), an active castle with the victim in block (8,13), a non-zero `RvRState`, or a non-zero `GTorreState`. `SameClan` is set when the two players are in opposing kingdoms (7 vs 8), which is the RvR enemy relation, not a literal "same clan" — the variable name is misleading.

`arena = BASE_GetArena(x,y)` returns 0..4 for guild-zone rectangles and 5 for "not in a guild zone"; `village = BASE_GetVillage(x,y)` does the same for city limits. `ZoneUnk` forces the arena test when the victim is in block (1,31) (`2167-2168`). The condition `arena != 5 || village != 5 || ZoneUnk` therefore means "inside a war zone or a city", and that entire branch is the *no-exp-loss* branch: it sends `_NN_In_Arena_No_Exp_Loss`, optionally appends a forensic line to `TesteDie.txt`, and creates item 431 on the ground when `arena` is a valid guild zone (`2185-2194`).

Three operational hazards live here. The `TesteDie.txt` write (`2176-2183`) calls `fprintf` on the result of `fopen(..., "a+")` with no NULL check, so an unwritable working directory crashes the server. The item-431 creation uses `CreateItem` with a random rotation and `Create = 1`, unconditionally, once per death inside a guild zone. And the "arena" naming in the message contradicts the actual test: players in a *city* also receive the "in arena, no exp loss" message.

**Rule workflow**:
1. Compute `AtWar` from guild war / castle block / RvR / GTorre.
2. Compute `arena`, `village`, `ZoneUnk`.
3. If `arena != 5 || village != 5 || ZoneUnk` → no-exp-loss branch: message, optional `TesteDie.txt` line, optional item 431.
4. Else → exp-loss branch (BR-41).

---

### Business Rule: BR-41 — Experience loss and exp-debt application

**Overview**: Rather than subtracting exp, the PvP death credits the computed loss into `extra.Hold`, which is then drained by future kills; low-level and newbie-server victims are exempt.

**Detailed description**:
The exp-loss branch (`2197-2230`) first re-applies the test-server divisor and the RvR `SameClan` bonus, then applies the two clamps. The debt is credited as `min(deltaexp, currentExp)` — the code writes `(pMob[target].MOB.Exp > deltaexp ? deltaexp : pMob[target].MOB.Exp)` (`2217`, `2220`), so a character with less exp than the loss accrues only what they have. Because `MOB.Exp` is `long long` and `Hold` is `unsigned int`, a character with more than 4.29 billion exp loses at most 4.29 billion per death, which is acceptable but not obvious.

The exemption ladder: if the victim is level 35+ or not mortal, and `NewbieEventServer == 0`, the debt is applied and the message `_DN_lose_D_exp` is sent (with `SendEtc`), but only when `DEADPOINT` is set or the killer is a player (`2222-2227`). Otherwise, if the victim is below `FREEEXP` (35) and is mortal and not on a newbie server, `_NN_Below_lv20_No_Exp_Loss` is sent and nothing is deducted (`2229-2230`).

The debt mechanic means exp loss is *deferred and non-monetary*: no exp leaves the character at death time, and the next kills pay it off before granting progress (BR-15). A character who stops playing keeps the debt on the record until their next kill, and the message the player sees ("you lost N exp") describes a debt that has not yet been paid.

**Rule workflow**:
1. Apply test-server divisor and same-kingdom bonus; clamp to 0..30,000.
2. If victim is level >= 35 or not mortal, and not newbie server: `Hold += min(deltaexp, MOB.Exp)`.
3. If `DEADPOINT` or the killer is a player: message + `SendEtc`.
4. Else if victim < 35 and mortal and not newbie server: "no exp loss below level 20" message.
5. Continue to the affiliation branches.

---

### Business Rule: BR-42 — Kill counters, PK points and RvR respawn

**Overview**: Same-kingdom (RvR enemy) kills award kill counts, may reduce PK points, and respawn the victim at their kingdom's spawn point with 2 HP.

**Detailed description**:
The `SameClan` block (`2232-2276`) increments the killer's current-kill and total-kill counters via `SetCurKill`/`SetTotKill` (which store into the invisible `Carry[KILL_MARK]` item, capped at 200 and 32,767 respectively) and calls `SendEtc(conn)`. When neither `AtWar` nor the victim's guilt applies, and the server is not in newbie mode, the killer's PK point is reduced by `3 * killed_pkpoint / -20`, floored at −9 and capped at 0, then applied with `SetPKPoint` and reported through `_DD_PKPointMinus` (`2242-2258`).

If the victim's tile is inside the RvR arena rectangle (1017..1290, 1911..2183), the victim is respawned with 2 HP: clan 7 → (1061..1062, 2129..2130) and `RvRRedPoint++`; clan 8 → (1237..1238, 1966..1967) and `RvRBluePoint++` (`2260-2275`). The random offsets are `rand() % 2`, i.e. one of two adjacent tiles. Because `MOB.CurrentScore.Hp = 2` is set before `DoTeleport`, the victim is not dead on arrival — a deliberate anti-camping measure.

In the *hostile* branch, the mirrored rule (`2429-2435`) grants the victim one PK point (up to 75) whenever they were not at war and the killer is a player, with the message `_DD_PKPointPlus`. PK points are therefore a two-sided ledger: the victim of a hostile kill becomes guiltier, and the RvR killer becomes less guilty.

**Rule workflow**:
1. If kingdoms are opposed:
   a. `SetCurKill(+1)`, `SetTotKill(+1)`, `SendEtc`.
   b. If not at war, not guilty, not newbie → PK point −(3·victimPK/20), floor −9, cap 0; message.
   c. If inside the arena → set HP 2, teleport to kingdom spawn, increment kingdom score.
2. Else (hostile):
   a. If not at war and not newbie → PK point −(3·victimPK/25), floor −9, cap 0; `×3` if either player holds item 548/549; message when non-zero.
   b. If victim PK < 75 and not at war and killer is a player → victim PK +1; message.

---

### Business Rule: BR-43 — PK item drop (compile-time disabled)

**Overview**: A large item-confiscation subsystem exists for hostile PvP kills but is excluded from the build.

**Detailed description**:
Lines `2305-2418` sit inside `#ifdef PKDrop`. `PKDrop` is commented out in `Code/Basedef.h:29` and is not defined in `Code/TMSrv/TMSrv.vcxproj` (verified: preprocessor definitions are `_CRT_SECURE_NO_WARNINGS;_PACKET_DEBUG;WIN32;_DEBUG;_WINDOWS` for Debug and `_CRT_SECURE_NO_WARNINGS;WIN32;NDEBUG;_WINDOWS` for Release). The block therefore never compiles, and hostile kills confiscate nothing.

For the record, the intended rule was two-stage: for victims with PK point <= 60, drop up to `(75 - pk)/10` carry items, each with a 1-in-5 roll, skipping empty slots, indices above `MAX_ITEMLIST`, items 508/509/522, the 526..531 band and item 446, honouring `CreateItem`'s index limits, logging logged items as `cho <code>`, clearing the slot and sending `_MSG_CNFDropItem`. For victims with PK point <= 35, additionally drop one equipped item from a random slot 1..14 (excluding 12) with the same index filters. Both stages guard against `CreateItem` failure and both write forensic logs.

Because it is dead code, it still rots: it uses `rand() % 5`, `rand() % 2` and `rand() % killed_loseitem` in ways that would consume the same RNG stream as the PvE loot loop if enabled, and it references `MAX_ITEMLIST`/`MAX_ITEM` with inconsistent comparison operators (`<=` vs `<`).

**Rule workflow** (dormant):
1. Compiled out entirely; no runtime behaviour.
2. If enabled: stage 1 confiscates carry items for victims at PK <= 60.
3. Stage 2 confiscates one equipped item for victims at PK <= 35.
4. Each confiscation logs `cho <item code>`, clears the slot and sends `_MSG_CNFDropItem`.

---

### Business Rule: BR-44 — Post-PvP appearance refresh and audit log

**Overview**: After resolving affiliation effects, the function refreshes both players' appearances for nearby clients and writes one audit line describing the kill.

**Detailed description**:
`GetCreateMob(conn)` + `GridMulticast` (`2423-2424`) re-publishes the killer, whose PK point or kill counters may have changed (both are encoded into the `MobName` tail by `GetCreateMob`, `GetFunc.cpp:955-976`). After the SameClan reset of the victim's kill counter (`2426-2427`) and the possible PK point increment, the victim's appearance is re-published too (`2436-2437`). This is what makes the PK aura visible without a client round trip.

The audit line (`2439-2443`) is emitted through `Log(..., "-system", 0)` and includes killer name, killer level, killer PK point, victim name, victim level, victim PK point, the `SameClan` flag and the `AtWar` flag. It does not include coordinates, item drops, exp amounts or the arena/village classification, so a post-mortem of a contested kill requires correlating several other log streams.

Two details are notable: the log line is emitted in *both* branches (it is outside the arena/hostile `if`/`else`), and the message uses `%d` for the killer's `CurrentScore.Level` but the victim's `CurrentScore.Level` while the rule logic used `BaseScore.Level`, so a discrepancy between base and current level will show up in the log as inconsistent data.

**Rule workflow**:
1. `GetCreateMob(killer)`; multicast.
2. If same-kingdom → `SetCurKill(target, 0)`.
3. Maybe increment victim PK; message.
4. `GetCreateMob(victim)`; multicast.
5. Write the `etc PK killer:... killed:... kingdom:... war:...` log line.

---

### Business Rule: BR-45 — Fairy sanc decay on PvP death

**Overview**: Fairy items 753 and 1726 lose one sanc point when their owner dies in PvP, and are destroyed at zero.

**Detailed description**:
After the PvP resolution, `2446` checks `Equip[13].sIndex` against 753 ("10X HP MOB") and 1726 ("20X HP MOB"). If `BASE_GetItemSanc` is positive it is decremented in place in whichever effect carries id 43; otherwise the item is cleared via `BASE_ClearItem`. The item is then resent with `SendItem(target, ITEM_PLACE_EQUIP, 13, ...)`.

This is the PvP counterpart of BR-03 but with a crucial difference: BR-03 *prevents* the death and grants a respawn animation, while BR-45 only decays the item after the death has already been resolved. The two rules are not chained: an item 769 fairy absorbs the hit and returns before the PvP block is reached, so it never decays here. Item 753/1726 fairies on a dead player lose durability on every death with no absorption benefit — strictly a penalty.

`SendItem` is called unconditionally, including when the item did not change because no effect-43 slot existed (in which case the sanc counter read may have come from the item's own refined-value field and the write-back silently failed). The mismatch between read and write paths is inherited from `BASE_GetItemSanc`.

**Rule workflow**:
1. Victim's `Equip[13].sIndex` is 753 or 1726.
2. Read sanc; if `> 0` → decrement in the first effect-43 slot; else `BASE_ClearItem`.
3. `SendItem(target, ITEM_PLACE_EQUIP, 13, item)`.

---

## 4. Component Structure

```
Code/TMSrv/MobKilled.cpp                       2,469 lines, 1 function (MobKilled), 0 headers
│
├── 1-40      License header + includes
│              <Windows.h> <stdio.h> <fcntl.h> <time.h> <math.h> <io.h> <errno.h>
│              ..\Basedef.h ..\CPSock.h ..\ItemEffect.h Language.h
│              CItem.h Server.h GetFunc.h SendFunc.h
│              ProcessClientMessage.h ProcessDBMessage.h
│              CCastleZakum.h CWarTower.h
│
├── 41-44     Function signature + entry guard
├── 46        Equip[13] aliased as FairySlot
├── 48        MSG_CreateMob reused by every early-return resurrection path
│
├── 50-63     BR-02  immortal entity (level >= 1000)
├── 65-104    BR-03  Nyerds fairy absorption
├── 105-109   BR-04  castle throne displacement
│
├── 111       Face = Equip[0].sIndex (summon identification)
├── 113-171   BR-05  "Crias EXP" mount growth  [pragma region]
│
├── 173-183   BR-06  MSG_CNFMobKill construction, HP zeroing
├── 185-188   BR-07  leader resolution
├── 190-205   BR-08  summon attribution / orphan cleanup
│
├── 206-2056  PvE domain  [pragma region PvE]
│   ├── 207-211   BR-09  eligibility gates
│   ├── 213-626   EXP_DISTRIBUTION  [pragma region]
│   │            (x4 near-identical member blocks: Pesa A / Pesa M / Pesa N / proximity)
│   │            BR-11, BR-12, BR-13, BR-14, BR-15, BR-16, BR-17
│   ├── 628-632   BR-18  death chatter
│   ├── 634       BR-06  broadcast (triggers level-up cascade in SendFunc)
│   ├── 636-637   BR-19  clan-gated early delete
│   ├── 639-2031  reward branch (nested else)
│   │   ├── 649-713   BR-20  Kefra boss
│   │   ├── 714       BR-21  terrain gate
│   │   ├── 718-756   BR-22  castle gate
│   │   ├── 758-1726  "Drops especiais" else-branch
│   │   │   ├── 760-979    BR-23  Agua M / N / A
│   │   │   ├── 980        BR-24  CCastleZakum::MobKilled
│   │   │   ├── 983-999    BR-25  orc / towers / kings
│   │   │   ├── 1000-1073  BR-26  boss loot tables
│   │   │   ├── 1074-1084  BR-27  nightmare respawn
│   │   │   ├── 1085-1351  BR-28  duel cards N / M / A
│   │   │   ├── 1352-1695  BR-29  rune quest LV0..LV6
│   │   │   └── 1696-1724  BR-30  RvR towers
│   │   ├── 1728-1758 BR-31  coin
│   │   ├── 1759-1790 BR-32  event item
│   │   ├── 1791-1946 BR-33  common loot loop (64 slots)
│   │   │   ├── 1904     BR-34  KeyDrop interception
│   │   │   └── 1907-1939 BR-35 audit log / DB day log
│   │   ├── 1947-1956 BR-36  TerraMistica amulet
│   │   ├── 1957-1989        commented-out HP-drain block (dead comment)
│   │   ├── 1990      BR-37  DeleteMob
│   │   ├── 1992-2025 BR-37  respawn rules
│   │   └── 2027      BR-37  CWarTower::MobKilled
│   └── 2029-2054   terrain-failure delete / dead heal branch
│
└── 2058-2468  PvP domain  [pragma region PvP]
    ├── 2061-2064  BR-38  level guard
    ├── 2066-2109  BR-39  exp-loss computation  [pragma region Lose EXP]
    ├── 2111-2125  BR-39  counters + PK band
    ├── 2130-2158  BR-40  war flags               [pragma region No lose At war]
    ├── 2161       BR-06  broadcast
    ├── 2163-2195  BR-40  arena / safe zone / item 431
    ├── 2197-2230  BR-41  exp debt
    ├── 2232-2276  BR-42  same-kingdom branch
    ├── 2277-2420  BR-42/43 hostile branch (+ disabled PKDrop)
    ├── 2423-2443  BR-44  appearance refresh + audit
    └── 2446-2466  BR-45  fairy sanc decay
```

**Structural observations.**

* **One function, 46 rule families.** There is no internal decomposition; every rule shares the same scope, so locals such as `party`, `exp`, `item`, `temp`, `sCreateMob` are reused across unrelated blocks. `#pragma region` markers are the only structural documentation.
* **Four-fold duplication.** The party exp block appears four times (`252-344`, `347-438`, `441-531`, `533-625`) — ~380 lines of effectively identical code differing only in the zone predicate.
* **Three-fold duplication elsewhere.** Water rooms (`762-978`), duel cards (`1087-1350`) and the rune-quest party-reward epilogue (`1384-1393`, `1469-1478`, `1585-1594`, `1627-1636`) each repeat the same pattern three to six times.
* **Include surface.** Six project headers plus five system headers; `Language.h` for message-table ids; `CCastleZakum.h` and `CWarTower.h` for the two delegated sub-domains.

---

## 5. Dependency Analysis

### Internal dependencies (call graph)

```
CALLERS (afferent, 6 call sites / 5 translation units)
  _MSG_Attack.cpp:1743  ──┐
  ProcessSecMinTimer.cpp:1940 ─┤
  Server.cpp:9203       ──┼──> MobKilled(target, conn, PosX, PosY)
  imple.cpp:1661        ──┤        (MobKilled.cpp:41)
  imple.cpp:1718        ──┘

CALLEES (efferent, grouped by owning module)
  MobKilled ──> SendFunc      GridMulticast, MapaMulticast, SendScore, SendEtc,
                              SendItem, SendEquip, SendEmotion, SendChat,
                              SendClientMessage, SendClientSignalParm,
                              SendNotice, SendNoticeArea, SendSay
  MobKilled ──> GetFunc       GetExpApply, GetCreateMob, GetCurKill, GetTotKill,
                              GetPKPoint, GetGuilty
  MobKilled ──> Server        DeleteMob, GenerateMob, PutItem, CreateItem,
                              UpdateItem, DoTeleport, DoRecall, ClearArea,
                              ClearAreaTeleport, RemoveParty (indirect),
                              MountProcess, DrawConfig, SetReqHp, Log
  MobKilled ──> Basedef       BASE_GetItemSanc, BASE_ClearItem, BASE_GetGuildName,
                              BASE_GetArena, BASE_GetVillage, SetItemBonus,
                              BASE_GetItemAbility (via KeyDrop)
  MobKilled ──> SetFuncs      SetCurKill, SetTotKill, SetPKPoint
  MobKilled ──> CCastleZakum  MobKilled, KeyDrop
  MobKilled ──> CWarTower     MobKilled
  MobKilled ──> CPSock        DBServerSocket.SendOneMessage  (2 message types)

INDIRECT (not called here, but triggered by the multicast)
  GridMulticast ──> SendFunc.cpp:920-947 ──> CMob::CheckGetLevel
                                        ──> SetCircletSubGod, DoItemLevel,
                                            SendScore, SendEmotion,
                                            SendClientMessage
  DeleteMob    ──> GridMulticast, RemoveParty, mNPCGen.pList[].CurrentNumMob
  PutItem      ──> SendItem, pMob[].MaxCarry bound
  GenerateMob  ──> GetEmptyNPCMob, mNPCGen.pList[], pMob[] population
```

### External dependencies

| Dependency | Kind | Used for | Notes |
|---|---|---|---|
| `DBServerSocket` (`CPSock`) | Socket / IPC to DBSrv | `MSG_UpdateExpRanking` (per member), `MSG_GuildInfo`, `_MSG_DBItemDayLog` | Fire-and-forget; no ack, no retry |
| CRT `rand()` | Process RNG | 38 call sites: drops, gold, boss tables, rune roles | No `srand()` anywhere in `Code/` |
| CRT `localtime()` | Time | Daily exp-log day key | Shared static buffer; not `localtime_s` |
| CRT `sprintf()` | Formatting | 22 call sites into the shared global `temp` | No length-bounded variant |
| Win32 `<Windows.h>` | Platform | Included but `MobKilled` itself uses no Win32 API | Indirect via headers |
| `pHeightGrid[4096][4096]` | Map data | Terrain gate | No bounds check on `PosX/PosY` |
| `pItemGrid[][]`, `pItem[5000]` | World items | Castle gate open | Bounds-checked at `721`, `728` |
| `pMobGrid[4096][4096]` | Spatial index | Kefra loot rectangle | Fixed rectangle, no bounds check |
| `mNPCGen` (`CNPCGenerator`, 8,192 slots) | Spawn data | `DieAction`, `CurrentNumMob`, `GenerateMob` | `DieAction` access lacks an upper-bound check |
| `g_pItemList[6500]` | Item metadata | `ReqLvl`, `nPos`, `Name` | Index filtered to 391..6499 |
| `g_pDropRate[64]`, `g_pDropBonus[64]` | Static tables | Loot probabilities | Hard-coded in `Basedef.cpp:204-218` |
| `g_pNextLevel[401]`, `g_pNextLevel_2[401]` | Static tables | PvP exp loss | Index `tlevel+1` guarded by the `tlevel <= 399` test |
| `PistaRune[7][10]` | Static table | Rune quest rewards | Fixed in `Server.cpp:451` |
| `Language.h` / `g_pMessageStringTable` | String table | All user-facing messages | 20+ message ids |

### Global state written by the component

`KefraLive`, `RvRBonus`, `RvRState`, `Kingdom1Clear`, `Kingdom2Clear`, `LiveTower[3]`, `WaterClear1[3][10]`, `CartaTime`, `CartaSala`, `Pista[7]`, `evCurrentIndex`, `RvRRedPoint`, `RvRBluePoint`, `CastleQuestClear` (via `CCastleZakum`), `g_dLevel`, `g_dLevel1`, `g_dLevel2` (leftovers from `SetItemBonus`), plus `pMob[]`, `pUser[]`, `pMobGrid[][]`, `pItem[]`/`pItemGrid[][]`.

---

## 6. Afferent and Efferent Coupling

Because the component is a single C function, "components" are measured at two levels: *call sites* (integration coupling) and *distinct external symbols used* (structural coupling). Both are reported.

### Afferent coupling (who depends on `MobKilled`)

| Caller | File:line | Trigger context | Notes |
|---|---|---|---|
| `Exec_MSG_Attack` | `_MSG_Attack.cpp:1743` | Batched loop over `TargetKilled[0..12]` after a player attack resolves | Primary gameplay path; validates `TargetKilled[i]` before the call |
| Monster damage tick | `ProcessSecMinTimer.cpp:1940` | AI/DoT damage application loop | Killer is the AI mob index |
| Generic damage helper | `Server.cpp:9203` | Central damage application when target HP reaches 0 | Used by area/effect damage |
| GM `killkefra` | `imple.cpp:1661` | Admin console command | Iterates all `KEFRA_BOSS` instances |
| GM `kill` | `imple.cpp:1718` | Admin console command, kills by name | Sets HP to 0 first |
| `CCastleZakum::MobKilled` | `CCastleZakum.cpp:210` | *Not* a caller of `MobKilled` — a parallel delegate with the same name/signature | Naming collision only |

**Afferent count: 6 call sites across 5 translation units** (`_MSG_Attack.cpp`, `ProcessSecMinTimer.cpp`, `Server.cpp`, `imple.cpp` ×2). This matches the architecture report's estimate of ~10 when indirect dependents (the message consumers of `MSG_CNFMobKill`) are counted.

### Efferent coupling (what `MobKilled` depends on)

| Component / collaborator | Efferent symbols used | Criticality | Why |
|---|---|---|---|
| `SendFunc` (projection layer) | 13 | High | Every client-visible effect; failure = silent desync |
| `Server` (world services) | 14 | High | Corpse removal, spawning, item placement, teleport |
| `GetFunc` (queries) | 6 | High | Exp formula and PK/kill accessors |
| `Basedef` (rules helpers) | 7 | Medium | Item sanc/clear, guild name, arena/village, item bonus |
| `GetFunc` setters | 3 | Medium | PK/kill counter mutation |
| `CCastleZakum` | 2 | Medium | Castle quest prizes + treasure keys |
| `CWarTower` | 1 | Low | Guild tower replacement |
| `CPSock` (DBSrv) | 1 API, 3 message types | High | Exp ranking, guild fame, item day log |
| CRT | 4 (`rand`, `localtime`, `sprintf`, `memset`) | Medium | Non-determinism and formatting |
| Global data | 14+ arrays/scalars | High | Shared mutable world state |

**Efferent count: 45 distinct external symbols** across 9 collaborator groups (11 groups if the CRT and globals are counted separately, matching the architecture report's 11).

### Coupling matrix (component-level)

| Component | Afferent | Efferent | Instability (Ce/(Ca+Ce)) | Critical |
|---|---|---|---|---|
| `MobKilled` (module) | 6 | 45 | 0.88 | Critical |
| `MobKilled` → `MSG_CNFMobKill` (implicit contract) | 5 broadcast sites | 1 packet, 4 fields, per-recipient mutation | — | Critical |
| `CCastleZakum::MobKilled` | 1 (MobKilled) | 1 group | 0.50 | Medium |
| `CCastleZakum::KeyDrop` | 1 (MobKilled) | 3 | 0.75 | Medium |
| `CWarTower::MobKilled` | 1 (MobKilled) | 4 | 0.80 | Low |
| `GetExpApply` | 2 (`Server.cpp`, `MobKilled`×4) | 0 | 0.00 | Critical |
| `PutItem` | 21 project-wide (2 here) | 2 | 0.09 | High |
| `DeleteMob` | 21 project-wide (5 here) | 3 | 0.13 | High |
| `GridMulticast` | Project-wide (8 here) | 3 + implicit cascade | — | Critical |

**Interpretation.**

* `MobKilled` is **highly unstable** (0.88): almost everything it touches is something else's responsibility, and only four translation units call it. Instability of this magnitude on a *hub* means the component is a change magnet — any signature or semantic change in `SendFunc`, `Server` or `GetFunc` propagates straight here.
* **Cohesion is very low.** The function performs at least nine unrelated responsibilities (exp, currency, loot, war state, dungeon progression, quest progression, PvP consequence, audit, corpse lifecycle). In cohesion terms it is coincidental/logical at best.
* **The strongest coupling is not a call, it is a packet.** `_MSG_CNFMobKill` carries the `Exp`/`Hold` values by mutation inside `GridMulticast` and triggers the level-up cascade there. Static analysis cannot see this edge; it exists only as a `msg->Type` comparison in `SendFunc.cpp:920`. This is the single most fragile contract in the component.
* **Data coupling dominates control coupling.** 14+ global arrays are read and written, and five process-wide event flags are mutated. Two callers in different subsystems can therefore observe intermediate states (e.g. exp granted but corpse not yet deleted).

---

## 7. Integration Points

| Integration | Type | Purpose | Protocol / Mechanism | Data Format | Error Handling |
|---|---|---|---|---|---|
| `_MSG_Attack` handler | Internal caller | Player-initiated kill resolution | Direct C call, batched over `MAX_TARGET` (13) | `(int target, int conn, 0, 0)` | None; caller pre-validates index and `MOB_EMPTY` |
| Monster AI / DoT tick | Internal caller | Mob-caused kill resolution | Direct C call from the second timer | `(int target, int index, 0, 0)` | None |
| `Server.cpp` damage helper | Internal caller | Generic lethal damage | Direct C call | `(int target, int attacker, 0, 0)` | None |
| GM console | Internal caller | Manual kill / Kefra kill | Direct C call from `ProcessImple` | `(int target, int conn, 0, 0)` | None; GM commands are trusted |
| `DBServerSocket` — `MSG_UpdateExpRanking` | Outbound IPC | Persist ranking position after each award | `CPSock::SendOneMessage` | Binary struct (name, exp, class, level, class id) | Fire-and-forget; no ack/retry/timeout |
| `DBServerSocket` — `MSG_GuildInfo` | Outbound IPC | Persist Kefra fame (+100) | `CPSock::SendOneMessage` | Binary struct (`MSG_GuildInfo`) | Fire-and-forget |
| `DBServerSocket` — `_MSG_DBItemDayLog` | Outbound IPC | Daily item-drop statistics | `CPSock::SendOneMessage` | `MSG_STANDARDPARM` with `Parm = item index` | Fire-and-forget; emitted even when `PutItem` fails |
| `GridMulticast` — `_MSG_CNFMobKill` | Client broadcast + internal cascade | Death notification, per-recipient exp/hold sync, level-up | 33×33 grid scan of `pMobGrid` | `MSG_CNFMobKill` (Type 56, ID 30000) | No delivery guarantee; recipients outside the view box miss the update |
| `GridMulticast` — `_MSG_CreateMob` | Client broadcast | Resurrection / appearance refresh | 33×33 grid scan | `MSG_CreateMob` | None |
| `GridMulticast` — `_MSG_UpdateItem` | Client broadcast | Castle gate open | 33×33 grid scan | `MSG_UpdateItem` (`ItemID + 10000`) | Logs `err,no castle gato to open` |
| `MapaMulticast(6, 28, …)` | Client broadcast | Duel-card room timer | Fixed map region | `MSG_STANDARDPARM` `_MSG_StartTime` | None |
| `SendClientSignalParm` | Client message | Water-dungeon countdown | Per-recipient unicast | `_MSG_StartTime`, `parm = time * 2` | None |
| `CCastleZakum::MobKilled` | Internal service | Castle quest boss prizes | Direct C++ static call | Items, exp, coin | Silent `PutItem` failure |
| `CCastleZakum::KeyDrop` | Internal service | Treasure-key conversion | Direct C++ static call, inverted `BOOL` | `STRUCT_ITEM` mutation | Returns `FALSE` when handled |
| `CWarTower::MobKilled` | Internal service | Guild tower replacement | Direct C++ static call | — | None |
| `GenerateMob` / `DeleteMob` | Internal service | Spawn lifecycle | Direct C call | Generator index | `GenerateMob` refills silently when no slot or at `MaxNumMob` |
| `PutItem` | Internal service | Inventory placement | Direct C call | `STRUCT_ITEM*` | Returns `FALSE` on no space / not playing; **return value ignored at 1905** |
| `CreateItem` | Internal service | Ground item creation | Direct C call | `STRUCT_ITEM*`, rotation | Index range checked by caller (`2329`), not by `MobKilled` at `2193` |
| `UpdateItem` | Internal service | Castle gate state | Direct C call | Item id + state | Returns `0` on failure; logged |
| `Log` | Audit | Drop / PK / quest audit lines | File write | `printf` format | None |
| `TesteDie.txt` | Audit (forensic) | Arena/village mismatch evidence | `fopen("a+")` + `fprintf` | Text | **No NULL check on `fopen`** (`2176-2182`) |
| `pHeightGrid` | World geometry | Terrain eligibility | Direct array read | `char[4096][4096]` | No bounds check on `PosX`/`PosY` |
| `Redis` / `PostgreSQL` / external HTTP | — | **Not used** | — | — | TMSrv has no external service integration in this component |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose | Assessment |
|---|---|---|---|---|
| **Facade / choke point** | One function fronts exp, loot, currency, war and quest state | `MobKilled.cpp:41` | Single entry for all death consequences | Achieves consistency of *ordering*; produces an extreme god-function |
| **Chain of Responsibility (implicit)** | Ordered `if`/`else if` ladder over `GenerateIndex`/`GenerateID` | `983-999`, `1000-1073`, `1076-1084`, `1087-1350`, `1354-1693`, `1697-1723` | Route a kill to the one subsystem that owns that generator | Works because ids are disjoint; ordering is undocumented and fragile |
| **Strategy (hard-coded, not polymorphic)** | Four zone predicates select an exp branch | `252`, `347`, `441`, `533` | Different sharing rules per map type | Duplicated code instead of a strategy table |
| **Template Method (inverted)** | `MSG_CNFMobKill` is a template mutated per recipient | `173-183` + `SendFunc.cpp:920-923` | One broadcast serves N personalised updates | Powerful and invisible; no type-level documentation |
| **Observer (implicit)** | `GridMulticast` performs recipient side effects | `SendFunc.cpp:920-947` | Level-up cascade without a call edge | The cascade is a *side effect of rendering*; hard to test |
| **Delegation / Mediator (partial)** | Two sub-domains extracted to classes | `980`, `2027` | Castle quests and guild towers | Only 2 of ~12 sub-domains extracted |
| **Table-driven configuration** | `g_pDropRate`, `g_pDropBonus`, `g_pNextLevel`, `PistaRune`, `CastleQuest`, `g_pGuildZone` | via `Basedef`, `Server.cpp` | Data-driven rates and rewards | Good; but the level-scaling curves are hard-coded in the function |
| **Guard Clauses** | Early returns for invalid, immortal, fairy, castle-blocker | `43`, `63`, `103`, `108`, `201`, `2064` | Fast rejection of non-events | Correct and readable |
| **Sentinel / "unhandled" convention** | `KeyDrop` returns `FALSE` when it *handled* the item | `CCastleZakum.cpp:299-312` | Prevent double placement | Inverted `BOOL` semantics — a readability defect |
| **Magic-number tables** | ~120 literal ids and thresholds inline | passim | Content configuration | No named constants; content changes require recompilation |
| **Latch flags** | `Kingdom1Clear`, `Kingdom2Clear`, `KefraLive`, `RvRBonus`, `CastleQuestClear` | `662`, `686`, `992`, `997`, `1716`, `220` | One-way world-state transitions | No reset path inside the component |

**Architectural decisions (observable, not documented).**

1. **Death consequences are applied in a fixed, economically-motivated order**: exp → announce → clan gate → boss/event hooks → gold → event item → loot → amulet → corpse removal → respawns. Any reordering changes the economy because several steps read state that earlier steps mutate (e.g. `KefraLive` affects BR-13 for *subsequent* kills; `RvRBonus` is set inside the hook block but tested in the exp block).
2. **The exp formula is computed per member, not per kill.** `GetExpApply` is called once per eligible member (up to 13 times per kill), each with the member's own level and class. This is correct for a ratio-based formula, but because the level-weighting term cancels out (BR-17) the per-member computation produces identical results.
3. **Progression is deferred to packet dispatch.** Level-up is not a consequence of adding exp; it is a consequence of *being told about a kill*. This decouples the award from the promotion and means the promotion can be skipped entirely (no `GridMulticast` recipient) or duplicated (two multicasts in one kill).
4. **Sub-domains are extracted only when they need their own data.** `CCastleZakum` owns `CastleQuest[]` and `CWarTower` owns `GTorreGuild`; everything whose data lives in `pMob`/`mNPCGen` stayed inline. The boundary is therefore data ownership, not responsibility.
5. **Determinism was not a design goal.** `rand()` is used inline for 38 decisions with no seeding, no re-entrancy guard and no reproducible seed; `localtime` is called per kill; `sprintf` writes to a shared global `temp` buffer.

---

## 9. Technical Debt & Risks

| ID | Risk Level | Component Area | Issue | Impact |
|---|---|---|---|---|
| R-01 | High | Whole module | One function of 2,429 lines with ~381 `if` statements, 106 `else`, 22 loops; no internal decomposition | Cyclomatic complexity in the ~380 range; any change requires whole-function comprehension; regression surface is the entire reward economy |
| R-02 | High | BR-12/BR-17 (`255`, `357`, `451`, `543`) | `exp = (UNK_1 + myLevel) * isExp / (UNK_1 + myLevel)` is an algebraic identity | Per-member level weighting (the documented purpose of `UNK_1 = 30`) has no effect; all members receive the same base share regardless of level difference |
| R-03 | High | BR-17 (`287`, `382`, `476` vs `569`) | Exp ceiling `exp > eMob` applied only in the open-field branch; commented out in all three Pesa branches | Inconsistent economy between map types; a celestial member can exceed the killer's base award inside nightmare rooms |
| R-04 | High | BR-10 (`228`) | `extra.ClassMaster` is used as a party-size proxy (`UNK_3`) to gate the entire epilogue | Semantically wrong field reuse; the gate is a superset of "has a class master". If a killer ever had `ClassMaster == 0` or `> 12` the mob would never be deleted (dangling HP-0 corpse), because `DeleteMob` is inside the gate |
| R-05 | High | BR-33 (`1793-1945`) | No `srand()` anywhere in `Code/`; 38 `rand()` uses in this file | The entire loot and reward sequence is reproducible from process start; players can predict drops; statistical validation of rates is meaningless across restarts |
| R-06 | High | BR-11 (`214`, `255`, …) | `(int)pMob[target].MOB.Exp` truncates a `long long` to `int` | Any victim with stored exp > 2,147,483,647 wraps; the `exp > 0` guard then silently zeroes the award |
| R-07 | High | BR-18 (`631`) | `mNPCGen.pList[GenerateIndex]` is checked only for `>= 0`, not `< MAX_NPCGENERATOR` | Out-of-bounds read of an 8,192-entry array if `GenerateIndex` is corrupted (inconsistent with `DeleteMob`, which checks both bounds) |
| R-08 | High | BR-40 (`2176-2182`) | `fopen("TesteDie.txt","a+")` result used by `fprintf` without a NULL check | Server crash on a non-writable working directory; reachable from any PvP death inside a city limit but outside a guild zone (`arena == 5 && village != 5`) |
| R-09 | Medium | BR-20 (`693-708`) | Kefra loot loop awards one item per occupied grid cell in a 60×59 rectangle | Up to 64 items (carry-limited) from one boss kill, drawn randomly from `Carry[]`; duplicated items and a strong duplication vector on a laggy respawn |
| R-10 | Medium | BR-33 (`1905`) | `PutItem` return value ignored, but the audit log and DB day-log are emitted unconditionally | Drop statistics overstated; no signal when loot is lost to a full inventory |
| R-11 | Medium | BR-33 (`1941-1942`) | Loop variable is mutated (`i = 11`) inside the loop body | Slots 9, 10 are skipped after a slot-8 drop; the pattern is fragile under refactoring |
| R-12 | Medium | BR-43 (`2305-2418`) | ~114 lines of `#ifdef PKDrop` code never compiled (`PKDrop` commented out at `Basedef.h:29`) | Dead code rots; readers believe item confiscation exists; if enabled it would consume the shared RNG stream |
| R-13 | Medium | BR-09 (`2047-2054`) | Heal-instead-of-death branch is unreachable given `MAX_LEVEL = 399` | Dead code; masks the intended behaviour for level 400..404 players (who instead return silently from the PvP guard at `2063`) |
| R-14 | Medium | BR-16 / Section 8 | Level-up is a side effect of `GridMulticast`, not of exp award | Party members outside the 33×33 view box gain exp without a level check in this call; promotions can be deferred or duplicated |
| R-15 | Medium | Camera of `MobKilled` (`634`, `2161`) | Two different broadcasts (`634`, `2161`) plus the `SendFunc` mutation make the packet contract 3-way coupled | Changing `MSG_CNFMobKill` requires coordinated edits in three files with no compile-time check |
| R-16 | Medium | BR-14 (`238-241`) | `localtime` per kill into a shared static buffer; no `localtime_s` | Data race and torn `tm` if processing ever becomes multi-threaded; also a syscall-per-kill cost |
| R-17 | Medium | BR-33 (`1801-1805`) | Drop-bonus inversion `10000/(bonus+1)` produces a divide-by-zero-free but wildly non-linear scale | Bonus above 900 yields sub-1% rates; bonus of 9000 yields 1/10.1 of base — undocumented and easy to misconfigure via equipment |
| R-18 | Medium | BR-22 (`719-755`) | Castle gate opens for any killer; only the tile bounds are validated | Any faction can open the castle gate; no guild/attacker check |
| R-19 | Medium | BR-40 (`2170`) | Message says "in arena, no exp loss" but the condition also matches cities and one hard-coded zone | Player-facing misinformation; GMs cannot distinguish the cases from the log |
| R-20 | Medium | BR-29 (`1494-1499`, `2001-2020`) | LV3 `MobCount` is incremented on each boss kill and never decremented to the reset value | Boss chain never terminates within the component; termination depends on external quest-state reset |
| R-21 | Medium | BR-33 (`1860-1866`) | `Rsv & 4` uses `Special[2]/10 + 10` to *reduce* drop rate by up to 30% | Reads as a bonus but is a penalty; naming/semantics inverted |
| R-22 | Low | BR-21 (`714`) | `pHeightGrid` is indexed with unvalidated `PosX`/`PosY` when non-zero | Out-of-bounds read on a 16 MB array if a future caller passes real coordinates |
| R-23 | Low | BR-05 (`124-145`) | Mount EXP stored in `stEffect[1].cEffect` (an effect *code* field) | Any effect-normalisation pass silently destroys mount progress |
| R-24 | Low | BR-23 (`779`, `852`, `925`) | `slot_free` declared and never used in three blocks | Dead locals; indicates an abandoned inventory-check design |
| R-25 | Low | passim | 22 `sprintf` calls into the shared global `temp` buffer | Buffer-size assumptions are hidden; `temp` is a project-wide singleton |
| R-26 | Low | passim | ~120 magic numbers (ids, thresholds, rectangles) inline | Content tuning requires recompilation; no named constants |
| R-27 | Low | BR-33 (`1798`) | Drop tables are `int[64]` but `MAX_CARRY` is 64 — correct but coincidental | A future change to `MAX_CARRY` would silently over-read `g_pDropRate` |
| R-28 | Low | `1957-1988` | ~30 lines of commented-out HP-drain logic remain | Dead comment block with live-looking code |
| R-29 | Low | 4× party block | ~380 lines duplicated four times | Any exp-formula change must be applied in four places (five if the derived `eMob` cap is counted) |
| R-30 | Low | BR-08 (`196-199`) | Orphan-summon path deletes only when `target > MAX_USER`; a player "killed" by an orphan summon is left at HP 0 | Inconsistent cleanup; player corpse is never broadcast |

**Build-level observations** (context for R-12): `Code/TMSrv/TMSrv.vcxproj` targets `Application`, `MultiByte`, `/W3`, with `WholeProgramOptimization` enabled and, notably, `MultiThreadedDebugDLL` runtime for the *Release* configuration (`TMSrv.vcxproj:80`) — an inconsistency that would surface as CRT mismatch if MOBs are mixed.

---

## 10. Test Coverage Analysis

### Verdict: **no automated tests exist anywhere in the repository**

Evidence, gathered by full-tree search of `E:\wyd\test-setup\w2pp`:

1. **No test project.** The only solution file is `W2PP Code Project.sln`, which contains `Code/TMSrv/TMSrv.vcxproj` and `Code/DBSrv/DBSrv.vcxproj` only. Neither project declares a test configuration, a test runner, or a post-build test step.
2. **No test directory.** Recursive directory enumeration for names matching `test|spec|unittest|gtest|catch2` returned only `openspec` and `openspec/specs` (documentation, excluded per the analysis scope).
3. **No test source files.** Recursive file enumeration for `(^test|test_|_test\.|\.test\.|unittest|gtest|doctest|catch_)` returned zero files outside the excluded directories.
4. **No test framework linkage.** Searching `TMSrv.vcxproj`/`DBSrv.vcxproj` for gtest/Catch2/doctest/CMake entries yields nothing; there is no `CMakeLists.txt`, `Makefile`, `Directory.Build.props` or `vcpkg.json` in the tree.
5. **No assertion harness.** The only `assert`-like constructs in `Code/` are game logic checks; there is no unit-test entry point (`main` is the Win32 message loop).
6. **Static-analysis corroboration.** The code graph reports "no tests found within 3 caller hops" for `MobKilled`, `GetExpApply`, `PutItem`, `DeleteMob`, `GenerateMob`, `DoRecall`, `BASE_GetArena` and every other symbol this component touches.
7. **Only manual harnesses exist.** The `TESTSERVER` and `LOCALSERVER` global flags (`Server.cpp:379-380`), the `TesteDie.txt` forensic dump, and the GM console commands `kill` / `killkefra` (`imple.cpp:1657-1722`) are the closest thing to a test harness — they are manual, non-deterministic and produce no assertions.

### Coverage table

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|---|---|---|---|---|
| `MobKilled` (entry, guards, dispatch) | 0 | 0 | 0% (no instrumentation) | None. Manual GM `kill` is the only exercise path |
| Exp distribution (BR-10..BR-17) | 0 | 0 | 0% | None. Four duplicated blocks, no fixture data for `g_pEmptyMob`/`PARTYBONUS` |
| `GetExpApply` (BR-11) | 0 | 0 | 0% | None. Pure function — the single easiest target in the component, untested |
| Coin drop (BR-31) | 0 | 0 | 0% | None. Requires RNG control that does not exist (no `srand`) |
| Loot loop (BR-33, BR-34) | 0 | 0 | 0% | None. 12 interacting modifiers, no boundary tests for `i` in {8,9,10,11} or `droprate` at 0/32000 |
| Kefra resolution (BR-20) | 0 | 0 | 0% | None. Manual `killkefra` only |
| Water / duel-card / rune quests (BR-23, BR-28, BR-29) | 0 | 0 | 0% | None. `CurrentNumMob == 1` boundary untested |
| RvR (BR-30, BR-42) | 0 | 0 | 0% | None |
| PvP exp loss (BR-39, BR-41) | 0 | 0 | 0% | None. 11 divisor tiers, none boundary-tested |
| PK point ledger (BR-42) | 0 | 0 | 0% | None. `GetCurKill`/`SetCurKill` round-trip untested |
| PK item drop (BR-43) | 0 | 0 | n/a (not compiled) | Not reachable in any build configuration |
| `CCastleZakum::MobKilled` / `KeyDrop` | 0 | 0 | 0% | None |
| `CWarTower::MobKilled` | 0 | 0 | 0% | None |

### Testability assessment

Four properties make this component expensive to test even if a harness were introduced:

1. **No dependency injection.** Every collaborator is a free function or a global (`pMob`, `pUser`, `pMobGrid`, `DBServerSocket`, `g_pDropRate`). Isolating `MobKilled` requires linking the whole TMSrv translation unit or inventing seam wrappers.
2. **Uncontrollable randomness.** 38 `rand()` call sites with no seeding hook and no injectable RNG. Deterministic verification of drop rates or boss tables is impossible without source modification.
3. **Wall-clock coupling.** `time()`/`localtime()` inside the hot path (`238-241`) makes the daily-log rule time-dependent.
4. **Side effects in the packet layer.** `GridMulticast` mutates the message and calls `CheckGetLevel`, so verifying exp also requires verifying client dispatch. Any test of BR-16 must observe `SendFunc`, not `MobKilled`.

The highest-value *analysis-time* verifications (achievable without writing tests) are: the algebraic identity at `255` (R-02), the commented-out caps at `287/382/476` (R-03), the missing `MAX_NPCGENERATOR` bound at `631` (R-07), the unguarded `fopen` at `2178` (R-08), and the absence of `srand` (R-05).

---

## Appendix A — Constants referenced by the component

| Constant | Value | Defined at | Used for |
|---|---|---|---|
| `MAX_USER` | 1000 | `Basedef.h:56` | Player/NPC index split |
| `MAX_MOB` | 25000 | `Basedef.h:107` | Array bound for `pMob[]` |
| `MAX_PARTY` | 12 | `Basedef.h:168` | Party iteration bound |
| `MAX_LEVEL` | 399 | `Basedef.h:117` | Level ceiling / celestial offset base |
| `MAX_CLEVEL` | 199 | `Basedef.h:118` | Celestial level ceiling |
| `MAX_CARRY` | 64 | `Basedef.h:76` | Loot loop bound |
| `MAX_EQUIP` | 16 | `Basedef.h:75` | Equip slots (13 = fairy, 14 = mount, 15 = costume) |
| `MAX_ITEMLIST` | 6500 | `Basedef.h:139` | Item index bound |
| `MAX_ITEM` | 5000 | `Basedef.h:103` | `pItem[]` bound |
| `MAX_GRIDX/Y` | 4096 | `Basedef.h:100-101` | Grid bounds |
| `HALFGRIDX/Y` | 16 | `Basedef.h:97-98` | Proximity box (±16) |
| `MAX_GUILD` | 4096 | `Basedef.h:147` | Guild id stride |
| `MORTAL / ARCH / CELESTIAL / CELESTIALCS / SCELESTIAL` | 2 / 1 / 3 / 4 / 5 | `Basedef.h:178-182` | Class-master branches |
| `FREEEXP` | 35 | `Server.cpp:43` | No-exp-loss level ceiling |
| `PARTYBONUS` | 100 | `Server.cpp:323` | Party size term (cancelled, see R-02) |
| `g_EmptyMob` | `MAX_USER` (1000) | `Server.cpp:48` | Party size term (cancelled) |
| `DEADPOINT` | 1 | `Server.cpp:315` | PvP debt message gate |
| `DOUBLEMODE` | 0 | `Server.cpp:313` | ×2 exp |
| `NewbieEventServer` | 0 | `Server.cpp:310` | Newbie bonus and ±15% sign |
| `TESTSERVER` / `LOCALSERVER` | 0 / 0 | `Server.cpp:379-380` | Rate divisors and debug echoes |
| `KefraLive` | 0 | `Server.cpp:394` | Global exp halving / minion respawn |
| `RvRBonus` / `RvRState` | 0 / 0 | `Server.cpp:344` / `342` | Kingdom exp bonus / war state |
| `GTorreState` | 0 | `Server.cpp:346` | Fairy suppression / `AtWar` |
| `CastleState` | 0 | `Server.cpp:392` | Fairy suppression / fairy decay / gate |
| `CastleQuestLevel` | −1 | `CCastleZakum.cpp:69` | Treasure-key quest stamp |
| `KEFRA_BOSS` | 396 | `Basedef.h:388` | Kefra resolution |
| `WATER_M/N/A_INITIAL` | 10 / 171 / 183 | `Basedef.h:290-292` | Water rooms |
| `SECRET_ROOM_N/M/A_*` | 2395..2424 | `Basedef.h:296-337` | Duel cards |
| `NIGHTMARE_M/N/A` | 2377..2394 / 2368..2375 / 2385..2394 | `Basedef.h:342-349` | Nightmare rooms + respawn |
| `RUNEQUEST_*` | 5653..5975 | `Basedef.h:351-381` | Rune ladder |
| `RVRTORRE_1/2` | 4236/4237 | `Basedef.h:383-384` | RvR towers |
| `GTORRE` | 1078 | `Basedef.h:386` | Guild tower |
| `REI_HARABARD / REI_GLANTUAR` | 8 / 9 | `Basedef.h:283-284` | Kingdom clear flags |
| `TORRE_NOATUM1..3` | 23..25 | `Basedef.h:286-288` | `LiveTower[]` |
| `ORC_GUERREIRO` | 3 | `Basedef.h:281` | `DoRecall` |
| `EF_KEYID / EF_QUEST` | 39 / 58 | `ItemEffect.h:79,98` | Treasure keys |
| `PKDrop` | **commented out** | `Basedef.h:29` | PK item drop (disabled) |

## Appendix B — Analysis method and limitations

* **Method.** Full read of `Code/TMSrv/MobKilled.cpp` (all 2,469 lines); brace-depth mapping to establish nesting boundaries (confirming that lines `628-2027` are inside the `ClassMaster` gate); call-site enumeration by full-tree symbol search; verification of every called symbol's signature and semantics in its defining translation unit; cross-check of the build configuration in `Code/TMSrv/TMSrv.vcxproj`; full-tree search for test infrastructure and for `srand`.
* **Confidence.** Line references and control flow are verbatim from source (high confidence). Business intent for undocumented rules (e.g. why `ClassMaster` doubles as party size) is inferred and marked where uncertain. The `UNK_*` local names are the author's own, indicating unknown intent at decompilation time.
* **Limitations.** No runtime execution was performed. Drop-rate constants and `g_ItemList` contents were read from source but their in-game balance was not validated. Content-specific meanings (which items 419/420/421/1106/1256/1418/1568/5134 are) depend on `Common/itemlist.csv`, which was not parsed for this report. The client-side meaning of `MSG_CNFMobKill` fields was not verified beyond the server's own usage.
