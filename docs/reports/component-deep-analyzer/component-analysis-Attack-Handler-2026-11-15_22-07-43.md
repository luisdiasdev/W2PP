# Component Deep Analysis Report — Attack Handler

**Component name:** Attack Handler
**Primary location:** `Code/TMSrv/_MSG_Attack.cpp` (44,145 bytes, 1,789 lines)
**Project:** W2PP — decompiled C/C++ (Win32, MSVC / v143) MMORPG server emulator for W2
**Host process:** `TMSrv` (game / world server)
**Analysis date:** 2026-11-15 22:07:43
**Analysis scope:** entire project root (`E:\wyd\test-setup\w2pp`), excluding `.agents`, `.codegraph`, `.opencode`, `openspec`
**Analysis mode:** read-only static analysis. No project file was modified.

---

## 1. Executive Summary

### 1.1 Purpose

The Attack Handler is the single authoritative server-side resolver for every combat action initiated in the world. It is implemented as one free function, `void Exec_MSG_Attack(int conn, char *pMsg)`, declared at `Code/TMSrv/ProcessClientMessage.h:74` and defined at `Code/TMSrv/_MSG_Attack.cpp:21`. It is registered in the client-message dispatch table at `Code/TMSrv/ProcessClientMessage.cpp:192-196` for three packet types: `_MSG_Attack` (103), `_MSG_AttackOne` (157) and `_MSG_AttackTwo` (158).

The handler performs the following, in one linear pass:

1. Session and state gating (trade mode, player mode, aliveness, only-trade mode).
2. Anti-cheat rate limiting and client-tick sanity validation.
3. Skill legality validation (index bounds, class ownership, learned-skill bitmask, passivity).
4. Skill resource consumption (coin for skill 85, mana for all skills) and stat derivation (skill level, delay, "Special" scaling value).
5. Target-slot iteration over up to 13 (`MAX_TARGET`) target records carried inside the attack packet.
6. Target eligibility filtering (dead, empty, merchant, invulnerable NPC, out-of-view).
7. Damage resolution for physical attacks (`Dam[i].Damage == -2`), skill attacks (`Dam[i].Damage == -1`), and a large family of non-damage skill behaviours keyed on `g_pSpell[].InstanceType`.
8. PvP/PK legality enforcement, guilt and PK-point bookkeeping, parry/block ("miss") rolls.
9. Post-processing of the resolved damage value: PvP damage scaling, force damage, friendly-fire zeroing, map-attribute legality, damage caps, on-hit status effects, reflection, mount absorption, mana-shield absorption, HP-multiplier accessories.
10. Actual HP mutation on the victim, HP/Mp client synchronization, logarithmic audit logging.
11. Kill registration and delegation to `MobKilled()`.
12. Aggro (`SetBattle`) propagation to both combatants and their summon parties.
13. Experience bonus accrual from healing, level-up segment detection and PK-point increment on level-up.
14. Grid broadcast of the resolved attack packet to all observers.

### 1.2 Role in the system

The architectural report classifies the Attack Handler as a **Command Handler (combat authority)** with **afferent coupling 1** and **efferent coupling 14** (module granularity). This analysis confirms both figures at module granularity: exactly one direct caller (`ProcessClientMessage`) and fourteen distinct dependent modules. At symbol granularity the efferent coupling is far higher — approximately 45 distinct functions plus a dozen global data structures — which is the defining structural characteristic of this component.

The handler is the *only* writer of combat damage to `pMob[idx].MOB.CurrentScore.Hp` outside of `MobKilled.cpp`, `_MSG_UseItem.cpp` and the timer loops. Every server-authoritative combat outcome (hit, miss, crit, parry, block, kill, aggro, PK guilt, exp) terminates here. It is therefore simultaneously the highest-value and highest-risk component in `TMSrv`.

Notably, the handler also serves **monster and pet attacks**: mob AI code in `Code/TMSrv/Server.cpp` synthesizes `MSG_Attack` packets and re-injects them into `ProcessClientMessage(..., TRUE)` at `Code/TMSrv/Server.cpp:4622`, `:5062`, `:5371` and `:7464`. The `isServer == TRUE` flag bypasses the client-tick ban at `Code/TMSrv/ProcessClientMessage.cpp:63`, which is what makes `SKIPCHECKTICK` (235,543,242, `Code/Basedef.h:172`) reachable inside the handler. The same function is thus both a network trust boundary and an internal AI sink, without any separation between the two trust levels.

### 1.3 Key findings

| # | Finding | Severity |
|---|---------|----------|
| 1 | `skillnum == 103` (`MAX_SKILLINDEX`) is accepted by the `<= MAX_SKILLINDEX` guards at `_MSG_Attack.cpp:488` and reaches `g_pSpell[103]`, while `g_pSpell` is declared `STRUCT_SPELL g_pSpell[MAX_SKILLINDEX]` (103 elements, valid 0..102) at `Code/Basedef.cpp:744` / `Code/Basedef.h:2464`. Out-of-bounds read on a client-controllable index. | High |
| 2 | `HpAbs` life-steal writes `pUser[conn].ReqHp = RecHP` instead of adding to the current value (`_MSG_Attack.cpp:1647`), silently discarding accumulated HP and desynchronizing client from server. | High |
| 3 | The per-skill cooldown read from `SkillData.bin` (`g_pSpell[skillnum].Delay`) is computed into the local `skilldelay` at `:128-133` and then never used; only a hard 700 ms floor at `:135` is enforced. Skill-delay data is effectively dead. | High |
| 4 | `UNK2` is used both as the heal-derived exp accumulator and as a boolean "an affect was applied" at `:1216` / `:1219`, so a successful affect application injects a spurious exp figure into the outbound packet. | Medium |
| 5 | The "damage must be -2/-1/0" integrity check at `:353-361` is the only bound on client-supplied damage; there is no verification that `Dam[i].TargetID` belongs to the attacker's legal target set beyond view range. | Medium |
| 6 | Map-attribute combat legality at `:1421-1434` requires bits `0x01` *and* `0x40` to be **set**, while the equivalent check in `GetFunc.cpp:1527-1530` zeroes damage when bit `0x01` is **set** (inverted predicate). The two code paths disagree. | Medium |
| 7 | Completely dead validations: the hit-position check at `:104-111` compares a value to itself; the range re-check at `:115-119` is unreachable; the entire mortar/skill-book block at `:158-193` is unreachable except for `skillnum == 103`. | Medium |
| 8 | No test project, no test file and no assertion exists anywhere in the repository for this or any other component. | High |
| 9 | Unbounded logging: `Log()` is invoked once per target per attack (`:1628`), producing one log line per damage event with attacker/victim names and coordinates. | Medium |
| 10 | Physical-damage critical state is carried in the loop-scoped `DoubleCritical` variable declared at `:284` rather than reset per iteration, leaking state between target slots. | Low |

---

## 2. Data Flow Analysis

### 2.1 Macro flow (network packet to world state)

```
1. TCP frame received on the client socket
   -> Code/TMSrv/Server.cpp:3975  pUser[User].cSock.ReadMessage()
2. Envelope validation
   -> Code/TMSrv/ProcessClientMessage.cpp:38  ProcessClientMessage(conn, pMsg, FALSE)
      - ID bounds check (:42)
      - ServerDown >= 120 short circuit (:53)
      - _MSG_Ping drop (:59)
      - client packets carrying SKIPCHECKTICK are dropped (:63)
3. Type dispatch
   -> Code/TMSrv/ProcessClientMessage.cpp:192-196  case _MSG_Attack/_MSG_AttackOne/_MSG_AttackTwo
4. Entry into the component
   -> Code/TMSrv/_MSG_Attack.cpp:21  Exec_MSG_Attack(conn, pMsg)
5. Packet field extraction
   -> m->ClientTick (:55), m->SkillIndex (:99), m->TargetX/Y (:101-102),
      m->Dam[i].TargetID / m->Dam[i].Damage (:309, :353), m->Size (:299-306)
6. Session gating
   -> pUser[conn].TradeMode (:27), pUser[conn].Mode != USER_PLAY (:33),
      pMob[conn].MOB.CurrentScore.Hp == 0 (:40), pUser[conn].OnlyTrade (:47)
7. Anti-cheat validation
   -> 800 ms inter-attack floor (:59-66)
   -> client-tick regression (>100 ms rewind) (:68-77)
   -> 15 s future / 120 s past window vs CurrentTime (:79-97)
   -> skill delay floor 700 ms (:135)
   -> class ownership (:147) and learned-skill bitmask (:184, :205)
8. Resource consumption
   -> skill 85 coin cost (:223-232)
   -> BASE_GetManaSpent -> pMob[conn].MOB.CurrentScore.Mp / pUser[conn].ReqMp (:239-256)
9. Pre-loop side effects
   -> DoRemoveHide(conn) for Class 3 (:271-272)
   -> DoRemoveSamaritano(conn) (:274)
   -> skill 79 damage replication across 6 slots (:286-293)
10. Target loop (i = 0 .. 12)
    -> slot/size gating (:297-307)
    -> target identity + eligibility (:309-351)
    -> damage sentinel verification (:353-361)
    -> guild/clan/leader identity resolution (:363-395)
    -> CWarTower::TowerAttack veto (:398)
    -> PK-mode / map-attribute consensual-PvP gate (:405-419)
    -> physical branch  (dam == -2)  (:421-485)
    -> skill branch     (dam == -1)  (:487-1270)
         -> InstanceType 1..5  elemental            (:518-589)
         -> skill 79           arrow storm          (:592-604)
         -> InstanceType 6     heal/detox-family    (:608-712)
         -> InstanceType 7     flash                (:716-723)
         -> InstanceType 8     detox                (:727-747)
         -> InstanceType 9     summon/teleport      (:751-786)
         -> InstanceType 10    invisibility         (:790-805)
         -> InstanceType 11    evoke                (:809-837)
         -> InstanceType 12    ethereal flames      (:841-884)
         -> skill 6            divine fury          (:888-966)
         -> skill 22           exterminate          (:970-1011)
         -> skill 30           divine judgement     (:1015-1021)
         -> skill 31           rebirth              (:1025-1048)
         -> skill 41 / 44      speed / magic weapon (:1052-1109)
         -> skill 47           cancel               (:1113-1137)
         -> skill 98           book vine            (:1141-1155)
         -> Aggressive buff/tick application         (:1159-1226)
         -> transformation 64/66/68/70/71            (:1230-1235)
         -> resurrection book 99                     (:1239-1267)
    -> post-damage pipeline              (:1279-1445)
         -> PvP 25% scaling / piercing     (:1284-1292)
         -> ForceDamage additive           (:1294-1303)
         -> PvPDamage percentage           (:1307-1316)
         -> friendly-fire zeroing          (:1319-1323)
         -> PK legality, guilt, parry      (:1330-1418)
         -> map-attribute legality         (:1421-1434)
         -> Clan 6 invulnerability         (:1436-1437)
         -> ForceMobDamage                 (:1439-1440)
         -> MAX_DAMAGE cap                 (:1442-1443)
         -> on-hit frost / drain           (:1450-1476)
         -> ReflectDamage / ReflectPvP     (:1478-1494)
         -> clan-safe zone window          (:1496-1502)
         -> adult-mount absorption         (:1504-1517)
         -> mana-shield absorption         (:1522-1552)
         -> HP mutation, fairy accessories  (:1554-1609)
         -> ReqHp synchronization          (:1622-1627)
         -> per-hit audit log              (:1628-1629)
         -> mount HP linkage               (:1631-1632)
         -> HP absorb (HpAbs)              (:1635-1648)
         -> death registration             (:1651-1656)
         -> aggro + party propagation      (:1658-1707)
11. Post-loop
    -> exp accrual, DOUBLEMODE, cap 200 (:1711-1721)
    -> spectral flag bit 8              (:1724-1725)
    -> response fields CurrentExp/CurrentHp (:1727-1728)
    -> ClientTick rewrite when SKIPCHECKTICK (:1730-1731)
    -> GridMulticast of the same packet (:1733-1734)
    -> skill 30 HP re-sync              (:1736-1737)
    -> MobKilled() for every killed slot (:1740-1745)
    -> level-segment detection, messages, PK point +5, DoItemLevel (:1748-1787)
```

### 2.2 Data mutation map

| Data element | Direction | Site |
|--------------|-----------|------|
| `m->ID` (set to `ESCENE_FIELD` 30000) | write | `:25` |
| `m->ClientTick` | read, then write | `:55`, `:1730-1731` |
| `m->CurrentMp`, `m->ReqMp` | write | `:254-255` |
| `m->DoubleCritical` | write | `:483`, `:1725` |
| `m->Dam[i].TargetID` | read, then zeroed on rejection | `:309`, `:325`, `:333`, `:342` |
| `m->Dam[i].Damage` | read, rewritten with resolved value | `:353`, `:1279`, `:1302`, `:1315`, `:1445`, `:1493`, `:1498`, `:1516`, `:1545` |
| `m->CurrentExp`, `m->CurrentHp` | write | `:1727-1728` |
| `pUser[conn].LastAttackTick`, `.LastAttack` | write | `:75-76`, `:96`, `:145` |
| `pUser[conn].ReqHp`, `.ReqMp` | write | `:250`, `:1645`, `:1647` |
| `pUser[conn].NumError` | write (indirect via `AddCrackError`) | `:43`, `:64`, `:71`, `:91`, `:108`, `:137`, `:186`, `:207`, `:358`, `:433`, `:496`, `:506` |
| `pMob[conn].MOB.Coin` | write | `:230` |
| `pMob[conn].MOB.CurrentScore.Mp` | write | `:249`, `:832`, `:974`, `:1029` |
| `pMob[conn].MOB.CurrentScore.Hp` | write | `:1019`, `:1248`, `:1258` |
| `pMob[conn].CurrentTarget` | write | `:1662` |
| `pMob[idx].MOB.CurrentScore.Hp` | write | `:655`, `:666`, `:678`, `:683`, `:1566`, `:1583`, `:1599`, `:1608`, `:1653` |
| `pMob[idx].MOB.CurrentScore.Mp` | write | `:857`, `:860`, `:1536` |
| `pMob[idx].MOB.Carry[KILL_MARK].stEffect[1]` (guilt) | write (via `SetGuilty`) | `:1371`, `:1374` |
| `pMob[idx].Affect[]` | write | `:737`, `:875`, `:1123-1126`, plus `SetAffect`/`SetTick` |
| `pMob[idx].Mode`, `.CurrentTarget`, `.EnemyList[]` | write | `:718-722`, `:802` |
| `pMob[idx].Route[0]` | write | `:902` |
| `pMob[summoner].MOB.Equip[14]` | write (via `ProcessAdultMount`/`LinkMountHp`) | `:1613`, `:1632` |
| `pMob[leader].PartyList[]` | write | `:1680`, `:1702` |

### 2.3 Two-phase damage model

The component uses a distinct two-phase model that is essential to understand its behaviour:

* **Sentinel phase** — the client (or mob AI) sends `Dam[i].Damage` as a *request code*, never as a number. `-2` means "resolve a physical attack for me", `-1` means "resolve skill damage for me", `0` means "this slot is unused". Any other value is treated as a protocol violation and the slot is zeroed with a crack-error penalty (`:353-361`).
* **Resolution phase** — the server overwrites `m->Dam[i].Damage` with its own computed value (`:1279`), then applies the full post-processing chain, then mutates HP.

This is a sound anti-tamper design: the client cannot state a damage figure. Its weaknesses are (a) it trusts `TargetID` and `TargetX/Y` wholesale, and (b) it trusts `SkillIndex` up to the inclusive bound that produces the out-of-bounds array read noted in §1.3.

---

## 3. Business Rules & Logic

### 3.1 Overview of the business rules

| Rule ID | Rule Type | Rule Description | Location |
|---------|-----------|------------------|----------|
| BR-01 | Validation / Gating | Attack rejected while the session is in trade mode (`TradeMode`), not `USER_PLAY`, dead (HP 0, except skill 99), or in only-trade mode | `_MSG_Attack.cpp:27-51` |
| BR-02 | Anti-cheat | Minimum 800 ms between two attack packets carrying valid client ticks | `:59-66` |
| BR-03 | Anti-cheat | Client tick may not regress more than 100 ms; regression adds 4 crack points | `:68-77` |
| BR-04 | Anti-cheat | Client tick must lie within `[CurrentTime - 120000, CurrentTime + 15000]` | `:79-97` |
| BR-05 | Anti-cheat | Hard 700 ms floor between skill uses, independent of skill data | `:124-143` |
| BR-06 | Validation | `SkillIndex` must resolve to a legal skill; passive skills are rejected outright | `:113-122`, `:158`, `:196` |
| BR-07 | Validation | Skill must belong to the attacker's class (`skillnum / 24 == Class`) for indices below 96; indices with group > 3 (book skills) are exempt | `:147-151` |
| BR-08 | Validation | The skill must be present in the 32-bit `LearnedSkill` mask | `:184-188`, `:202-209` |
| BR-09 | Validation | Mortar (97) requires a mortar item (index 746) on the ground grid at the requested position | `:158-178` (unreachable, see §10.1) |
| BR-10 | Cost | Skill 85 (Golden Shield) costs `100 × SkillLevel` coins, rejected if insufficient | `:222-233` |
| BR-11 | Cost | Every skill in `[0, MAX_SKILLINDEX)` consumes `ManaSpent × (Special/2+100)/100 × (100-SaveMana)/100` MP; rejected if it would go negative | `:239-256` |
| BR-12 | Derivation | Combat mastery `master = Special[2]/20`, clamped `[0,15]`, only for `Class == 0` with `LearnedSkill & 0x4000` | `:258-269` |
| BR-13 | Targeting | At most `MAX_TARGET` (13) target slots, gated by `m->Size` (`MSG_AttackOne` → 1, `MSG_AttackTwo` → 2, `MSG_Attack` → 13) | `:297-307` |
| BR-14 | Targeting | Slots with `TargetID` outside `(0, MAX_MOB)` are skipped; empty mobs are removed from the client view | `:309-321` |
| BR-15 | Targeting | NPCs wearing items 219/220 standing on map block (26,12) are damage-immune | `:323-329` |
| BR-16 | Targeting | Targets already at HP ≤ 0 are zeroed and removed, except when the packet skill is 31 (rebirth) or 99 (resurrection) | `:331-338` |
| BR-17 | Targeting | Merchants of type 1, 4, 43 and 100 cannot be damaged | `:340-345` |
| BR-18 | Targeting | Non-skill-42 attacks require the target to be within `VIEWGRIDX/VIEWGRIDY` (±33) of the attacker | `:347-351` |
| BR-19 | Integrity | `Dam[i].Damage` must be exactly `-2`, `-1` or `0`; anything else is zeroed and penalised 10 crack points | `:353-361` |
| BR-20 | War | Guard-tower targets may only be attacked by a different guild; otherwise the slot is voided | `:398-403` (via `CWarTower::TowerAttack`) |
| BR-21 | PvP | Non-consensual PvP is refused when the victim is neither in PK mode nor guilty on a non-PK map | `:405-419` |
| BR-22 | Combat | Physical range: `BASE_GetDistance(Pos, Target) ≤ pUser[conn].Range` and `≤ 23` | `:424-427` |
| BR-23 | Combat | Multi-target physical attacks require `MSG_AttackTwo` or larger, or Huntress class, or learned skill `0x40` | `:431-435` |
| BR-24 | Combat | Physical base damage = `CurrentScore.Damage`; user AC ×3, mob AC ×1; `BASE_GetDamage` variance applied | `:442-457` |
| BR-25 | Combat | Critical multiplier applies **before** armour: ×(1.3–1.4) vs players, ×(1.5–1.6) vs mobs | `:444-450` |
| BR-26 | Combat | Double-critical flag applies ×2 **after** armour reduction | `:480-481` |
| BR-27 | Combat | Huntress double-strike: `Class == 3`, `LearnedSkill & 0x200000`, 25% chance; adds `(Special[3]+Str)` through armour, min 60, marks flag bit 2 | `:459-478` |
| BR-28 | Skills | Per-skill target cap `g_pSpell[].MaxTarget` enforced | `:492-502` |
| BR-29 | Skills | Party-only skills (`bParty != 0`) require same leader or same guild | `:504-512` |
| BR-30 | Skills | Elemental skills (InstanceType 1–5) scale damage by `(150 - Resist)/100`; player AC ×2, Foema AC ×1.5 | `:518-589` |
| BR-31 | Skills | Weather modifies elemental damage: region rule zeroes weather; `instance 2` ×0.9, `instance 5` ×1.3 in weather 1, `instance 3` ×1.2 in weather 2 | `:520-530`, `Basedef.cpp:6406-6416` |
| BR-32 | Skills | Fairy skills 32/34/36 fired with `Motion == 254` compute damage as `sanc × {200,300,350} + Level × 8`, capped 1000 | `:533-554` |
| BR-33 | Skills | Arrow storm (79): damage recomputed from `CurrentScore.Damage`, AC ×3 for players, then halved | `:592-604` |
| BR-34 | Skills | Heal skills (InstanceType 6) heal `Special×2+V` (skill 27) or `Special×3/2+V`, ×2 for celestial tiers, capped 1100 / 510, minimum 6 | `:608-645` |
| BR-35 | Skills | Healing is refused into a combat-flagged cell from a non-flagged cell | `:613-621` |
| BR-36 | Skills | Healing a target outside a village grants exp `(ΔHP >> 3)` capped 120 | `:693-711` |
| BR-37 | Skills | Heal value is divided by fairy accessory sanctification (786 / 1936 / 1937) | `:649-683` |
| BR-38 | Skills | Flash (InstanceType 7) forces the target to `MOB_PEACE`, clears target and enemy list | `:716-723` |
| BR-39 | Skills | Detox (InstanceType 8) strips affect types 1,3,5,7,10,12,20 and 32 (if `LearnedSkill & 0x80`) | `:727-747` |
| BR-40 | Skills | Teleport (InstanceType 9) summons the target; blocked for dead targets, `0x04` map attribute with level < 1000, HP above `attacker HP + Special + 30`, or unset map id | `:751-786` |
| BR-41 | Skills | Invisibility (InstanceType 10) retargets all mobs whose current target was the invisible player | `:790-805` |
| BR-42 | Skills | Evoke (InstanceType 11) spawns `Special[2]/30`, `/40`, `/80` or exactly 1 summon depending on `InstanceValue`; MP refunded on failure | `:809-837` |
| BR-43 | Skills | Ethereal flames (InstanceType 12) burn 10–19% of the victim's MP and strip affects 14/16/18/19 | `:841-884` |
| BR-44 | Skills | Divine fury (6) knocks the target back with a chance of `Special[1]/10+20` (mobs `+40`), level-difference adjusted, capped 50% | `:888-966` |
| BR-45 | Skills | Exterminate (22) converts the attacker's entire MP pool plus `Int/2` into bonus damage | `:970-1011` |
| BR-46 | Skills | Divine judgement (30) adds the attacker's current HP to damage and leaves the attacker at `HP/6 + 1` | `:1015-1021` |
| BR-47 | Skills | Rebirth (31) has a 70% chance to revive the target with 10–19% of its MaxHP; the caster's MP drops to 0 | `:1025-1048` |
| BR-48 | Skills | Speed (41) and Magic Weapon (44) apply their affect to `Special/25 + 2` targets, clamped `[2,13]`, players only | `:1052-1109` |
| BR-49 | Skills | Cancel (47) removes affect type 19 from the first valid target | `:1113-1137` |
| BR-50 | Skills | Book Vine (98) spawns a `Boss`-class "Vinha" at the target coordinates after an attribute check | `:1141-1155` |
| BR-51 | Effects | `AffectType`/`TickType` are applied to a target unless it is a party/guild mate (for aggressive skills), blocked by `RSV_BLOCK`, or Clan 6; a `RegenMP + AffectResist + difLevel` roll gates application | `:1159-1226` |
| BR-52 | Effects | Skill 102 applies its own delay schedule: 200 ms base, `-(39-level)×4` below level 39, 400 ms at level ≥ 199 | `:1195-1213` |
| BR-53 | Effects | Transformation skills 64/66/68/70/71 recalculate the attacker's score and resend score + equipment | `:1230-1235` |
| BR-54 | Effects | Resurrection book (99) requires the caster to be dead; `rand % 115` roll (values >100 become 85) with a 40-threshold recall, then HP is set to 1–50% MaxHP | `:1239-1267` |
| BR-55 | PvP | Damage against players (or Clan 4 mobs) is scaled to 25% (`dam >> 2`); with the Huntress flag the extra hit is added back | `:1284-1292` |
| BR-56 | PvP | `ForceDamage` is added to any positive damage, or replaces damage ≤ 1 | `:1294-1303` |
| BR-57 | PvP | `PvPDamage` adds a percentage against player targets only | `:1307-1316` |
| BR-58 | PvP | Same leader or same guild suppresses damage entirely | `:1319-1320` |
| BR-59 | PvP | Clan 7 vs 7 or 8 vs 8 (`isFrag`) cannot damage mobs | `:1322-1323` |
| BR-60 | PvP | Attacker with `PKPoint ≤ 10` cannot damage a victim above 10 outside arenas, villages, the PK map and war zones | `:1360-1367` |
| BR-61 | PvP | Attacking a guilty victim marks **both** parties guilty (8) outside safe zones | `:1368-1389` |
| BR-62 | PvP | Parry roll: `rand%1000+1 < GetParryRate(...)` → damage becomes `-3`; inner roll `< 100` with victim `Rsv & 0x200` → `-4` | `:1393-1417` |
| BR-63 | War | Guild-war pairs and castle sieges are detected and treated as legal combat | `:1346-1357` |
| BR-64 | Environment | For player targets below `MAX_LEVEL`, both cell attributes must carry `0x01` and `0x40` or damage is zeroed | `:1421-1434` |
| BR-65 | Environment | Clan 6 targets are immune while the attacker is below `MAX_LEVEL` | `:1436-1437` |
| BR-66 | Combat | `ForceMobDamage` is added to mob-only damage | `:1439-1440` |
| BR-67 | Combat | Final damage is capped at `MAX_DAMAGE` (1,000,000) | `:1442-1443` |
| BR-68 | Effects | `RSV_FROST` on the attacker has a 50% chance to apply affect 36 (`Special[1]+150` duration, `Special[1]` level) | `:1450-1462` |
| BR-69 | Effects | `RSV_DRAIN` on the attacker has a 50% chance to apply affect 40 | `:1464-1476` |
| BR-70 | Defence | Player victim `ReflectDamage` subtracts flat, minimum 1; `ReflectPvP` subtracts a percentage, minimum 1 | `:1478-1494` |
| BR-71 | Environment | Inside x∈[1017,1290], y∈[1911,2183] no damage lands between clan-mates | `:1496-1502` |
| BR-72 | Defence | Adult mounts (equipment 2360–2389 with HP > 0) absorb 25% of damage; the mount takes half of that | `:1504-1517`, `:1612-1613` |
| BR-73 | Defence | Mana shield (affect 18) spends MP first while victim MP > 10% of MaxMp; 30% (or 33% with attacker bit `1<<23`) of the remainder reaches HP | `:1522-1552` |
| BR-74 | Defence | Fairy accessories 786 / 1936 / 1937 divide the HP loss by `sanc`, `sanc×10` or `sanc×1000` while the packet still reports full damage | `:1554-1609` |
| BR-75 | Integrity | HP loss is clamped to the victim's current HP; HP is clamped to 0 on death | `:1563-1564`, `:1651-1655` |
| BR-76 | Effects | `HpAbs` grants a 50% chance to recover `(dam × HpAbs + 1)/100` HP, capped 350 | `:1635-1648` |
| BR-77 | Outcomes | Every target reduced to 0 HP is queued and passed to `MobKilled` after the loop | `:1651-1656`, `:1740-1745` |
| BR-78 | Aggro | Surviving targets and the attacker are linked with `SetBattle`; both parties' summon lists are pulled into combat and purged of dead entries | `:1658-1707` |
| BR-79 | Reward | Healing-derived exp is doubled under `DOUBLEMODE` and capped at 200 | `:1711-1721` |
| BR-80 | Response | Spectral flag (`LearnedSkill & 1<<29`) sets `DoubleCritical` bit 3 (value 8) in the reply | `:1724-1725` |
| BR-81 | Response | The packet is broadcast to the grid unless the skill is 102; `ClientTick` is rewritten to `CurrentTime` for server-originated attacks | `:1730-1734` |
| BR-82 | Reward | Level segments 1–4 emit client messages; segment 4 grants `+5` PK points, an emotion, `SendEtc` and `DoItemLevel` for mortals | `:1748-1787` |

### 3.2 Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Session State Gate

**Covers:** BR-01
**Location:** `Code/TMSrv/_MSG_Attack.cpp:25-51`

**Overview:**
Before any combat computation, the handler refuses the attack unless the connection is in a fully playing, non-transactional state and the character is alive. This is the first trust boundary of the component and the only one that inspects the `CUser` session object rather than the `CMob` entity.

**Detailed description:**

The very first statement of the function overwrites the incoming packet's `ID` field with `ESCENE_FIELD` (30000, `Code/Basedef.h:170`) at `:25`. This is significant for two reasons. First, `ID` normally carries the originating connection index and is validated by `ProcessClientMessage` at `Code/TMSrv/ProcessClientMessage.cpp:42`; overwriting it means the handler deliberately re-purposes the field so that the packet, when broadcast back out through `GridMulticast` at `:1734`, is interpreted by every receiving client as a *scene* event rather than a player-specific message. Second, it destroys the original sender index, so any downstream consumer of the broadcast packet cannot recover who sent it — the receiver relies solely on `AttackerID` (`Code/Basedef.h:2028`), which the handler never sets and which is therefore whatever the client supplied. This asymmetry is a notable design characteristic: the server trusts its own `conn` parameter absolutely but trusts the packet's `AttackerID` implicitly.

The gate itself is four sequential rejections. `pUser[conn].TradeMode` non-zero means an auto-trade session is active and the attack is silently refused with the `_NN_CantWhenAutoTrade` message (`:27-31`). `pUser[conn].Mode != USER_PLAY` (22, `Code/TMSrv/CUser.h:36`) causes a `SendHpMode(conn)` call and return, which is a "resynchronize your HP display" instruction rather than a diagnostic — the server does not explain the refusal, it just corrects the client's belief about its own HP and abandons the action (`:33-37`). A dead character (`CurrentScore.Hp == 0`) is refused unless `SkillIndex` is exactly 99, the resurrection book, and this path additionally registers a crack-error of weight 1, type 8 (`:40-45`). Finally `pUser[conn].OnlyTrade` produces the `_NN_ONLYTRADE` message (`:47-51`).

The choice of `SendHpMode` as the rejection response for the mode and death cases is worth emphasising because it recurs throughout the component: the server's strategy for illegitimate actions is generally to resynchronise the client rather than to disconnect it. This makes the handler tolerant of client-side desynchronisation, but it also means that an attacker probing the protocol receives a semantically meaningful signal (a HP correction) rather than a rejection, which is a weak but real information leak. Only the dead-character case escalates to a crack-error accumulation, and even that is weighted at 1, whereas the ban threshold in `AddCrackError` is 2,000,000,000 (`Code/TMSrv/Server.cpp:694`).

**Rule workflow:**
```
receive(conn, m)
  m->ID = ESCENE_FIELD
  if pUser[conn].TradeMode        -> msg(_NN_CantWhenAutoTrade); STOP
  if pUser[conn].Mode != USER_PLAY-> SendHpMode(conn);          STOP
  if pMob[conn].HP == 0 && skill != 99
                                  -> SendHpMode(conn); AddCrackError(1,8); STOP
  if pUser[conn].OnlyTrade        -> msg(_NN_ONLYTRADE);        STOP
  continue to BR-02
```

**Edge cases and ambiguities:**
* The dead-character exemption is keyed on the literal value 99; there is no lookup of a "resurrection" skill property. Any future skill numbered 99 inherits the exemption.
* `TradeMode` and `OnlyTrade` are both checked although `_MSG_PKMode` (`Code/TMSrv/_MSG_PKMode.cpp:24-36`) already cancels trades when PK mode is toggled; the two checks are not obviously redundant but the interaction is undocumented.

---

### Business Rule: BR-02..BR-04 — Client Tick Anti-Cheat Envelope

**Covers:** BR-02, BR-03, BR-04
**Location:** `Code/TMSrv/_MSG_Attack.cpp:53-97`

**Overview:**
The client supplies a millisecond timestamp in every attack packet. The handler uses it to enforce a minimum attack interval, to detect clock manipulation, and to bound the tick to a plausible window around the server's own `CurrentTime`. All three checks are skipped when either the incoming tick or the recorded tick equals `SKIPCHECKTICK` (235,543,242), the sentinel used by server-generated packets.

**Detailed description:**

The 800 ms rule at `:59-66` is the component's primary rate limiter. It fires when both `m->ClientTick` and `pUser[conn].LastAttackTick` are *not* the sentinel and the new tick is less than the previous tick plus 800. Rejection writes a formatted line `"err,attack %d %d 800ms limit"` through `Log`, registers a crack error of weight 1 type 107, and returns *before any state mutation*. This ordering matters: because the early return precedes the `LastAttackTick` write at `:75`/`:96`/`:145`, a rejected packet does not refresh the timer, so a client that spams packets at 100 ms intervals remains blocked indefinitely rather than gradually drifting forward. In practice this turns the rate limiter into a hard floor on the client's own clock, which a modified client can bypass simply by advancing its timestamps by 800 each time — the check constrains the *reported* interval, not the observed arrival time. No server-side arrival timestamp is consulted anywhere in this component, so the 800 ms rule is a soft obstacle only. The subsequent 15-second window at `:79-97` is the actual defence against that bypass: it compares the client's claimed tick against `CurrentTime` and rejects anything more than 15 s in the future or more than 120 s in the past (`ClientTick15sec`, computed with a floor at `0x1D4C0` ≈ 120,000 to avoid underflow near server start). A cheating client must therefore keep its claimed clock within 120 s of the server's, which caps the achievable attack rate inflation at roughly 150× the intended rate for a burst, and at 1× sustained. The check is a genuine but coarse mitigation.

The regression check at `:68-77` is the weakest of the three. It compares `ClientTick` against `LastAttackTick - 100` using *unsigned* arithmetic: `if (ClientTick < (unsigned int)pUser[conn].LastAttackTick - 100)`. Because `LastAttackTick` is an `int` cast to `unsigned int` before subtraction, this expression only behaves as intended when `LastAttackTick >= 100`. With `LastAttackTick` still at its initialising value of 0 (a fresh session, or a session created through the `ProcessDBMessage.cpp:827` path that sets it to `SKIPCHECKTICK`, then reset), the subtraction wraps to about 4.29×10⁹ and the condition is never true, so no regression is detected. The penalty is also asymmetric: a detected regression adds 4 crack points (type 7) but execution continues, and the recorded tick is subsequently overwritten at `:96` regardless. The rule therefore detects but does not prevent clock rewinds.

The interaction between these rules and `SKIPCHECKTICK` is the most important structural detail. `ProcessClientMessage` at `Code/TMSrv/ProcessClientMessage.cpp:63` drops any packet arriving from a real client whose `ClientTick` equals the sentinel, so a network attacker cannot select the skip path. Only the four internal re-injection sites in `Server.cpp` (lines 4622, 5062, 5371 and 7464) can produce such packets, because they call `ProcessClientMessage(..., TRUE)` where `isServer == TRUE`. This is a legitimate and necessary design — mob attacks have no client clock — but it means the entire anti-cheat envelope is bypassed by definition for AI-originated combat, and the handler has no way to distinguish "trusted internal packet" from "packet that merely claims to be internal" if any future code path forwards client bytes without clearing the flag.

**Rule workflow:**
```
tick      = m->ClientTick
prevTick  = pUser[conn].LastAttackTick
if tick != SKIP && prevTick != SKIP:
    if tick < prevTick + 800                 -> log; crack(1,107); STOP
if tick != SKIP && prevTick != SKIP:
    if tick < (unsigned)prevTick - 100       -> crack(4,7)          [non-fatal]
    prevTickSaved = prevTick
    prevTick = tick ; LastAttack = skillnum
if tick != SKIP:
    lower = (CurrentTime <= 0x1D4C0) ? 0 : CurrentTime - 120000
    if tick > CurrentTime + 15000 || tick < lower
                                             -> log; crack(1,107); STOP
    prevTick = tick
```

**Edge cases and ambiguities:**
* Initial `LastAttackTick == 0` disables the regression branch for the first attack of a session.
* `LastAttackTick` is written three times in three different guards (`:75`, `:96`, `:145`) with different conditions; the final value depends on which guards were entered.
* The skill-delay branch at `:124` tests `LastAttackTick != -1`, i.e. against `0xFFFFFFFF`, which the variable can never hold given the sentinel is 235,543,242 and the initial value is 0. The guard is effectively always true.

---

### Business Rule: BR-05 — Skill Delay Floor (700 ms)

**Covers:** BR-05
**Location:** `Code/TMSrv/_MSG_Attack.cpp:124-143`

**Overview:**
For any skill in `[0, MAX_SKILLINDEX)` the handler enforces a fixed 700 ms minimum between uses and, if the interval is shorter, registers a 10-point crack error of type 30 and abandons the attack.

**Detailed description:**

The block computes `isTime = ClientTick - LastAttackTick` and compares it to the literal 700. It sits inside the `skillnum >= 0 && skillnum < MAX_SKILLINDEX` guard at `:113` and the `ClientTick != SKIPCHECKTICK` guard at `:124`, so it never applies to physical attacks (`skillnum == -1`) nor to server-originated packets. Rejection is severe relative to the other anti-cheat rules: 10 crack points versus 1, and a formatted `"skill delay skillnum: %d-%d"` log line carrying both timestamps. Because the ban threshold is 2×10⁹ points, 10 points per violation implies 200 million violations before any consequence, so the penalty is effectively a logging counter rather than a sanction.

The critical finding is that the block *computes and discards* the skill's real cooldown. Lines 128-133 read `g_pSpell[skillnum].Delay`, decrement it by one when `pMob[conn].MOB.Rsv & RSV_CAST` (0x40) is set and the delay is at least 2, multiply by 1000 to convert seconds to milliseconds, and assign the result to a local `int skilldelay`. No subsequent statement in the file references `skilldelay`. The only threshold actually enforced is the hard-coded 700. Consequently the entire `Delay` column of `SkillData.bin` — the primary balance lever for per-skill cooldowns — is inert in this code path. Two skills with nominal delays of 1 s and 8 s are throttled identically at 700 ms, and a server operator editing `SkillData.bin` to slow a powerful skill will observe no effect. The `RSV_CAST` fast-cast bonus, which is meant to shorten delays, is likewise dead. This is not merely a code smell; it is a divergence between configuration data and runtime behaviour that affects game balance and would mislead any operator tuning the shard.

Note also that the 700 ms floor is *independent* of the 800 ms floor from BR-02 and is evaluated against the same `LastAttackTick`. For a skill use whose previous action was also a skill, `ClientTick - LastAttackTick` will typically be at least 800 anyway, so the 10-point branch is reachable only when the client advances its tick by more than 800 but the *previous* recorded tick came from a different write site. In most operational sequences the 800 ms rule fires first and this rule is unreachable — meaning the harsher penalty is the one that rarely applies, and the effective cooldown for every skill in the game is 800 ms.

**Rule workflow:**
```
if skillnum in [0, MAX_SKILLINDEX) and tick != SKIP:
    if Passive(skill) == 1                        -> STOP              (BR-06)
    if prevTick != -1 and tick != SKIP:
        isTime     = tick - prevTick
        skilldelay = g_pSpell[skill].Delay
        if (Rsv & RSV_CAST) && skilldelay >= 2    -> skilldelay--
        skilldelay = skilldelay * 1000             [DEAD VALUE — never read]
        if isTime < 700 -> crack(10,30); log; STOP
    LastAttackTick = m->ClientTick
    if (skill/24) != Class && (skill/24) <= 3      -> log; STOP      (BR-07)
```

---

### Business Rule: BR-06..BR-08 — Skill Legality, Class Ownership and Learned-Skill Mask

**Covers:** BR-06, BR-07, BR-08
**Location:** `Code/TMSrv/_MSG_Attack.cpp:113-220`

**Overview:**
Three independent validations decide whether the requested skill may be executed: the skill must not be passive, it must belong to the attacker's class (below index 96), and its bit must be present in the character's 32-bit `LearnedSkill` mask. Failure of the class check aborts the whole attack; failure of the mask check depends on the return value of the crack-error accumulator.

**Detailed description:**

The class-ownership rule at `:147-151` computes `skillnum / 24` — integer division by `MAX_SKILL` (24, `Code/Basedef.h:111`) — and requires it to equal `pMob[conn].MOB.Class`, but only when the resulting group is `<= 3`. Given four playable classes (0..3) each owning 24 consecutive skill slots, indices 0–95 are strictly class-bound. Indices 96 and above divide to 4 or more and are therefore exempt, which is the mechanism that allows "book" skills — resurrection (99), book vine (98), mortar (97), the skill-102 buff — to be used by every class. The check is written as a negative with a compound condition, `if ((skillnum / 24) != Class && (skillnum / 24) <= 3)`, which reads as "wrong class and class-bounded", so a mage using a warrior skill is rejected while any class may use index ≥ 96. Rejection is a log plus a bare `return`, with no client message: the player sees nothing, and the server records `"MSG_Attack, Request other class skill"`.

The learned-skill mask is validated twice, in two different code branches, using two different bit derivations. For `skillnum < MAX_SKILLINDEX` (the normal skill range) the bit is `1 << (skillnum % MAX_SKILL)` at `:202-203`; for the book range the bit is `1 << (skillnum - 72)` at `:181-182`. Both compare against the 32-bit `pMob[conn].MOB.LearnedSkill`. The critical detail is that these checks are wrapped in `if (AddCrackError(conn, 8, 10)) return;` and `if (AddCrackError(conn, 208, 1)) return;` respectively, exploiting the fact that `AddCrackError` returns `TRUE` only when the accumulated `NumError` crosses 2,000,000,000 (`Code/TMSrv/Server.cpp:684-707`). In other words **using an unlearned skill does not prevent the skill from executing** — it merely adds 8 or 208 crack points and continues. The function name reads as if it enforces the rule; it does not. The 208-point variant is applied only to the book range, which suggests the developers intended book skills to be far more suspicious, but both are accumulation-only.

The structural consequence is that skill possession is advisory. A player with a modified client can cast any skill of their own class, learned or not, and the server will resolve its full damage in the same pass, because the mask check neither returns nor neutralises `dam`. It does not even clear `m->Dam[i].Damage`, so the normal resolution path continues unchanged. The only skills that cannot be used unlearned are those for which the *class* check fires, i.e. cross-class skills below index 96. Whether this is an intentional "soft" enforcement posture (log-and-allow) or a decompilation artefact of an original `return` is not determinable from the source; it is documented here as observed behaviour with high confidence about the code path and medium confidence about intent.

**Rule workflow:**
```
if skill in [0, MAX_SKILLINDEX) and tick != SKIP:
    if skill < -1 || skill > MAX_SKILLINDEX -> log; STOP           [DEAD]
    if Passive(skill) -> STOP
    ... 700ms floor ...
    LastAttackTick = tick
    if (skill/24) != Class && (skill/24) <= 3 -> log; STOP
Level / Delay / Special derivation:
    if skill < 0 || skill >= MAX_SKILLINDEX:
        if skill >= 96 && skill <= MAX_SKILLINDEX:                 [only 103 reachable]
            if skill == 97 -> mortar item validation               [DEAD]
            else           -> if !(1<<(skill-72) & LearnedSkill) crack(208,1)
            Level = CurrentScore.Level ; Delay += Level ; Special = Level
    else:
        skillkind = skill % 24 / 8 + 1                             [1..3]
        if !(1<<(skill%24) & LearnedSkill) -> crack(8,10)
        if skillkind < 0 || skillkind > 3 -> log; STOP             [DEAD]
        Level = CurrentScore.Special[skillkind] ; Delay += Level ; Special = Level
```

---

### Business Rule: BR-09 — Mortar Ground-Item Validation

**Covers:** BR-09
**Location:** `Code/TMSrv/_MSG_Attack.cpp:158-178`

**Overview:**
Skill 97 (mortar) is supposed to require a valid mortar item (index 746) placed on the ground grid at the requested coordinate, with the coordinates bounded to `(0,4096)`. The rule validates the item, the slot range and the packet position, and forces `m->Motion = 1` on success.

**Detailed description:**

The mortars rule reads `pItemGrid[m->PosY][m->PosX]` after bounding `m->PosX/PosY` to `(0, 4096)`, then requires the resulting inventory slot to be in `(0, MAX_ITEM)` and the item's `sIndex` to be exactly 746. Any failure writes `"err,mortar invalid"` and returns from the entire function. The design intent is clear: the mortar is an artillery skill whose projectile origin is an object in the world, not the caster, so the server must confirm the object exists before computing a trajectory. Forcing `Motion = 1` communicates the special animation to observers.

The rule is nevertheless **unreachable**. It is nested inside `if (skillnum < 0 || skillnum >= MAX_SKILLINDEX)` (`:158`), whose guard admits only negative indices and indices ≥ 103, and inside `if (skillnum >= 96 && skillnum <= MAX_SKILLINDEX)` (`:160`), which admits only 96..103. The intersection is the single value 103 — never 97. Consequently no client can ever reach the mortar validation, and conversely the `else` branch at `:179-189` that computes `learned = 1 << (skillnum - 72)` is reachable only for `skillnum == 103`, producing `1 << 31`, which is undefined behaviour for a signed 32-bit shift. The expression `int learned = 1 << (skillnum - 72)` at `:182` therefore has UB when evaluated, and `learned & pMob[conn].MOB.LearnedSkill` afterwards is meaningless.

Two further consequences follow. First, skill 97 arriving from a client falls through to the `else` branch at `:196-220` where `skillkind = 97 % 24 / 8 + 1 = 1`, so its scaling value is taken from `Special[1]` rather than from the character's level — which is almost certainly not intended for an artillery skill. Second, the mortar-item rule is a documented, implemented business rule that no longer executes; any balance or exploit analysis that assumes it is active will be wrong. The most likely explanation is decompilation of a nested conditional whose bounds were originally `skill >= 96 && skill < MAX_SKILLINDEX` in the outer guard; the current `>=` makes the inner range a dead zone. This is reported as a high-confidence structural defect and a medium-confidence root cause.

**Rule workflow:**
```
[only reachable if skill == 103]
if skill == 97:
    if PosX outside (0,4096) or PosY outside (0,4096)      -> log "mortar invalid"; STOP
    slot = pItemGrid[PosY][PosX]
    if slot <= 0 or slot >= MAX_ITEM or pItem[slot].sIndex != 746
                                                          -> log "mortar invalid"; STOP
    m->Motion = 1
else:
    learned = 1 << (skill - 72)                            [UB for 103]
    if tick != SKIP and !(learned & LearnedSkill)          -> crack(208,1)
Level = Level ; Delay += Level ; Special = Level
```

---

### Business Rule: BR-10 — Golden Shield Coin Cost

**Covers:** BR-10
**Location:** `Code/TMSrv/_MSG_Attack.cpp:222-233`

**Overview:**
Skill 85 ("Escudo Dourado") is purchased with in-game currency per cast rather than (or in addition to) mana: the cost is `100 × Level`, where `Level` is the character's `CurrentScore.Level` for book-range skills. Insufficient coin aborts the attack silently.

**Detailed description:**

The rule is the only place in the component where `pMob[conn].MOB.Coin` is read or written. It computes `coin = 100 * Level`, rejects the cast with a bare `return` when `pMob[conn].MOB.Coin < coin` — no client message, no crack error — and otherwise deducts and calls `SendEtc(conn)` to refresh the client's currency display. Because the block runs *before* the mana block at `:239`, a caster who lacks coin loses nothing; a caster who has coin is charged before any validation of the skill's targets or effects, so a subsequently-failing skill still consumes the currency. That asymmetry (charge first, validate later) is consistent with the rest of the resource model in the component: mana is also deducted at `:249` before the target loop, so an attack that lands on zero valid targets still costs mana and coin.

`SendEtc` is a bulk "resend miscellaneous client state" call used 8 times elsewhere in `TMSrv` for currency, PK info and quest state. Its use here, rather than a targeted currency update, means every Golden Shield cast triggers a larger-than-necessary client update. Functionally correct, but it contributes to the component's chatty output profile.

The `Level` used at `:225` is resolved by the *normal* skill branch at `:196-220` (skill 85 is below `MAX_SKILLINDEX`, so the book branch never sees it): `skillkind = 85 % 24 / 8 + 1 = 13 / 8 + 1 = 2`, therefore `Level = CurrentScore.Special[2]`. The cost is `100 × Special[2]`, which for a developed character (`Special[2]` in the hundreds) is a substantial recurring gold sink — `30,000` coins per cast at `Special[2] = 300`. This makes BR-10 one of the few rules in the component that couples combat to the economy, and the only one that can be starved by a non-combat resource.

**Rule workflow:**
```
if skill == 85:
    coin = 100 * Level              [Level == CurrentScore.Special[2]]
    if Coin < coin -> STOP (silent)
    Coin -= coin ; SendEtc(conn)
```

---

### Business Rule: BR-11 — Mana Consumption Model

**Covers:** BR-11
**Location:** `Code/TMSrv/_MSG_Attack.cpp:235-256`; `Code/Basedef.cpp:5415-5421`

**Overview:**
Every skill in `[0, MAX_SKILLINDEX)` consumes mana computed by `BASE_GetManaSpent`, and the cast is refused (with an HP/MP resynchronisation) if the cost would drive the pool negative. The cost is scaled by the character's `Special` value and reduced by the `SaveMana` stat.

**Detailed description:**

`BASE_GetManaSpent` (`Code/Basedef.cpp:5415-5421`) is a two-factor formula: `manaspent = g_pSpell[skill].ManaSpent * (Special/2 + 100) / 100`, then `manaspent = (100 - SaveMana) * manaspent / 100`. The first factor inflates the base cost by up to 50% as the character's Special stat approaches 100 for that skill group; the second discounts it linearly by the `SaveMana` percentage. Because both are integer divisions applied in sequence, rounding truncates twice, which makes the effective cost slightly cheaper than the nominal product — most visible at low `ManaSpent` values where truncation can reach 1 MP. The `Special` value passed in is the one derived in BR-06/BR-09 (`CurrentScore.Special[skillkind]` for normal skills, `CurrentScore.Level` for book skills, `0` for negative or out-of-range indices).

The deduction at `:249-251` performs three coupled writes: the authoritative pool `pMob[conn].MOB.CurrentScore.Mp`, the client-projection `pUser[conn].ReqMp`, and `SetReqMp(conn)` to push the new value to the client. Keeping a separate `ReqMp` shadow is the project's general pattern for fields the client must be told about but must not be trusted for; it is a correctness hazard because the two can drift, and this analysis found at least one place (`:832-833`) where they are restored together from saved values, implying the developers were aware of the coupling. The mana check uses `CurrentScore.Mp - ManaSpent < 0` rather than comparing against `ManaSpent`, which is equivalent but tolerates `ManaSpent == 0` (no-op) and negative costs (which would *grant* mana) without special-casing.

After the deduction — and regardless of whether the skill later fails its target loop — `m->CurrentMp` and `m->ReqMp` are stamped into the outbound packet at `:254-255`, so observers see the reduced mana even for a whiffed cast. This is desirable for animation fidelity but means the packet broadcast at `:1734` leaks the caster's exact mana pool to everyone in view range. Because the pool is also visible through the standard score messages, this is not a new disclosure, merely a duplicated one.

**Rule workflow:**
```
Mp = CurrentScore.Mp ; ReqMp = pUser[conn].ReqMp
if skill in [0, MAX_SKILLINDEX):
    cost = BASE_GetManaSpent(skill, SaveMana, Special)
         = ManaSpent * (Special/2 + 100)/100 * (100 - SaveMana)/100
    if CurrentScore.Mp - cost < 0 -> SendSetHpMp(conn); STOP
    CurrentScore.Mp -= cost
    pUser[conn].ReqMp -= cost
    SetReqMp(conn)
m->CurrentMp = CurrentScore.Mp ; m->ReqMp = pUser[conn].ReqMp
```

---

### Business Rule: BR-12 — Combat Mastery Derivation

**Covers:** BR-12
**Location:** `Code/TMSrv/_MSG_Attack.cpp:258-269`; consumed at `:457`, `:465`, `:567`, `:601`

**Overview:**
A single integer, `master`, is derived once per attack and passed to every damage formula as the "combat" variance term. It is available only to Class 0 characters who learned skill bit `0x4000` and equals `Special[2]/20`, clamped to `[0,15]`.

**Detailed description:**

`master` is the component's only per-attack derived combat statistic. It is computed as `pMob[conn].MOB.CurrentScore.Special[2] / 20` when `pMob[conn].MOB.Class == 0` (Taoist/Knight-line, the only `Class == 0` branch used elsewhere in the file) and `pMob[conn].MOB.LearnedSkill & 0x4000` is set; otherwise it remains 0. Two clamps follow: negative values become 0 and values above 15 become 15. With `Special[2]` capped by gameplay at roughly 300, the practical maximum is reached at `Special[2] = 300`, so the clamp is meaningful only for boosted or corrupted characters.

The value flows into `BASE_GetDamage(dam, ac, master)` and `BASE_GetSkillDamage(dam, def, master)`. Those functions (`Code/Basedef.cpp:1225-1256` and `:1446-1477`) use it twice: as a divisor halving step (`combat /= 2`, then capped at 7 for physical, 15 for skills) that shrinks a random range, and as a bias added to a random roll (`rnd = rand() % delta + combat + 99` for physical, `+ 90` for skills). Because `delta = 12 - combat` for physical, a higher `master` simultaneously narrows the variance band and shifts its centre upward — the tightest, highest-damage roll occurs at `combat = 7`, i.e. `master >= 14`. The same structure appears in the skill formula with `delta = 21 - combat` and the cap at 15. `master` is therefore a pure "consistency and floor" stat: it does not raise the ceiling, it raises the average and reduces the spread.

Because `master` is computed before the target loop, every target slot of an area attack shares the same value — the variance reduction applies uniformly rather than being re-rolled per target, which is a deliberate and defensible design (area attacks feel consistent) but is not documented anywhere in the code. The `0x4000` bit-condition is a hard-coded learned-skill index with no named constant; it cannot be identified from source alone which skill this represents, and is documented here as an ambiguity.

**Rule workflow:**
```
master = 0
if Class == 0 and (LearnedSkill & 0x4000):
    master = CurrentScore.Special[2] / 20
    if master < 0  -> master = 0
    if master > 15 -> master = 15
... master passed to BASE_GetDamage / BASE_GetSkillDamage for every target slot ...
```

---

### Business Rule: BR-13, BR-14, BR-18 — Target Slot Enumeration and View-Range Gate

**Covers:** BR-13, BR-14, BR-18
**Location:** `Code/TMSrv/_MSG_Attack.cpp:297-351`

**Overview:**
The handler iterates a fixed 13-slot array, deciding how many slots are meaningful from the declared packet size, and validates each slot's target identity, liveness and view-range membership before any damage work.

**Detailed description:**

The loop header is `for (int i = 0; i < MAX_TARGET; i++)` with three gates at the top. The first, `if (i >= MAX_TARGET && m->Size <= sizeof(MSG_Attack))`, can never be true because `i < MAX_TARGET` is the loop condition — it is a decompiler artefact of a guard that originally tested something else (probably `m->Size <= sizeof(MSG_AttackOne)` at `i >= 1`). The second and third gates implement the real protocol: `i >= 1` breaks when the size is at most `MSG_AttackOne` (a one-target packet), and `i >= 2` breaks when the size is at most `MSG_AttackTwo` (a two-target packet). The resulting contract matches the struct definitions at `Code/Basedef.h:2011-2133`: `MSG_Attack.Dam[MAX_TARGET]` (13), `MSG_AttackOne.Dam[1]` and `MSG_AttackTwo.Dam[2]`. Note that `MSG_AttackOne` is compiled under `#pragma pack(push,1)` while the others are not, so the three sizes are not trivially comparable; the comparisons are nevertheless only used as upper bounds and remain correct.

Slot identity is then resolved at `:309-312`: `int idx = m->Dam[i].TargetID;` with a rejection for `idx <= 0 || idx >= MAX_MOB`. This bounds the index into the 25,000-entry `pMob` array and is the component's most important memory-safety guard — every subsequent `pMob[idx]` access relies on it. Because `TargetID` is a client-supplied 32-bit integer compared against `MAX_MOB` (25,000), a negative or oversized value is safely skipped. Empty mobs (`MOB_EMPTY == 0`) are answered with `SendRemoveMob(conn, idx, 1, 0)`, which tells the client to delete the stale visual. Already-dead targets (`Hp <= 0`) are zeroed and removed at `:331-338` unless the packet's skill is 31 (rebirth) or 99 (resurrection), which is the mechanism that lets those two skills operate on corpses.

The view-range gate at `:347-351` is the closest thing the component has to a target-legitimacy check. It requires — for all skills except 42 — that the attacker's *target cell* (`pMob[conn].TargetX/Y`, i.e. where the attacker last aimed) be within ±`VIEWGRIDX`/±`VIEWGRIDY` (33 each, `Code/Basedef.h:95-96`) of the victim's position. Two observations follow. First, the check is asymmetric and surprisingly loose: it compares the attacker's *aim point* rather than the attacker's body, so an attacker standing far away but aiming near the victim passes. Second, on failure the handler calls `SendRemoveMob` and `continue` without zeroing `m->Dam[i]`, so the untouched `-2`/`-1` sentinel remains in the packet that is broadcast at `:1734`; receivers are expected to ignore slots whose target was removed, but the sentinel is not cleared, which is an inconsistency relative to every other rejection path (all of which zero both `TargetID` and `Damage`). Skill 42 is exempt from the range gate entirely, a special case with no comment explaining it.

**Rule workflow:**
```
for i in [0, 13):
    if i>=1 and Size <= sizeof(MSG_AttackOne)  -> break
    if i>=2 and Size <= sizeof(MSG_AttackTwo)  -> break
    idx = Dam[i].TargetID
    if idx <= 0 or idx >= MAX_MOB             -> continue
    if idx is Class 3                        -> DoRemoveHide(idx)
    if pMob[idx].Mode == MOB_EMPTY            -> SendRemoveMob; continue
    if <invulnerable NPC rule BR-15>          -> zero slot; continue
    if HP <= 0 and skill not in {31,99}       -> zero slot; SendRemoveMob; continue
    if <merchant rule BR-17>                  -> zero slot; continue
    if skill != 42 and target outside ±33 grid -> SendRemoveMob; continue
    ... BR-19 ... BR-21 ...
```

---

### Business Rule: BR-15, BR-16, BR-17 — Target Immunity Classes

**Covers:** BR-15, BR-16, BR-17
**Location:** `Code/TMSrv/_MSG_Attack.cpp:323-345`

**Overview:**
Three categories of entity are protected from damage by unconditional filters: NPCs wearing special items on a specific map cell, corpses (outside the two revive skills), and merchant NPCs of four specific types.

**Detailed description:**

The NPC immunity rule at `:323-329` is the most narrow and most puzzling. It requires `idx >= MAX_USER` (so only non-player entities), `pMob[idx].MOB.Equip[0].sIndex` equal to 219 or 220, and the NPC to be standing in map block `(26,12)` computed as `TargetX/128 == 26 && TargetY/128 == 12`. Items 219 and 220 are the same pair referenced in BR-44 (`:893`) and in the exterminate knockback check (`:987`), which suggests they are protective/immobile NPC items rather than ordinary equipment. The coordinate gate makes the protection map-specific — the same NPC elsewhere is vulnerable. There is no comment identifying the map; from the block arithmetic the cell spans x∈[3328,3455], y∈[1536,1663]. When the rule fires, both `TargetID` and `Damage` are zeroed, which is the correct and consistent rejection form.

The corpse rule at `:331-338` treats any target at `Hp <= 0` as invalid unless the packet's skill is 31 or 99, in which case the slot is allowed to proceed into the skill branch so that rebirth or resurrection can restore it. The exemption list is by literal skill number and is duplicated in BR-01's aliveness gate, so both the attacker's death and the victim's death use the same two magic numbers with the same special-case list. This duplication is fragile: adding a third revival skill requires editing two distant blocks, and skill 31's own handler at `:1025-1048` only revives on a 70% roll, so a failed rebirth still consumes the caster's MP (zeroed at `:1029-1030`).

The merchant rule at `:340-345` blocks damage to any mob whose `MOB.Merchant` field equals 1, 4, 43 or 100. `Merchant` distinguishes shop/quest/storage NPC variants; the four values presumably correspond to shop keeper, repair, storage and a special type. This is a broad protection that applies everywhere on every map, unlike BR-15. Together, the three rules mean that a client cannot damage a shop NPC, a corpse, or a specific decorative NPC, and the server enforces all three before any damage computation — which is the correct ordering, since these are cheap integer tests that should shadow the expensive arithmetic.

**Rule workflow:**
```
if idx >= MAX_USER
   and Equip[0].sIndex in {219,220}
   and (TargetX/128)==26 and (TargetY/128)==12  -> zero slot; continue
if HP <= 0 and skill not in {31,99}             -> zero slot; SendRemoveMob; continue
if Merchant in {1,4,43,100}                     -> zero slot; continue
```

---

### Business Rule: BR-19 — Damage Field Integrity (Anti-Tamper Sentinel)

**Covers:** BR-19
**Location:** `Code/TMSrv/_MSG_Attack.cpp:353-361`

**Overview:**
The client is not permitted to state a damage number. Each slot's `Damage` must be exactly `-2` (physical), `-1` (skill) or `0` (unused); any other value is zeroed, penalised with 10 crack points of type 77, and the slot abandoned.

**Detailed description:**

The check is written with a 64-bit temporary: `long long dam = m->Dam[i].Damage;` followed by `if (dam != -2 && dam != -1 && dam)`. Widening to `long long` before comparing is defensive — it guarantees that any 32-bit value the client writes is compared exactly against the three literals with no promotion surprises. The negation form `&& dam` means "non-zero", so the accepted set is `{-2, -1, 0}`. Values outside that set are both zeroed and penalised. This is the single most important anti-cheat rule in the component because it establishes the invariant that every damage figure in the outbound packet was produced by `BASE_GetDamage`/`BASE_GetSkillDamage` or by a hard-coded skill formula, never by the client.

What the rule does *not* do is equally important. It does not validate `TargetID` against any server-side list of legal targets (only the range/immunity filters above), it does not validate `PosX/PosY`/`TargetX/TargetY` against the attacker's actual position (the position check that would have done so at `:104-111` compares values to themselves and is therefore dead), and it does not validate `m->Size` against the actual received frame length — `m->Size` is taken from the packet header, which `CPSock::ReadMessage` uses to frame the message, so it is bounded by the socket buffer but not by the declared struct. A client can therefore declare `MSG_Attack` size and send 13 slots, all with `Damage = -1`, all with `TargetID` pointing at distinct mobs within view — and the handler will resolve 13 independent skill hits per packet, subject only to the `MaxTarget` cap at `:492-502` for skills and the 800 ms inter-packet rule. This is the practical shape of the area-attack exploit surface in this component.

The penalty is `AddCrackError(conn, 10, 77)`, 10 points, which as established is 1/200,000,000 of the ban threshold. A client that sends valid sentinels only is never penalised at all, so the integrity rule functions primarily as a "malformed client" detector rather than a cheat detector.

**Rule workflow:**
```
dam = (long long)Dam[i].Damage
if dam != -2 and dam != -1 and dam != 0:
    Dam[i].Damage = 0
    AddCrackError(conn, 10, 77)
    continue
branch on dam:
    -2 -> physical resolution   (BR-22..BR-27)
    -1 -> skill resolution      (BR-28..BR-54)
     0 -> skip (no-op, falls through both branches to the "invalid" else)
```

**Ambiguity:** a slot with `dam == 0` passes the integrity check but matches neither the `dam == -2` nor the `dam == -1` branch, so control reaches the `else` at `:1273-1277`, logs `"err,dam not -2 -1 MSG_ATTACK"` and **returns from the entire function**, aborting all remaining target slots. A benign client that leaves unused slots as `0` in a `MSG_Attack` packet would therefore truncate its own multi-hit. Whether clients ever do this cannot be determined from server source alone.

---

### Business Rule: BR-20 — Guard Tower Attack Restriction

**Covers:** BR-20
**Location:** `Code/TMSrv/_MSG_Attack.cpp:398-403`; `Code/TMSrv/CWarTower.cpp:147-156`

**Overview:**
Entities whose `GenerateIndex` is the guard-tower generator may only be attacked by a player who has a guild and whose guild differs from the tower's guild. All other attackers have the slot voided.

**Detailed description:**

`CWarTower::TowerAttack(conn, idx)` is a three-line predicate: if the target is not a tower it returns `TRUE` (attack allowed); if the attacker has guild 0, or the same guild as the tower, it returns `FALSE`; otherwise `TRUE`. The caller voids the slot (`TargetID = 0`, `Damage = 0`) and continues on `FALSE`. This is the component's only guild-identity rule that is not also enforced later, and it is evaluated *before* the leader/guild identity block at `:363-395` — meaning the tower predicate re-reads `MOB.Guild` independently and does not honour the `GuildDisable` override applied at `:375-381`. A player with `GuildDisable` set (guild temporarily hidden, e.g. during a guild-war concealment) is treated as guild 0 by `TowerAttack` and therefore cannot attack towers during the concealment window, while every other rule in the handler treats them as guildless-and-attackable. That inconsistency is a real behavioural asymmetry.

The rule matters architecturally beyond its own scope: it is the only place in the handler where a different component makes the allow/deny decision rather than the handler itself. Everything else is inlined. This makes the tower policy independently testable and changeable — a positive — but it also means the handler's veto logic is split across two files with no shared contract beyond a boolean.

**Rule workflow:**
```
if TowerAttack(conn, idx) == FALSE:
    Dam[i].TargetID = 0
    Dam[i].Damage   = 0
    continue
TowerAttack:
    if GenerateIndex != GTORRE           -> TRUE
    if attacker.Guild == 0               -> FALSE
    if attacker.Guild == tower.Guild     -> FALSE
    -> TRUE
```

---

### Business Rule: BR-21 — Consensual PvP Gate (PK Mode and Guilt)

**Covers:** BR-21
**Location:** `Code/TMSrv/_MSG_Attack.cpp:405-419`

**Overview:**
Outside of RvR, castle siege, guard-tower war and newbie-event servers, a player may not damage another player who is neither in PK mode nor guilty, provided the cell the attacker is aiming at carries the `0x40` (combat-permitted) attribute. The rule applies to physical attacks unconditionally and to skills only when the skill is flagged `Aggressive`.

**Detailed description:**

The guard at `:405` is a five-way conjunction: both endpoints must be players (`conn`/`idx` in `(0, MAX_USER)` and `conn != idx`), and all four global war states must be zero (`RvRState`, `CastleState`, `GTorreState`, `NewbieEventServer`). Inside it, the handler reads `pUser[conn].PKMode`, `pUser[idx].PKMode`, `GetGuilty(idx)` and the `0x40` bit of the attacker's aimed cell. The rejection condition is a two-branch disjunction merged into one expression: `(dam == -2 && !TargetPK && !TargetGuilty && attributemap)` for physical, or `(dam == -1 && skill in [0,MAX_SKILLINDEX] && Aggressive && attributemap && !TargetPK && !TargetGuilty)` for skills. Both mean "unconsensual PvP attempt inside a combat-permitted zone is refused". Refusal zeroes the slot cleanly.

The semantics deserve care because they are counter-intuitive: the `0x40` attribute means combat is *allowed* in that cell, and the rule refuses damage when the victim has not opted in. In other words `0x40` marks the "wilderness" where non-consensual PvP is *possible* but still requires the victim to be flagged either by PK mode or by guilt. Outside `0x40` cells, damage simply falls through this gate and is later removed by BR-64's attribute requirement — so the two rules combine to mean "no PvP damage anywhere the map does not permit it, and no PvP damage to unflagged victims even where it does". The victim's own `PKMode` flag is the consent token; `GetGuilty` is the second token, granted when a player attacks a guilty player (BR-61) or via other components.

`GetGuilty` (`Code/TMSrv/GetFunc.cpp:2114-2127`) reads a single byte from `MOB.Carry[KILL_MARK].stEffect[1].cEffect` and self-heals values above 50 back to 0 — a stale-value scrub executed on read, which means simply *querying* a player's guilt can mutate their state. `SetGuilty` clamps to `[0,50]` and ignores out-of-range connection indices. Using an inventory slot (`Carry[KILL_MARK]`, where `KILL_MARK == 63`, `Code/Basedef.h:166`) as a persistence field for PK state is a notable design choice: it piggybacks on the character save format so guilt survives relog, but it also means a player's guilt is an item-shaped value subject to any future item-handling code that touches slot 63.

**Rule workflow:**
```
if conn != idx and both are players and all war states == 0:
    AttackerPK  = pUser[conn].PKMode
    TargetPK    = pUser[idx].PKMode
    TargetGuilt = GetGuilty(idx)
    cellAttr    = GetAttribute(attacker.TargetX, attacker.TargetY) & 0x40
    if (dam == -2 and !TargetPK and !TargetGuilt and cellAttr)
    or (dam == -1 and skill in range and Aggressive and cellAttr
        and !TargetPK and !TargetGuilt):
        zero slot; continue
```

---

### Business Rule: BR-22..BR-27 — Physical Attack Resolution

**Covers:** BR-22, BR-23, BR-24, BR-25, BR-26, BR-27
**Location:** `Code/TMSrv/_MSG_Attack.cpp:421-485`

**Overview:**
The physical branch resolves a melee/ranged attack: range validation, multi-target permission, base damage from the character's computed `Damage` stat, an optional pre-armour critical multiplier, armour reduction via `BASE_GetDamage`, an optional post-armour doubling, and an optional Huntress double-strike.

**Detailed description:**

Range is checked first at `:424-427`: `dis = BASE_GetDistance(m->PosX, m->PosY, m->TargetX, m->TargetY)` must not exceed `pUser[conn].Range` nor the hard ceiling 23. `BASE_GetDistance` (`Code/Basedef.cpp:5828-5849`) is a Chebyshev-like metric using a precomputed table for offsets up to 6 and `max(dx,dy)+1` beyond, so the `> 23` ceiling effectively limits attacks to a 23-cell radius (the `+1` in the metric means the true geometric radius is one less). `pUser[conn].Range` is set in `CMob::GetCurrentScore` (`Code/TMSrv/CMob.cpp:661-662`) where it is assigned from `BASE_GetMobAbility(EF_RANGE)` and then **immediately overwritten with 23**, making the item-derived range entirely inert and both halves of the `dis > Range || dis > 23` test identical. Note the failure mode: exceeding range triggers a bare `return`, not a `continue`, so an out-of-range physical attack aborts all remaining target slots too. This is a meaningful behavioural quirk for multi-target physical packets — one distant target can suppress the resolution of closer ones depending on slot order.

The multi-target permission at `:431-435` fires when `i > 0`, the packet is smaller than `MSG_AttackTwo`, the attacker is not a Huntress (`Class != 3`), and learned-skill bit `0x40` is absent; it registers 10 crack points of type 24 and skips the slot. This rule exists because a `MSG_AttackOne` packet physically has room for one target, so `i > 0` is only reachable when a client declared a larger size than the struct it claims to send — i.e. the rule detects size/struct mismatch rather than a gameplay condition. Since `m->Size` comes from the packet header, a client declaring `MSG_Attack` size simply never trips it; the rule cannot actually prevent multi-target melee.

Damage computation is a five-step chain. (1) `CriticalDouble` is computed once, for `i == 0` only, via `BASE_GetDoubleCritical(&pMob[conn].MOB, &pUser[conn].cProgress, &m->Progress, &DoubleCritical)` (`Code/Basedef.cpp:5466-5536`), which advances a 1024-entry hit-rate table index (`cProgress`) and derives two bits: bit 0 from comparing `g_pHitRate[cProgress]` against `100 * ((AttackRun >> 4) - 5)`, bit 1 from `rand() % 255 < MOB.Critical`. (2) The base value is `pMob[conn].MOB.CurrentScore.Damage`. (3) If bit 1 is set, damage is multiplied by `(13 + rand%2)/10` for player victims or `(15 + rand%2)/10` for mobs — a 1.3–1.4× or 1.5–1.6× multiplier applied *before* armour, so it is amplified by any armour subtraction. (4) Armour `Ac` is multiplied by 3 for player victims only, then `BASE_GetDamage(dam, Ac, master)` applies `dam - Ac/2`, a `rnd` percentage in `[combat+99, combat+delta+99)`, floor behaviours for negative results (`+50` then `/7`), a small-value booster (`5*t/4 + 7` when `0 <= t <= 50`), and a final floor of 1. (5) If bit 0 is set, the post-armour value is doubled at `:480-481` — a "double critical" that stacks multiplicatively with the pre-armour critical. A character can therefore land up to roughly 2.8× (players) or 3.2× (mobs) the nominal damage on a single hit.

The Huntress double-strike at `:459-478` is a separate, conditional mechanism: `Class == 3`, learned bit `0x200000`, packet at least `MSG_AttackTwo`, 25% chance. It computes `Special[3] + Str` through `BASE_GetDamage` against the victim's raw `Ac` (not the ×3 player value), halves and floors it at 60, stores it into `m->Dam[1]` — an out-of-band scratch slot — sets `DoubleCritical |= 4` and adds it to the main damage. The scratch-slot technique is reused downstream at `:1287-1288` (piercing) and `:1547-1548` (mana shield), and it is fragile: `Dam[1]` is a real target slot in `MSG_Attack`/`MSG_AttackTwo`, so on the `i == 1` iteration the value can be overwritten by that slot's own resolution, and the downstream reads then use whatever is there. In practice the loop skips target 0, so `Dam[1]` survives — but only because of the skip, not by construction.

Finally, `m->DoubleCritical = DoubleCritical` at `:483` publishes the three bits plus (later) the spectral bit, and `int CriticalDouble = 0;` at `:437` is assigned but never read — a dead local.

**Rule workflow:**
```
if dam == -2:
    dis = BASE_GetDistance(Pos, Target)
    if dis > Range or dis > 23                       -> RETURN (aborts all slots)
    dam = 0
    if i > 0 and Size < sizeof(MSG_AttackTwo)
       and Class != 3 and !(LearnedSkill & 0x40)     -> crack(10,24); continue
    if i == 0: BASE_GetDoubleCritical(...) -> DoubleCritical
    dam = CurrentScore.Damage
    if (DoubleCritical & 2):
        dam = ((rand%2 + (idx<MAX_USER ? 13 : 15)) * dam) / 10
    Ac  = pMob[idx].Ac ; if idx < MAX_USER -> Ac *= 3
    dam = BASE_GetDamage(dam, Ac, master)
    if i == 0 and Size >= sizeof(MSG_AttackTwo) and Class == 3
       and (LearnedSkill & 0x200000) and rand%4 == 0:
        skilldam = BASE_GetDamage(Special[3]+Str, pMob[idx].Ac, master)
        if skilldam > 0 -> skilldam /= 2 ; if skilldam < 60 -> skilldam = 60
        Dam[1] = skilldam ; DoubleCritical |= 4 ; dam += skilldam
    if (DoubleCritical & 1): dam *= 2
    m->DoubleCritical = DoubleCritical
```

---

### Business Rule: BR-28, BR-29 — Skill Target Cap and Party-Only Skill Validation

**Covers:** BR-28, BR-29
**Location:** `Code/TMSrv/_MSG_Attack.cpp:492-512`

**Overview:**
Each skill carries a declared maximum target count (`g_pSpell[].MaxTarget`) and an optional party-only flag (`bParty`). Slots beyond the cap, and non-party recipients of a party-only skill, are rejected.

**Detailed description:**

The target cap compares the loop index directly against the data-driven value: `if (ClientTick != SKIPCHECKTICK && i > maxtarget)`. Note the strictly-greater comparison, so a skill declaring `MaxTarget = 1` permits `i` of 0 and 1 — two targets. This off-by-one is a genuine deviation from the declared contract and affects every area skill in the game by one extra target slot. Rejection registers 10 crack points of type 28 and logs `"skill target max num %d"` with the client tick, then continues to the next slot rather than aborting the attack. As with BR-19's penalty, 10 points is negligible.

The party rule at `:504-512` fires when `g_pSpell[skill].bParty != 0` and the attacker and victim have neither the same leader nor the same guild. Leader identity is pre-resolved at `:363-371` with the convention that a leaderless entity is its own leader (`leader == 0 -> leader = conn`), and guild identity at `:373-384` with `GuildDisable` forcing 0 and the special rule that when *both* sides are guildless the comparison value becomes `-1` (so that two guildless players compare equal and the skill is permitted). That `-1` trick is the sole reason guildless party-less bystanders can be buffed by a guildless caster. There is also a battlefield exception at `:386-387`: when `BrState && BRItem > 0` and the attacker stands inside x∈[2604,2648], y∈[1708,1744], `Guild` is forced to `-1`, i.e. everyone is treated as guildless-friend for party-skill purposes inside the battle-royale arena.

The party rule registers 10 crack points of type 27 and logs `"party skill - skill:%d leader:%d tleader:%d"` on violation, then continues. It is the *only* validation that uses the pre-resolved leader/guild values in an enforcement role; the same values are reused later at BR-58 and BR-51 for suppression and buffing, so a mismatch in any of those three readings produces inconsistent behaviour across the same attack.

**Rule workflow:**
```
maxtarget = g_pSpell[skill].MaxTarget
if tick != SKIP and i > maxtarget            -> crack(10,28); log; continue
if g_pSpell[skill].bParty != 0 and leader != mobleader and Guild != MobGuild
                                             -> crack(10,27); log; continue
```

---

### Business Rule: BR-30, BR-31 — Elemental Skill Damage, Resistance and Weather

**Covers:** BR-30, BR-31
**Location:** `Code/TMSrv/_MSG_Attack.cpp:517-589`; `Code/Basedef.cpp:6342-6442`

**Overview:**
Skills with `InstanceType` 1–5 are elemental. Their damage is computed by `BASE_GetSkillDamage(skill, mob, weather, weaponDamage)`, reduced by the victim's defence (player AC ×2, Foema AC ×1.5), then scaled by `(150 - Resist[index]) / 100`.

**Detailed description:**

The elemental formula in `BASE_GetSkillDamage` is class- and skill-dependent and is the densest business logic the Attack Handler delegates. For `InstanceType` 1–5 it branches on `mob->Class` and on `skillnum / 8`: Class 0 with `skind == 1` uses `3×weaponDamage + 3×Str + level + special + base`; Class 0 otherwise uses `special + base + weaponDamage + level + Int/4 + Int/40`; Classes 1 and 2 (Foema, BeastMaster) share `Int/30 + Int/3 + level + base + 2×special`; Class 3 uses `3×weaponDamage + 3×Str + level/2 + special + base`. Skill 97 is special-cased to `15×level + base` before any class branch. Weather then applies multipliers: in weather 1, `InstanceType == 2` takes ×0.9 and `InstanceType == 5` takes ×1.3; in weather 2, `InstanceType == 3` takes ×1.2. Finally a magic-power amplifier applies — `(4×Magic + 100)/100` then `×5/4` for all casters except Class 3 and the Class-0 melee case — which makes `MOB.Magic` (a computed stat) multiplicative on all non-physical elemental damage.

Weather is normally `CurrentWeather`, but the handler overrides it to 0 inside a specific map region: if `TargetX/128 < 12` and `TargetY/128 > 25` then `ApplyWeather = 1` and weather is zeroed (`:520-530`). This is an undocumented "no weather" zone (a cave or indoor area), and it is expressed in map-block coordinates rather than any named constant. It means the same skill against the same target does different damage depending on which block the attacker stands in.

Defence application is asymmetric in a way that matters for balance: player victims multiply `Ac` by 2 and Foema victims (`MOB.Class == 1`) multiply by 1.5, so a Foema takes *more* damage than a player of another class with equal AC. Resistance then multiplies by `(150 - MobResist)/100` where `MobResist` is halved for mob victims (`idx > MAX_USER` in InstanceType 1, `idx >= MAX_USER` in 2–5 — note the inconsistent comparison operators, so index exactly `MAX_USER` is treated as a mob in one branch and a player in the other). With resistance values above 50 the multiplier exceeds 1.0, i.e. resistance becomes a *vulnerability amplifier* — a stat inversion that is either an intentional high-risk/high-reward mechanic or a sign error in the original source. `CMob::GetCurrentScore` clamps mob resistances to 50 (`Code/TMSrv/CMob.cpp:641-651`) precisely to prevent that, but the clamp is applied only when `GeneIdx` is valid, so mobs with `GenerateIndex == 0` can carry unclamped values.

**Rule workflow:**
```
if InstanceType in [1,5]:
    weather = CurrentWeather
    if (TargetX/128) < 12 and (TargetY/128) > 25 -> weather = 0
    if <fairy override BR-32> else dam = BASE_GetSkillDamage(skill, MOB, weather, WeaponDamage)
    def = victim.Ac
    if victim is player      -> def *= 2
    if victim.Class == 1     -> def = def*3/2
    dam = BASE_GetSkillDamage(dam, def, master)
    if InstanceType == 1:
        r = victim.Resist[0] ; if mob victim r /= 2
        dam = ((150 - r) * dam) / 100
    else if InstanceType in [2,5]:
        r = victim.Resist[InstanceType-2] ; if mob victim r /= 2
        dam = ((150 - r) * dam) / 100
```

---

### Business Rule: BR-32 — Fairy Companion Skill Damage

**Covers:** BR-32
**Location:** `Code/TMSrv/_MSG_Attack.cpp:532-554`

**Overview:**
Skills 32, 34 and 36, when executed with `Motion == 254` on a `SKIPCHECKTICK` packet, bypass the normal damage formula and compute a fixed value from the companion's sanctification and the owner's level, capped at 1000.

**Detailed description:**

The branch requires the exotic combination `ClientTick == SKIPCHECKTICK && m->Motion == 254 && skill in {32,34,36}`. Since `ProcessClientMessage` blocks client packets carrying the sentinel, this is reachable only for server-injected packets — the fairy AI in `Server.cpp` — but the fairy AI's synthesised packets are built with `ClientTick = CurrentTime` (`Code/TMSrv/GetFunc.cpp:1281`, `:1568`), not the sentinel. The branch is therefore likely reachable only through a path this analysis could not identify with confidence; it may be effectively dead. This is recorded as an ambiguity with medium confidence.

The formula is `familiarsanc × {200, 300, 350} + Level × 8` for skills 32, 34 and 36 respectively, where `familiarsanc = BASE_GetItemSanc(&pMob[conn].MOB.Equip[13])` — the sanctification of the companion equipment slot (slot 13 is the "fairy/pet" slot throughout the project, see also BR-37 and BR-74). `BASE_GetItemSanc` (`Code/Basedef.cpp:2095-2141`) extracts the first effect in 116–125 or `EF_SANC`, then folds high sanctification codes 230–253 into the 10–15 "refined" range and otherwise takes the last decimal digit. The result is a small integer, typically 0–15.

The consequence is a damage floor-plus-level term with a hard 1000 cap: at maximum sanctification (15) skill 36 yields `15 × 350 + Level × 8 = 5250 + 8×Level`, immediately clipped to 1000 for every character above level 60. The formula is therefore almost entirely capped and behaves as a flat 1000 for high-level companions and as `sanc×350` for low-level ones. There is no armour reduction on this path — the value is assigned directly to `dam` and skips `BASE_GetDamage` because the `else` at `:556` is what would have called the standard formula — so fairy damage is unmitigated, which is why the cap exists. Note also that this branch lives inside the `InstanceType 1..5` block, so it applies only if skills 32/34/36 are configured as elemental skills in `SkillData.bin`; if they are not, the branch is never entered at all.

**Rule workflow:**
```
if InstanceType in [1,5]:
    ... weather ...
    if tick == SKIP and Motion == 254 and skill in {32,34,36}:
        sanc = BASE_GetItemSanc(Equip[13])
        switch skill:
            32 -> dam = sanc*200 + Level*8
            34 -> dam = sanc*300 + Level*8
            36 -> dam = sanc*350 + Level*8
        if dam > 1000 -> dam = 1000
    else:
        dam = BASE_GetSkillDamage(...)
```

---

### Business Rule: BR-33 — Arrow Storm (Skill 79) Special Path

**Covers:** BR-33
**Location:** `Code/TMSrv/_MSG_Attack.cpp:286-293`, `:592-604`, `:1405-1406`

**Overview:**
Skill 79 is a physical-multiplier area skill with three special behaviours: it replicates target 0's damage and identity across the first six slots before the loop, it recomputes damage from the attacker's physical `Damage` stat with ×3 armour for players and then halves it, and its parry rate is reduced to 30%.

**Detailed description:**

The pre-loop replication at `:286-293` copies `Dam[0].Damage` and `Dam[0].TargetID` into slots 1–5, so up to six mobs receive identical damage to the primary target. This is how arrow storm achieves area damage without a real area-of-effect calculation: the client sends one resolved slot and the server fans it out. Because it copies the *sentinel* (`-2` or `-1`) rather than a resolved number, each replicated slot is then independently resolved in the loop — meaning the damage is not actually identical per target unless their armour and resistance are equal. The fan-out only reaches six of the thirteen slots, a hard-coded limit with no named constant.

The damage path at `:592-604` re-derives damage from scratch even though an earlier branch may have run: `dam = BASE_GetSkillDamage(skill, &MOB, CurrentWeather, WeaponDamage)`, and because `BASE_GetSkillDamage` special-cases skill 79 with `dam = mob->CurrentScore.Damage` (`Code/Basedef.cpp:6438-6439`), the effective base is the attacker's physical damage stat. Armour is then `Ac`, multiplied by 3 for players, reduced through `BASE_GetDamage`, and finally halved. The halving is what makes the six-target fan-out balanced relative to a single-target attack.

The parry interaction at `:1405-1406` is the third special case: `if (skillnum == 79 || skillnum == 22) parryretn = 30 * parryretn / 100;`. Arrow storm and exterminate are the only two skills whose parry chance is reduced to 30% of normal. Since the parry roll is the only true "miss" mechanism between players, this makes both skills markedly more reliable than any other, in addition to their damage properties. Both skills are also the two that gated the attack into the "any target" mode, and both appear in the NOPK-free list — they are consistently treated as privileged in every rule that mentions them.

**Rule workflow:**
```
before loop:
    if skill == 79: for x in [1,6): Dam[x] = Dam[0]
in loop (dam == -1, skill == 79):
    dam = BASE_GetSkillDamage(79, MOB, CurrentWeather, WeaponDamage)
        -> resolves to MOB.CurrentScore.Damage
    Ac = victim.Ac ; if player -> Ac *= 3
    dam = BASE_GetDamage(dam, Ac, master) / 2
later:
    parryretn = 30% of GetParryRate(...)
```

---

### Business Rule: BR-34..BR-37 — Healing, Recovery and Heal-Derived Experience

**Covers:** BR-34, BR-35, BR-36, BR-37
**Location:** `Code/TMSrv/_MSG_Attack.cpp:607-713`

**Overview:**
`InstanceType == 6` is not a damage skill at all: it heals. The heal amount is derived from `Special`, doubled for celestial tiers, capped at 1100 or 510, floored at 6, divided by the recipient's fairy accessory, applied as *negative damage* to HP, clamped to MaxHp — and it awards the healer a small amount of experience when performed outside a village.

**Detailed description:**

The heal magnitude uses two formulas: skill 27 gets `Special×2 + InstanceValue`, every other heal skill gets `Special×3/2 + InstanceValue`. The celestial-tier branch at `:629-641` is written with a doubled negative condition (`ClassMaster != MORTAL && != ARCH`) tested twice, once to double the value and once to select the 1100 cap instead of 510. `MORTAL` is 2 and `ARCH` is 1 (`Code/Basedef.h:178-179`), so every non-mortal, non-arch class master — `CELESTIAL` (3), `CELESTIALCS` (4), `SCELESTIAL` (5) — doubles its heal and uses the higher cap. Duplicating the condition rather than using an `else` is redundant but harmless. The value is then negated (`dam = -dam`) because the downstream HP-application code performs subtraction unconditionally, and floored at `-6` so that no heal is ever smaller than 6 HP — a floor applied as `if (dam < 0 && dam > -6) dam = -6`, which correctly catches only the small-negative window.

The immunity rule at `:610-611` skips Clan 4 recipients entirely. Clan 4 is the summon/pet clan throughout the project (see `AddEnemyList`'s `MOB.Clan == 4` handling and `LinkMountHp`'s guards), so pets cannot be healed by player heal skills. Then the map-attribute rule at `:613-621` refuses to heal *into* a `0x40` cell from a non-`0x40` cell — the inverse of the PvP gate, and a rule that exists so that a safe-zone healer cannot reach into a PvP zone to sustain a fighter. Failure here zeroes the slot with `TargetID = MOB_EMPTY` (0) rather than 0-and-continue, which is functionally the same.

HP application is filtered through the fairy accessory in slot 13 with three cases: index 786 divides the heal by `sanc` (minimum 2), index 1936 by `sanc × 10` then by another 10 (`sanc × 100` effectively), and index 1937 by `sanc × 1000` then by 20. The three cases are structurally identical to the damage-side accessory rules at BR-74 but use different divisors — the damage side divides by `sanc`, `sanc×10` and `sanc×1000` respectively, while the heal side divides by `sanc`, `sanc×100` and `sanc×20000`. Whether this is intentional asymmetry or decompilation drift cannot be determined; it is documented as observed. Since these divisors reduce or eliminate healing, high-sanctification accessories effectively make the wearer unhealable — a severe, non-obvious interaction.

The experience rule at `:693-711` computes `calc_exp = (newHp - oldHp) >> 3`, clamps to `[0, 120]`, requires `conn != idx`, the caster to be a player, and `BASE_GetVillage(position)` to be outside all five city rectangles (`village == 5`; the `village < 0` half of the guard is unreachable since `BASE_GetVillage` returns 0..5). The maximum survives into `UNK2` and is later used as the base for the packet's exp figure (BR-79). Note that `UNK2` is reused as a boolean by the buff block at BR-51, so a successful buff can set the exp bonus to exactly 1.

**Rule workflow:**
```
if InstanceType == 6:
    if victim.Clan == 4                                -> continue
    if attacker cell lacks 0x40 and victim cell has 0x40 -> zero slot; continue
    dam = (skill == 27) ? Special*2 + InstanceValue
                        : (Special*3)/2 + InstanceValue
    if ClassMaster not in {MORTAL, ARCH}:
        dam *= 2 ; if dam >= 1090 -> dam = 1100
    else:
        if dam >= 510 -> dam = 510
    dam = -dam ; if dam < 0 and dam > -6 -> dam = -6
    switch victim.Equip[13].sIndex:
        786  -> Hp -= dam / max(sanc,2)
        1936 -> Hp -= dam / (max(sanc,2)*10*10)
        1937 -> Hp -= dam / (max(sanc,2)*1000*20)
        else -> Hp -= dam
    if Hp > MaxHp -> Hp = MaxHp
    if victim is player -> SetReqHp(idx)
    gain = (Hp_after - Hp_before) >> 3
    clamp gain to [0,120]
    if gain > 0 and conn != idx and caster is player:
        if BASE_GetVillage(victim) not in [0,5) -> UNK2 = max(UNK2, gain)
```

---

### Business Rule: BR-38..BR-43 — Utility Skill Behaviours (Flash, Detox, Teleport, Invisibility, Evoke, Ethereal Flames)

**Covers:** BR-38, BR-39, BR-40, BR-41, BR-42, BR-43
**Location:** `Code/TMSrv/_MSG_Attack.cpp:716-884`

**Overview:**
Six `InstanceType` values implement non-damage utility effects: forced peace (7), affect cleansing (8), summon/teleport (9), aggro redirection (10), pet summoning (11) and mana burn with affect stripping (12). Each is a self-contained block with its own preconditions and its own termination behaviour.

**Detailed description:**

`InstanceType 7` (flash) is the simplest: it sets `pMob[idx].Mode = MOB_PEACE`, clears `CurrentTarget`, and zeroes every entry of `EnemyList` (`:716-723`). It works on any entity, players included, and does not check any resistance or level — a strictly superior "reset aggro" with no counterplay. Because `MOB_PEACE` is the state the AI checks in `CMob::BattleProcessor` (`Code/TMSrv/CMob.cpp:213-218`) to decide there is no fight, this is a hard state override on a target that may be an active player. There is no `continue` or `break`, so the block falls through into the buff region and beyond.

`InstanceType 8` (detox) walks all 32 affect slots and clears types 1, 3, 5, 7, 10, 12, 20 — and type 32 only if the *caster* has learned bit `0x80`. The condition is written as `(Type == 32 && (1 << 7) & pMob[conn].MOB.LearnedSkill)` inside a large `||` chain (`:735`), which parses as `Type==32 && (0x80 & LearnedSkill)`; operator precedence is correct but the mixed `&&`/`||` without parentheses is a readability hazard. If anything was cleared, the victim's score is recomputed and resent (`:742-746`). This is the only cleansing behaviour in the Attack Handler; the `_MSG_UseItem` handler has its own potion-based equivalents.

`InstanceType 9` (teleport/summon) is the most rule-dense utility block (`:751-786`) and has four sequential gates plus one early `break`. A dead target produces the `_NN_Cant_Summon_Dead_Person` message and a `break` — note the `break` leaves the target loop entirely, so remaining slots are silently discarded, which is inconsistent with every other rejection that uses `continue`. A target cell carrying the `0x04` attribute with caster level below 1000 produces `_NN_Summon_Not_Allowed_Here`. A target whose HP exceeds the caster's HP plus `Special` plus 30 produces `_NN_Too_High_Level_To_Summon` — an HP-based rather than level-based power gate, which allows a low-HP high-level caster to summon nothing while a high-HP low-level one summons freely. A target whose coordinates both have their high byte zero (`(TargetX & 0xFF00) == 0 && (TargetY & 0xFF00) == 0`) is treated as an invalid map and returns from the whole function. Success calls `DoSummon(idx, attacker.TargetX, TargetY)` and messages the victim with `_SN_Summoned_By_s`.

`InstanceType 10` (invisibility) iterates all mob slots from `MAX_USER` to `MAX_MOB` looking for mobs in `MOB_COMBAT` whose `CurrentTarget == idx`, and rewrites both the enemy-list entries and the current target to point at the caster (`:790-805`). This is a silent aggro transfer, and it is the mechanism by which an invisibility skill protects its caster's ally by redirecting mob attention. It only considers non-player entities and only combat-state ones, so a mob in another state that has the target in its enemy list is ignored.

`InstanceType 11` (evoke) computes summon count from `CurrentScore.Special[2]` divided by 30, 40 or 80 for `InstanceValue` 1–2, 3–5 and 6–7 respectively, or exactly 1 for `InstanceValue == 8`, then calls `GenerateSummon(conn, InstanceValue - 1, 0, summons)`. On failure the previously captured `Mp` and `ReqMp` are restored (`:832-833`), which is the component's only mana refund path and confirms the developers' awareness of the two-field mana model. `dam` is forced to 0 at `:836` so no damage is applied. Note `InstanceValue` values outside 1–50 skip the whole block and still leave `dam = 0`, so a misconfigured evoke deals no damage and summons nothing.

`InstanceType 12` (ethereal flames) has a player-only body (`:848`) and therefore does nothing at all against mobs — the `else` is absent, so mob targets fall through with whatever `dam` the earlier branches left. Against players it rolls `Rand = rand() % 100` and compares against `chance = (BaseScore.Special[1] + 1) / 7` — a low chance (roughly 0–6%) to *resist* the burn, given the inverted comparison `if (Rand > chance)` enters the burn path. The burn removes `((Mp+1)/100) * (10 + rand()%10)` — 10–19% of the victim's MP — clamps at zero, then syncs `ReqMp` and resends HP/MP and score. If the resistance roll succeeds instead, the handler strips affects 14, 16, 18 and 19 and recomputes the victim's score, which means the *resisted* outcome is the one that removes the victim's buffs. That inversion is almost certainly a defect: the branch that reads as "resisted" applies the harsher effect.

**Rule workflow:**
```
InstanceType 7:  Mode = MOB_PEACE; CurrentTarget = 0; EnemyList[*] = 0
InstanceType 8:  for each Affect: if Type in {1,3,5,7,10,12,20} or
                                  (Type==32 and LearnedSkill & 0x80) -> clear
                 if any cleared -> GetCurrentScore; SendScore
InstanceType 9:  dead victim                  -> msg; BREAK (all slots)
                 0x04 cell and level < 1000   -> msg; continue
                 victim.Hp > caster.Hp + Special + 30 -> msg; continue
                 (X&0xFF00)==0 and (Y&0xFF00)==0 -> msg; RETURN
                 DoSummon(victim, caster.TargetX/Y); msg victim
InstanceType 10: for mob in [MAX_USER, MAX_MOB):
                     if Mode == MOB_COMBAT and CurrentTarget == idx:
                         EnemyList[*] = conn ; CurrentTarget = conn
InstanceType 11: summons = f(InstanceValue)
                 if GenerateSummon(conn, InstanceValue-1, 0, summons) == 0
                     -> restore Mp and ReqMp
                 dam = 0
InstanceType 12: player only
                 Rand = rand()%100 ; chance = (Special[1]+1)/7
                 if Rand > chance:  burn 10..19% MP; sync; continue
                 else:              clear affects 14,16,18,19; resync; continue
```

---

### Business Rule: BR-44 — Divine Fury Knockback (Skill 6)

**Covers:** BR-44
**Location:** `Code/TMSrv/_MSG_Attack.cpp:888-966`

**Overview:**
Skill 6 displaces the target one cell away from the caster on a probabilistic basis. Four victim classes are immune, the chance is derived from the caster's `Special[1]` plus a level-difference term and capped at 50%, and success emits action packets and, for mobs, engages combat.

**Detailed description:**

The immunity list at `:890-900` is the most specific in the component: mobs with a non-zero `Merchant` field, any entity wearing items 219, 220 or 362 in slot 0, entities with `GenerateIndex` 8 or 9, and any entity in Clan 6. The three item indices overlap partially with BR-15's protective pair (219/220) and add 362, suggesting a family of "immovable object" markers. Clan 6 immunity appears again at BR-65 and in the invisible-clan check at `:1188`, marking Clan 6 as a privileged/administrative clan. Each immunity exits via `break`, terminating the whole target loop — so a single immune target in slot 0 prevents knockback on every later slot.

The chance formula at `:923-940` is complex enough to warrant restating: `kindvalue = (Special[1] / 10) + 20` for player victims or `(Special[1] / 5) + 40` for mobs; `leveldif` is computed with a branching expression that mixes mortal/arch and celestial level scales (`:930`), then `>> 2`; `_chance = kindvalue + leveldif`, capped at 50; and the roll succeeds when `rand() % 100 < _chance`. The expression at `:930` is highly suspicious: it reads

`ClassMaster != MORTAL && != ARCH ? attacker.Level : attacker.Level + MAX_LEVEL - (victim.ClassMaster != MORTAL && != ARCH ? victim.Level + MAX_LEVEL : victim.Level)`

so for a celestial attacker the ternary yields `attacker.Level` alone with no victim term, while for a mortal/arch attacker it yields `attacker.Level + MAX_LEVEL - victim.Level`. The two branches are not symmetric and the celestial case discards the level comparison entirely. Combined with the hard 50% cap, the practical effect is a near-constant 50% knockback chance for any caster with `Special[1] >= 300`, regardless of target.

Success emits a `MSG_Action` with `Effect = 2`, `Speed = 6` via `GridMulticast`, sends the same packet to the victim's own socket if it is a player (duplicating the broadcast), and calls `SetBattle(idx, conn)` for mob victims only (`:962-963`). The knockback destination is computed by stepping one cell toward the caster in each axis and then validating with `GetEmptyMobGrid`, which searches outward in rings up to radius 4 (`Code/TMSrv/GetFunc.cpp:1901-2000`). If no free cell exists the rule aborts. The caster's own `Route[0]` is zeroed before the calculation (`:902`) for reasons not evident from the code.

**Rule workflow:**
```
if skill == 6:
    if mob victim with Merchant != 0            -> break
    if Equip[0].sIndex in {219,220,362}         -> break
    if GenerateIndex in {8,9}                   -> break
    if victim.Clan == 6                         -> break
    pMob[idx].Route[0] = 0
    step X/Y one cell toward caster
    if GetEmptyMobGrid(idx, &X, &Y) == FALSE    -> break
    kindvalue = (Special[1]/10 + 20) or (Special[1]/5 + 40) for mobs
    leveldif  = <scale-dependent expression> >> 2
    chance    = min(kindvalue + leveldif, 50)
    if rand()%100 >= chance                     -> break
    emit MSG_Action(Effect=2, Speed=6) to grid and to victim socket
    if victim is mob -> SetBattle(victim, conn)
    break
```

---

### Business Rule: BR-45, BR-46, BR-47 — Exterminate, Divine Judgement, Rebirth

**Covers:** BR-45, BR-46, BR-47
**Location:** `Code/TMSrv/_MSG_Attack.cpp:969-1048`

**Overview:**
Three skills convert caster resources into damage or revival. Skill 22 expends the entire MP pool, skill 30 expends most of the caster's HP, and skill 31 resurrects the target at 10–19% MaxHP with a 70% success chance.

**Detailed description:**

Exterminate (22) sets `CurrentScore.Mp = 0`, `pUser[conn].ReqMp = 0` and `m->CurrentMp = 0` (`:972-977`), then adds the *captured* `CurrentMp` and half the caster's `Int` to the already-computed `dam` at `:982`. Because the MP is zeroed before the packet broadcast, observers see an empty bar, and because the addition happens after the elemental/utility branches, the skill's own elemental damage (if it has an `InstanceType`) is preserved and supplemented. The block also emits two `MSG_Action` packets with `Type = 2, Speed = 2` — one for the victim and one for the caster — provided the caster has an empty adjacent grid and the victim is not wearing 219/220. That item check duplicates BR-15/BR-44's protective pair for a third time. Note that `kind4` is read at `:979` and never used: a dead local.

Divine Judgement (30) is the only skill that damages the caster as well. It adds the caster's current HP (captured at `:281` before any computation) to `dam`, then sets `pMob[conn].MOB.CurrentScore.Hp = (HP / 6) + 1` and syncs `ReqHp` (`:1017-1020`). The consequence is that the caster always survives with at least 1 HP, and the skill becomes proportionally stronger the healthier the caster is — a "sacrifice the body, keep the damage" mechanic. `SendSetHpMp(conn)` is additionally called after the broadcast at `:1736-1737`, which is required because the score packet was already queued with the pre-sacrifice HP.

Rebirth (31) resurrects, but only on `rand() % 100 < 70`. It computes `hp = (rand() % 10 + 10) × ((MaxHp + 1) / 100)` — 10–19% of the *victim's* MaxHP — zeroes the caster's MP and `ReqMp`, syncs the caster's HP/MP, and on success assigns the HP, clears the victim's crack counter (`NumError = 0`), resends score, HP/MP and misc state, and broadcasts a fresh `MSG_CreateMob` so the victim reappears (`:1036-1047`). On failure the entire mana cost is spent and the victim stays dead — there is no refund path here, unlike evoke (BR-42). Clearing `NumError` on the victim is a security-relevant side effect: a player who has accumulated crack points can have them wiped by being revived by a Cleric, which offers a collusion route to launder accumulated anti-cheat penalties.

**Rule workflow:**
```
skill == 22:
    savedMp = CurrentScore.Mp ; CurrentScore.Mp = 0 ; ReqMp = 0 ; m->CurrentMp = 0
    dam = dam + savedMp + (Int / 2)
    emit reposition actions for victim and caster
skill == 30:
    dam = dam + casterHp_at_entry
    CurrentScore.Hp = CurrentScore.Hp/6 + 1 ; ReqHp = that
    (SendSetHpMp deferred to :1736)
skill == 31:
    hp = (rand%10 + 10) * ((victim.MaxHp + 1)/100)
    CurrentScore.Mp = 0 ; ReqMp = 0 ; m->CurrentMp = 0 ; SendSetHpMp(conn)
    if rand()%100 < 70:
        victim.Hp = hp ; victim.NumError = 0
        SendScore; SendSetHpMp; SendEtc; GridMulticast(MSG_CreateMob)
```

---

### Business Rule: BR-48..BR-50 — Speed, Magic Weapon, Cancel and Book Vine

**Covers:** BR-48, BR-49, BR-50
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1052-1155`

**Overview:**
Two buff skills (41, 44) fan their affect out to multiple players, one dispel (47) strips affect 19, and one summoning book (98) spawns a boss mob at the aim point.

**Detailed description:**

Skills 41 and 44 share identical implementations, differing only in the skill number. Both compute `skill_target = (Special / 25) + 2`, clamp it to `[2, MAX_TARGET]` (the lower clamp is redundant because the formula already yields at least 2), and then re-walk the packet's own slot array with an elaborate compound loop condition that gates by packet size (`:1062`, `:1093`). Inside they accept only `targetidx in (MOB_EMPTY, MAX_USER)` — i.e. player indices — and only `pUser[targetidx].Mode == USER_PLAY`, then call `SetAffect(targetidx, skill, Delay, Level)`. Because the outer loop has already processed these slots and may have zeroed `TargetID` for invalid ones, the re-walk operates on a mutated array; and because the re-walk can touch slots the outer loop has not yet reached, a slot can receive the affect twice (once here, once when its own iteration runs the buff block at BR-51). Duplicate `SetAffect` is idempotent for time and level (the function keeps the larger) but calls `GetCurrentScore` twice, which is wasteful rather than incorrect.

`SetAffect` (`Code/TMSrv/Server.cpp:8270-8315`) has its own preconditions that override the caller's: it refuses if the target is a merchant (`MOB.Merchant == 1`) or has an index above `MAX_USER` — note the unusual `conn > MAX_USER` (strictly greater) which allows index exactly `MAX_USER` through — and refuses any affect on a target whose `MOB.Rsv & RSV_BLOCK` is set while the skill is aggressive. It selects an empty affect slot via `GetEmptyAffect(type)`, shortens duration to 4 ticks for types 1/3/10, and preserves the greater of old/new time and level when the type matches. `SetTick` (`:8317-8351`) mirrors it for damage-over-time ticks with a separate 500,000,000 time cap and a 2-tick cap for types 1/3/10. Neither function validates the *caster*.

The speed buffs both `break` out of the target loop after their re-walk, so any remaining slots are discarded. That means a speed buff cast as part of an area attack terminates the attack after the first slot — a behavioural limitation that would matter for any future multi-effect attack.

Skill 47 (cancel) walks the victim's 32 affect slots looking for type 19, clears type/value/level/time in place, recomputes and resends the victim's score, and `break`s out of the target loop via the `CancelContinue` flag (`:1115-1136`). It therefore cancels at most one buff on at most one target per cast, and terminates the whole attack on success.

Skill 98 (book vine) is unusual in using the packet's *aim* coordinates rather than a target: it bounds `m->TargetX/Y` to `[0, MAX_GRIDX/Y)`, reads the `0x40` attribute, applies the same non-combat-to-combat refusal as BR-35 (`if map_att == 0 && getAttribute == 1) continue;` — note the comparison is against the literal 1 rather than being a truthiness test, which happens to work because `getAttribute` was masked to `0x40` and `0x40 & 0x40 == 0x40`, not 1; the branch is therefore **never taken**, a latent defect), and then calls `CreateMob("Vinha", X, Y, "Boss", 3)`. The spawn is unconditional once the bounds check passes, which means the skill's only cost is its mana and its only limit is the map bounds.

**Rule workflow:**
```
skill in {41, 44}:
    n = clamp(Special/25 + 2, 2, MAX_TARGET)
    for q in [0, n) with size gating:
        t = Dam[q].TargetID
        if t <= 0 or t >= MAX_USER        -> continue
        if pUser[t].Mode != USER_PLAY     -> continue
        if SetAffect(t, skill, Delay, Level) -> SendScore(t)
    break
skill == 47:
    for k in Affect: if Type == 19 -> clear all fields; recompute; SendScore; break
    break
skill == 98:
    if aim outside grid                -> break
    attr = GetAttribute(aim) & 0x40
    if map_att == 0 and attr == 1      -> continue        [never true]
    CreateMob("Vinha", aimX, aimY, "Boss", 3)
    break
```

---

### Business Rule: BR-51, BR-52, BR-53 — Affect Application, Resistance Roll and Transformation

**Covers:** BR-51, BR-52, BR-53
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1159-1235`

**Overview:**
The handler decides per target whether to apply the skill's `AffectType`/`TickType`. For aggressive skills the affect is applied only to targets that are **not** party-mates and **not** guild-mates — i.e. debuffs land on enemies — subject to a resistance-style roll, an `RSV_BLOCK` veto and a Clan 6 veto. For non-aggressive (friendly) skills the affect is applied unconditionally to every valid target. Skill 102 overrides the delay schedule, and five transformation skills force a caster resync.

**Detailed description:**

The controlling variable is named `SameLeaderGuild`, which is misleading: it is initialised to 1 and cleared to 0 exactly when the target **is** a party-mate or guild-mate (`:1165-1166`). The affect block then runs `if (SameLeaderGuild)`, so the flag actually means "target should receive the affect". For an aggressive skill this correctly restricts debuffs to non-party, non-guild targets; for a non-aggressive skill the entire `if (Aggressive != 0)` block is skipped, the flag stays 1, and the affect is applied to everyone including party members. Two consequences follow that are easy to misread from the variable name: (a) a guildless caster buffing a guildless bystander is *not* treated as friendly, because the `Guild = -1` normalisation at `:383-384` makes both sides equal, so an aggressive skill will debuff a co-guildless player; (b) an aggressive skill that is also flagged `bParty` is self-contradictory — BR-29 requires the same leader/guild to pass, and BR-51 then requires the opposite to apply the affect, so such a skill deals damage to party-mates and applies no affect at all.

The resistance roll at `:1177-1183` is evaluated only for `AffectResist` values in 1..4 and is written `if (rand() % 100 > victim.RegenMP + AffectResist + difLevel) -> do NOT apply`. Because the threshold is the sum of the victim's resistance statistic and the skill's own resistance-penetration value, a victim with a high `RegenMP` is *less* likely to be skipped — the victim's resistance stat protects them by making the affect land. The sign is inverted relative to the intent implied by both field names. `difLevel` is `-(victimLevel − casterLevel)/2`, so a higher-level victim reduces the threshold and is therefore harder to affect, which is the one component of the formula that behaves as expected. In addition, `connlevel` at `:1171` tests `pMob[idx].extra.ClassMaster == ARCH` — the *victim's* class master — where it should test the attacker's, so the two level scales are computed with mismatched tier detection. The result is that the resistance system is at best unreliable and at worst actively inverted for high-resistance targets.

Two unconditional vetoes follow the roll: `pMob[idx].MOB.Rsv & 0x80` (`RSV_BLOCK`, `Code/Basedef.h:191`) suppresses application, and Clan 6 victims are suppressed when the caster is a player (`:1185-1189`). When the affect is applied, `SetAffect` and `SetTick` are called, `UNK2` is set to `TRUE` if either succeeded, and the victim's score is recomputed and resent. This is where `UNK2` — the heal-derived exp accumulator from BR-36 — is overloaded as a boolean, so any successful affect application contributes at least 1 to the packet's exp figure (BR-79). Because `SetAffect` and `SetTick` each call `GetCurrentScore` internally, a single application recomputes the victim's score up to three times.

Skill 102's delay override (`:1195-1213`) is the only place in the handler where a duration is computed from character state: mortals get 200; celestial tiers get 200 with a `-(39 - level) × 4` reduction below level 39 and a flat 400 at level 199 or above. For a level-1 celestial this yields `200 - 152 = 48`, and the jump to 400 at level 199 is a discontinuity rather than a curve. Skill 102 is also the only skill excluded from the grid broadcast at `:1733`, so its application is invisible to observers. The transformation block (`:1230-1235`) fires for skills 64, 66, 68, 70 and 71 and calls `GetCurrentScore(conn)`, `SendScore(conn)` and `SendEquip(conn, 0)`; it runs per target slot, so a transformation skill in a multi-target packet triggers the resync once per slot.

**Rule workflow:**
```
Aggressive = g_pSpell[skill].Aggressive
SameLeaderGuild = 1                       [means: apply the affect]
if Aggressive != 0:
    if leader == mobleader or Guild == MobGuild        -> SameLeaderGuild = 0
    ar = g_pSpell[skill].AffectResist
    difLevel = -((victimLevel_scaled - casterLevel_scaled) / 2)
    if ar in [1,4] and rand()%100 > victim.RegenMP + ar + difLevel
                                                       -> SameLeaderGuild = 0
    if victim.Rsv & 0x80                               -> SameLeaderGuild = 0
    if attacker is player and victim.Clan == 6         -> SameLeaderGuild = 0
if SameLeaderGuild:
    if skill == 102: compute Delay from ClassMaster and Level
    if SetAffect(idx, skill, Delay, Special) != 0      -> UNK2 = TRUE
    if SetTick  (idx, skill, Delay, Special) != 0      -> UNK2 = TRUE
    if UNK2: GetCurrentScore(idx); SendScore(idx)
if skill in {64,66,68,70,71}: GetCurrentScore(conn); SendScore(conn); SendEquip(conn,0)
```

---

### Business Rule: BR-54 — Resurrection Book

**Covers:** BR-54
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1238-1268`

**Overview:**
Skill 99 is a self-resurrection book usable only by a dead caster. It performs a probability roll with a recall side effect, then unconditionally restores the caster to 1–50% of MaxHP.

**Detailed description:**

The block is guarded by `pMob[conn].MOB.CurrentScore.Hp == 0 && skillnum == 99`, mirroring the BR-01 gate that lets a dead player reach this point. The roll is `rev = rand() % 115`, then `if (rev > 100) rev -= 15` — which maps 101–114 down to 86–99, so the effective distribution is 0–100 plus a duplicate band at 86–99. The threshold is `rev >= 40`, giving a success probability of roughly `(61 + 14)/116 ≈ 64.7%`. On success the caster is set to 2 HP, `NumError` is cleared, score and HP/MP are resent, `DoRecall(conn)` teleports them home, and `SendEtc` refreshes the misc state.

Regardless of the roll's outcome, the code then unconditionally sets `hp = (rand() % 50 + 1) × ((MaxHp + 1)/100)` and assigns it, clears `NumError` again, resends everything, and broadcasts a `MSG_CreateMob` (`:1256-1266`). This means the "failure" branch is not a failure at all: the caster is resurrected in both cases, and the only difference is whether the recall to town happened. The 2-HP intermediate value is overwritten within the same function, so its only purpose is to satisfy any code between the two assignments — there is none. The rule is therefore better described as "always resurrects, with a ~65% chance of also recalling home". The `NumError` clearing here duplicates BR-47's identical side effect and gives every player a free crack-point reset on death.

**Rule workflow:**
```
if caster.Hp == 0 and skill == 99:
    rev = rand() % 115 ; if rev > 100 -> rev -= 15
    if rev >= 40:
        Hp = 2 ; NumError = 0 ; SendScore ; SendSetHpMp ; DoRecall ; SendEtc
    hp = (rand()%50 + 1) * ((MaxHp + 1)/100)
    Hp = hp ; NumError = 0 ; SendScore ; SendSetHpMp ; SendEtc
    GridMulticast(MSG_CreateMob)
```

---

### Business Rule: BR-55..BR-59 — Post-Resolution Damage Modifiers

**Covers:** BR-55, BR-56, BR-57, BR-58, BR-59
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1279-1323`

**Overview:**
After the branch-specific damage is computed and written back into the packet, five modifiers run in fixed order: a 25% PvP reduction, `ForceDamage`, `PvPDamage`, friendly-fire zeroing, and fragment zeroing.

**Detailed description:**

The 25% PvP rule at `:1284-1292` applies to player victims **or** any Clan 4 mob (pets/summons are treated as players for damage purposes). With the Huntress flag bit 4 set, the value becomes `Dam[1].Damage + (dam >> 2)` — the extra hit is added in full while the primary component is quartered; otherwise damage becomes simply `dam >> 2`. Because `>> 2` floors, a hit of 3 becomes 0 at this stage and is then rescued by the `if (dam <= 0) continue;` immediately below, meaning small hits against players vanish entirely rather than being floored to 1. This is the primary reason PvP feels different from PvE: the same weapon that deals 100 to a mob deals 25 to a player, and anything under 4 deals nothing.

`ForceDamage` (`:1294-1303`) is the jewel/gem-derived flat bonus computed in `CMob::GetCurrentScore` (`Code/TMSrv/CMob.cpp:832`). The handler adds it to any positive damage, or — when damage is ≤ 1 — *replaces* the value with `ForceDamage`. That replacement is generous: a player whose attack was reduced to zero or one by armour or by the 25% rule receives the full `ForceDamage` instead, and `ForceDamage` can reach `80 × isanc` per gem. Against players the ordering means `ForceDamage` is applied *after* the 25% cut, so it is not itself quartered — a flat-damage bypass of the PvP reduction. `PvPDamage` (`:1307-1316`) then adds a percentage for player victims only, using two different formulas depending on whether damage is ≤ 1 or positive — `dam += dam * PvPDamage/100` versus `dam += dam/100 * PvPDamage`. These differ only in integer-division order and produce results that can differ by 1. `PvPDamage` is `(BASE_GetMobAbility(EF_HWORDGUILD) + 1)/10` (`CMob.cpp:844-848`), i.e. a tenth of the "high word guild" ability.

Friendly-fire suppression at `:1319-1323` is the enforcement complement of the affect-direction logic in BR-51: `if (leader == mobleader || Guild == MobGuild) dam = 0;`. Note it does not check whether the target was a player, so a guild-mate's pet with the same leader also becomes immune — which is desirable. The `isFrag` rule immediately after zeroes damage from clan-7-vs-7 or clan-8-vs-8 attackers against *mobs only*; the same-clan case for players is already handled by the guild/leader rule. `isFrag` is computed once per slot at `:392-395`, and clan 7/8 are the two "faction" clans (`REINO_BLUE`/`REINO_RED`, `Code/Basedef.h:175-176`), so this rule prevents same-faction players from farming mobs while the opposing factions can.

**Rule workflow:**
```
Dam[i].Damage = (int)dam
if dam <= 0 -> continue
if victim is player or victim.Clan == 4:
    if (m->DoubleCritical & 4): dam = Dam[1].Damage + (dam >> 2)
    else:                       dam = dam >> 2
if caster.ForceDamage != 0:
    dam = (dam <= 1) ? ForceDamage : dam + ForceDamage
    Dam[i].Damage = dam
if caster.PvPDamage != 0 and victim is player:
    dam += (dam <= 1) ? dam * PvPDamage / 100 : dam / 100 * PvPDamage
    Dam[i].Damage = dam
if leader == mobleader or Guild == MobGuild -> dam = 0
if victim is mob and isFrag               -> dam = 0
```

---

### Business Rule: BR-60..BR-63 — PK Legality, Guilt, War Detection and Parry

**Covers:** BR-60, BR-61, BR-62, BR-63
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1325-1418`

**Overview:**
This is the component's PvP adjudication core. It resolves the effective "summoner" of the victim, determines whether the location is a safe zone, a war zone, the free-PK map or a guild arena, implements the "innocent cannot be killed" rule, marks both parties guilty when a guilty player is attacked, and rolls parry/block to convert a hit into a miss.

**Detailed description:**

The summoner resolution at `:1325-1328` promotes a Clan 4 pet to its owning player when the owner is online and playing, so all subsequent PvP logic treats the pet as its owner. This is the correct design — otherwise attacking a pet would be a guilt-free way to damage a player's assets — but it also means a pet whose `Summoner` is offline or not `USER_PLAY` remains a mob for PK purposes and can be freely destroyed.

Location classification then computes four independent facts: `pointPK = GetPKPoint(conn)` and `SummonerPointPK = GetPKPoint(Summoner)`; `arena = BASE_GetArena(position)` which returns 0–4 for the five guild-war rectangles or 5 when outside all of them; `village = BASE_GetVillage(position)` similarly 0–4 or 5; `mapX/mapY = TargetX>>7 / TargetY>>7` (map blocks); `isWar` set by mutual `g_pGuildWar` entries or by `CastleState != 0` with `mapX == 8 && mapY == 13`; and `MapPK` set for `mapX == 1 && mapY == 31`. The "safe zone" test is `arena == 5 && village == 5 && MapPK == FALSE && isWar == FALSE`, i.e. the attacker must be outside every arena, outside every city, off the free-PK map and not at war. Only then do the PK rules apply. Note that the coordinates used are the attacker's aim point, not the victim's position or the attacker's body — a player standing in a city but aiming at a cell outside it is treated as outside.

Inside the safe-zone branch, the "cannot kill the innocent" rule at `:1362-1367` triggers when the attacker has `pointPK <= 10` and the victim has `SummonerPointPK > 10`: the client receives `_DN_CantKillUser` (a message taking the PK point as a format argument) and damage is zeroed. This encodes the game's core anti-griefing invariant — a player who has not accumulated PK points (i.e. is "clean") may not damage a player who has — and it is checked *per target slot*, so in an area attack some slots may be zeroed while others proceed. The complementary branch at `:1368-1389` handles the case where the *attacker* is the one with PK points: any positive damage sets both the attacker's and the victim's guilt to 8 via `SetGuilty`, and if either party previously had guilt 0, a fresh `MSG_CreateMob` is broadcast for them so observers re-render the "guilty" visual. Setting the *victim's* guilt is the contentious part: it means being attacked by a PK-er makes you guilty, which in turn makes you attackable by clean players under the previous rule. Whether this is intended escalation or an inverted assignment cannot be determined from source; it is documented with high confidence as code behaviour and low confidence as design intent.

Parry is the component's only miss mechanic (`:1393-1417`). `attackdex = Dex/5`, plus 100 when learned bit `0x1000000` is present, plus 500 when `Rsv & 0x40` (fast-cast) is present. `GetParryRate` (`Code/TMSrv/GetFunc.cpp:562-608`) clamps the additive term to `[0,100]`, caps the victim's Dex at 1000, then computes `targetDex/2 + add + (Dex-1000)/4 (capped 2000) + (Dex-3000)/8 − attackerdex`, plus 100 for attacker `Rsv & 0x20`, 50 for `Rsv & 0x80`, 50 for `Rsv & 0x200`, clamped to `[1, 650]`. The caller then multiplies by 30% for skills 79 and 22 and rolls `rand() % 1000 + 1`. A result strictly below the parry rate converts damage to `-3`; if the victim also has `Rsv & 0x200` and the roll was under 100, damage becomes `-4`. These two negative codes are the wire protocol's miss indicators and are the only negative values the handler ever writes into `Dam[i].Damage` during resolution — every other rejection path writes 0. Because the cap is 650 and the roll is out of 1000, the maximum parry chance is 65%, and the minimum is 0.1%.

**Rule workflow:**
```
if victim is Clan 4 pet with online player owner -> Summoner = owner
if Summoner < MAX_USER:
    pointPK = GetPKPoint(conn) ; spPK = GetPKPoint(Summoner)
    arena = BASE_GetArena(aim) ; village = BASE_GetVillage(aim)
    mapX = aimX >> 7 ; mapY = aimY >> 7
    isWar = mutual g_pGuildWar, or CastleState and (8,13)
    MapPK = (mapX == 1 and mapY == 31)
    if arena == 5 and village == 5 and !MapPK and !isWar:
        if pointPK <= 10 and spPK > 10:
            msg(_DN_CantKillUser, pointPK) ; dam = 0
        else if spPK > 10 and dam > 0:
            gA = GetGuilty(conn) ; SetGuilty(conn, 8)
            gV = GetGuilty(idx)  ; SetGuilty(idx, 8)
            if gA == 0 -> broadcast MSG_CreateMob(conn)
            if gV == 0 -> broadcast MSG_CreateMob(idx)
    if idx != conn:
        attackdex = Dex/5
        if LearnedSkill & 0x1000000 -> attackdex += 100
        if Rsv & 0x40              -> attackdex += 500
        pr = GetParryRate(victim, victim.Parry, attackdex, attacker.Rsv)
        if skill in {79,22} -> pr = 30*pr/100
        rd = rand()%1000 + 1
        if rd < pr: dam = -3 ; if (victim.Rsv & 0x200) and rd < 100 -> dam = -4
```

---

### Business Rule: BR-64, BR-65 — Map Attribute and Clan Immunity Gate

**Covers:** BR-64, BR-65
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1421-1437`

**Overview:**
For player victims while the attacker is at or below `MAX_LEVEL`, damage requires both the victim's cell and the attacker's aim cell to carry attribute bits `0x01` and `0x40`. Independently, a Clan 6 victim is immune to any attacker below `MAX_LEVEL`.

**Detailed description:**

The attribute rule reads
`if ((TargetMapAttribute & 0x1) == 0 || (TargetMapAttribute & 64) == 0) dam = 0;` and the same for the attacker's cell. As written, damage survives only if **both** bits are set on **both** cells. The strict application of this rule means every PvP kill requires four conditions on map data, and any cell missing `0x01` is a complete PvP-immunity cell. The equivalent logic in the mob-side path, `Code/TMSrv/GetFunc.cpp:1527-1530`, reads `if (GetAttribute(...) & 1 || !(GetAttribute(...) & 0x40)) fisdam = 0;` — i.e. damage is zeroed when bit `0x01` **is set** or `0x40` is **not set**. The two predicates are direct negations of each other with respect to `0x01`. They cannot both be right. Since the Attack Handler is the authoritative path for player attacks and `GetAttack`/`GetAttackArea` are the mob-side projections, this is a genuine inconsistency between two implementations of the same policy, and it makes the meaning of bit `0x01` undeterminable from source alone. The attribute array itself (`g_pAttribute`, `GetAttribute` at `Code/TMSrv/GetFunc.cpp:1257-1273`) is loaded from map data with no accompanying semantics; only bit `0x02` has an observable meaning elsewhere (`BASE_ApplyAttribute` uses it to mark impassable height, `Code/Basedef.cpp:2601-2602`) and bit `0x04` blocks teleport for low-level characters (BR-40).

The `mylevel <= MAX_LEVEL` guard means the rule applies to every normal character (levels 0–399 are `<= 399`) but not to celestial-tier characters whose level is expressed above `MAX_LEVEL`. In other words, high-tier characters bypass the map-attribute PvP gate entirely — they can damage anyone anywhere — which is a significant and probably unintended consequence of using level comparison as a class-tier proxy.

Clan 6 is separately special-cased at `:1436-1437`: `if (pMob[idx].MOB.Clan == 6 && Level < MAX_LEVEL) dam = 0;`. Clan 6 appears in five distinct places across the component (BR-44 immunity, BR-51 hostility, BR-65 immunity, `:899`, `:1188`) and is consistently treated as untouchable. Combined with the level guard, only a celestial-tier attacker of sufficient level can damage a Clan 6 entity. From the code alone Clan 6 is best inferred to be an administrative/event clan; nothing declares it.

**Rule workflow:**
```
if attacker level <= MAX_LEVEL and victim is player:
    tatt = GetAttribute(victim.X, victim.Y)
    aatt = GetAttribute(attacker.TargetX, attacker.TargetY)
    if (tatt & 0x01) == 0 or (tatt & 0x40) == 0 -> dam = 0
    if (aatt & 0x01) == 0 or (aatt & 0x40) == 0 -> dam = 0
if victim.Clan == 6 and Level < MAX_LEVEL -> dam = 0
if victim is mob and dam >= 1 -> dam += caster.ForceMobDamage
if dam >= MAX_DAMAGE -> dam = MAX_DAMAGE
Dam[i].Damage = dam
if dam <= 0 -> continue
```

---

### Business Rule: BR-66, BR-67, BR-68, BR-69 — Force Mob Damage, Damage Cap and On-Hit Status

**Covers:** BR-66, BR-67, BR-68, BR-69
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1439-1476`

**Overview:**
Mob-only damage receives the `ForceMobDamage` bonus; all damage is capped at 1,000,000; and the attacker's `RSV_FROST` and `RSV_DRAIN` reservation flags each grant a 50% chance to inflict a status affect.

**Detailed description:**

`ForceMobDamage` is the pet/summon damage bonus accumulated in `BASE_GetCurrentScore` via its `int *ForceMobDamage` out-parameter (`Code/TMSrv/CMob.cpp:674`); it is added only when the victim is a mob (`idx > MAX_USER` — strictly greater, so index exactly `MAX_USER` is treated as a player) and only when damage is at least 1. The cap at `MAX_DAMAGE` (1,000,000) is applied after every other additive or multiplicative modifier, so it is a genuine last line of defence against overflow-driven one-shots. It is applied *before* `ReflectDamage` and *before* the mana-shield and accessory divisions, so the number the victim sees can still be far below 1,000,000 even if the cap was hit.

The frost rule (`:1450-1462`) triggers on `pMob[conn].MOB.Rsv & RSV_FROST` (0x01). `Rsv` is the victim's reservation block, normally zeroed for mobs at `CMob.cpp:654`; for players it is derived from equipment bonuses. A 50% roll applies affect type 36 with time `Special[1] + 150` and level `Special[1]` through both `SetAffect` and `SetTick`, then resends the victim's score if either succeeded. Because `SetAffect` and `SetTick` both call `GetCurrentScore` internally, and the caller resends on top, a single frost proc can recompute the victim's score three times. The drain rule (`:1464-1476`) is a structural clone with affect type 40. Both rules are guarded only by the `dam > 0` check earlier at `:1447`, so they do not fire on a miss (`-3`/`-4`) or on a zeroed slot, which is correct.

The two on-hit statuses are the component's only equipment-driven "proc" mechanics; every other proc-like behaviour (Huntress double-strike, HP absorb) is class- or stat-driven. Note that they are checked once per target slot, so an area attack with `RSV_FROST` rolls independently for each victim and can freeze an entire group.

**Rule workflow:**
```
if victim is mob and dam >= 1 -> dam += caster.ForceMobDamage
if dam >= MAX_DAMAGE -> dam = MAX_DAMAGE
Dam[i].Damage = dam
if dam <= 0 -> continue
if caster.Rsv & RSV_FROST and rand()%2 == 0:
    s = caster.Special[1]
    if SetAffect(victim, 36, s+150, s) -> SendScore(victim)
    if SetTick  (victim, 36, s+150, s) -> SendScore(victim)
if caster.Rsv & RSV_DRAIN and rand()%2 == 0:
    s = caster.Special[1]
    if SetAffect(victim, 40, s+150, s) -> SendScore(victim)
    if SetTick  (victim, 40, s+150, s) -> SendScore(victim)
```

---

### Business Rule: BR-70, BR-71 — Reflection and Clan-Safe Zone

**Covers:** BR-70, BR-71
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1478-1502`

**Overview:**
Player victims can reduce incoming damage through two independent reflection stats, each floored at 1 damage, and a specific rectangular region forbids clan-mates from damaging each other.

**Detailed description:**

The reflection block is player-victim only (`idx > 0 && idx < MAX_USER && dam > 0`). `ReflectDamage` subtracts a flat amount — `dam = dam - pMob[idx].ReflectDamage` — and then floors at 1, so a victim with `ReflectDamage >= 999,999` effectively becomes immune to all non-capped damage. `ReflectPvP` then subtracts `dam/100 × ReflectPvP`, i.e. a percentage, and floors at 1 again. Both are computed in `CMob::GetCurrentScore` (`Code/TMSrv/CMob.cpp:838` for `ReflectDamage`, `:850-854` for `ReflectPvP`), the former from inventory gem effect 3 scaled by item grade and sanctification, the latter from `EF_LWORDGUILD`. Note that "reflection" here means *reduction*, not damage returned to the attacker: nowhere in this component is damage reflected back. The naming is misleading and would cause an operator tuning `ReflectDamage` to expect an offensive effect.

Because the floor is 1 rather than 0, a fully-reflecting victim still takes 1 damage per hit. That guarantees that any attacker can eventually kill any victim regardless of gear, which is a deliberate design decision (no true invulnerability) and interacts with the mana-shield and accessory divisions downstream — each of those has its own floor-at-1 or clamp, so the effective minimum is 1 by construction rather than by a single guarantee.

The clan-safe zone at `:1496-1502` is a hard-coded rectangle: x ∈ [1017, 1290], y ∈ [1911, 2183], with `dam > 0` and `pMob[conn].MOB.Clan == pMob[idx].MOB.Clan`. Inside it, clan-mates cannot damage each other at all and both `Damage` and `TargetID` are zeroed. The rectangle overlaps substantially with the RvR rectangle used in `_MSG_PKMode` (x 1023–1280, y 1919–2179) and with the LastServer `summon` region, which suggests a shared map (an event arena). Expressing it as raw coordinates rather than a named zone is consistent with the rest of the file but makes it impossible to retune without a rebuild. Notably this rule applies to *all* entities including mobs, so same-clan mobs also become mutually immune inside the rectangle.

**Rule workflow:**
```
if victim is player and dam > 0:
    if victim.ReflectDamage > 0: dam -= victim.ReflectDamage ; if dam <= 0 -> dam = 1
    if victim.ReflectPvP    > 0: dam -= dam/100 * victim.ReflectPvP ; if dam <= 0 -> dam = 1
    Dam[i].Damage = dam
if caster in [1017..1290]x[1911..2183] and dam > 0
   and caster.Clan == victim.Clan:
    Dam[i].Damage = 0 ; Dam[i].TargetID = 0 ; continue
```

---

### Business Rule: BR-72 — Adult Mount Damage Absorption

**Covers:** BR-72
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1504-1517`, `:1612-1613`; `Code/TMSrv/Server.cpp:4263-4298`

**Overview:**
A player wearing an adult mount (equipment slot 14, item index 2360–2389, with a positive HP pool in effect slot 0) absorbs 25% of incoming damage into the mount, which loses half of that value.

**Detailed description:**

The handler detects the mount by index range and by a positive `stEffect[0].sValue`, then computes `_pDamage = (dam × 3) >> 2` as the damage that actually reaches the player (75%, floored) and `_calcDamage = dam − _pDamage` as the absorbed portion (25%). `_pDamage` is floored at 1 and written into the packet so that the client displays the reduced number. The absorbed value is passed to `ProcessAdultMount(idx, _calcDamage / 2)` — half of the absorbed amount — which subtracts it from the mount's HP pool in `stEffect[0].sValue`, clamps to `mSummon.Mob[idx+10].CurrentScore.MaxHp`, sets the "unfed" effect to 0 when the pool empties, triggers a full `SendEquip` when the mount crosses between alive and dead, and sends a targeted item update.

The design intent is clear: the mount is a damage sponge with a shared resource pool, and it degrades visibly as it absorbs. What is not clear is the 25%/12.5% split — the player still takes three quarters of the hit even while mounted, so a mount provides a modest 25% mitigation rather than the near-immunity a "mount shield" would suggest. The mount's HP cost is half the absorbed value, i.e. 12.5% of the raw hit, so a 10,000-HP mount survives about 80 hits of 1,000 raw damage. Because `AddEnemyList` refuses to add hidden or merchant targets and `SetBattle` restricts mob aggro to a 23-cell box, mounts are also protected from aggro while ridden, making the mount a substantial defensive asset in PvE.

The interaction with the later HP-application block is important: `_pDamage` (not `dam`) is captured as `tDamage` and used for the actual HP subtraction at `:1554-1609`, so the mount reduction is applied exactly once and is not double-counted by the mana-shield or accessory divisions. The packet field, however, was set to `_pDamage` and can then be overwritten again by the mana-shield and piercing code paths, so the value the client displays may not equal the HP actually lost. That divergence is deliberate for the accessory cases (BR-74) and incidental elsewhere.

**Rule workflow:**
```
mountId = victim.Equip[14].sIndex
_pDamage = dam
if victim is player and mountId in [2360,2390) and Equip[14].stEffect[0].sValue > 0:
    _pDamage    = (dam * 3) >> 2 ; if _pDamage <= 0 -> _pDamage = 1
    _calcDamage = dam - _pDamage
    Dam[i].Damage = _pDamage
tDamage = _pDamage
... mana shield, accessory, HP application ...
if _calcDamage > 0 -> ProcessAdultMount(victim, _calcDamage / 2)
```

---

### Business Rule: BR-73 — Mana Shield Absorption

**Covers:** BR-73
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1522-1552`

**Overview:**
A victim carrying affect type 18 converts incoming damage into mana loss while its MP is above 10% of MaxMp, passing only about 30% (or 33%) of the damage through to HP.

**Detailed description:**

The rule scans all 32 affect slots for type 18 and, on the first match, tests `CurrentScore.Mp > (MaxMp / 100) × 10`. If MP is above that threshold, it computes `mana = Mp − tDamage`; if that would go negative, the shortfall is added back to `tDamage` and `mana` is set to 0, so HP damage receives only the portion the mana could not cover. It then writes `CurrentScore.Mp`, recomputes `pUser[idx].ReqMp` by subtracting the full `tDamage` (not the portion actually taken from mana — a subtle inconsistency that makes the client-side pool diverge from the server's when the pool is drained), calls `SetReqMp`, and finally rescales the HP-bound damage as `((tDamage >> 1) + (tDamage << 4)) / abs`, where `abs` is 55 normally and 50 when the attacker has learned bit `1 << 23`. That expression is `(0.5t + 16t) / 55 ≈ 0.30t`, or `≈ 0.33t` with the bit set.

Several structural issues are visible. First, the `ReqMp` subtraction uses `tDamage` rather than the absorbed amount, so once the pool is exhausted the client's shadow value is decremented by an amount that was never actually spent. Second, the loop does not `break` after applying the shield, so if a victim somehow carried two type-18 affects the shield would be applied twice, each time against the already-reduced `tDamage`. `SetAffect`'s dedup by type makes duplicates unlikely but not impossible (two affects of the same type can coexist if `GetEmptyAffect` picks different slots, which depends on the implementation in `Code/TMSrv/GetFunc.cpp:610-625`). Third, when the Huntress flag bit 4 is set, `Dam[1].Damage` is overwritten with the post-shield `tDamage`, changing the piercing scratch value that was already used at `:1288` — an ordering dependency that is invisible at the call site.

The reading `abs = (1 << 23) & pMob[conn].MOB.LearnedSkill ? 50 : 55` has a precedence subtlety: `<<` binds tighter than `&`, so it evaluates `(1 << 23) & LearnedSkill` and then the ternary — correct, but only by accident of precedence. If the mask ever needed two bits, this would silently become a different expression.

**Rule workflow:**
```
for c in [0, MAX_AFFECT):
    if victim.Affect[c].Type == 18:
        if victim.Mp > (victim.MaxMp / 100) * 10:
            mana = victim.Mp - tDamage
            if mana < 0: tDamage -= mana ; mana = 0
            victim.Mp = mana
            pUser[idx].ReqMp -= tDamage ; SetReqMp(idx)
            abs = ((1<<23) & caster.LearnedSkill) ? 50 : 55
            tDamage = ((tDamage >> 1) + (tDamage << 4)) / abs
            Dam[i].Damage = tDamage
            if (DoubleCritical & 4): Dam[1].Damage = tDamage
```

---

### Business Rule: BR-74 — Fairy Accessory HP-Multiplier Items

**Covers:** BR-74
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1554-1609`

**Overview:**
Three specific fairy accessory indices in equipment slot 13 multiply the victim's *effective* HP by dividing the HP damage it actually loses: 786 by `max(sanc, 2)`, 1936 by `max(sanc, 2) × 10`, and 1937 by `max(sanc, 2) × 1000` — while the packet continues to report the undivided damage.

**Detailed description:**

The three branches are structurally identical and differ only in a single pre-multiplication of the sanctification value. Item 786 divides by `itemSanc` floored at 2; item 1936 multiplies `itemSanc` by 10 and then divides by the product; item 1937 multiplies by 1000 and divides by the product. The comments label 1936 as `//10X HP MONSTRO` and 1937 as `//20X HP MONSTRO`, and the 1936 comment matches its code exactly — a ×10 multiplier on the divisor is algebraically a ×10 multiplier on effective HP. The 1937 comment does not match: the code applies ×1000, not ×20, so that accessory grants **1000×** effective HP rather than the documented 20×, a 50× discrepancy. At the minimum sanctification of 2 the divisor is 2000, so a fully capped 1,000,000-damage hit removes 500 HP; a monster with a large MaxHp pool and this accessory is effectively unkillable by normal combat, and the discrepancy between the comment and the code suggests the constant was changed at some point without updating the annotation.

The mechanism is deliberately confined to HP *loss*, not to the reported damage. `m->Dam[i].Damage` retains `_pDamage` (or the mana-shield-reduced value), so the attacking client and every observer see full damage numbers while the victim loses a fraction. The consequence is a player-visible lie: combat statistics, damage meters and player judgement of their own output are all decoupled from reality for these three accessories. It also means a developer diagnosing "why is this boss not dying" cannot use the damage figures in the logs at `:1628`, which record `m->Dam[i].Damage` rather than `tDamage`.

The minimum-sanctification floor of 2 is applied before the multiplication in all three branches, so the smallest possible divisor is 2 (item 786), 20 (1936) or 2000 (1937). No branch caps the divisor or validates the sanctification value's upper bound, so a corrupted item with `cValue = 255` yields a divisor of 255,000 for item 1937 — a de-facto invulnerability. Because `stEffect[0].cValue` is a single byte with no cross-check against the item's `sIndex`, and because item instances are loaded from the database, this is a data-integrity dependency: the handler trusts an item effect byte to bound combat outcome.

Comparing with the heal-side implementation of the same three indices (BR-37, `:649-683`) reveals an asymmetry: the heal path applies an *additional* multiplication (`sanc *= 10` then `/ (sanc * 10)` for 1936, and `sanc *= 1000` then `/ (sanc * 20)` for 1937), giving heal divisors of `sanc × 100` and `sanc × 20000`. The damage path uses the plain products. The two paths therefore disagree by 10× and 20× respectively, which — if intentional — means these accessories both inflate HP and suppress incoming healing by a different factor. Nothing in the code documents this; it is flagged as a probable defect in one of the two paths.

**Rule workflow:**
```
if victim.Equip[13].sIndex == 786:
    s = max(Equip[13].stEffect[0].cValue, 2)
    tDamage = _pDamage / s
elif victim.Equip[13].sIndex == 1936:
    s = max(Equip[13].stEffect[0].cValue, 2) * 10
    tDamage = _pDamage / s
elif victim.Equip[13].sIndex == 1937:
    s = max(Equip[13].stEffect[0].cValue, 2) * 1000
    tDamage = _pDamage / s
else:
    tDamage = _pDamage
if tDamage > victim.Hp -> tDamage = victim.Hp
victim.Hp -= tDamage
```

---

### Business Rule: BR-75..BR-77 — HP Application, HP Absorb and Kill Registration

**Covers:** BR-75, BR-76, BR-77
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1612-1656`

**Overview:**
Damage is subtracted from HP with a clamp at current HP; the attacker may recover a capped amount of HP via `HpAbs`; and any victim reduced to 0 is queued for `MobKilled` after the loop.

**Detailed description:**

HP application is preceded by two side effects: `ProcessAdultMount(idx, _calcDamage / 2)` for the absorbed mount damage and a `SendSay` debug line gated on `LOCALSERVER != 0` that prints the mob name, raw damage and post-damage HP into the world chat channel (`:1615-1619`). The latter is a development aid left in production code; on any server with `LOCALSERVER` enabled it leaks exact damage and HP figures to all nearby players. The HP write is then preceded by a clamp: `if (tDamage > pMob[idx].MOB.CurrentScore.Hp) tDamage = pMob[idx].MOB.CurrentScore.Hp;`, which prevents negative HP at the source (and makes the later `if (Hp <= 0) Hp = 0` at `:1653` redundant for this path, though not for the heal path which can round differently). Player victims additionally get `pUser[idx].ReqHp -= tDamage` and `SetReqHp(idx)` so the client's shadow value tracks the server's.

Every hit is logged unconditionally at `:1628-1629` with the format `"etc,msg_attack attacker:%d name:%s target:%d name:%s dam:%d x:%d y:%d"`. This is one formatted write per target slot per attack. At a modest 100 players fighting 10 mobs each at 2 attacks per second, that is 2,000 log lines per second, each with five numeric conversions and two string copies. `Log` itself is implemented at `Code/TMSrv/Server.cpp:6701` using `sprintf` into a shared `temp` buffer followed by `fprintf`, and the architectural report has already flagged its self-referential format bug. Combat logging is therefore both a throughput bottleneck and a correctness hazard.

Clan 4 mobs with damage applied get `LinkMountHp(idx)` (`:1631-1632`), which propagates the summon's HP back into the owner's mount equipment item so the owner's client shows the real mount HP. This is the reverse direction of BR-72's absorption and the two together implement a fully synchronized mount HP model across three objects (item, summon entity, owner client).

The `HpAbs` life-steal rule at `:1635-1648` fires on a 50% roll when the attacker's `HpAbs` is non-zero and damage is at least 1. It computes `RecHP = (dam × HpAbs + 1) / 100`, caps it at 350, then computes `reqhp = pUser[conn].ReqHp + RecHP` and applies `if (reqhp > MaxHp) pUser[conn].ReqHp = MaxHp; else pUser[conn].ReqHp = RecHP;`. The `else` branch is a defect: it should add `RecHP` to the current value, but instead **assigns** `RecHP`, discarding all accumulated HP. A character at 5,000/10,000 HP who procs a 100-point absorb drops to 100 HP on the client's shadow value. The server-side `MOB.CurrentScore.Hp` is not modified at all by this rule, so the client and server diverge immediately and permanently until another HP sync (a `SendSetHpMp`, a level-up, or a death) corrects it. Since `ReqHp` is what the client displays and what many other code paths read as "the player's HP", this is a user-visible, exploitable desynchronisation. `HpAbs` is populated by `BASE_GetCurrentScore`'s out-parameter (`Code/TMSrv/CMob.cpp:674`) from equipment.

Death registration is minimal: `if (Hp <= 0) { Hp = 0; TargetKilled[i] = idx; continue; }`. Nothing else happens inside the loop; all kill consequences (exp, drops, PK point changes, guilt, respawn) are deferred to `MobKilled` at `:1743`, which is called once per queued slot after the broadcast. Deferring is correct because the broadcast must show the killing blow before the victim is removed, but it also means that for the remainder of the loop `pMob[idx]` still exists at 0 HP and can be processed by later slots (if the same victim appears twice) or referenced by the aggro block (which is skipped by the `continue`).

**Rule workflow:**
```
if (LOCALSERVER) SendSay(victim, "%s: %d(%d) - %d", ...)
if victim is player: pUser[idx].ReqHp -= tDamage ; SetReqHp(idx)
Log("etc,msg_attack attacker:... dam:%d x:%d y:%d", ...)
if victim is Clan 4 mob: LinkMountHp(idx)
if caster.HpAbs != 0 and rand()%2 == 0 and dam >= 1:
    RecHP = (dam * HpAbs + 1) / 100 ; if RecHP > 350 -> RecHP = 350
    if ReqHp + RecHP > MaxHp -> ReqHp = MaxHp
    else                     -> ReqHp = RecHP            [DEFECT]
if victim.Hp <= 0: victim.Hp = 0 ; TargetKilled[i] = idx ; continue
```

---

### Business Rule: BR-78 — Aggro and Party Combat Propagation

**Covers:** BR-78
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1658-1707`

**Overview:**
A surviving victim is linked to the attacker via `SetBattle`, the attacker's current target is set to the victim, and both combatants' summon parties are pulled into the fight with dead members purged.

**Detailed description:**

`SetBattle(mob, target)` (`Code/TMSrv/Server.cpp:7218-7252`) is asymmetric by design: it only acts when the *first* argument is a non-player (`mob >= MAX_USER`) and the second is a player (`target >= MAX_USER` or `pUser[target].Mode == USER_PLAY`), and only within a ±23-cell box. It sets `MOB_COMBAT`, adds the target to the enemy list, randomises a battle cry from the NPC generator's `FightAction` table, and broadcasts it. Because of the asymmetry, `SetBattle(idx, conn)` at `:1660` makes the *mob* aggro the *player*, while a player attacking another player produces no state change at all — player-versus-player combat has no persistent aggro model in this component. The subsequent `pMob[conn].CurrentTarget = idx` at `:1662` is the only record of the player-to-player relationship and is overwritten by the next attack.

The two party loops at `:1669-1706` iterate `MAX_PARTY` (12) entries of the leader's `PartyList` and skip entries `<= MAX_USER`, i.e. they process only non-player members (summons/pets). For each, an empty or dead member is deleted and its slot cleared; a live member is drawn into the fight with `SetBattle(partyMemberId, idx)` for the attacker's party and `SetBattle(ptMemberId, conn)` for the victim's party. The leader fallback is inconsistent between the two loops: the first uses `if (mleader <= MOB_EMPTY) mleader = conn;` (≤ 0) while the second uses `if (mleader <= 0) mleader = idx;` — equivalent, but one uses the named constant and the other a literal. The first loop also starts its counter at `MOB_EMPTY` (`for (r = MOB_EMPTY; ...)`) rather than 0, a stylistic artefact.

A defect is visible at `:1671-1673`: `int partyMemberId = pMob[mleader].PartyList[r]; if (partyMemberId <= MAX_USER) continue;` — this skips any party member with an index ≤ 1000, which excludes all players *and* index 1000 itself, and then the code dereferences `pMob[partyMemberId]` without an upper bound check against `MAX_MOB`. A corrupted `PartyList` entry above 25,000 would produce an out-of-bounds read. `PartyList` is `short[MAX_PARTY]`, so the maximum representable value is 32,767 — greater than `MAX_MOB == 25,000`. The guard is therefore insufficient, and since `PartyList` is written by party-management code elsewhere, a stale or corrupted entry is the only prerequisite. This is a latent memory-safety issue with low likelihood but high impact.

**Rule workflow:**
```
if victim.Mode != MOB_EMPTY and victim.Hp > 0:
    SetBattle(victim, conn)
    pMob[conn].CurrentTarget = victim
    leaderA = pMob[conn].Leader ; if leaderA <= 0 -> leaderA = conn
    for r in [0, MAX_PARTY):
        m = pMob[leaderA].PartyList[r]
        if m <= MAX_USER -> continue
        if pMob[m].Mode == MOB_EMPTY or pMob[m].Hp <= 0:
            if Mode != MOB_EMPTY -> DeleteMob(m, 1)
            pMob[leaderA].PartyList[r] = MOB_EMPTY
        else SetBattle(m, victim)
    leaderV = pMob[victim].Leader ; if leaderV <= 0 -> leaderV = victim
    for r in [0, MAX_PARTY):
        m = pMob[leaderV].PartyList[r]
        if m <= MAX_USER -> continue
        if dead/empty -> delete and clear
        else SetBattle(m, conn)
```

---

### Business Rule: BR-79..BR-82 — Experience Accrual, Response Projection, Broadcast and Level Segments

**Covers:** BR-79, BR-80, BR-81, BR-82
**Location:** `Code/TMSrv/_MSG_Attack.cpp:1711-1787`

**Overview:**
After the target loop the handler finalises the exp figure carried in the packet, sets the spectral bit, stamps the caster's live exp and HP, optionally rewrites the client tick, broadcasts the packet to the grid, dispatches kills, and finally performs the level-up segment check with its rewards.

**Detailed description:**

The exp finalisation is three clamped assignments: `if (UNK2 > 0) Exp = UNK2;` then `if (DOUBLEMODE) Exp *= 2;` then clamps to `[0, 200]`. The cap of 200 makes the doubling almost meaningless — a heal that already earned 120 becomes 240 and is cut back to 200, so `DOUBLEMODE` only affects gains below 100. More importantly `UNK2` is the shared variable already used as a boolean for buff success (BR-51), so the packet's `CurrentExp`-adjacent exp value is really "max heal-derived gain, or 1 if a buff landed". This is not the *character's* experience — that is `pMob[conn].MOB.Exp`, stamped separately at `:1727` — but a per-attack display value whose consumer is the client's floating exp text. The two are unrelated in magnitude and origin.

The spectral flag at `:1724-1725` sets `m->DoubleCritical |= 8` when the attacker has learned skill bit `1 << 29`. Since `DoubleCritical` already carries bits 0/1/2 from the physical branch, bit 3 is a free slot used to signal a client-side visual. Using a field named `DoubleCritical` to carry an unrelated render flag is a protocol-design smell: five semantic bits share one byte with no documentation, and the client must know which bit means what.

Broadcast at `:1733-1734` sends the whole packet via `GridMulticast` to every observer in the attacker's view, except when the skill is 102. Because the packet was mutated in place, every observer receives the server-resolved `Damage` values, the caster's MP and HP, the exp figure and the `TargetID` list — including target slots that were rejected with `continue` without zeroing (the view-range path at `:349`). `GridMulticast` (`Code/TMSrv/SendFunc.cpp:620`) queues the message per socket, so the cost is O(observers) per attack. `m->ClientTick` is replaced with `CurrentTime` for server-originated attacks so that clients can animate mob attacks, and `skill 30`'s post-hoc HP reduction triggers a `SendSetHpMp` because the score packet already left with stale HP.

The final level check calls `CMob::CheckGetLevel()` (`Code/TMSrv/CMob.cpp:1034-1152`), which returns 0 for no change, 1–3 for crossing a quarter-segment of the current level, and 4 for an actual level-up (with the level increment, MaxHp/MaxMp growth, skill/special bonus points, AC increment and full HP/MP restore performed inside the method). The handler then emits the corresponding client message, sends score and an emotion, and on segment 4 sends `SendEtc`, grants **+5 PK points** via `SetPKPoint`, and calls `DoItemLevel(conn)` for mortals. Awarding PK points for levelling is counter-intuitive — a clean player becomes progressively more "PK-marked" as they level, which under BR-60 makes them *immune* to attack by clean players while making them attackable by... nothing, since the rule only protects high-PK victims from clean attackers. The net effect is that levelling confers PK immunity to the attacker. That is either a deliberate anti-griefing mechanic (rewarding progression with protection) or a mis-signed increment; source alone cannot decide, and this is documented as a high-uncertainty rule.

`DoItemLevel(conn)` (`Code/TMSrv/Server.cpp:8805`) is called only for `ClassMaster == MORTAL`, so celestial tiers gain levels without the item-level side effect. The level-up log at `:1784-1785` records the mob name and `BaseScore.Level`.

**Rule workflow:**
```
if UNK2 > 0 -> Exp = UNK2
if DOUBLEMODE -> Exp *= 2
if Exp <= 0 -> Exp = 0
if Exp > 200 -> Exp = 200
if LearnedSkill & (1<<29) -> m->DoubleCritical |= 8
m->CurrentExp = MOB.Exp ; m->CurrentHp = MOB.CurrentScore.Hp
if ClientTick == SKIP -> m->ClientTick = CurrentTime
if skill != 102 -> GridMulticast(attacker.X, attacker.Y, m)
if skill == 30 -> SendSetHpMp(conn)
for each TargetKilled[i] -> MobKilled(id, conn, 0, 0)
Segment = CheckGetLevel()
if Segment in [1,4]:
    message per segment (level up / 3-quarter / 2-quarter / 1-quarter)
    SendScore ; SendEmotion(conn, 14, 3)
    if Segment == 4:
        SendEtc ; SetPKPoint(conn, GetPKPoint(conn) + 5)
        GridMulticast(MSG_CreateMob) ; Log("lvl ... level up to %d")
```

---

## 4. Component Structure

### 4.1 Physical structure

The component is a **single translation unit** with **one exported function**. There is no header of its own, no namespace, no class, and no internal module boundaries.

```
Code/TMSrv/_MSG_Attack.cpp                      1,789 lines, 44,145 bytes
│
├── lines    1-18   GPLv3 license header
├── line      19    #include "ProcessClientMessage.h"   (aggregate include)
├── lines 21-1789   void Exec_MSG_Attack(int conn, char *pMsg)
│
├── [A] Entry and session gating                          :25-51
├── [B] Anti-cheat tick envelope                          :53-97
├── [C] Skill legality and delay                          :99-152
│     └── dead mortar block                               :158-193
├── [D] Resource consumption                              :222-256
├── [E] Derived combat stats                              :258-274
├── [F] Pre-loop packet mutation (skill 79 fan-out)        :286-293
├── [G] Target loop                                       :297-1708
│   ├── G1  slot/size gating and identity                 :297-361
│   ├── G2  guild/clan/leader resolution                  :363-403
│   ├── G3  consensual-PvP gate                           :405-419
│   ├── G4  physical resolution                           :421-485
│   ├── G5  skill resolution (13 sub-blocks)              :487-1270
│   ├── G6  damage post-processing                        :1273-1445
│   ├── G7  on-hit status, reflection, safe zone          :1450-1502
│   ├── G8  mount, mana shield, accessories, HP write      :1504-1609
│   ├── G9  absorb, death, aggro                          :1612-1707
├── [H] Post-loop exp, projection, broadcast              :1711-1737
└── [I] Kill dispatch and level segments                  :1739-1787
```

### 4.2 Region markers as an implicit module map

The file uses MSVC `#pragma region` as its only structural delimiters. They are the de facto module boundaries and are listed here in source order, since they are the best available evidence of the author's intended decomposition:

| Region marker | Lines | Concern |
|---|---|---|
| `Escudo_dourado` | 222-233 | Golden Shield coin cost |
| `Consumo de mana` | 238-256 | Mana cost |
| `Loop Target` | 296-1709 | Whole target loop |
| `Ataque físico` | 421-485 | Physical damage |
| `Skills` | 487-1270 | Skill dispatch |
| `Ataques com elementos` | 517-605 | Elemental damage |
| `Ataque de Fada` | 532-554 | Fairy damage |
| `HEAL_RECOVERY` | 607-713 | Healing |
| `Flash` | 715-724 | Forced peace |
| `Desintoxicar` | 726-748 | Detox |
| `Teleporte` | 750-787 | Summon/teleport |
| `Invisibilidade` | 789-806 | Aggro redirect |
| `Evock` | 808-838 | Pet summon |
| `Chamas etereas` | 840-885 | Mana burn |
| `Furia divina` | 887-967 | Knockback |
| `Exterminar` | 969-1012 | MP-to-damage |
| `Julgamento divino` | 1014-1022 | HP-to-damage |
| `Renascimento` | 1024-1049 | Rebirth |
| `Velocidade` | 1051-1080 | Speed buff |
| `Arma Magica` | 1082-1110 | Magic weapon |
| `Cancelamento` | 1112-1138 | Dispel |
| `Book Vinha` | 1140-1157 | Boss summon |
| `Buff` | 1192-1227 | Affect/tick application |
| `TRANSFORMATION` | 1229-1236 | Caster resync |
| `SKILLBOOK_RESSURECTION` | 1238-1268 | Self resurrection |
| `Perfuracao` | 1284-1304 | PvP scaling + force damage |
| `Ataque PvP` | 1306-1317 | PvP percentage |
| `PK - War - Miss` | 1330-1419 | PK legality + parry |
| `Joia Abs` | 1634-1649 | HP absorb |
| `MobKilled Check` | 1739-1745 | Kill dispatch |
| `Level Check` | 1746-1788 | Level segments |

The markers themselves reveal the component's mixed vocabulary: Portuguese (`Consumo de mana`, `Perfeuracao`, `Chamas etereas`, `Julgamento divino`), English (`HEAL_RECOVERY`, `Flash`, `TRANSFORMATION`, `Level Check`), and mixed-case compounds (`Book Vinha`, `SKILLBOOK_RESSURECTION` with a misspelling). This layering is consistent with the project's stated history of decompilation plus community contributions over time.

### 4.3 Declared interface

```cpp
// Code/TMSrv/ProcessClientMessage.h:74
void Exec_MSG_Attack(int conn, char *pMsg);
```

There is exactly one parameter of interest (`conn`, the connection/mob index) and one opaque message buffer. The function returns `void` and communicates failures exclusively through side effects (client messages, crack-error accumulation, and mutations to `pMsg`). No status code, no error enum, and no way for the caller to know whether the attack was accepted, partially applied, or abandoned.

### 4.4 Owned vs borrowed data

The component owns **no** state. Every field it reads or writes belongs to one of these global structures:

| Global | Declared | Role in this component |
|---|---|---|
| `pUser[MAX_USER]` | `Code/TMSrv/ProcessClientMessage.h:68` | Session state, tick books, HP/MP shadows, PK mode, range, crack counter |
| `pMob[MAX_MOB]` | `Code/TMSrv/CMob.h:122` | Entity state: HP/MP, scores, affects, leader/party/enemy lists, equipment |
| `pItemGrid[MAX_GRIDY][MAX_GRIDX]` | `Code/TMSrv/ProcessClientMessage.h:59` | Ground item lookup for the mortar rule |
| `pItem[...]` | external | Item instances referenced by the mortar rule |
| `g_pSpell[MAX_SKILLINDEX]` | `Code/Basedef.h:2464` | All skill data, read-only |
| `g_pMessageStringTable[]` | `Code/TMSrv/Language.h` | Localised client strings |
| `g_pGuildWar[65536]` | `Code/TMSrv/ProcessClientMessage.h:54` | Mutual war detection |
| `g_pGuildZone[MAX_GUILDZONE]` | external | Arena and village rectangles |
| `temp[]` | external | Shared formatting buffer used by every `sprintf` |

Shared mutable globals plus a shared `temp` buffer are the component's principal concurrency constraint: everything here assumes a single-threaded Win32 message loop (which `Server.cpp`'s `GetMessage` pump provides), so no locking is needed — but it also means no part of this component can be moved to a worker thread without introducing data races on `temp` alone.

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains

```
ProcessClientMessage  (Code/TMSrv/ProcessClientMessage.cpp:38)
  └─> Exec_MSG_Attack  (Code/TMSrv/_MSG_Attack.cpp:21)
        │
        ├─> [session] CUser fields (TradeMode, Mode, OnlyTrade, LastAttackTick,
        │              LastAttack, ReqMp, ReqHp, cProgress, PKMode, Range, NumError)
        │
        ├─> [anti-cheat] AddCrackError ──> CharLogOut ──> DB persistence
        │
        ├─> [skills]  g_pSpell[] ──> BASE_ReadSkillBin (XOR 0x5A obfuscated SkillData.bin)
        │             BASE_GetManaSpent
        │             BASE_GetSkillDamage
        │
        ├─> [damage]  BASE_GetDamage
        │             BASE_GetDoubleCritical ──> g_pHitRate[1024]
        │
        ├─> [defence] GetParryRate
        │             GetAttribute ──> g_pAttribute[][]
        │             BASE_GetDistance ──> g_pDistanceTable[][]
        │             BASE_GetArena / BASE_GetVillage ──> g_pGuildZone[]
        │
        ├─> [pvp]     GetPKPoint / SetPKPoint / GetGuilty / SetGuilty
        │                                    └─> MOB.Carry[KILL_MARK] (item slot 63)
        │             CWarTower::TowerAttack
        │             g_pGuildWar[]
        │
        ├─> [effects] SetAffect  ──> GetEmptyAffect, GetCurrentScore
        │             SetTick    ──> GetEmptyAffect, GetCurrentScore
        │             DoRemoveHide / DoRemoveSamaritano / DoRemoveHide(target)
        │
        ├─> [world]   CreateMob, DoSummon, GenerateSummon, GetEmptyMobGrid,
        │             SetBattle ──> AddEnemyList ──> mNPCGen.pList[].FightAction
        │             DeleteMob, SendRemoveMob, LinkMountHp, ProcessAdultMount
        │
        ├─> [outcomes] MobKilled (Code/TMSrv/MobKilled.cpp:41) ──> exp, drops,
        │                                                        PK points, guilt,
        │                                                        CCastleZakum::MobKilled
        │              CMob::CheckGetLevel ──> g_pNextLevel[], g_pNextLevel_2[]
        │              DoItemLevel
        │
        └─> [projection] SendClientMessage, SendHpMode, SendEtc, SendSetHpMp,
                         SendScore, SendEquip, SendSay, SendEmotion, GridMulticast
```

### 5.2 External dependencies

| Dependency | Kind | Purpose in this component | Notes |
|---|---|---|---|
| `SkillData.bin` | Binary data file | All `g_pSpell[]` fields: `Passive`, `Delay`, `MaxTarget`, `bParty`, `InstanceType`, `InstanceValue`, `AffectType/Value/Time`, `TickType/Value`, `Aggressive`, `AffectResist`, `ManaSpent` | Loaded with `BASE_ReadSkillBin` (`Code/Basedef.cpp:2792-2826`); XOR-0x5A obfuscated; checksum validation commented out (`:2815-2818`) |
| `g_pItemList` / `ItemList.bin` | Binary data file | Item 746 (mortar), 219/220/362 (immovable), 786/1936/1937 (fairy), 2360–2389 (mounts), 2330–2390 (mount equipment) | Referenced by index only; no symbolic constants |
| Map attribute grid | Server data | `0x01`, `0x02`, `0x04`, `0x40` cell semantics | Loaded into `g_pAttribute[1024][1024]`; semantics undocumented |
| Guild zone config | Server data | `BASE_GetArena`, `BASE_GetVillage` rectangles | `g_pGuildZone[MAX_GUILDZONE]` |
| Windows API | OS | `rand()`, `sprintf`, `memset`, `timeGetTime` (via `CurrentTime`) | Non-cryptographic RNG; `rand()` is used for every combat roll, including crit, parry, knockback and resurrection |
| File log | Filesystem | `Log()` audit trail, one line per hit | Synchronous `fprintf` |
| DBSrv | Internal service | Not called directly; all persistence is deferred to `MobKilled`/`CharLogOut` | Decoupled |

### 5.3 Coupling to global state — quantitative

| Category | Count | Examples |
|---|---|---|
| Distinct called functions | ~45 | `BASE_GetDamage`, `SetAffect`, `MobKilled`, `GridMulticast`, … |
| Distinct global arrays/objects mutated | 6 | `pUser`, `pMob`, `pItemGrid`, `mNPCGen`, `temp`, `g_pSpell` (read) |
| Distinct fields of `pUser[conn]` written | 7 | `LastAttackTick`, `LastAttack`, `ReqMp`, `ReqHp`, `NumError`, `Coin` (via `pMob`), `cProgress` (by ref) |
| Distinct fields of `pMob[conn]` written | 8 | `MOB.Coin`, `MOB.CurrentScore.Mp/Hp`, `CurrentTarget`, `extra.*` (via resync) |
| Distinct fields of `pMob[idx]` written | 12 | `MOB.CurrentScore.Hp/Mp`, `Affect[]`, `Mode`, `CurrentTarget`, `EnemyList[]`, `Route[]`, `MOB.Carry[]`, `MOB.Equip[14]` |
| Magic numeric literals | >40 | `700`, `800`, `15000`, `120000`, `100`, `4096`, `1017/1290/1911/2183`, `2604/2648/1708/1744`, `8/13`, `1/31`, `2360–2390`, `746`, `219/220/362`, `786/1936/1937`, `23`, `33`, `13`, `6`, `30`, `60`, `350`, `348…` |

### 5.4 Compile-time vs runtime dependency graph

* **Compile-time:** the file depends on a single aggregate header (`ProcessClientMessage.h`) that transitively includes `Basedef.h`, `CPSock.h`, `ItemEffect.h`, `Language.h`, `CItem.h`, `Server.h`, `GetFunc.h`, `SendFunc.h`, `CMob.h`, `CUser.h`, `CNPCGene.h`, `CReadFiles.h`, `CCastleZakum.h` and `CWarTower.h`. Any change to `STRUCT_SPELL`, `STRUCT_MOB`, `STRUCT_AFFECT`, `MSG_Attack*` or `STRUCT_DAM` recompiles this unit.
* **Link-time:** 45 external symbols across 6 TMSrv translation units plus `Basedef.cpp`. The unit is in the middle of a near-total dependency cycle: it calls `Server.cpp`, which calls it back through `ProcessClientMessage` (lines 4622, 5062, 5371, 7464).
* **Runtime data dependency:** the semantics of roughly 60 branches depend on values in `SkillData.bin`, map data and item data that are not visible in source. Any behavioural specification of this component is therefore incomplete without those data files, which are not present in the repository (only `release/` binaries exist).

---

## 6. Afferent and Efferent Coupling

Coupling is measured at two granularities. The architectural report's figures (Ca = 1, Ce = 14) are at **module** granularity; the symbol table below is at **function** granularity.

### 6.1 Component-level (module granularity)

| Component | Afferent (Ca) | Efferent (Ce) | Instability | Criticality |
|-----------|---------------|---------------|-------------|-------------|
| Attack Handler (`_MSG_Attack.cpp`) | 1 (`ProcessClientMessage`) + 4 indirect re-entry sites | 14 modules | 0.93 | Critical |

The instability value `Ce / (Ca + Ce) = 14/15 ≈ 0.93` places this component at the extreme "unstable" end of the scale: it depends on almost everything and almost nothing depends on it. That is expected for a top-of-stack command handler, but combined with a `void` return and side-effect-only failure signalling it means a defect here is invisible to its single caller and manifest only as corrupted global state.

### 6.2 Symbol-level coupling

| Symbol | Kind | Afferent (callers) | Efferent | Criticality | Notes |
|--------|------|--------------------|----------|-------------|-------|
| `Exec_MSG_Attack` | function | 1 direct + 4 indirect | ~45 | Critical | The component itself |
| `BASE_GetDamage` | function | 3 | 0 | High | Pure; used for every physical and skill hit |
| `BASE_GetSkillDamage` | function | 1 | 0 | High | Pure; used only by this component (plus `GetAttack`) |
| `BASE_GetDoubleCritical` | function | 1 | `g_pHitRate` | High | Mutates `pUser.cProgress` by pointer |
| `GetParryRate` | function | 3 | 0 | High | Pure; miss probability |
| `GetAttribute` | function | 10 | `g_pAttribute` | High | Bounds-checked lookup |
| `GetGuilty` / `SetGuilty` | function | 7 / 2 | `pMob` | High | Getter mutates on stale values |
| `GetPKPoint` / `SetPKPoint` | function | 7 / 7 | `pMob` | High | Uses `Carry[63]` as storage |
| `SetAffect` | function | 7 | `GetEmptyAffect`, `GetCurrentScore` | High | Recomputes victim score |
| `SetTick` | function | 5 | `GetEmptyAffect`, `GetCurrentScore` | Medium | Damage-over-time twin of `SetAffect` |
| `SetBattle` | function | 3 | `AddEnemyList`, `SendChat` | High | Asymmetric by argument order |
| `MobKilled` | function | 5 | Whole reward subsystem | Critical | Triggered per kill |
| `GetEmptyMobGrid` | function | 4 | `pMobGrid`, `pHeightGrid` | Medium | Ring search to radius 4 |
| `CWarTower::TowerAttack` | method | 1 | none | Low | Only external policy call |
| `ProcessAdultMount` | function | 1 | `mSummon`, `SendItem` | Medium | Only mount-absorption path |
| `LinkMountHp` | function | 1 | `pMob`, `SendItem` | Medium | Reverse mount sync |
| `CMob::CheckGetLevel` | method | 4 | `g_pNextLevel*` | High | Performs the level-up itself |
| `AddCrackError` | function | many | `Log`, `CharLogOut` | High | Accumulator; returns only at 2e9 |
| `SendClientMessage` / `SendScore` / `SendSetHpMp` / `SendEquip` / `SendEtc` / `SendSay` / `SendEmotion` | function | 5-30 each | `CPSock` | Medium | Projection layer |
| `GridMulticast` | function | ~20 | `CPSock` per observer | Critical | O(observers) per attack |

### 6.3 Cohesion assessment

Although the file is a single function, it is not a single *responsibility*. Its regions map onto at least nine distinct concerns: session validation, anti-cheat, resource management, target selection, damage arithmetic, status effects, PvP policy, reward accounting and client projection. Measured as a unit, cohesion is low (functional cohesion at best, coincidental at worst). Measured per region, cohesion is high — the elemental block is entirely about elemental damage, the PK block entirely about PK policy. The component is therefore best described as **nine cohesive modules collapsed into one function body** with a shared 40-variable local scope. The practical consequence is that no region can be unit-tested, mocked or reasoned about in isolation, which is the root cause of most findings in §11.

---

## 7. Endpoints

The component exposes no HTTP, GraphQL or gRPC surface. It is a **binary packet handler** in a custom TCP protocol. Its inbound message types are:

| Message | Opcode | Direction | Wire struct | Max targets | Description |
|---------|--------|-----------|-------------|-------------|-------------|
| `_MSG_Attack` | 103 | client↔game | `MSG_Attack` (`Basedef.h:2012`) | 13 | Area/multi-target attack, full damage table |
| `_MSG_AttackOne` | 157 | client↔game | `MSG_AttackOne` (`Basedef.h:2064`, `#pragma pack(1)`) | 1 | Single-target attack |
| `_MSG_AttackTwo` | 158 | client↔game | `MSG_AttackTwo` (`Basedef.h:2100`) | 2 | Two-target attack (double strike / cleave) |

**Inbound contract (client → server)**

| Field | Type | Consumed as | Validated |
|-------|------|-------------|-----------|
| `Size` | `short` | Slot count gate (`:299-306`) | No cross-check against actual frame length |
| `KeyWord`, `CheckSum` | `char` | Not read by the handler | Checked (if enabled) by `BASE_CheckPacket` under `_PACKET_DEBUG` only (`Server.cpp:3983`) |
| `Type` | `short` | Dispatch key (`ProcessClientMessage.cpp:192`) | Exact match |
| `ID` | `short` | **Overwritten** with `ESCENE_FIELD` (`:25`) | Bounds-checked by caller |
| `ClientTick` | `unsigned int` | Anti-cheat (`:59-97`) | 800 ms floor, 15 s/120 s window, 100 ms rewind |
| `PosX`, `PosY` | `unsigned short` | Physical range origin (`:424`) | Range ≤ 23 |
| `TargetX`, `TargetY` | `unsigned short` | Aim point, mortar coords, book-vine coords | Grid bounds for skills 97/98 |
| `AttackerID` | `unsigned short` | **Never read** | None |
| `Progress` | `unsigned short` | Hit-table index into `g_pHitRate[1024]` | Passed by pointer to `BASE_GetDoubleCritical` |
| `Motion` | `unsigned char` | Fairy-skill gate (`:533`) | Must equal 254 for that branch |
| `SkillParm` | `unsigned char` | **Never read** | None |
| `DoubleCritical` | `unsigned char` | **Overwritten** (`:483`, `:1725`) | n/a |
| `FlagLocal` | `unsigned char` | **Never read** | None |
| `Rsv` | `short` | **Never read** | None |
| `CurrentMp`, `CurrentHp`, `CurrentExp` | various | **Overwritten** (`:254`, `:1727-1728`) | n/a |
| `SkillIndex` | `short` | Skill dispatch (`:99`) | Class, learned mask, passive, delay; **`== 103` causes OOB read** |
| `ReqMp` | `short` | **Overwritten** (`:255`) | n/a |
| `Dam[0..12].TargetID` | `int` | Entity index (`:309`) | `(0, MAX_MOB)` |
| `Dam[0..12].Damage` | `int` | Sentinel code (`:353`) | Must be `-2`, `-1` or `0` |

**Outbound contract (server → grid)**

The handler re-emits the *same* struct through `GridMulticast` at `:1734` after mutating it in place. Effective outbound meaning:

| Field | Outbound value |
|-------|----------------|
| `ID` | `ESCENE_FIELD` (30000) |
| `ClientTick` | Client's tick, or `CurrentTime` for server-originated attacks |
| `CurrentMp`, `ReqMp` | Post-consumption caster values |
| `CurrentHp`, `CurrentExp` | Caster's live values (`:1727-1728`) |
| `DoubleCritical` | bit0 double-hit, bit1 crit, bit2 Huntress extra, bit3 spectral |
| `Dam[i].TargetID` | Original index, or 0 when rejected |
| `Dam[i].Damage` | Resolved damage, `-3` parry, `-4` block, or 0 |

**Related endpoints that bypass this handler**

`sMobAttack`-class attacks are generated by AI and injected as packets rather than calling a function (`Server.cpp:4622/5062/5371/7464`), and mob melee is resolved by the separate `MobAttack(int attacker, MSG_Attack sm)` at `Server.cpp:8927`. There is no endpoint or function that exposes the handler's logic for re-use.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol / mechanism | Data format | Error handling |
|-------------|------|---------|----------------------|-------------|----------------|
| Client socket (player → TMSrv) | Inbound network | Attack requests | Custom TCP, `CPSock::ReadMessage` (`Server.cpp:3975`) | Binary packet | `ProcessClientMessage` envelope checks; handler returns `void` |
| Client sockets (TMSrv → grid observers) | Outbound network | Attack projection | `GridMulticast` (`SendFunc.cpp:620`) | Binary packet, 1 per observer | None; queued per socket, no delivery confirmation |
| Mob AI (in-process) | Internal call | Mob and pet attacks re-enter the handler | `ProcessClientMessage(conn, msg, TRUE)` | Synthesised `MSG_Attack*` | Same path as client; `isServer` bypasses the tick ban |
| `MobKilled` | Internal call | Death resolution, exp, drops, PK, castle events | Direct function call | N/A | None; `MobKilled` performs its own bounds checks |
| `DBSrv` | Internal service | Persistence of the outcome (via `MobKilled` → save paths, and `CharLogOut` on crack-ban) | Async message queue, not called from here | Binary | Handled in `ProcessDBMessage` |
| `g_pSpell[]` / `SkillData.bin` | Local data file | All skill semantics | `fread` + XOR 0x5A (`Basedef.cpp:2792`) | Binary array of `STRUCT_SPELL` | `MessageBox` + return FALSE on missing file |
| Map attribute grid | Local data | PvP legality, heal direction, mortar/vine placement | In-memory `g_pAttribute[1024][1024]` | `unsigned char` per 4×4 cell | `GetAttribute` returns 0 out of bounds |
| Guild zone table | Local data | Arena / village classification | In-memory `g_pGuildZone[5]` | Rectangles | Returns 5 when outside all zones |
| Guild war table | In-memory | `isWar` detection | `g_pGuildWar[65536]` | `unsigned short` mutual lookup | Indexed without upper-bound check on `connGuild`/`SummonerGuild` beyond `> 0 && < 65536` |
| Item list / instances | Local data | Mortar, fairy, mount, immovable-item identification | In-memory `g_pItemList`, `pItem[]` | `STRUCT_ITEM` | Index bounds checked for the mortar only |
| File log | Filesystem | Audit trail per hit, crack events, level-ups | `Log()` → `fprintf` (`Server.cpp:6701`) | Plain text | None; shared `temp` buffer |
| NPC generator | In-memory | Battle cries on aggro, summon stats | `mNPCGen.pList[GenerateIndex]` | `CNPCGene` records | `GenerateIndex` range-checked in `SetBattle` |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose | Assessment |
|---------|----------------|----------|---------|------------|
| **Command Handler / Message Endpoint** | One `Exec_MSG_*` per packet type, dispatched by a flat `switch` | `ProcessClientMessage.cpp:66-260` | Decouples the socket from game logic | Consistent across 60 handlers; zero abstraction |
| **Sentinel Protocol (Request Code)** | `Dam[i].Damage ∈ {-2, -1, 0}` is a verb, not a value; the server overwrites it | `_MSG_Attack.cpp:353`, `:1279` | Anti-tamper: the client cannot state damage | Effective; undermined by the OOB `SkillIndex` and unsanitised `TargetID` |
| **Table-Driven Behaviour** | `g_pSpell[].InstanceType` selects one of 13 behaviour blocks | `:518-884` | Data-driven skills without code changes | The single best structural decision in the component; the `switch`-by-magic-number skill cases (`6, 22, 30, 31, 41, 44, 47, 79, 98`) are the exception and should also have been `InstanceType` values |
| **Strategy by Dispatch Table** | Elemental damage varies by `Class` inside `BASE_GetSkillDamage` | `Basedef.cpp:6391-6404` | Class-specific formulas | Works, but pulls balance data out of `SkillData.bin` into compiled branches |
| **Two-Phase Damage Pipeline** | Compute → modify → cap → absorb → apply | `:1279-1609` | Separates arithmetic from state mutation | Well-ordered but undocumented; the ordering is load-bearing (e.g. 25% PvP cut before `ForceDamage`) |
| **Shadow Field (Client Projection)** | `pUser[].ReqHp` / `ReqMp` mirror server truth for the client | throughout | Avoid trusting the client | Sound, but drifts in two places (BR-73, BR-76) |
| **Guard Clause** | Early `return`/`continue` per validation | 40+ sites | Flat control flow | Inconsistent: some rejections `return` (aborting all slots), some `continue` (skip one) |
| **Accumulator Threshold (Soft Enforcement)** | `AddCrackError` sums and returns TRUE only at 2×10⁹ | `Server.cpp:684` | Progressive anti-cheat sanction | Effectively never fires; combined with `if (AddCrackError(...)) return;` it converts hard rules into logging |
| **Proxy / Deferred Resolution** | Kill effects deferred to `MobKilled` after the broadcast | `:1651`, `:1740` | Guarantee the killing blow is rendered first | Correct and clean |
| **Scratch Slot Reuse** | `Dam[1]` used as an out-of-band channel for the Huntress hit | `:473`, `:1288`, `:1548` | Avoid a new local | Fragile; depends on the loop skipping target 0 |
| **Hard-Coded Zone Constants** | Coordinate rectangles inline for PK-safe zones, battle royale, no-weather area | `:386`, `:523`, `:1353`, `:1496` | Zone-specific rules | Untunable without a rebuild; 4 distinct rectangles, none named |
| **Shared Mutable Global** | All state lives in `pUser`/`pMob`; `temp[]` is shared across all `sprintf` | throughout | Zero allocation, single-thread assumption | Precludes concurrency; `temp` is a latent race |

### 9.1 Architectural decisions observed

1. **Server-authoritative combat with a request-code protocol.** The client proposes *intent* (`-2`/`-1`) and targets; the server decides every number. This is the correct decision for an MMO and is the reason the component has survived decompilation.
2. **One function per packet type, no layering.** There is no service layer, no domain model, no repository. Game rules are expressed directly as statements over globals. Cost: untestable, unextensible. Benefit: matches the decompiled origin and performs predictably.
3. **Skill behaviour selected by data, damage formula selected by code.** `InstanceType` is data-driven; the arithmetic inside each instance type is not. This hybrid gives operators control over *which* effect a skill has but not over *how much* it does.
4. **Deferred consequences.** Damage is applied in the loop, consequences (exp, drops, PK, castle) after the broadcast. This keeps the client's view consistent and isolates the highest-risk subsystem (`MobKilled`) behind a single call boundary.
5. **Soft anti-cheat.** Nearly every integrity violation accumulates points rather than rejecting the action. Whether this is deliberate tolerance for a lossy decompiled protocol or accidence of `AddCrackError`'s `BOOL` return being misused in `if` conditions cannot be determined; the effect is that the only hard rejections are the tick window, the tick floor, the damage sentinel, and the skill-delay floor.

---

## 10. Technical Debt & Risks

### 10.1 Risk register

| Risk | Rule | Component Area | Issue | Evidence | Impact |
|------|------|----------------|-------|----------|--------|
| **High** | BR-06/BR-28 | Skill dispatch | `skillnum == MAX_SKILLINDEX` (103) passes the `<= MAX_SKILLINDEX` guard at `:488` and indexes `g_pSpell[103]`, one past the 103-element array declared at `Basedef.cpp:744` and `Basedef.h:2464` | `_MSG_Attack.cpp:488` with `<=`; compare `:113` which correctly uses `<` | Out-of-bounds read of `STRUCT_SPELL` fields (`MaxTarget`, `bParty`, `InstanceType`, `Aggressive`, `AffectResist`) and of the same index inside `BASE_GetSkillDamage`. `GetAttackArea` legitimately synthesises `SkillIndex = 103` (`GetFunc.cpp:1628`), so 103 is a value present in real traffic. A client-sent 103 also reaches the branch at `:158-193` where `1 << (103-72)` is `1 << 31` (UB). |
| **High** | BR-76 | HP absorb | `pUser[conn].ReqHp = RecHP;` assigns instead of accumulating | `_MSG_Attack.cpp:1647` | Client HP shadow collapses to `RecHP` (≤ 350) on every successful absorb while server HP is untouched. Visible desync; downstream readers of `ReqHp` operate on a wrong value. |
| **High** | BR-05 | Cooldowns | `skilldelay` computed from `g_pSpell[].Delay` and `RSV_CAST` then never used; only a 700 ms literal is enforced | `_MSG_Attack.cpp:128-133` (assigned), `:135` (only reader of `isTime`) | Entire `Delay` column of `SkillData.bin` is inert. Operator tuning of cooldowns has no effect. Fast-cast bonus inert. |
| **High** | BR-74 | Damage mitigation | Item 1937 divides HP damage by `sanc × 1000` while its comment documents 20×; no upper bound on `sanc`; heal path uses `sanc × 20000` for the same item | `_MSG_Attack.cpp:1592-1594` vs comment `:1586`; heal side `:676-678` | That accessory grants 1000× effective HP (50× more than documented); at minimum sanctification a capped 1,000,000-damage hit removes 500 HP. Damage/heal paths disagree by 20× for the same item. |
| **High** | — | Testability | No test project, no test file, no assertion, no harness, no CI | See §11 | Every rule in this report is unverified by executable specification; regressions are undetectable except in production. |
| **High** | BR-08 | Skill ownership | Learned-skill check is `if (AddCrackError(...)) return;` which only returns at 2×10⁹ accumulated points | `_MSG_Attack.cpp:184-188`, `:202-209`; `Server.cpp:684-707` | Unlearned skills execute with full effect. Skill-gating is advisory. |
| **Medium** | BR-64 | PvP legality | Map-attribute predicate is inverted relative to the mob-side implementation | `_MSG_Attack.cpp:1428`, `:1431` vs `GetFunc.cpp:1527-1530`, `:1826-1830` | Two specifications of the same policy; at least one is wrong. Cannot be resolved from source. |
| **Medium** | BR-09 | Dead branch | Book-skill block `:158-193` reachable only for `skillnum == 103` | `:158` (`>= MAX_SKILLINDEX`) intersected with `:160` (`96..MAX_SKILLINDEX`) | Mortar item requirement (BR-09) never enforced; the resurrection, book-vine and skill-102 entries below index 103 bypass the level-scaled `Level`/`Delay`/`Special` derivation and instead use `Special[1]`. |
| **Medium** | BR-19 | Target legitimacy | `TargetID` validated only for bounds and view distance; no server-side target list | `:309-351` | A single 13-slot packet can resolve 13 independent skill hits on 13 different mobs in view, subject only to the 800 ms rule and skill `MaxTarget`. |
| **Medium** | BR-51/BR-79 | Affect-experience coupling | `UNK2` doubles as heal-exp accumulator and affect-applied boolean | `:1216`, `:1219`, `:1711` | Every successful affect application contributes at least 1 to the packet's exp figure; a heal+affect reports the heal gain. |
| **Medium** | BR-51 | Resistance inversion | The victim's `RegenMP` and the skill's `AffectResist` are *added* to the threshold that must be exceeded to skip the affect, so higher victim resistance makes the affect more likely to land; `connlevel` also tests the victim's `ClassMaster` instead of the attacker's | `:1170-1183` | Affect resistance behaves inversely to its name; celestial/mortal level scaling is computed with the wrong side's tier. |
| **Medium** | BR-43 | Ethereal flames | The "resisted" branch (`Rand <= chance`) applies the affect-stripping effect while the "burn" branch only drains MP | `:852-882` | Inverted outcome: resisting the burn strips the victim's buffs. |
| **Medium** | BR-47/BR-54 | Crack-point laundering | Both resurrection paths set `NumError = 0` on the target/caster | `:1039`, `:1249`, `:1259` | A player can erase accumulated anti-cheat points (up to 2×10⁹) by dying and being revived, or by using the resurrection book. |
| **Medium** | BR-21 | Guilt assignment | Attacking a guilty player sets the *victim's* guilt to 8 | `:1371-1374` | Being attacked by a PK-er makes the victim guilty, which under BR-60 exposes them to attack by clean players. Escalation or inversion — undetermined. |
| **Medium** | BR-82 | PK-point on level-up | `SetPKPoint(conn, GetPKPoint(conn) + 5)` on every level-up | `:1775-1776` | Levelling progressively marks clean players as PK, conferring immunity under BR-60. |
| **Medium** | BR-02..BR-04 | Anti-cheat | No server-side arrival-time measurement; 800 ms rule trusts the client's declared ticks and is bounded only by the 120 s/15 s window | `:59-97` | Rate inflation up to ~150× in a burst. |
| **Medium** | BR-78 | Party propagation | `pMob[partyMemberId]` dereferenced after only a lower-bound check; `PartyList` is `short` (max 32,767) while `MAX_MOB` is 25,000 | `:1671-1685`, `:1691-1706` | Out-of-bounds read on a corrupted/stale party entry. Low likelihood, high impact. |
| **Medium** | — | Logging | One `Log()` per target per attack plus `SendSay` HP disclosure when `LOCALSERVER != 0` | `:1615-1619`, `:1628-1629` | Throughput bottleneck; `Log` uses a self-referential `sprintf` into a shared buffer (already flagged in the architectural report as a format-string hazard); `SendSay` leaks exact HP and damage to nearby players. |
| **Medium** | BR-55 | Damage floor | `dam >> 2` floors hits of 1–3 against players to 0, then `continue` | `:1284-1292`, `:1447` | Small hits vanish rather than being clamped to 1, contradicting the floor-at-1 policy used by reflection, ForceDamage and mount absorption. |
| **Low** | BR-13 | Dead gate | `if (i >= MAX_TARGET && ...)` can never be true | `:299` | Dead code; indicates a lost size check. |
| **Low** | BR-03 | Unsigned underflow | `(unsigned int)LastAttackTick - 100` wraps when `LastAttackTick < 100` | `:70` | Regression detection disabled at session start. |
| **Low** | BR-21 | Tautological check | `tx`/`ty` assigned from `m->TargetX/Y` then compared to them | `:101-104` | Hit-position validation is dead; the intended anti-teleport check is absent. |
| **Low** | BR-06 | Dead validation | `skillnum < -1 || skillnum > MAX_SKILLINDEX` inside a guard that already bounds it | `:115-119` | Dead code. |
| **Low** | BR-25 | Dead local | `int CriticalDouble = 0;` assigned from `BASE_GetDoubleCritical` and never read | `:437-440` | Compiler warning; no behaviour. |
| **Low** | BR-45 | Dead local | `int kind4 = ...Special[3]` never used | `:979` | Dead code. |
| **Low** | BR-27 | Cross-iteration state | `DoubleCritical` (declared `:284`, outside the loop) and the `Dam[1]` scratch slot persist across target iterations | `:284`, `:473`, `:1288`, `:1548` | Sanity of the piercing/mana-shield values for slots > 0 depends on iteration order and on target 0 being skipped. |
| **Low** | BR-27 | Overwrite of `Dam[1].TargetID` | Huntress extra hit writes `Dam[1].TargetID = MOB_EMPTY` (0), which is a real slot in `MSG_Attack`/`MSG_AttackTwo` | `:473` | The legitimate second target of a two-target packet is silently discarded when the Huntress proc fires. |
| **Low** | BR-71 | Zone duplication | Four separate hard-coded coordinate rectangles implement zone policy, two of them overlapping | `:386`, `:523`, `:1353`, `:1496` | Untunable; drift between the PK-mode rectangle (`_MSG_PKMode.cpp:51`) and the handler's own copies. |
| **Low** | BR-40 | Inconsistent abort | Teleport uses `break` (aborts all slots) where every other rejection uses `continue` | `:756`, `:777` | One dead target in slot 0 suppresses the remaining slots for that skill. |
| **Low** | BR-22 | Abort on range | Out-of-range physical attack uses `return` (aborts the function) not `continue` | `:427` | One distant target suppresses resolution of closer targets depending on slot order. |
| **Low** | BR-22 | Inert stat | `pUser[idx].Range` assigned from `EF_RANGE` then immediately overwritten with 23 | `CMob.cpp:661-662` | Item range stat has no effect; both halves of the `:426` test are identical. |
| **Low** | BR-50 | Never-taken branch | `if (map_att == 0 && getAttribute == 1)` compares a `0x40`-masked value against 1 | `:1149` | The combat-zone restriction on Book Vine never applies. |
| **Low** | BR-79 | Dead approximation | `if (dam >= 1090) dam = 1100;` reserves 10 points of range that can never occur | `:634-635` | Cosmetic; the intended cap is probably 1090. |

### 10.2 Code smells

| Smell | Manifestation | Extent |
|-------|---------------|--------|
| Long method | One function, 1,769 lines, cyclomatic complexity estimated in the high hundreds | Absolute |
| Magic numbers | `700`, `800`, `120000`, `15000`, `100`, `4096`, `23`, `33`, `13`, `6`, `30`, `60`, `350`, `1017/1290`, `1911/2183`, `2604/2648`, `1708/1744`, `8/13`, `1/31`, `2360`, `2390`, `746`, `219`, `220`, `362`, `786`, `1936`, `1937`, `0x4000`, `0x200000`, `0x1000000`, `1<<23`, `1<<29`, `0x40`, `0x80`, `0x200`, `0x04` | 40+ |
| Duplicated logic | Huntress formula in 3 places; fairy accessory divisor in 2 places (different divisors); resurrection in 2 places; immovable-item list in 4 places; PK/guilt block duplicated in concept by `MobKilled.cpp:2111-2432` | 5 families |
| Dead code | 3 unreachable validation blocks, 2 dead locals, 1 dead `skilldelay`, 1 never-taken branch, 1 tautology | 8 sites |
| Commented-out code | `//pUser[idx].ReqMp = ... ((tDamage >> 1) + (tDamage >> 2));` at `:1538`; several commented alters in `GetFunc.cpp` | Scattered |
| Misleading names | `ReflectDamage`/`ReflectPvP` reduce incoming damage (no reflection); `DoubleCritical` carries 4 unrelated bits; `UNK2` is both exp and a boolean; `Heal` applied as negative damage | 4 |
| Inconsistent rejection semantics | `return` vs `continue` vs `break` vs zero-and-continue vs zero-and-`break`, with no discernible policy | 30+ sites |
| Primitive obsession | `int` used for connection, mob, target, affect type, skill index, damage, class master, clan, guild, and map attribute flag interchangeably | Pervasive |
| Feature envy | The handler reads and writes 20+ fields of `pMob[idx]` (score, affects, mode, lists, route, equipment) that conceptually belong to `CMob` | Pervasive |
| Shotgun surgery risk | Changing the fairy accessory divisor requires editing two disjoint blocks with two different formulas | Confirmed |
| Comment/code divergence | `//20X HP MONSTRO` next to a ×1000 divisor; the heal path uses `×100`/`×20000` for the same two items as the damage path's `×10`/`×1000`; `Ataque de Fada` block whose gate is likely unreachable | 3 |

### 10.3 Security posture summary

| Property | State |
|----------|-------|
| Damage authority | Server-side (sentinel protocol). Strong. |
| Input validation | Partial: `TargetID` bounded, `SkillIndex` bounded but off-by-one at 103, `Dam[].Damage` exact-match, tick window bounded, `Size` trusted from header. |
| Authentication / authorisation | Inherited from connection state (`USER_PLAY`); no per-action revalidation of account/character ownership. |
| Memory safety | One confirmed OOB read (`g_pSpell[103]`), one UB (`1 << 31`), one latent OOB (`pMob[PartyList[r]]`), no bounds check on `g_pGuildWar` beyond range comparison. |
| Integer safety | Widespread `int`/`long long` mixing; one deliberate widening at `:353`; several potential overflows (`100 * Level`, `familiarsanc * 350`, `dam * HpAbs`) mitigated only by the `MAX_DAMAGE` cap. |
| Cryptographic concerns | `rand()` used for all combat randomness — predictable, and the same PRNG stream is shared process-wide with every other subsystem. |
| Audit trail | Present but noisy (one line per hit), format-string unsafe (`Log`), and bypassable (`NumError` reset on revive). |
| Denial of service | One 13-slot packet can force 13 `Log` writes, 13 `GridMulticast`s and multiple `GetCurrentScore` recomputations; `GridMulticast` is O(observers). |

---

## 11. Test Coverage Analysis

### 11.1 Direct answer: there are no tests

**No test file, test project, test harness, assertion, mock, fixture or CI pipeline exists anywhere in the repository for this component or any other.** The evidence is as follows.

| Evidence | Observation |
|----------|-------------|
| `W2PP Code Project.sln` (lines 5-10) | Contains exactly two projects: `DBSrv` and `TMSrv`. No test project. |
| `Code/TMSrv/TMSrv.vcxproj` | `<ConfigurationType>Application</ConfigurationType>` for all four configurations; the only `ClCompile` entry for this component is `_MSG_Attack.cpp`. No `ProjectReference`, no test target. |
| Repository file census (excluding `.git`, `.agents`, `.codegraph`, `.opencode`, `openspec`, `.vs`) | 80 `.cpp`, 24 `.h`, 3 `.txt`, 3 extensionless, 4 `.md`, 233 `.obj`, 55 `.tlog`, 7 `.pdb`. No source file outside `Code/`. |
| Name filter `(?i)test\|spec\|mock\|fixture\|catch\|gtest\|unittest` across all non-ignored files | **0 matches.** |
| CI configuration (`*.yml`, `*.yaml`, `CMakeLists.txt`, `Makefile`, `*.mk`) | **0 files.** No `.github/`, no `azure-pipelines.yml`, no `.gitlab-ci.yml`, no `appveyor.yml`. |
| Build tooling | MSVC solution only (upgraded to v143 per `git log`: `dfe35d0 feat: upgrade solution to visual studio 2022 (v143)`). No CMake, no vcpkg manifest, no package manager. |
| Assertions in production code | No `assert()`, no `_ASSERT`, no `static_assert` on domain invariants anywhere in `Code/TMSrv`. |
| Documentation | Only 4 `.md` files: `README.md`, `docs/reports/MANIFEST.md`, the dependency audit report and the architectural report. `README.md`'s "Things to do" lists "Fix all bugs", "Create an documentation", "Re-create the project" — no testing item. |

### 11.2 Coverage table

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `Exec_MSG_Attack` (entry, gating, tick envelope) | 0 | 0 | 0% | None. No fixture, no fake socket, no `pUser`/`pMob` seeding harness. |
| Skill legality, class ownership, learned mask | 0 | 0 | 0% | None. `g_pSpell[]` requires `SkillData.bin`, which is absent from the repository. |
| Resource consumption (coin, mana) | 0 | 0 | 0% | None. |
| Physical damage resolution + crit + Huntress | 0 | 0 | 0% | None. `BASE_GetDamage`/`BASE_GetDoubleCritical` are pure-ish but depend on `rand()` and `g_pHitRate`, which is built from `ItemList.bin`. |
| Elemental damage + resistance + weather | 0 | 0 | 0% | None. Requires item list and map data. |
| Healing + heal-derived exp | 0 | 0 | 0% | None. |
| Utility skills (InstanceType 7-12) | 0 | 0 | 0% | None. |
| Skill-specific blocks (6, 22, 30, 31, 41, 44, 47, 79, 85, 98, 99) | 0 | 0 | 0% | None. |
| PvP / PK / guilt / parry | 0 | 0 | 0% | None. `GetGuilty`/`GetPKPoint` have hidden state in `MOB.Carry[63]` with no seam. |
| Damage post-processing pipeline | 0 | 0 | 0% | None. |
| Aggro / party propagation | 0 | 0 | 0% | None. |
| Kill dispatch / level segments | 0 | 0 | 0% | None. `MobKilled` and `CheckGetLevel` mutate global state. |
| **Component total** | **0** | **0** | **0%** | **None** |

### 11.3 Why the component is currently untestable

Four structural obstacles, in order of severity:

1. **No seam.** `Exec_MSG_Attack` reads and writes ten file-scope globals directly. There is no injection point for a fake `pUser`, `pMob`, `g_pSpell`, map grid or item grid. Any test would have to link the whole `TMSrv` executable and manipulate the actual globals.
2. **Deep transitive side effects.** A single call can reach `MobKilled` → drops → `CreateItem` → grid mutation → DB writes, and `CheckGetLevel` → level-up → bonus points → `DoItemLevel`. Verifying a damage number requires suppressing the entire reward subsystem.
3. **Non-determinism without control.** `rand()` is called at ~25 sites in this file, and the sequence is shared with every other subsystem in the process. `BASE_GetDoubleCritical` additionally mutates `pUser[conn].cProgress` through a pointer, so tests are order-dependent.
4. **Missing data assets.** All skill behaviour depends on `SkillData.bin`, which is XOR-0x5A obfuscated and not present in the repository (only the compiled `release/` artefacts). `MAX_SKILLINDEX`-sized `STRUCT_SPELL` arrays would be all zeros in a test run, silently turning every skill into "no instance type, no damage".

### 11.4 Observable behavioural evidence in lieu of tests

The only executable specification available is the running server. Two artefacts partially substitute for tests and should be treated as such:

* **`git log` for the file** — 5 commits, of which `f5d5fbe Update _MSG_Attack.cpp` is the only content change after the initial import. There is no history of bug-fix commits against specific rules, so no regression knowledge is encoded anywhere.
* **In-code assertions of intent** — the `#pragma region` names, the Portuguese/English comments (`//Personagem morto tentando usar uma skill que não é ressureição` at `:39`, `//10X HP MONSTRO` at `:1569`, `//Tempestade de flechas` at `:591`, `//Espectral` at `:1723`) and the log strings (`"err,mortar invalid"`, `"skill delay skillnum"`, `"party skill - skill:%d"`, `"err,dam not -2 -1 MSG_ATTACK"`) are the closest thing to a specification. Several of them (notably the two HP-monster comments) are contradicted by the code they annotate.

### 11.5 Recommended verification targets (analysis only, no implementation)

The following rules are the highest-value first targets for any future characterisation testing, ordered by the risk register: `g_pSpell[103]` bounds, the `HpAbs` assignment, the inert `skilldelay`, the accessory divisors, the learned-skill soft enforcement, the map-attribute inversion, and the `UNK2` double use. Each is a single-site, deterministic check that a table-driven harness over `g_pSpell` + `pMob` fixtures could cover exhaustively.

---

## 12. Analysis Limitations

| Limitation | Effect on this report |
|------------|-----------------------|
| `SkillData.bin`, `ItemList.bin`, map attribute data and `g_pGuildZone` configuration are absent from the repository | The *values* of `InstanceType`, `MaxTarget`, `bParty`, `Aggressive`, `AffectResist`, `Delay`, `ManaSpent` for each of the 103 skills are unknown. All statements about which skills take which branch are conditional on data not visible here. |
| Source is a decompilation | Some patterns (tautological comparisons, dead locals, unreachable intersections) are plausibly compiler artefacts rather than author errors. Where intent cannot be recovered, it is marked as an ambiguity rather than a defect. |
| No runtime access | All behaviour is derived statically. No packet capture, no live server, no debugger session. Rules that depend on client behaviour (e.g. whether clients send `dam == 0` slots in `MSG_Attack`) cannot be resolved. |
| Cross-component logic out of scope | `MobKilled` (74 KB), `_MSG_UseItem` (160 KB), `ProcessSecMinTimer` and the mob AI in `Server.cpp` are referenced only as integration points, not analysed. Kill consequences, potion effects and AI attack generation are therefore described at their boundary only. |
| Clan, guild and class-master semantics are not declared anywhere | Clan 4 (pets), 6 (protected), 7/8 (factions); `ClassMaster` 1–5; learned-skill bit constants — all inferred from usage. Each inference is stated as an inference. |
| Item indices are magic | 219/220/362 (immovable), 746 (mortar), 786/1936/1937 (fairy), 2360–2389 (adult mounts), 2330–2390 (mount equipment) are identified by number only; their names cannot be recovered without the item list. |

---

## 13. Summary of Referenced Files

| File | Lines referenced | Role |
|------|------------------|------|
| `Code/TMSrv/_MSG_Attack.cpp` | 1-1789 (all) | The component |
| `Code/TMSrv/ProcessClientMessage.cpp` | 38-64, 192-196 | Dispatcher and envelope validation |
| `Code/TMSrv/ProcessClientMessage.h` | 47-132 | Declaration, globals, include surface |
| `Code/TMSrv/Server.cpp` | 684-707, 2572-2643, 3975-4001, 4223-4298, 4622, 5062, 5371, 6701, 7218-7252, 7464, 8168-8210, 8270-8351, 8805, 8927-9050 | `AddCrackError`, `GenerateSummon`, socket entry, mount handling, AI re-entry, `Log`, `SetBattle`, hide/affect helpers, `DoItemLevel`, `MobAttack` |
| `Code/TMSrv/GetFunc.cpp` | 562-608, 610-640, 1257-1273, 1275-1560, 1564-1900, 1901-2000, 2105-2159 | `GetParryRate`, `GetEmptyAffect`, `GetAttribute`, `GetAttack`, `GetAttackArea`, `GetEmptyMobGrid`, PK/guilt accessors |
| `Code/TMSrv/GetFunc.h` | 46-47 | `GetAttack`, `GetAttackArea` declarations |
| `Code/TMSrv/CMob.cpp` | 209-304, 306-468, 630-679, 820-855, 1034-1152 | `BattleProcessor`, enemy list, score recomputation, `CheckGetLevel` |
| `Code/TMSrv/CMob.h` | 24-35, 37-120 | `CMob` fields used by the handler |
| `Code/TMSrv/MobKilled.cpp` | 41-143, 2095-2432 | Kill resolution (integration point) |
| `Code/TMSrv/CCastleZakum.cpp` | 210 | Castle kill hook |
| `Code/TMSrv/CWarTower.cpp` | 147-156 | `TowerAttack` policy |
| `Code/TMSrv/_MSG_PKMode.cpp` | 20-64 | PK mode toggling, zone rectangles |
| `Code/TMSrv/CUser.h` | 39-121 | Session fields consumed |
| `Code/TMSrv/SendFunc.cpp` | 333-346, 494, 620, 1055, 1136, 1590, 1622, 1647, 1737 | Outbound projection functions |
| `Code/Basedef.h` | 56-206, 483-599, 858-880, 2004-2133, 2380-2464 | Constants, `STRUCT_AFFECT`, `STRUCT_SPELL`, `MSG_Attack*`, `STRUCT_DAM`, `g_pSpell` |
| `Code/Basedef.cpp` | 744, 1174-1256, 1446-1477, 2095-2141, 2792-2826, 4247-4267, 5415-5421, 5466-5536, 5828-5849, 6342-6442 | `g_pSpell`, `BASE_GetDamage`, `BASE_GetSkillDamage`, `BASE_GetItemSanc`, `BASE_ReadSkillBin`, arena/village, `BASE_GetManaSpent`, `BASE_GetDoubleCritical`, `BASE_GetDistance`, skill damage by class |
| `W2PP Code Project.sln` | 5-10 | Proof of two-project solution (no tests) |
| `Code/TMSrv/TMSrv.vcxproj` | all | Proof of `Application` only (no tests) |
| `README.md` | 1-60 | Project provenance, build toolchain, debt acknowledged by the authors |

---

*End of report. This analysis was read-only; no project file was created, modified or deleted except this report.*


