# Component Deep Analysis Report

**Component:** Quest Handler
**Primary artifact:** `Code/TMSrv/_MSG_Quest.cpp` (2,712 lines, 73,906 bytes, single function `Exec_MSG_Quest`)
**Project:** W2PP (decompiled C/C++, Win32/MSVC, MMORPG server emulator for W2)
**Host process:** TMSrv (game / world server)
**Analysis date:** 2026-11-15 22:08:25
**Analysis scope:** entire project root `E:\wyd\test-setup\w2pp`, excluding `.agents`, `.codegraph`, `.opencode`, `openspec`

---

## 1. Executive Summary

`Quest Handler` is the TMSrv subsystem that implements every NPC-driven quest, ritual, teleport, exchange and progression service reachable by a player through the single client message `_MSG_Quest`. The entire component is one function, `Exec_MSG_Quest(int conn, char *pMsg)` at `Code/TMSrv/_MSG_Quest.cpp:20`, containing 2,712 lines of straight-line "transaction script" code organized as a single `switch` over a derived NPC mode value (35 named cases, IDs 1..35 of `Code/Basedef.h:240-274`, plus a `default` audit branch).

Its role in the system is that of an **NPC interaction engine**: it accepts a client-supplied NPC mob index plus a confirmation flag, resolves which quest service the NPC represents from two NPC properties (`MOB.Merchant` and the `EF_GRADE0` effect of the NPC's face item), validates player state and inventory, consumes items/coin/sapphires/fame/quest tickets, mutates persistent character structures (`pMob[conn].MOB`, `pMob[conn].extra`, `MOB.Carry`, `MOB.Equip`, `MOB.LearnedSkill`, `pMob[conn].QuestFlag`), grants rewards, teleports the player, and emits client projections plus asynchronous DB messages. It never persists state itself; persistence is delegated to DBSrv through `DBServerSocket.SendOneMessage` and the TMSrv character-save paths.

Key findings:

- **Single 2,712-line function, 35 branches, cyclomatic complexity of at least 39 decision points feeding the switch**; the component has no sub-functions, no per-quest abstraction and no shared validation layer beyond ad-hoc guard clauses.
- **No authority check on the NPC argument.** `npcIndex` is taken verbatim from the client (`_MSG_Quest.cpp:24`); only the domain `MAX_USER <= npcIndex < MAX_MOB` is validated (`:27-28`). There is no range/proximity check, no line-of-sight and no verification that the client is actually interacting with that NPC. Any authenticated client can drive any quest NPC on the map remotely.
- **No packet-size validation.** The message is cast to `MSG_STANDARDPARM2` (`:22`) and both `Parm1`/`Parm2` are read without consulting `m->Size`, unlike the header-level checks performed elsewhere in `ProcessClientMessage`.
- **Multiple correctness defects in reward logic**, most notably `GUARDA_REAL_EVT1`, which consumes the quest item and announces a reward that is never actually inserted into the inventory (`:2676-2701`, no `PutItem`), `BLACKORACLE` reading `Carry[i + 1]` where `i` may be `MaxCarry - 1` (`:2270`), `GOLD_DRAGON` indexing `g_pTreasure[x]` instead of `g_pTreasure[i]` for one rate tier (`:1512`), and `EXPLOIT_LEADER` computing a bonus value that is never written to the reward item (`:1667-1676`).
- **Economy-sensitive flows are only partially guarded.** Sapphire counters (`697` = 1 unit, `4131` = 10 units) are consumed with three different algorithms in three different branches, one of which can leave the requirement partially unpaid (`:1801-1818`).
- **Persistence is fire-and-forget.** `KING`, `KINGDOM` and `CombineTreasureMap` send DB messages and continue mutating local state without acknowledgement, rollback or idempotency handling.
- **Zero test coverage exists in the repository** — no test project, no test framework, no fixture files (evidence in section 11).

---

## 2. Data Flow Analysis

```
 1. Client sends _MSG_Quest  (type 139 | FLAG_CLIENT2GAME = 0x0200+139)
    payload: MSG_STANDARDPARM2 { Size, Type, ID, ClientTick, Parm1 = npcIndex, Parm2 = confirm }
 2. ProcessClientMessage.cpp:38  validates ID domain (:42-51), ServerDown (:53),
    ClientTick != SKIPCHECKTICK (:63), refreshes pUser[conn].LastReceiveTime (:57)
 3. ProcessClientMessage.cpp:184-186  dispatches to Exec_MSG_Quest(conn, pMsg)
 4. _MSG_Quest.cpp:22  cast to MSG_STANDARDPARM2, no size check
 5. _MSG_Quest.cpp:24-25  npcIndex = Parm1, confirm = Parm2
 6. _MSG_Quest.cpp:27-28  domain guard: MAX_USER (1000) <= npcIndex < MAX_MOB (25000)
 7. _MSG_Quest.cpp:32-33  read NPC identity:
       npcMerc  = pMob[npcIndex].MOB.Merchant
       npcGrade = BASE_GetItemAbilityNosanc(&pMob[npcIndex].MOB.Equip[0], EF_GRADE0)
 8. _MSG_Quest.cpp:35-46  trade-state guards (RemoveTrade both sides / auto-trade refusal)
 9. _MSG_Quest.cpp:51-160  map (npcMerc, npcGrade) -> npcMode (35 modes); -1 means "not a quest NPC" -> return
10. _MSG_Quest.cpp:164-2711 switch(npcMode): per-quest script
       a. preconditions (class, level, quest-state flags, inventory, coin, time windows, party state)
       b. consumption (BASE_ClearItem + SendItem, coin subtraction, sapphire stacks, ticket items)
       c. state mutation (QuestInfo.*, LearnedSkill, ScoreBonus/SpecialBonus, Equip/Carry, Coin, Fame,
          Citizen, KefraTicket, QuestFlag, guild tables)
       d. recomputation (GetCurrentScore, BASE_GetBonusScorePoint, BASE_GetBonusSkillPoint, BASE_GetHpMp)
       e. reward delivery (PutItem, Combine, SetItemBonus, BASE_SetItemSanc, BASE_SetItemDate)
       f. world effects (DoTeleport, SetAffect, MountProcess, CharLogOut, GridMulticast, GetCreateMob)
       g. projections (SendSay, SendClientMessage, SendItem, SendCarry, SendEtc, SendScore, SendEquip, SendEmotion)
       h. audit (Log to "-system" file or to the player's account/IP)
11. Async persistence (no ack / no rollback):
       KING            -> _MSG_DBCreateArchCharacter            (Server.cpp socket, :843-857)
       KING            -> _MSG_DBUpdateSapphire                 (:898-908, :1000-1010)
       KING            -> _MSG_GuildInfo                        (:950-967, :1094-1110)
       KINGDOM         -> _MSG_SetClan (grid multicast)         (:1225-1236)
       KIBITA          -> _MSG_GuildInfo                        (:2439-2455)
       GOLD_DRAGON     -> _MSG_MessageDBRecord via CombineTreasureMap (Server.cpp:8435-8445)
12. Downstream enforcement outside the component:
       ProcessSecMinTimer.cpp:1192-1209 recalls any player standing in a ticket-quest zone
       whose pMob[].QuestFlag does not match that zone
```

Reverse flow (state read back into the component on later invocations): `QuestInfo.Mortal.Newbie` (newbie chain state machine), `QuestInfo.Mortal.TerraMistica` (Terra Mistica chain), `QuestInfo.Circle` (Shama one-shot flag), `extra.Fame`, `extra.Citizen`, `extra.KefraTicket`, `MOB.Coin`, `MOB.LearnedSkill` bit 30, `MOB.Clan`, `Pista[]` arena registration table, and `pMob[].QuestFlag` written here and consumed by the 1-second timer.

---

## 3. Business Rules & Logic

### Overview of the business rules

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Entry guard | NPC index must satisfy `MAX_USER <= npcIndex < MAX_MOB`; otherwise silent return | `Code/TMSrv/_MSG_Quest.cpp:27-28` |
| Entry guard | An open trade with another player is force-closed for both sides and the quest aborts | `_MSG_Quest.cpp:35-40` |
| Entry guard | Auto-trade mode (`TradeMode != 0`) refuses the quest with `_NN_CantWhenAutoTrade` | `_MSG_Quest.cpp:42-46` |
| Resolution | NPC mode = f(Merchant id, face grade effect); unmapped pairs return silently | `_MSG_Quest.cpp:51-160` |
| Validation | Mount revival requires mount item 2330..2389 in slot 14 with life value 0 | `_MSG_Quest.cpp:169-181` |
| Business Logic | Mount revival costs the mount item's Price; success re-rolls life (`EF_MOUNTLIFE - rand()%3`), failure destroys the mount | `_MSG_Quest.cpp:183-231` |
| Reporting | Zakum NPC reports the number of players inside rectangle (2180,1160)-(2296,1270) minus one | `_MSG_Quest.cpp:237-246` |
| Business Logic | Terra Mistica quest acceptance requires MORTAL, `TerraMistica == 0`, `confirm != 0`, party leadership and >= 1 party member; sets `TerraMistica = 1` | `_MSG_Quest.cpp:252-286` |
| Business Logic | Ticket teleport quest 1 (Coveiro): MORTAL or ARCH, level 39..114, consume item 4038, set `QuestFlag = 1`, teleport to (2398,2105) | `_MSG_Quest.cpp:292-331` |
| Business Logic | Ticket teleport quest 2 (Jardineiro): level 115..189, consume 4039, `QuestFlag = 2`, (2234,1714) | `_MSG_Quest.cpp:337-375` |
| Business Logic | Ticket teleport quest 3 (Kaizen): level 190..264, consume 4040, `QuestFlag = 3`, (464,3902) | `_MSG_Quest.cpp:381-419` |
| Business Logic | Ticket teleport quest 4 (Hidra): level 265..319, consume 4041, `QuestFlag = 4`, (668,3756) | `_MSG_Quest.cpp:425-463` |
| Business Logic | Ticket teleport quest 5 (Elfos): level 320..349, consume 4042, `QuestFlag = 5`, (1322,4041) | `_MSG_Quest.cpp:469-507` |
| Business Logic | Jeffi circle upgrade: 447 -> random 448..450 for 1,000,000 coin; 692 -> random 693..695 for 5,000,000 coin | `_MSG_Quest.cpp:513-556` |
| Business Logic | Jeffi material combine: 10x item 419 -> 412 and/or 10x 420 -> 413 for a single 1,000,000 coin fee | `_MSG_Quest.cpp:557-604` |
| Validation | Shama skill reset requires circle item 448/449/450 (mode 1) or 693/694/695 (mode 2) equipped in slot 13 | `_MSG_Quest.cpp:611-630` |
| Business Logic | Shama converts up to 50 (mode 1) / 100 (mode 2) points of each special stat into `SpecialBonus` and wipes the matching skill tier | `_MSG_Quest.cpp:638-686` |
| Business Logic | King mantle upgrade: clan 7/8 gate, Sapphire cost from global `Sapphire` (inverted for clan 8), mantle item id remap per cape mode | `_MSG_Quest.cpp:692-1121` |
| Business Logic | King ideal-stone creation consumes stones 5334..5337 plus equipped 1742 and Sephirot 1760..1763 | `_MSG_Quest.cpp:765-836` |
| Business Logic | King Arch ascension at MORTAL level >= 299 with ideal stone + Sephirot equipped; defers to `_MSG_DBCreateArchCharacter` and parks the session in `MOB_WAITDB` | `_MSG_Quest.cpp:838-862` |
| Business Logic | Kingdom broker converts a clan mantle to its "kingdom-less" variant for 16 sapphires, weekdays only | `_MSG_Quest.cpp:1126-1239` |
| Business Logic | Urnammu sub-celestial respec: `CELESTIALCS`, Sephirot 1760..1763 equipped, `Fame >= 2000`, consumes 2,000 fame | `_MSG_Quest.cpp:1246-1306` |
| Validation | Rune quest (Uxmal) registration is blocked during minutes 0-15, 20-35, 40-55 of every hour | `_MSG_Quest.cpp:1317-1321` |
| Business Logic | Rune quest registration requires a 5134 ticket, party leadership, a free arena slot (2 slots in arena 0, 3 elsewhere), one registration per leader | `_MSG_Quest.cpp:1323-1390` |
| Business Logic | Arzan snowimp craft: 2,000,000 coin + 10x1721, 2x1722, 4x1723, 1x1724, 1x1725 -> item 1726 with `EF_SANC = rand()%4` | `_MSG_Quest.cpp:1396-1464` |
| Business Logic | Golden Dragon treasure map: 6-piece layout 788/789/790/791/792/793 at offsets 0,1,9,10,18,19 is consumed and replaced by a lottery roll | `_MSG_Quest.cpp:1470-1541` / `Server.cpp:8398-8507` |
| Business Logic | Golden Dragon crystal enchant: 7 crystals 421..427 upgrade the equipped weapon's bonus to `25*count/10 + ReqLvl`, only for sanctification 0 weapons | `_MSG_Quest.cpp:1544-1619` |
| Business Logic | Terra Mistica reward: level-tiered item (551-554 / 555-558 / 559-562) with three random effects; sets `TerraMistica = 3` | `_MSG_Quest.cpp:1639-1738` |
| Business Logic | Master Hab stat reset: up to 100 points per attribute returned to `ScoreBonus`, cost `StatSapphire` sapphires or item 3336, requires equipment slots 1..7 empty | `_MSG_Quest.cpp:1744-1888` |
| Business Logic | Newbie quest stage 1: MORTAL, level < `FREEEXP`, `Newbie == 0`, hold item 451 -> `Newbie = 1`, reward potion 682 (amount 20) | `_MSG_Quest.cpp:1894-1931` |
| Business Logic | Newbie quest stage 2: `Newbie == 1`, hold 452 -> `Newbie = 2`, weapon bonus rebuild `SetItemBonus(50 + ReqLvl)` for weapons with `ReqLvl <= 39` | `_MSG_Quest.cpp:1937-1985` |
| Business Logic | Newbie quest stage 3: `Newbie == 2`, hold 453 -> `Newbie = 3`, bonus + sanc 3 applied to slots 1..7 | `_MSG_Quest.cpp:1991-2039` |
| Business Logic | Newbie quest stage 4: `Newbie == 3`, hold 524 -> `Newbie = 4`, consumes 524, random reward 682 / 481 / 652 | `_MSG_Quest.cpp:2045-2095` |
| Business Logic | Sephirot creation: 8 distinct stones 1744..1751 + 30,000,000 coin -> item `1760 + NPC class` | `_MSG_Quest.cpp:2101-2160` |
| Business Logic | Capa Verde teleport: MORTAL, level 99..149 -> (2245,1576) | `_MSG_Quest.cpp:2166-2181` |
| Business Logic | Capa Verde graduation: MORTAL, level 99..149, `Equip[13] == 4080`, cape slot empty -> item 4006 | `_MSG_Quest.cpp:2187-2229` |
| Business Logic | Molar Gargula teleport: MORTAL, level 199..253 -> (817,4062) | `_MSG_Quest.cpp:2235-2250` |
| Business Logic | Black Oracle merge: items 1740 + 1741 in adjacent inventory slots + 10 sapphires -> item 1742 | `_MSG_Quest.cpp:2256-2335` |
| Business Logic | Quest Capa Real teleport: MORTAL, level 199..253 -> (1740,1725) | `_MSG_Quest.cpp:2341-2356` |
| Business Logic | Carbuncle buff: MORTAL or ARCH, level < 100, applies up to 4 NPC skill-bar effects for 400 seconds | `_MSG_Quest.cpp:2362-2393` |
| Business Logic | Perzen exchange: the item matching NPC `Carry[0]` becomes NPC `Carry[1]` with a 30-day lifetime | `_MSG_Quest.cpp:2399-2425` |
| Business Logic | Kibita citizenship: 4,000,000 coin sets `Citizen = ServerIndex + 1` and syncs the guild record | `_MSG_Quest.cpp:2431-2461` |
| Business Logic | Kibita "Limit of Soul": MORTAL, level >= 369, skill bit 30 unset, class stone consumed -> bit 30 set, mantle 3194/3195/3196 by clan, forced logout | `_MSG_Quest.cpp:2516-2558` |
| Business Logic | Government PK reset: every 100 kill points convert into +20 PK points capped at 150 | `_MSG_Quest.cpp:2566-2592` |
| Business Logic | Survivor exchange: item 4127 -> `KefraTicket += 100` | `_MSG_Quest.cpp:2599-2619` |
| Business Logic | Arch apprentice graduation: ARCH, base level 99..149, cape slot empty -> item 4006 | `_MSG_Quest.cpp:2626-2659` |
| Business Logic | Royal Guard event exchange: item 4055 -> random 412 or 413 (reward never delivered) | `_MSG_Quest.cpp:2665-2704` |
| Audit | Unknown NPC mode logs `etc,quest <merc> <grade>` with account and IP | `_MSG_Quest.cpp:2707-2710` |
| Enforcement | Players inside a ticket-quest zone without the matching `QuestFlag` are recalled | `Code/TMSrv/ProcessSecMinTimer.cpp:1192-1209` |

---

### Detailed breakdown of the business rules

---

### Business Rule: Packet entry and NPC index domain validation

**Overview**:
`_MSG_Quest` is the sole entry point of the component. The handler extracts two integers from the client payload and accepts the NPC argument only if it falls in the mob index space.

**Detailed description**:
`Exec_MSG_Quest` casts the raw buffer to `MSG_STANDARDPARM2` (`_MSG_Quest.cpp:22`), which the wire definition places at `Code/Basedef.h:954-959` as `{MSG header, int Parm1, int Parm2}`. `Parm1` is interpreted as `npcIndex` and `Parm2` as `confirm`. The only domain check is `if (npcIndex < MAX_USER || npcIndex >= MAX_MOB) return;` (`:27-28`) with `MAX_USER = 1000` and `MAX_MOB = 25000` (`Basedef.h:56,107`), which prevents a client from addressing another player's mob slot as an NPC but does not constrain which NPC may be addressed. The dispatcher has already validated `std->ID` as a user index and that `ServerDown < 120` (`ProcessClientMessage.cpp:42-54`), yet there is no `pUser[conn].Mode == USER_PLAY` check and no `m->Size` check before `Parm1`/`Parm2` are dereferenced.

Because the NPC index is client-authored, the handler is effectively a remote-procedure endpoint keyed by world entity index: any NPC on the current map instance can be invoked regardless of the player's coordinates, and repeated invocations are unthrottled. The NPC's own coordinates and the caller's coordinates are never compared, and the handler never validates that the NPC is alive, visible, or a quest NPC before entering the mode switch.

Persisted consequences of a successful invocation are applied directly to `pMob[conn]` and `pUser[conn]`, so the entry guard is the only authority boundary of the entire subsystem.

**Rule workflow**:
1. Dispatcher validates header ID, server-down state and `ClientTick`.
2. Handler reads `Parm1`, `Parm2` without a size check.
3. Guard rejects indices `< 1000` or `>= 25000` and returns with no client feedback.
4. Handler reads the NPC merchant id and the `EF_GRADE0` effect of `pMob[npcIndex].MOB.Equip[0]`.
5. Execution continues to the trade guard and the mode resolution; unmapped NPCs return silently.

---

### Business Rule: Trade-state guards

**Overview**:
A quest interaction may not run while the player is trading with another player or while the player is in auto-trade mode.

**Detailed description**:
Two distinct trade states are handled. If `pUser[conn].Trade.OpponentID` is non-zero the handler calls `RemoveTrade` for both the opponent (`:37`) and the caller (`:38`) and then returns without any message (`:39`). This is a destructive side effect rather than a refusal: the open trade session is terminated as a consequence of a quest packet. If `pUser[conn].TradeMode` is non-zero the handler instead refuses politely with `_NN_CantWhenAutoTrade` (`:42-46`) and returns.

The asymmetry is deliberate-looking but undocumented: an interactive trade is cancelled, whereas an automated trade (player vendor / auto-trade mode) merely blocks the quest. Neither branch logs an audit record, so the cancellation from line 37-38 leaves no trace of why the trade ended.

Both checks occur after the NPC index validation but before NPC identity resolution, therefore they protect every one of the 35 quest branches uniformly.

**Rule workflow**:
1. `Trade.OpponentID != 0` -> `RemoveTrade(opponent)`, `RemoveTrade(conn)`, return (silent).
2. `TradeMode != 0` -> send `_NN_CantWhenAutoTrade`, return.
3. Otherwise continue to NPC mode resolution.

---

### Business Rule: NPC mode resolution (merchant id + face grade)

**Overview**:
The quest service executed is determined by a two-key lookup over the NPC's `MOB.Merchant` value and the grade effect of the NPC's face item; the result is one of 35 `npcMode` constants.

**Detailed description**:
`npcMerc = pMob[npcIndex].MOB.Merchant` and `npcGrade = BASE_GetItemAbilityNosanc(&pMob[npcIndex].MOB.Equip[0], EF_GRADE0)` (`:32-33`). `EF_GRADE0` is item effect 100 (`Code/ItemEffect.h:136`), so grade 0..5 correspond to `EF_GRADE0..EF_GRADE5`. The resolution block (`:51-158`) is a flat sequence of 38 independent `if` statements rather than a `switch` or table. Merchant `100` is the "generic quest NPC" class whose grade selects the service (grade 0 Coveiro, 1 Jardineiro, 2 Kaizen, 3 Hidra, 4 Elfos, 5 Lider Aprendiz, 7/8/9 Perzen, 13 Capa Real, 14 Capa Verde teleport, 15 Molar Gargula, 16 Newbie trainer 4, 22 Survivor, 30 Royal Guard event); other merchant ids map one-to-one (72 Uxmal, 36/40/41 newbie trainers 1..3, 8 Capa Verde trade, 78 Black Oracle, 120 Carbuncle, 4 Golden Dragon, 10 Mystic Amulet, 11 Exploit Leader, 12 Jeffi, 13/14/15 Shama/King, 19 Sephirot component, 26 Kingdom, 30 Zakum, 31 Master Hab, 68 Government, 58 Mount Master, 62 Arzan Dragon, 76 Urnammu, 74 Kibita).

Because the branches are not chained with `else if`, later assignments overwrite earlier ones whenever two conditions hold simultaneously, but in practice the merchant keys are mutually exclusive except for the merchant-100 grade family, whose conditions each require merchant 100 and a distinct grade. Grades 6, 10, 11, 12, 17..21, 23..29 and 31+ produce `npcMode == -1` and the function returns silently (`:159-160`), which is the defined behaviour for "this NPC has no quest service".

Merchant 14 and merchant 15 both resolve to `KING`, making two different merchants behave identically; there is no comment explaining why two ids are aliased.

**Rule workflow**:
1. Read `Merchant` and face grade effect.
2. Apply 38 independent mapping predicates, last match wins.
3. If no predicate matched (`npcMode == -1`), return silently.
4. Otherwise dispatch to `switch (npcMode)`.

---

### Business Rule: Mount Master mount revival (mode 12)

**Overview**:
Restores a dead mount occupying equipment slot 14 by charging the mount item's price and re-rolling its life; an unlucky roll destroys the mount permanently.

**Detailed description**:
The handler requires that slot 14 contains a mount (`sIndex` in `2330..2389`, `:171-175`) and that `stEffect[0].sValue` (the mount's "dead" marker) is greater than zero (`:177-181`); otherwise the NPC says `_NN_Cure_animals`. With `confirm == 0` the NPC only quotes the operation with `_DS_S_cure_price_D` formatted from the item name and `g_pItemList[mountIndex].Price` (`:185-190`), which is the standard two-step confirm protocol used by several branches.

On confirmation the fee is paid from `MOB.Coin` (`:192-201`) with a sanity bound rejecting prices outside `0..2,000,000,000` immediately before the deduction (`:198-199`). The new life value is `BASE_GetItemAbility(&Equip[14], 79) - rand()%3`, i.e. `EF_MOUNTLIFE` minus 0..2 (`:203-205`). If the result is positive the mount is marked alive (`stEffect[0].sValue = 20`), its life is written to `stEffect[1].cValue`, `stEffect[2].cEffect` is set to 5 with no value (`:207-213`), and the client receives `_NN_Cured`. If the result is zero or negative the NPC says `_NN_Cure_failed` and the mount item is zeroed by `memset` (`:214-218`), destroying it while the fee is still charged.

The branch then recomputes scores, re-projects the slot, calls `MountProcess(conn, 0)` to re-synchronize the summon/mount entity, and writes an audit line containing the mount index, the rolled life and the (unset) effect value (`:221-231`).

**Rule workflow**:
1. Reject if slot 14 is not a mount or the mount is alive.
2. If `confirm == 0`, quote the price and stop.
3. Reject if coin is insufficient or the price is outside the sanity bound.
4. Deduct the price; roll `life - rand()%3`.
5. Positive roll: restore the mount; non-positive roll: destroy the mount.
6. Recompute score, resend slot 14, run `MountProcess`, `SendEtc`, write audit log.

---

### Business Rule: Zakum area census (mode 13)

**Overview**:
A read-only report of how many players currently stand inside the Zakum staging rectangle.

**Detailed description**:
The branch calls `GetUserInArea(2180, 1160, 2296, 1270, UserNameArea)` (`:239`) and formats either `_NN_Zakum_Quest_empty` when the count is below 1 or `_SD_Zakum_Quest_by_S_N` with the last collected player name and `count - 1` (`:241-246`). `GetUserInArea` (`Server.cpp:4157-4176`) iterates `pUser[0..MAX_USER)`, requires `Mode == USER_PLAY` and `pMob[i].Mode != 0`, and copies each matching name into the caller's buffer, so only the final matching name survives — the message therefore always shows one arbitrary participant while the count is correct.

The `count - 1` subtraction presumes the requester is inside the rectangle; if the requester is outside, the reported number is one less than reality. No state is mutated and no audit is written, making this the only purely informational branch in the component.

**Rule workflow**:
1. Count `USER_PLAY` mobs inside the rectangle.
2. `count < 1` -> empty message.
3. Otherwise -> message with last seen name and `count - 1`.
4. `SendSay` to the NPC location.

---

### Business Rule: Terra Mistica quest acceptance (mode 2)

**Overview**:
Starts the "watch the town" quest line for mortal characters by setting a persistent quest flag, gated on party leadership.

**Detailed description**:
The branch refuses when `QuestInfo.Mortal.TerraMistica != 0` or when the player is not `MORTAL` (`:252-256`), meaning the quest can be taken exactly once per character and only before class change. With `confirm == 0` the NPC delivers the narrative prompt only (`:258-262`). On confirmation the player must be a party leader: `if (pMob[conn].Leader)` rejects any member whose `Leader` field is non-zero (`:264-268`), relying on the convention that a party leader has `Leader == 0` (or `-1` for a non-party mob, per the same convention used in `_MSG_Action.cpp:184` and `MobKilled.cpp`). At least one party member must exist in `PartyList` with an index in `1..MAX_USER` (`:270-280`).

When all checks pass, `QuestInfo.Mortal.TerraMistica` is set to 1 (`:282`), the player receives `_NN_Get_Watching_Town_Mission`, and an audit line `etc,mysticalland complete. name:<name>` is written to the system log (`:283-286`). Completion is handled by the Exploit Leader branch, creating a 0 -> 1 -> 2 -> 3 state machine across two NPCs.

**Rule workflow**:
1. Reject if `TerraMistica != 0` or class is not MORTAL.
2. If `confirm == 0`, show the prompt and stop.
3. Reject if the caller is not the party leader or the party is empty.
4. Set `TerraMistica = 1`, notify, log.

---

### Business Rule: Ticket teleport chain (modes 20, 21, 22, 23, 24)

**Overview**:
Five structurally identical NPCs each exchange one specific ticket item for a teleport into a level-banded hunting zone, stamping a `QuestFlag` that the timer subsystem uses to admit or expel the player.

**Detailed description**:
All five branches share the same algorithm: class restriction `MORTAL` or `ARCH` (`:292,337,381,425,469`), an inclusive lower and exclusive upper level bound expressed as two ternary expressions whose branches are identical (the `ClassMaster` ternary is redundant because both arms contain the same literal), a linear scan of `MOB.Carry` for the required item index, `BASE_ClearItem` + `SendItem` on match, and `DoTeleport` to a fixed coordinate jittered by `rand()%5-3` (which yields -3..+1, not a symmetric -3..+3). If the item is absent the NPC answers with `_SN_BRINGITEM` formatted with `g_pItemList[item].Name`.

| Mode | NPC (merchant/grade) | Classes | Level band | Ticket | QuestFlag | Destination |
|------|----------------------|---------|------------|--------|-----------|-------------|
| 20 | 100 / grade 0 (Coveiro) | MORTAL, ARCH | 39..114 | 4038 | 1 | (2398,2105) |
| 21 | 100 / grade 1 (Jardineiro) | MORTAL, ARCH | 115..189 | 4039 | 2 | (2234,1714) |
| 22 | 100 / grade 2 (Kaizen) | MORTAL, ARCH | 190..264 | 4040 | 3 | (464,3902) |
| 23 | 100 / grade 3 (Hidra) | MORTAL, ARCH | 265..319 | 4041 | 4 | (668,3756) |
| 24 | 100 / grade 4 (Elfos) | MORTAL, ARCH | 320..349 | 4042 | 5 | (1322,4041) |

The `QuestFlag` is the admission token enforced outside the component: `ProcessSecMinTimer.cpp:1192-1209` recalls any player whose `TargetX/TargetY` falls inside the corresponding rectangle and whose `QuestFlag` differs from the zone number, with the additional condition `BaseScore.Level < 1000`. `ProcessSecMinTimer.cpp:535-536` also resets `QuestFlag` to 0 when the player leaves the play state, and `Server.cpp:5749` resets it on mob initialisation, so the flag is a transient, non-persisted pass. The audit line `etc,quest256 ticket <name>` is written for each successful ticket use (`:330,374,418,462,506`).

**Rule workflow**:
1. Verify class is MORTAL or ARCH, else `_NN_Level_Limit2`.
2. Verify level inside the band, else `_NN_Level_limit`.
3. Scan inventory for the ticket; on absence answer `_SN_BRINGITEM` and stop.
4. Consume ticket, project the slot.
5. Set `QuestFlag = <zone>`.
6. `DoTeleport` to the zone with a -3..+1 jitter.
7. Write the audit line.

---

### Business Rule: Jeffi circle upgrade (mode 14, upgrade path)

**Overview**:
Upgrades a "circle" (class-change token) equipped in slot 13 to a random variant for a fixed coin fee.

**Detailed description**:
If slot 13 contains item 447 the fee is 1,000,000 and the result is `rand()%3 + 448` (448..450); if it contains 692 the fee is 5,000,000 and the result is `rand()%3 + 693` (693..695) (`:513-537`). The `else` arm re-tests `!= 692` and breaks, so other equipped items fall through to the material-combine path documented in the next rule. If `Coin >= price` the fee is deducted, the new index is written in place (effects are not cleared or re-rolled), the slot and etc panel are re-projected, the NPC answers `_NN_Processing_Complete`, `SetAffect(conn, 44, 20, 20)` plays effect 44 for 20 units at level 20, `SendScore` refreshes derived stats, and an audit line `etc,jeffi make reset50-100` is written (`:540-547`). If coin is insufficient the NPC names the exact amount via `_NN_Need_1000000_Gold` or `_NN_Need_5000000_Gold` (`:549-555`).

The upgrade is lossless with respect to the item: only `sIndex` changes, so any sanctification or bonus effects previously stored on the circle survive the transformation. There is no class or level restriction and no per-character flag, so the operation can be repeated indefinitely as long as the player can afford it and obtains another base circle.

**Rule workflow**:
1. If slot 13 is 447 -> price 1,000,000, result 448..450.
2. Else if slot 13 is 692 -> price 5,000,000, result 693..695.
3. Else fall through to the material-combine path.
4. Compare coin; on success deduct, rewrite `sIndex`, re-project, affect, score, log; on failure send the specific "need gold" message.

---

### Business Rule: Jeffi material combine (mode 14, combine path)

**Overview**:
Converts ten units of material 419 into one 412 and ten units of 420 into one 413, for a single flat fee of 1,000,000 coin per interaction.

**Detailed description**:
When slot 13 holds neither upgrade item, the handler counts stackable material in the inventory (`:562-569`): for each carry slot whose `sIndex` is 419 it adds `stEffect[0].cValue` when `stEffect[0].cEffect == EF_AMOUNT` (61) or 1 otherwise; the same for 420. If neither counter reaches 10 the NPC answers `_NN_Need_10_Particle` (`:602-603`). If enough material exists but `Coin < 1,000,000` the NPC answers `_NN_Need_1000000_Gold` (`:598-599`).

With both conditions satisfied the handler enters `for (; Ori >= 10 || Lac >= 10;)` and calls `Combine(conn, 419, 412)` or `Combine(conn, 420, 413)` per batch, decrementing the local counters by 10 each time (`:575-587`). `Combine` (`Server.cpp:8509-8589`) re-scans the inventory, requires at least 10 units, removes units in slot order (decrementing `EF_AMOUNT` stacks larger than 10 by 10, otherwise clearing the slot) and writes the result into the first empty slot, or drops it on the ground at `TargetX/TargetY` when the inventory is full. After the loop the NPC says `_NN_Processing_Complete`, deducts the single 1,000,000 fee, applies effect 44, refreshes score/carry/etc and logs `etc,jeffi make lac_ori` (`:588-596`).

The loop trusts `Combine` to reduce inventory material; if `Combine` returns early (for example because the counted stack semantics differ from `Combine`'s own internal recount), the counters are still decremented, so termination is guaranteed by the local counters rather than by inventory state — the loop cannot hang, but it can report success while producing fewer rewards than the counters imply.

**Rule workflow**:
1. Count 419 and 420 units across the inventory.
2. Reject if both counts are below 10; reject if coin < 1,000,000.
3. Loop while either count >= 10: call `Combine` for the corresponding batch, decrement by 10.
4. Announce completion, deduct 1,000,000 once, apply effect, refresh projections, log.

---

### Business Rule: Shama skill reset (mode 15)

**Overview**:
Consumes a circle item to convert special stat points into `SpecialBonus` and to wipe one tier of learned skills.

**Detailed description**:
The equipped circle in slot 13 must be 448/449/450 (mode 1, `ncircle = sIndex - 448`) or 693/694/695 (mode 2, `ncircle = sIndex - 693`); anything else yields `_NN_Need_Pure_Divine` (`:611-630`). Mode 1 additionally refuses when `QuestInfo.Circle == 1`, making the first-tier reset a once-per-character operation; mode 2 has no such flag (`:632-636`). The conversion threshold is `specialreset = 50` for mode 1 and `100` for mode 2 (`:638-641`).

For each of the four special stats the handler adds `min(value, specialreset)` to `MOB.SpecialBonus` and subtracts the same amount from `MOB.BaseScore.Special[i]`; the comparison uses `<=` so a stat exactly equal to the threshold is fully drained (`:645-657`). The recomputed total is written back to `MOB.SpecialBonus` (`:659`). The skill mask is then cleared by tier: `ncircle == 0` clears the low byte (`0xFFFFFF00`), `1` clears the second byte (`0xFFFF00FF`), `2` clears the third (`0xFF00FFFF`) of `MOB.LearnedSkill` (`:664-671`), and `BASE_GetBonusSkillPoint` recomputes the skill point pool (`:673`).

Finally the circle item is destroyed (`memset` of slot 13 + `SendItem`), the NPC says `_NN_Initialize_Skill`, effect 44 is applied, score is refreshed, the client receives `_NN_Qest06Complete`, and the audit line records item id, mode and tier with the player's account and IP (`:675-685`).

**Rule workflow**:
1. Resolve mode/tier from slot 13; reject unknown items.
2. Reject mode 1 if already used (`QuestInfo.Circle == 1`).
3. Convert up to 50 (mode 1) / 100 (mode 2) points of each special stat into `SpecialBonus`.
4. Clear the skill byte matching the tier; recompute skill points.
5. Destroy the circle, apply effect, refresh score, notify, log.
6. Mode 1 marks `QuestInfo.Circle = 1`.

---

### Business Rule: King mantle upgrade, ideal stone, Arch ascension (mode 16)

**Overview**:
The largest branch (432 lines) implements four distinct services: ideal-stone assembly, mortal-to-Arch ascension, celestial mantle upgrade, and mortal mantle upgrade, all gated by clan identity and a sapphire currency.

**Detailed description**:
The branch first derives the NPC's clan (`ReiClan = pMob[npcIndex].MOB.Clan`) and the player's effective clan, overriding `MOB.Clan` from the cape in slot 15 according to a 12-entry table (543/545/734/736/3191/3194/3197 -> 7; 544/546/735/737/3192/3195/3198 -> 8) and deriving `CapeMode`: 2 for "charged/kingdom" capes (543/544/3191/3192/3194/3195/3197/3198), 1 for the intermediate capes (545/546/549/3193/3196), 0 otherwise (`:692-734`). A player whose effective clan is neither 0 nor `ReiClan` is rejected silently (`:736-737`). The sapphire price is the global `Sapphire` (`Server.cpp:51`, default 8) with an inverse lookup applied when `ReiClan == 8`: 1->60, 2->32, 4->16, 16->4, 32->2, 64->1 (`:740-756`). With `confirm == 0` the NPC quotes the price via `_DN_Need_D_Sapphire` (`:758-763`).

The ideal-stone service requires `Equip[10] == 1742` and `Equip[11]` in `1760..1763` plus one each of 5334 (water), 5335 (earth), 5336 (sun) and 5337 (wind) in the inventory (`:765-788`). It rewrites the water-stone slot in place to 5338 (the resulting ideal stone), clears the three other elemental stones, then clears `Equip[10]` and `Equip[11]` and answers `_NN_My_King_Bless1` (`:790-836`). Arch ascension re-uses the same two equipped items: for a MORTAL at level >= 299 whose `Mode != MOB_WAITDB`, the handler computes `cls = Equip[11] - 1760`, fills `MSG_DBCreateArchCharacter` with class, `MortalFace`, `MortalSlot` and name, sets `pMob[conn].Mode = MOB_WAITDB` and sends the message to DBSrv (`:838-862`); the actual class change is completed asynchronously by `ProcessDBMessage`, and the wait state prevents double submission.

For `CapeMode >= 2` the branch emits a blessing message and stops (`:865-873`). Celestial classes (3, 4, 5) must wear cape 3199 and pay `Saphire` units of sapphire (item 697 = 1 unit, 4131 = 10 units); the count is validated, `_MSG_DBUpdateSapphire` is sent with `Parm = (ReiClan == 7)`, stacks are consumed, and the cape becomes 3197 (clan 7) or 3198 (clan 8) (`:875-943`). When the player is a guild master (level 9) the guild record is rewritten with the new clan and `Fame = 0` and pushed to DBSrv (`:945-967`). The mortal path requires level >= 219 and, when `CapeMode != 1` or level >= 255, repeats the sapphire payment (waived when `Equip[13] == 4081`, in which case that slot is cleared instead), then maps the cape: `CapeMode == 0` -> 545/546 (preserving 548), `CapeMode == 1` -> 3191/3192 from 3193, 3194/3195 from 3196, otherwise 543/544 (preserving 549) (`:974-1115`). A mortal with `CapeMode == 1` and level 219..254 receives `_NN_Need_Level` instead.

Every successful path ends with `SendItem` of slot 15, the clan blessing message, the optional guild synchronization, and an audit line `etc,get mantle <item> - sapphire:<remaining>` carrying account and IP.

**Rule workflow**:
1. Derive `ReiClan`, effective clan and `CapeMode` from the cape table.
2. Reject non-matching clan silently.
3. Convert the sapphire price for clan 8; if `confirm == 0`, quote and stop.
4. Ideal-stone path: validate 1742 + Sephirot + four elemental stones; merge; clear equipment; stop.
5. Arch path: MORTAL level >= 299 -> send `_MSG_DBCreateArchCharacter`, park in `MOB_WAITDB`, stop.
6. Charged cape (`CapeMode >= 2`) -> blessing message, stop.
7. Celestial path: cape 3199 + sapphires -> 3197/3198, guild sync, stop.
8. Mortal path: level >= 219 -> payment (or 4081 waiver) -> mantle remap -> guild sync -> log.

---

### Business Rule: Kingdom mantle broker (mode 17)

**Overview**:
Converts an owned kingdom mantle back to its "kingdom-less" counterpart for 16 sapphires, available only on weekdays to members of clan 7 or 8.

**Detailed description**:
The branch rejects Saturday and Sunday via `localtime`/`tm_wday == 6 || tm_wday == 0` with `_NN_NotEquip_Saturday` (`:1129-1139`). It then tests `if (Clan != 7 && Clan != 8)` and inside that block re-tests `if (Clan == 7)`, an unreachable condition, so every rejection emits `_NN_My_King_Bless2` regardless of the player's actual clan (`:1141-1149`). With `confirm == 0` the NPC only warns that the player is leaving the kingdom (`:1151-1156`).

The price is hard-coded at 16 sapphires, counted as 697 = 1 and 4131 = 10, validated and then consumed (`:1157-1198`). The cape is then remapped to its kingdom-less variant: 543/544 -> 549, 545/546 -> 548, 3191/3192 -> 3193, 3194/3195 -> 3196, 3197/3198 -> 3199 (`:1201-1214`). After the remap the handler sends the blessing message, projects slot 15, recomputes scores, builds a `_MSG_SetClan` grid message whose `Parm` is the player's *current* `MOB.Clan`, mutates `m->ClientTick` on the incoming packet buffer (`:1232`), multicasts it, sends the full equipment list and writes the audit line `etc,remove mantle` (`:1216-1238`).

Local `MOB.Clan` is never cleared in TMSrv; the clan change is expected to be derived from the new cape id or reconciled by DBSrv. This intent is not documented in the component and is stated here as an inference from the code shape.

**Rule workflow**:
1. Reject weekend days.
2. Reject clans other than 7/8 (message is always the Bless2 text).
3. If `confirm == 0`, warn and stop.
4. Require and consume 16 sapphires.
5. Remap cape id to the kingdom-less variant.
6. Notify, project equipment, recompute score, multicast `_MSG_SetClan`, `SendEquip`, log.

---

### Business Rule: Urnammu sub-celestial respec (mode 18)

**Overview**:
Rebuilds the secondary celestial slot (index 1 of `SaveCelestial`) from the class encoded in the equipped Sephirot, at the cost of 2,000 fame.

**Detailed description**:
The branch requires `ClassMaster == CELESTIALCS` (4) and answers `_SN_NEEDCLASS` with "Sub Celestial" otherwise (`:1246-1251`). The Sephirot must be equipped in slot 11 with index `1760..1763`, otherwise `_SN_WANTEQUIPITEM` (`:1252-1257`). Fame must be at least 2,000, otherwise `_DN_NEEDFAME` (`:1258-1263`).

On success the handler logs the transition, subtracts 2,000 fame, sets `SaveCelestial[1].Class` from the Sephirot index, copies the class base scores from `BaseSIDCHM[MOB.Class][0..3]`, clears `SkillBar1` (4 bytes) and `SkillBar2` (16 bytes), and redistributes special stats with a 55-point threshold into `SpecialBonus` (`:1265-1297`). `LearnedSkill` is masked with `0xFF000000`, discarding the low 24 bits (all normal skill tiers), the Sephirot is destroyed, and the NPC answers `_NN_My_King_Bless1` (`:1299-1305`).

The branch has no `confirm` step, so a single packet both pays and applies the respec; the only protection against accidental use is the class and equipment requirement.

**Rule workflow**:
1. Require class `CELESTIALCS`; require Sephirot 1760..1763 equipped; require fame >= 2000.
2. Log the class transition.
3. Deduct 2,000 fame.
4. Rebuild `SaveCelestial[1]` class, base scores, special bonus (55 threshold) and skill masks.
5. Destroy the Sephirot; notify.

---

### Business Rule: Rune quest registration (mode 25, Uxmal)

**Overview**:
Registers a party leader into one of seven rune-quest arenas using a ticket whose sanctification selects the arena, inside a narrow repeating time window.

**Detailed description**:
The branch rejects requests whose `tm_min` lies in 0-15, 20-35 or 40-55, leaving only minutes 16-19, 36-39 and 56-59 as valid (`:1312-1321`). It then requires the caller to be a party leader (`pMob[conn].Leader != -1 && pMob[conn].Leader` rejects members) (`:1323-1327`). The ticket is item 5134; its sanctification (`BASE_GetItemSanc`) is clamped to 6 and used as the arena index `sala`, so `Pista[0..6]` is addressed with the ticket's upgrade level (`:1329-1344`).

Capacity depends on the arena: arena 0 accepts 2 parties, all others 3 (`:1354-1364`). If no free party slot exists the NPC answers `_NN_Night_Limited`; if this leader is already registered in any of the three slots the NPC answers `_NN_Night_Already` (`:1366-1376`). Registration writes `LeaderID`, `MobCount = 0`, `Sala` and `LeaderName` into `Pista[sala].Party[j]`, consumes the ticket and answers `_NN_TicketUsed` (`:1378-1389`). The audit line records arena type, connection id, name and level.

`Pista` is a process-global table (`Server.cpp:448`, `STRUCT_PISTA Pista[7]`, `Basedef.h:646-656`) shared with `MobKilled.cpp` (which increments `MobCount` when rune-quest towers die) and `_MSG_Action.cpp` (which validates arena entry), so this branch is the sole producer of arena registrations and the correctness of the whole rune-quest feature depends on it.

**Rule workflow**:
1. Reject inside the blocked minute windows.
2. Reject non-leaders.
3. Find ticket 5134; clamp its sanc to 6 as the arena id; reject if missing (`_SN_BRINGITEM`).
4. Find a free party slot (2 for arena 0, 3 otherwise); reject if full.
5. Reject duplicate registration of the same leader.
6. Write registration, consume ticket, notify, log.

---

### Business Rule: Arzan snowimp craft (mode 5)

**Overview**:
Combines five material types and 2,000,000 coin into a snowimp pet item with a random sanctification.

**Detailed description**:
The branch requires `Coin >= 2,000,000` (`:1396-1400`) and then counts inventory items `1721..1725` into a 5-element array (`:1402-1412`). The recipe is 10x1721, 2x1722, 4x1723, 1x1724 and 1x1725, validated with `<` comparisons and answered with `_NN_Need_5_materials` when short (`:1414-1418`). Counts above the requirement are clamped so only the recipe amount is consumed (`:1420-1433`), and the consumption nested loop removes exactly the clamped quantity per material type by scanning the inventory from slot 0 (`:1435-1446`).

After consumption the fee is deducted, `SendEtc` refreshes the coin display, and the result item 1726 is created with `stEffect[0].cEffect = EF_SANC (43)` and `stEffect[0].cValue = rand()%4` (`:1448-1459`), so the pet is produced at sanctification 0..3. `PutItem` places it in the first empty slot or reports "no space"; the NPC then answers `_NN_snowimp_create_success` and the audit line records the crafter and level (`:1461-1464`).

**Rule workflow**:
1. Require 2,000,000 coin.
2. Count materials 1721..1725.
3. Validate the 10/2/4/1/1 recipe; clamp excess.
4. Consume exactly the recipe quantities.
5. Deduct the fee, create item 1726 with `EF_SANC = rand()%4`, `PutItem`.
6. Notify and log.

---

### Business Rule: Golden Dragon treasure maps and crystal enchant (mode 1)

**Overview**:
Two independent services: assembling six map fragments into a lottery roll, and consuming seven elemental crystals to re-roll a weapon's bonus.

**Detailed description**:
The branch first calls `CombineTreasureMap(conn)` and returns immediately when it reports success (`:1470-1473`). `CombineTreasureMap` (`Server.cpp:8398-8507`) searches for a 2-wide, 3-tall fragment layout at contiguous offsets 0,1,9,10,18,19 holding items 788,789,790,791,792,793; on a match it writes an `ETC treasure_map` record to DBSrv, zeroes the six slots, sends `_NN_Treasure_Map` and returns TRUE. If no full layout exists it converts *every* inventory item in 788..793 into item 485, sends `_NN_Congratulations` and returns TRUE whenever at least one such item was converted — so a partial map set is destroyed and reported as a success. The quest branch treats any TRUE as terminal.

When no map exists, the handler iterates `g_pTreasure[0..7]` (`STRUCT_TREASURE`, `Basedef.h:637-644`: `Source`, five `Target` items, five `Rate` thresholds), skips entries with `Source == 0`, and for the first entry whose source item is present in the inventory consumes it and rolls `rand()%1000` against `Rate[0..4]` to select one of the five targets (`:1475-1522`). A zero `sIndex` result means the roll exceeded all thresholds: the NPC answers `_NN_Next_Chance` and the function returns without refunding the consumed map (`:1524-1528`). Otherwise the prize is logged and inserted with `PutItem` and the NPC congratulates (`:1530-1540`). Note that the second comparison reads `g_pTreasure[x].Rate[1]` where `x` is the inventory slot index rather than the table index `i` (`:1512`), so the second-tier threshold is read from an unrelated table entry.

If no treasure entry matched, the branch falls through to the crystal path: it requires exactly one each of items 421..427 in the inventory (`:1544-1558`), a weapon in `Equip[6]` (`:1560-1564`) and weapon sanctification 0 (`:1566-1572`). It then snapshots the three existing effects, clears them, and calls `SetItemBonus(&Equip[6], (25 * 7 / 10) + ReqLvl, 1, DropBonus)` — i.e. an effective level of `17 + ReqLvl` — clears the seven crystals, re-projects carry and the weapon, sends `_SN_I_Wwill_Enchant_Your_Weapon` and `_SN_Now_I_Will_Enchant_Your`, applies effect 44 for 200 units, refreshes score, and logs the before/after effect tuples (`:1580-1614`). If the seven crystals are not all present the NPC answers `_NN_Gather_7_Clistals` (`:1619`).

**Rule workflow**:
1. Run `CombineTreasureMap`; if it reports success, stop.
2. Scan `g_pTreasure`; on a source hit, consume, roll 1d1000, pick a tier, `PutItem`, stop (no prize on a failed roll).
3. Otherwise require one each of 421..427 and a weapon with sanc 0.
4. Clear weapon effects; `SetItemBonus(17 + ReqLvl, 1, DropBonus)`; clear crystals.
5. Re-project carry and weapon, apply effect 44, log before/after effects.

---

### Business Rule: Terra Mistica reward (mode 4, Exploit Leader)

**Overview**:
Pays the second stage of the Terra Mistica line with a level-tiered item carrying one sanctification slot and two random bonus effects, then flags the quest complete.

**Detailed description**:
The branch first inspects the persistent quest stage: `TerraMistica == 0` means the quest was never taken and yields `_NN_Guard_This_Village` (`:1625-1630`); a value other than `2` means the reward was already collected and yields the "villagers thank you" message (`:1632-1637`), which makes stage 3 a permanent terminal state. Only stage 2 proceeds.

Reward selection is level-banded on `CurrentScore.Level`: below 50 -> one of 551..554; 50..79 -> one of 555..558; 80 or more -> one of 559..562 (`:1639-1660`). The item receives `stEffect[0].cEffect = EF_SANC (43)` (`:1676`), and two additional effects are rolled per slot with `rand()%8`: a zero roll yields `EF_HP` (4) with 20..60, rolls 1..7 yield `EF_DAMAGE` (2) 5..25, `EF_ATTSPEED` (26) 5..15, `EF_MP` (5) 20..70, `EF_MAGIC` (60) 2..8, `EF_STR` (7), `EF_INT` (8) or `EF_DEX` (9) each 5..20 (`:1678-1727`).

Despite computing `bonustype`, `bonusvalue1`, `bonusvalue2`, `fvalue` and `tvalue` from `g_pBonusType`/`g_pBonusValue` (`:1667-1674`), the code never assigns `tvalue` to `stEffect[0].cValue`; the sanctification value of the reward is therefore always 0. The item is delivered with `PutItem`, the NPC says `_NN_Give_You_Some_Reward`, `TerraMistica` is set to 3, the client receives `_NN_Watching_Town_Awarded`, and a detailed audit line records the item and all three effects (`:1729-1738`).

**Rule workflow**:
1. Stage 0 -> "guard this village" message, stop. Stage 1 -> nothing happens (no branch for stage 1). Stage 3 -> "already rewarded" message, stop.
2. Stage 2 -> choose the item id by level tier.
3. Set `EF_SANC` on effect 0 (value left at 0), roll effects 1 and 2.
4. `PutItem`, notify, set `TerraMistica = 3`, log.

---

### Business Rule: Master Hab stat reset (mode 3)

**Overview**:
Converts up to 100 points from every base attribute back into free `ScoreBonus`, paid either with `StatSapphire` sapphires or with a single reset item, and only while no equipment is worn.

**Detailed description**:
The branch quotes the configured price `StatSapphire` (global, default 30 at `Server.cpp:316`, adjustable 0..64 by the `+statsapphire` admin command at `imple.cpp:918-925`) when `confirm == 0` (`:1744-1749`). It then requires equipment slots 1 through 7 to be empty and answers `_NN_Cant_with_armor` — via `return`, not `break`, so processing of the packet ends immediately (`:1751-1758`). Inventory is scanned once to count sapphires (697 = 1, 4131 = 10) and to detect the alternative payment item 3336 (`:1760-1774`).

If the sapphire total is below `StatSapphire` and item 3336 is absent the player is told the requirement (`:1776-1781`). If 3336 is present it is consumed and execution jumps to the reset label, bypassing payment (`:1783-1797`). Otherwise the count is clamped to the requirement and consumed in slot order; note that item 4131 is only consumed when at least 10 units are still owed (`:1811`), so a requirement partially covered by single sapphires can be satisfied while leaving the 10-unit item untouched. Class must be within 0..3 (`:1821-1824`).

The reset itself uses a fixed threshold of 100 per attribute: for each of STR/INT/DEX/CON, `delta = BaseScore - BaseSIDCHM[class][index]`; if `delta <= 100` the whole delta is removed and added to `ScoreBonus`, otherwise 100 points are removed (`:1826-1876`). After the conversion the handler logs `etc,mestre_hab resetscore 100` with the connection id, recomputes bonus points and HP/MP, applies effect 44, and projects score and etc panels with `_NN_Stat_Initialized` (`:1878-1888`).

**Rule workflow**:
1. If `confirm == 0`, quote `StatSapphire` and stop.
2. Reject when any of equipment slots 1..7 is occupied.
3. Count sapphires and detect item 3336.
4. Reject if sapphires < requirement and 3336 absent.
5. Consume 3336 (skip payment) or consume sapphires up to the requirement.
6. Reset up to 100 points per attribute into `ScoreBonus`; recompute derived stats; notify; log.

---

### Business Rule: Newbie quest chain (modes 6, 7, 8, 9)

**Overview**:
A four-stage starter chain driven by `QuestInfo.Mortal.Newbie`, gated on mortal class and on being below the free-experience level, rewarding consumables and equipment bonuses.

**Detailed description**:
All four stages share the same precondition shape: `ClassMaster != MORTAL || CurrentScore.Level >= FREEEXP || QuestInfo.Mortal.Newbie != <expected stage>`, answered with `_NN_NewbieQuest_AlreadyN` (`:1894,1937,1991,2045`). `FREEEXP` is a runtime global (`Server.cpp:43`, default 35, settable by the `billmode freeexp` config at `Server.cpp:899-918`), so the chain is only available while the character is below that level. The stage machine is strictly sequential: stage 1 requires `Newbie == 0` and sets it to 1, stage 2 requires 1 and increments to 2, stage 3 requires 2 and increments to 3, stage 4 requires 3 and sets 4.

Each stage requires a specific quest item: 451 for stage 1, 452 for stage 2, 453 for stage 3 and 524 for stage 4 (`:1901-1911`, `:1944-1954`, `:1998-2008`, `:2052-2062`). Only stage 4 consumes its item (`BASE_ClearItem` at `:2073`); stages 1-3 leave 451/452/453 in the inventory, so the quest items accumulate as permanent inventory clutter (and can be traded or stored unless the item definitions mark them untradeable). Each stage writes an audit line `etc,newbiequestN complete`.

Rewards escalate in value. Stage 1 grants item 682 with `EF_AMOUNT = 20` (`:1919-1927`). Stage 2 rebuilds the bonus of the equipped weapon (`Equip[6]`) when its item template requires level 39 or less, by clearing all three effects and calling `SetItemBonus(&Equip[6], 50 + ReqLvl, 1, 0)` (`:1962-1981`). Stage 3 applies the same treatment to every equipment slot 1..7 and additionally forces sanctification 3 with `BASE_SetItemSanc(&Equip[j], 3, 0)` (`:2016-2036`); note that the loop reads `pMob[conn].MOB.Equip[6].sIndex` for the `ReqLvl` lookup on every slot, so all slots are levelled from the weapon's requirement. Stage 4 grants one of three items by `rand()%3`: 682 with amount 20, 481, or 652 (`:2075-2091`).

Every stage finishes with effect 44 (200 units), `SendScore` and `SendCarry`.

**Rule workflow**:
1. Validate class MORTAL, level < `FREEEXP`, and the exact expected `Newbie` stage.
2. Scan inventory for the stage item; on absence answer `_NN_NewbieQuest_CheerupN`.
3. Log completion; advance the stage counter.
4. Stage 1: grant potion 682 (x20). Stage 2: rebuild weapon bonus if `ReqLvl <= 39`. Stage 3: rebuild slots 1..7 and set sanc 3. Stage 4: consume 524, grant random reward.
5. Apply effect, refresh score and carry.

---

### Business Rule: Sephirot creation (mode 10)

**Overview**:
Crafts a class Sephirot from eight distinct elemental stones plus 30,000,000 coin.

**Detailed description**:
With `confirm == 0` the NPC only describes the ritual (`_NN_Soul_n_8_Sephera`, `:2101-2105`). The handler then verifies that each of the eight stone ids `1744..1751` is present at least once by iterating `j` over 0..7 and breaking on the first match in the inventory, incrementing a counter only when a match is found (`:2106-2118`). Fewer than eight yields `_NN_Need_8_Sephera`; the same message is reused when `Coin < 30,000,000` (`:2120-2129`), so a rich player missing a stone and a poor player with all stones receive identical feedback.

On success the fee is deducted, `SendEtc` refreshes coin, and a second nested loop removes exactly one instance of each stone id (`:2131-2147`). The produced item is `1760 + pMob[npcIndex].MOB.Class`, i.e. the Sephirot of the *NPC's* class rather than the player's (`:2153`), which requires the quest NPC to be class-specific by design. `PutItem` delivers it, the NPC answers `_NN_Sepherot_created`, and the audit line records the NPC class, player name and connection (`:2155-2160`).

**Rule workflow**:
1. If `confirm == 0`, describe the ritual and stop.
2. Verify presence of one each of 1744..1751; verify coin >= 30,000,000.
3. Deduct 30,000,000; consume one of each stone.
4. Create `1760 + NPC class`, `PutItem`, notify, log.

---

### Business Rule: Capa Verde teleport (mode 11)

**Overview**:
Teleports mortal characters in the 99..149 level band to the Capa Verde training area.

**Detailed description**:
The branch requires `ClassMaster == MORTAL` and a level in `99..149` (lower inclusive, upper exclusive), answering `_NN_Level_Limit2` for class and `_NN_Level_limit` for level (`:2166-2176`). It then teleports to `(2245 + rand()%5 - 3, 1576 + rand()%5 - 3)` (`:2178`) and logs `etc,quest_aprendiz1 npcteleport`.

There is no item cost, no cooldown, no quest flag and no restriction on repeat use; the branch functions as a permanent free teleport service for the level band. The jitter expression yields -3..+1 rather than -3..+3, the same asymmetric pattern used by every teleport in this component.

**Rule workflow**:
1. Require MORTAL; require level 99..149.
2. `DoTeleport` to (2245,1576) with jitter.
3. Log the teleport.

---

### Business Rule: Capa Verde graduation (mode 19)

**Overview**:
Consumes a quest ring and grants the apprentice graduation cape, once per character.

**Detailed description**:
The branch requires MORTAL and level `99..149` (`:2187-2197`), an equipped ring `Equip[13] == 4080` (`:2199-2204`) and an empty cape slot `Equip[15] == 0` (`:2206-2210`), which is the idempotency guard: a player who already owns any cape cannot repeat the graduation. It then logs completion, clears the ring, writes item 4006 into slot 15, projects the slot, recomputes scores, plays emotion 14 with parameter 3, and sends the "Formatura Aprendiz" play-quest message (`:2212-2229`).

The cape is inserted with a full `memset` of the slot followed by `sIndex = 4006`, so any pre-existing effects are discarded — which is consistent with the "cape slot must be empty" rule.

**Rule workflow**:
1. Require MORTAL and level 99..149.
2. Require ring 4080 equipped; require empty cape slot.
3. Log; clear the ring; insert cape 4006.
4. Recompute score, emotion, notify.

---

### Business Rule: Molar Gargula teleport (mode 26)

**Overview**:
Teleports mortal characters in the 199..253 level band into the Molar Gargula zone.

**Detailed description**:
The branch requires `MORTAL` and level `199..254` (upper exclusive) (`:2235-2245`), logs `etc,quest_molar1 npcteleport`, and teleports to `(817,4062)` with the standard -3..+1 jitter (`:2247-2250`).

The service is free, repeatable and has no quest-state coupling; it is a pure transport convenience whose only gating is class and level. The level band deliberately stops at 254, leaving the 254..368 range without this service.

**Rule workflow**:
1. Require MORTAL; require level 199..253.
2. Log; `DoTeleport` to (817,4062) with jitter.

---

### Business Rule: Black Oracle soul merge (mode 27)

**Overview**:
Merges a soul item and its catalyst into the "stone of immortality" for 10 sapphires.

**Detailed description**:
The handler searches for item 1740 and requires the *immediately following* carry slot to contain 1741 (`:2256-2274`); both a missing 1740 and a non-adjacent 1741 produce the same `_NN_Set_Soul` message, so slot layout is a hidden requirement. Sapphires (697 = 1, 4131 = 10) must total at least 10 (`:2277-2294`) and are then consumed with the same decrement pattern used by the King branch (`:2297-2317`).

The two source slots are then cleared and item 1742 is created and inserted, the NPC answers `_NN_Soul_Merged`, and the audit line `etc,stone_immortality create` records the operation (`:2319-2335`). Because the search loop can terminate with `i == MaxCarry - 1`, the subsequent read of `Carry[i + 1]` at `:2270` can address index `MaxCarry`, one past the end of the fixed 64-slot array (`Basedef.h:76`), which is an out-of-bounds read of the item array.

**Rule workflow**:
1. Find 1740; reject if absent.
2. Require 1741 in the next slot; reject otherwise.
3. Require 10 sapphires; consume them.
4. Clear both slots; create and insert 1742.
5. Notify and log.

---

### Business Rule: Quest Capa Real teleport (mode 28)

**Overview**:
Teleports mortal characters in the 199..253 level band into the Capa Real zone.

**Detailed description**:
The branch mirrors the Molar Gargula teleport: `MORTAL` and level `199..254` are required (`:2341-2351`), then `DoTeleport` to `(1740,1725)` with the standard jitter, preceded (unusually) by the teleport and followed by the audit line `etc,questreal npcteleport` (`:2353-2356`).

The log is written after the movement, unlike the other teleport branches which log first; the ordering has no functional effect but is inconsistent with the rest of the component.

**Rule workflow**:
1. Require MORTAL; require level 199..253.
2. `DoTeleport` to (1740,1725) with jitter.
3. Log the teleport.

---

### Business Rule: Carbuncle wind buff (mode 29)

**Overview**:
Applies up to four beneficial effects defined by the NPC's own skill bar to low-level mortal or Arch characters.

**Detailed description**:
The branch accepts `MORTAL` or `ARCH` and requires `CurrentScore.Level < 100` (`:2362-2372`). It then iterates the NPC's `SkillBar[0..3]`; every entry not equal to 255 is applied to the player with `SetAffect(conn, skill, 400, 200)` — 400 units of duration at level 200 (`:2374-2384`). Up to four effects can therefore be granted per interaction.

`SetAffect` (`Server.cpp:8270+`) rejects merchant mobs and connection ids beyond `MAX_USER`, refuses aggressive spells on `RSV_BLOCK` targets, and allocates a free affect slot via `GetEmptyAffect`; if no slot is free the individual effect is silently dropped. After applying, the handler recomputes scores, sends the score panel, says `_SN_CARBUNCLEMSG` with the character name, and logs `etc,carbuncle buff` (`:2386-2393`).

There is no cooldown and no cost, so the buff can be renewed by repeated interaction; the only limiter is the level-100 ceiling and the affect slot count.

**Rule workflow**:
1. Require MORTAL or ARCH; require level < 100.
2. For each NPC skill-bar entry != 255: `SetAffect(player, skill, 400, 200)`.
3. Recompute score; notify with the character name; log.

---

### Business Rule: Perzen mount exchange (mode 30)

**Overview**:
Exchanges the item the NPC carries in slot 0 for the item in slot 1, stamped with a 30-day lifetime.

**Detailed description**:
The required input is not hard-coded: it is read from `pMob[npcIndex].MOB.Carry[0].sIndex`, and the output from `Carry[1].sIndex` (`:2403,2418`), making the NPC's own inventory the configuration for this exchange. If the input item is absent the NPC answers `_SN_BRINGITEM` with the item name resolved through `g_pItemList` (`:2409-2414`).

On success the input slot is cleared with `BASE_ClearItem`, the slot is rewritten with the output index, and `BASE_SetItemDate(&Carry[i], 30)` marks the item as expiring in 30 days (`:2416-2422`). Only the index is copied: any effects, sanctification or count on the NPC's template item are not transferred, because `BASE_ClearItem` has already zeroed the destination. The audit line records the produced type id, player name and connection (`:2424-2425`).

**Rule workflow**:
1. Find the inventory item matching NPC `Carry[0]`; reject with "bring item" if missing.
2. Clear the slot; write NPC `Carry[1].sIndex`.
3. Apply a 30-day date; project the slot; log.

---

### Business Rule: Kibita citizenship and Limit of Soul (mode 31)

**Overview**:
Sells citizenship of the current server for 4,000,000 coin and unlocks the "Limit of Soul" mantle at level 369 for mortals holding their class stone.

**Detailed description**:
The first sub-rule applies when the player has no citizen record and at least 4,000,000 coin: it deducts the fee, sets `extra.Citizen = ServerIndex + 1` (server-scoped tag), sends `_DD_JOINTOWNPEP`, and — when the player is a guild master — writes `GuildInfo[Guild].Citizen` and pushes `_MSG_GuildInfo` to DBSrv (`:2431-2461`). If the player is already a citizen or cannot afford it, execution continues to the remaining sub-rules rather than stopping, which means an already-citizen player receives the "sorry" message from the tail of the branch.

A blocked sub-rule guarded by `#ifdef KIBITA_SOUL` implements an hourly soul buff (weekdays, 21:00, MORTAL, level < 369, item 420 consumed, affect 29 for `AFFECT_1H`, teleport to (2454,1843)) (`:2468-2515`). The macro `KIBITA_SOUL` is not defined anywhere in the repository (`grep` over all `*.h`/`*.cpp` returns only this `#ifdef`), so the block is never compiled and is effectively dead code.

The effective "Limit of Soul" sub-rule requires `MORTAL`, `LearnedSkill` bit 30 clear and level >= 369 (`:2516`). It builds a class-indexed stone table `{5334, 5336, 5335, 5337}` and requires the stone at `MOB.Class`, consuming it on match (`:2520-2533`); a class value outside 0..3 would index past the four-element array. On success bit 30 is set in `LearnedSkill`, the cape is replaced by 3194 (clan 7), 3195 (clan 8) or 3196 (clanless), the player is forcibly logged out with `CharLogOut`, the audit line `etc,learned limit of soul mortal` is written, and `_MSG_SendArchEffect` is signalled to the field (`:2535-2556`). The forced logout is required because the skill-bit change only takes effect after re-login. If nothing applies the NPC answers `_DN_TOWN_SORRY`.

**Rule workflow**:
1. If not a citizen and coin >= 4,000,000: charge, tag citizen, sync guild, log, stop.
2. (Compiled out) soul-buff block.
3. If MORTAL, bit 30 clear, level >= 369: require class stone; consume; set bit 30; assign mantle 3194/3195/3196; force logout; signal arch effect; log; stop.
4. Otherwise send `_DN_TOWN_SORRY`.

---

### Business Rule: Government PK reset (mode 32)

**Overview**:
Converts kill points into PK penalty points in blocks of 100, capping the PK point pool at 150.

**Detailed description**:
The branch reads `GetCurKill(conn)` and `GetPKPoint(conn)` (`:2566-2567`). When the kill counter reaches 100 it adds 20 PK points, clamps the result to 150, subtracts exactly 100 from the kill counter and writes both values back (`:2569-2577`). It then logs `etc,resetpk` with the new PK value, rebuilds the player's `MSG_CreateMob` projection with `GetCreateMob` and multicasts it in the local grid so surrounding clients update the player's appearance, and plays emotion 14/3 (`:2579-2589`). Below 100 kills the NPC answers `_NN_MOREKILLPOINT` (`:2592`).

Only one block of 100 kills is converted per interaction; a player with 500 kills must invoke the NPC five times. The 20-for-100 exchange rate is fixed and not configurable in the component.

**Rule workflow**:
1. Read kill counter and PK points.
2. If kills >= 100: PK += 20 (cap 150), kills -= 100, persist both, log, rebuild and multicast the mob, play emotion.
3. Else answer `_NN_MOREKILLPOINT`.

---

### Business Rule: Survivor kefra ticket exchange (mode 33)

**Overview**:
Converts item 4127 into 100 Kefra tickets.

**Detailed description**:
The branch scans the inventory for item 4127 (`:2599-2605`); when found it clears the slot, projects it, adds 100 to `extra.KefraTicket`, logs `etc,kefraticket ticket:<total>`, and sends `_DN_CHANGE_COUNT` with the new total (`:2607-2619`). When the item is absent the branch produces no message at all (silent no-op), which is inconsistent with the "bring item" pattern used elsewhere.

There is no cap on `KefraTicket` and no audit distinction between repeated conversions, so the item is a pure currency conversion.

**Rule workflow**:
1. Scan inventory for 4127.
2. If present: clear slot, `KefraTicket += 100`, log, notify with the new total.
3. If absent: return silently.

---

### Business Rule: Arch apprentice graduation (mode 34)

**Overview**:
Grants the same graduation cape as the mortal Capa Verde branch to Arch characters in the 99..149 level band.

**Detailed description**:
The branch requires `ClassMaster == ARCH` (`:2626-2631`), a `BaseScore.Level` in `99..149` inclusive on both ends (`:2633-2637`), and an empty cape slot (`:2639-2643`). It then logs, writes cape 4006 into slot 15 after a full `memset`, re-projects, recomputes scores, plays emotion 14/3 and sends the "Formatura Aprendiz" message (`:2645-2659`).

Unlike the mortal variant, this branch does not consume any ring item: the only cost is the empty cape slot. It also uses `BaseScore.Level` where every other level check in the component uses `CurrentScore.Level`, so temporary level modifiers would be ignored here.

**Rule workflow**:
1. Require ARCH; require base level 99..149; require empty cape slot.
2. Log; insert cape 4006; project.
3. Recompute score; emotion; notify.

---

### Business Rule: Royal Guard event exchange (mode 35)

**Overview**:
Exchanges event item 4055 for an elemental particle, intended to be a 50/50 roll between items 412 and 413.

**Detailed description**:
The branch scans for item 4055 (`:2665-2674`). When present it clears the slot and projects it, then rolls `rand()%100` and assigns 413 for rolls below 25, 412 for 25..49, 413 for 50..74 and 412 otherwise — an effective 50% chance for each item (`:2676-2696`). It formats `_SN_Item_Arrived` with the selected item's name and sends it to the client, then breaks (`:2698-2700`).

The selected item is never inserted: there is no `PutItem`, no `CreateItem` and no direct `Carry` write, so the player loses the event item and receives only a text message naming a reward they do not get. When 4055 is absent the NPC answers `_SN_BRINGITEM` (`:2703-2704`).

**Rule workflow**:
1. Scan inventory for 4055.
2. If present: clear and project the slot; roll 1d100; select 412 or 413; announce the reward; stop (reward not delivered).
3. If absent: answer "bring item".

---

### Business Rule: Unknown quest NPC audit (default branch)

**Overview**:
Any NPC mode that reaches the switch but matches no case is logged rather than ignored silently.

**Detailed description**:
The `default` label formats `etc,quest <merc> <grade>` and writes it through `Log` using the player's account name and IP (`:2707-2710`). Because `npcMode` is only ever set to one of the 35 defined constants or left at `-1` (which returns before the switch at `:159-160`), the default branch is unreachable in the current code: every reachable mode has a case. Its practical value is as a diagnostic if a new constant is added to `Basedef.h` without a corresponding case.

Note that the mode constants in `Basedef.h:240-274` are sequential 1..35, so a future mode would still fall into `default` and be recorded.

**Rule workflow**:
1. Switch falls through to `default`.
2. Format merchant/grade.
3. Write the audit line with account and IP.

---

### Business Rule: Ticket-quest zone recall enforcement

**Overview**:
Outside the component, the 1-second timer expels players who stand inside a ticket-quest zone without the matching `QuestFlag`.

**Detailed description**:
`ProcessSecMinTimer.cpp:1192-1209` checks five rectangles (Coveiro 2379-2426 / 2076-2133, Jardineiro 2228-2257 / 1700-1728, Kaizen 459-497 / 3887-3916, Hidra 658-703 / 3728-3762, Elfos 1312-1348 / 4027-4055) and calls `DoRecall(i)` whenever the player is inside the rectangle and `pMob[i].QuestFlag` differs from that zone's number, with the guard `BaseScore.Level < 1000`.

This rule is the enforcement half of the ticket teleport rules: the `QuestFlag` written by `_MSG_Quest.cpp:326/370/414/458/502` is the admission token, and the timer is the only consumer that ejects intruders. `ProcessSecMinTimer.cpp:535-536` clears the flag when the player leaves the play state and `Server.cpp:5749` clears it on mob initialisation, so the pass is intentionally transient and is lost on logout or reconnection.

Because the rectangles are hard-coded in two different translation units, any geometry change must be made in both places; there is no shared constant.

**Rule workflow**:
1. Each second, for every active mob, test the five rectangles.
2. If inside a rectangle and `QuestFlag != zone`, `DoRecall`.
3. Clear `QuestFlag` on session state change or mob init.

---

## 4. Component Structure

```
Code/
├── Basedef.h                                  shared domain contract (message structs, item effects,
│                                              quest NPC mode constants, STRUCT_MOBEXTRA)
├── Basedef.cpp                                domain kernel (item ability/sanc helpers, score/HP re-derivation)
└── TMSrv/
    ├── _MSG_Quest.cpp                         THE COMPONENT (2,712 lines, one function)
    │   ├── lines 20-46      entry, NPC index validation, trade guards
    │   ├── lines 49-162     NPC mode resolution (38 independent predicates)
    │   ├── lines 164-2711   switch (npcMode) with 35 cases + default
    │   └──   - mode 1..35   one transactional script per NPC service
    ├── ProcessClientMessage.cpp  dispatcher: case _MSG_Quest -> Exec_MSG_Quest (:184-186)
    ├── ProcessClientMessage.h    handler declaration (:131)
    ├── Server.cpp/.h             globals (pUser, pMob, temp, Sapphire, StatSapphire, FREEEXP),
    │                             PutItem, SetItemBonus, Combine, CombineTreasureMap,
    │                             DoTeleport, SetAffect, MountProcess, CharLogOut, GetUserInArea,
    │                             Log, RemoveTrade, Pista[], g_pTreasure[], GuildInfo[]
    ├── SendFunc.cpp/.h           client projection layer (SendSay, SendClientMessage, SendItem,
    │                             SendCarry, SendScore, SendEquip, SendEtc, SendEmotion, GridMulticast)
    ├── GetFunc.cpp/.h            derivations (GetPKPoint, SetPKPoint, GetCurKill, SetCurKill,
    │                             GetEmptyAffect, GetCreateMob)
    ├── CMob.cpp/.h               mob entity: GetCurrentScore, QuestFlag, Leader, MaxCarry, PartyList
    ├── ProcessSecMinTimer.cpp    recall enforcement for ticket-quest zones (:1192-1209)
    └── MobKilled.cpp             consumes Pista[] registrations produced by UXMAL (:1395-1500)
```

No unit of this component is reusable in isolation: the file has exactly one external symbol (`Exec_MSG_Quest`), no internal helpers, no data structures of its own, and no configuration surface other than the globals it reads.

---

## 5. Dependency Analysis

**Internal Dependencies**

```
ProcessClientMessage (dispatcher)
  -> Exec_MSG_Quest
       -> NPC identity:      pMob[npcIndex].MOB.Merchant, BASE_GetItemAbilityNosanc(EF_GRADE0)
       -> Player state:      pMob[conn].MOB (Coin, Carry, Equip, Class, Clan, Guild, LearnedSkill,
                              SpecialBonus, ScoreBonus, BaseScore, CurrentScore, MobName),
                             pMob[conn].extra (ClassMaster, QuestInfo, Fame, Citizen, KefraTicket,
                              SaveCelestial[], MortalFace),
                             pMob[conn].QuestFlag, pMob[conn].Leader, pMob[conn].PartyList[],
                             pMob[conn].Mode, pMob[conn].TargetX/TargetY,
                             pUser[conn] (AccountName, IP, Slot, Mode, Trade, TradeMode, cSock)
       -> Domain helpers:    11 BASE_* functions (item clear/sanc/ability/date, score/HP/skill/score
                             bonus, item code, language string)
       -> Inventory ops:     PutItem, Combine, CombineTreasureMap, SetItemBonus, BASE_SetItemSanc
       -> World ops:         DoTeleport, SetAffect, MountProcess, CharLogOut, GetCreateMob,
                             GridMulticast, SendClientSignalParm
       -> NPC registry:      Pista[7] (rune arenas), g_pTreasure[8] (treasure lottery),
                             g_pItemList[6500] (prices, names, ReqLvl), g_pBonusType/g_pBonusValue,
                             g_pSpell[] (affect type/aggressive for SetAffect),
                             g_pMessageStringTable[] (all player-visible text)
       -> Persistence:       DBServerSocket.SendOneMessage (_MSG_DBCreateArchCharacter,
                             _MSG_DBUpdateSapphire, _MSG_GuildInfo, _MSG_MessageDBRecord)
       -> Audit:             Log(file, account, ip)
       -> Cross-component:   GuildInfo[] shared with guild subsystem;
                             pMob[].QuestFlag consumed by ProcessSecMinTimer;
                             Pista[] consumed by MobKilled and _MSG_Action
```

**External Dependencies**

| Dependency | Version / origin | Purpose |
|------------|------------------|---------|
| Win32 CRT (`sprintf`, `memset`, `memcpy`, `strncpy`, `strcat`, `time`, `localtime`, `rand`) | MSVC, `Code/TMSrv/_MSG_Quest.cpp:19-36` | formatting, buffer copies, date and RNG |
| DBSrv message socket (`DBServerSocket`) | `Code/DBSrv` | asynchronous character/guild persistence |
| Client socket transport (`pUser[].cSock`, `CPSock`) | `Code/CPSock.cpp` | all player-visible output |
| Item/message configuration files | loaded by `CReadFiles.cpp` (external `Common/Settings`) | item names, prices, required levels, localized strings |
| Runtime configuration globals | `Server.cpp:43,51,316` (`FREEEXP`, `Sapphire`, `StatSapphire`) | level ceiling for newbie chain, sapphire price, stat-reset price |
| `KIBITA_SOUL` compile switch | never defined | conditionally compiled soul-buff block (`:2468-2515`) |

---

## 6. Afferent and Efferent Coupling

Coupling is measured on the C translation-unit paradigm: the "component" is the function `Exec_MSG_Quest`, and the units it depends on are the free functions and global tables listed above. Afferent/efferent counts are call sites observed in `Code/TMSrv/*.cpp` (plus `Code/Basedef.cpp` for `BASE_*` helpers).

| Component | Afferent Coupling | Efferent Coupling | Critical |
|-----------|-------------------|-------------------|----------|
| `Exec_MSG_Quest` (`_MSG_Quest.cpp:20`) | 1 (`ProcessClientMessage.cpp:185`) | 39 distinct callees + 14 global tables | Critical |
| NPC mode resolution block (`_MSG_Quest.cpp:51-160`) | 1 | 3 (`pMob`, `MOB.Merchant`, `BASE_GetItemAbilityNosanc`) | Medium |
| `CombineTreasureMap` (`Server.cpp:8398`) | 1 (`_MSG_Quest.cpp:1470`) | 4 (`SendItem`, `SendClientMessage`, `DBServerSocket`, `pMob`) | Low |
| `Combine` (`Server.cpp:8509`) | 1 (`_MSG_Quest.cpp:579,584`) | 3 (`BASE_ClearItem`, `CreateItem`, `pMob`) | Low |
| `PutItem` (`Server.cpp:709`) | 60 | 2 (`SendItem`, `SendClientMessage`) | High |
| `SetItemBonus` (`Server.cpp:1393`) | 11 | 8 (`g_pItemList`, RNG, effect tables) | High |
| `SendSay` (`SendFunc.cpp:1647`) | 104 | 2 (`GridMulticast`, `pMob`) | High |
| `DoTeleport` (`Server.cpp:8212`) | 71 | 5 (`GetEmptyMobGrid`, `GetAction`, `cSock`, `GridMulticast`) | High |
| `SetAffect` (`Server.cpp:8270`) | 25 | 4 (`g_pSpell`, `GetEmptyAffect`, `pMob`) | High |
| `BASE_ClearItem` (`Basedef.cpp:4281`) | 37 | 1 | High |
| `BASE_GetItemAbility` (`Basedef.cpp:1647`) | 112 | 2 | High |
| `BASE_GetItemAbilityNosanc` (`Basedef.cpp:1497`) | 8 | 2 | Medium |
| `BASE_GetItemSanc` (`Basedef.cpp:2095`) | 83 | 1 | High |
| `BASE_SetItemSanc` (`Basedef.cpp:2239`) | 55 | 2 | Medium |
| `BASE_GetLanguage` (`Basedef.cpp`) | 11 | 1 (`g_pMessageStringTable`) | Medium |
| `BASE_SetItemDate` (`Basedef.cpp:6732`) | 6 | 1 | Low |
| `BASE_GetBonusScorePoint` (`Basedef.cpp:858`) | 10 | 3 | Medium |
| `BASE_GetBonusSkillPoint` (`Basedef.cpp:810`) | 4 | 3 | Medium |
| `BASE_GetHpMp` (`Basedef.cpp:1174`) | 6 | 2 | Medium |
| `GetUserInArea` (`Server.cpp:4157`) | 9 | 2 (`pUser`, `pMob`) | Low |
| `MountProcess` (`Server.cpp:4178`) | 11 | 6 (`DeleteMob`, `BASE_GetItemAbility`, `pMob`) | Medium |
| `CharLogOut` (`Server.cpp:7085`) | 11 | many (session save path) | High |
| `GetEmptyAffect` (`GetFunc.cpp:610`) | 11 | 1 | Medium |
| `GetPKPoint` / `SetPKPoint` (`GetFunc.cpp:2105,2145`) | 12 / 10 | 1 / 1 | Low |
| `GetCurKill` / `SetCurKill` (`GetFunc.cpp:2046,2071`) | 6 / 4 | 1 / 1 | Low |
| `GetCreateMob` (`GetFunc.cpp:947`) | 31 | 3 | Medium |
| `SendClientMessage` (`SendFunc.cpp:27`) | 671 | 1 (`cSock`) | High |
| `SendItem` (`SendFunc.cpp:1055`) | 385 | 1 | High |
| `SendScore` (`SendFunc.cpp:1136`) | 100 | 3 | High |
| `SendCarry` (`SendFunc.cpp:1534`) | 14 | 2 | Medium |
| `SendEquip` (`SendFunc.cpp:50`) | 11 | 2 | Medium |
| `SendEtc` (`SendFunc.cpp:1195`) | 57 | 1 | High |
| `SendEmotion` (`SendFunc.cpp:825`) | 93 | 1 | High |
| `SendClientSignalParm` (`SendFunc.h:30`) | 104 | 2 | High |
| `GridMulticast` (`SendFunc.cpp`) | 105 | 2 | High |
| `RemoveTrade` (`Server.cpp:7319`) | 91 | 3 | High |
| `Log` (`Server.cpp:6701`) | 42 (in this file; global use is pervasive) | 2 (filesystem) | Medium |
| `Pista[]` (`Server.cpp:448`) | 3 units (Quest, MobKilled, Action) | 0 | Critical (shared mutable state) |
| `g_pTreasure[]` (`Server.cpp:273`) | 2 units (Quest, admin loader `imple.cpp`) | 0 | Medium |
| `GuildInfo[]` (`Server.cpp`) | 3 units (Quest, guild handlers, DB sync) | 0 | High |

Interpretation: `Exec_MSG_Quest` is a **high-efferent, low-afferent** unit (1 in, 39 out) sitting on top of heavily shared infrastructure (`PutItem` 60 call sites, `SendClientMessage` 671, `BASE_GetItemAbility` 112). Its instability is maximal (Ce/(Ca+Ce) = 0.975) and, per Martin's metrics, it is the least abstract and least stable unit in the server: every infrastructure change propagates into it, while nothing depends on it except the dispatcher. `Pista[]` is the only piece of state in the component with three writers, making it the highest-risk shared structure.

---

## 7. Endpoints

The component does not expose REST, GraphQL or gRPC endpoints. It exposes exactly one binary protocol message on the client-to-game channel.

| Endpoint | Protocol | Value | Payload | Description |
|----------|----------|-------|---------|-------------|
| `_MSG_Quest` | TMSrv binary client message (`FLAG_CLIENT2GAME = 0x0200`) | `139 \| 0x0200` = 651 (`Basedef.h:1801`) | `MSG_STANDARDPARM2` — `{int Size; short Type; short ID; int ClientTick; int Parm1 = npcIndex; int Parm2 = confirm}` (`Basedef.h:954-959`) | Single entry point for all 35 NPC quest services; `Parm2 != 0` means "confirmed" for branches that use a two-step confirm protocol |

Dispatch path: `ProcessClientMessage.cpp:38` -> `case _MSG_Quest:` (`:184`) -> `Exec_MSG_Quest` (`:185`). Pre-handler validation performed by the dispatcher: header `ID` in `0..MAX_USER` (`:42-51`), `ServerDown < 120` (`:53`), `ClientTick != SKIPCHECKTICK` for non-server messages (`:63`). No size check, no state check and no sequence/anti-replay check are applied to this message type.

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol / mechanism | Data Format | Error Handling |
|-------------|------|---------|----------------------|-------------|----------------|
| Client session | Player I/O | Receive quest requests, project NPC speech, inventory, score, equipment, emotion | `pUser[conn].cSock` / `GridMulticast` | `MSG_*` binary structs | none; failed sends are not observed |
| DBSrv | Internal service | Arch character creation (`_MSG_DBCreateArchCharacter`), sapphire ledger update (`_MSG_DBUpdateSapphire`), guild record sync (`_MSG_GuildInfo`), treasure-map audit record (`_MSG_MessageDBRecord`) | `DBServerSocket.SendOneMessage` | fixed-size binary structs | fire-and-forget; no ack, no retry, no rollback; `KING` parks the session in `MOB_WAITDB` to prevent double submission |
| Player mob table `pMob[MAX_MOB]` | In-process global state | Character state, inventory, equipment, quest flags | direct memory access | `CMob` / `STRUCT_MOB` / `STRUCT_MOBEXTRA` | none; range checks are ad hoc |
| Player table `pUser[MAX_USER]` | In-process global state | Account name, IP, slot, trade state, socket | direct memory access | `CUser` | none |
| Item template table `g_pItemList[MAX_ITEMLIST]` | Configuration | Prices, names, required levels | direct indexed read | `STRUCT_ITEMLIST` | indexes come from item ids validated only by range in a few branches (`:1965`, `:2020`) |
| Message string table `g_pMessageStringTable` | Configuration | All localized NPC text | indexed read | `char*` | no bounds validation of index (compile-time constants) |
| Treasure table `g_pTreasure[8]` | Configuration | Treasure-map lottery targets and rates | direct read | `STRUCT_TREASURE` | reads `Rate[1]` from the wrong index (`:1512`) |
| Bonus tables `g_pBonusType`, `g_pBonusValue` | Configuration | Reward effect selection | direct read | arrays | values computed but never used (`:1667-1676`) |
| Spell table `g_pSpell[]` | Configuration | Affect type/aggression for `SetAffect` | indirect via `SetAffect` | `STRUCT_SPELL` | invalid skill ids are not validated here |
| Rune arena table `Pista[7]` | In-process shared state | Registration produced here, consumed by `MobKilled`/`_MSG_Action` | direct memory access | `STRUCT_PISTA` | capacity and duplicate checks local to this branch |
| Guild table `GuildInfo[]` | In-process shared state | Clan/citizen/fame updates pushed to DB | direct write + `_MSG_GuildInfo` | `MSG_GuildInfo` | no validation of guild index against `MAX_GUILD` |
| 1-second timer | Internal scheduler | Recall enforcement for quest zones | `ProcessSecMinTimer.cpp:1192-1209` | reads `QuestFlag` | none |
| Filesystem log | Audit | `Log()` lines for economy-sensitive actions | text append | `etc,...` / `tra,...` lines | no error handling |
| Runtime configuration | Process globals | `FREEEXP`, `Sapphire`, `StatSapphire` | set by config file / admin commands | integers | no range validation at use site (`MESTREHAB` clamps only by comparison) |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---------|----------------|----------|---------|
| Message-handler dispatch (Command) | `switch (std->Type)` in the client dispatcher | `ProcessClientMessage.cpp:66-209` | routes `_MSG_Quest` to the handler |
| Interpreter over a derived mode | `switch (npcMode)` with 35 cases | `_MSG_Quest.cpp:164-2711` | maps NPC identity to a quest script |
| Two-key lookup table (encoded as `if` chain) | `(Merchant, face grade)` -> mode | `_MSG_Quest.cpp:51-160` | data-driven NPC role resolution without a table |
| Transaction Script | each case is a flat sequence: validate -> consume -> mutate -> reward -> notify -> log | entire switch | keeps each quest readable in isolation, at the cost of duplication |
| Guard clauses / early return | precondition checks with `SendSay`/`SendClientMessage` + `break`/`return` | every case | fail fast before any state change |
| Two-step confirmation protocol | `confirm == 0` quotes, `confirm != 0` executes | `MOUNT_MASTER`, `AMU_MISTICO`, `KING`, `KINGDOM`, `MESTREHAB`, `COMP_SEPHI` | prevents accidental economy spend |
| State machine encoded in a persisted byte | `QuestInfo.Mortal.TerraMistica` 0->1->2->3, `QuestInfo.Mortal.Newbie` 0..4, `QuestInfo.Circle` 0/1 | `:252-286, 1625-1738, 1894-2095, 632-662` | resumable multi-NPC quest chains without a quest table |
| Admission token | `pMob[].QuestFlag` written here, enforced by the timer | `:326..502` + `ProcessSecMinTimer.cpp:1192-1209` | zone access control for ticket quests |
| Token currency abstraction | item 697 = 1 unit, 4131 = 10 units, consumed by repeated scan | `KING`, `KINGDOM`, `BLACKORACLE`, `MESTREHAB` | lets the game sell sapphires in two denominations |
| Service layer / domain kernel | `BASE_*` helpers in `Basedef.cpp` and `Server.cpp` | called 39 times per invocation | centralizes item and score algebra |
| Projection layer | `SendFunc.cpp` produces all client-visible messages | `SendSay`, `SendItem`, `SendScore`, ... | separates state mutation from wire formatting |
| Fire-and-forget asynchronous persistence | `DBServerSocket.SendOneMessage` + session parking | `:843-857, 898-908, 950-967, 1000-1010` | avoids blocking the game loop on DBSrv |
| God function (anti-pattern) | one 2,712-line function with 35 responsibilities | whole file | none; consequence of decompilation and incremental feature growth |
| Magic-number protocol (anti-pattern) | item ids 4038..4081, 5134, 5334..5338, 696..697, 4131, 4006, 1740..1760, 3191..3199, 620..695 | entire file | implicit item contract with client and DB |
| Dead conditional compilation | `#ifdef KIBITA_SOUL` without a definition | `:2468-2515` | abandoned soul-buff feature |

Architectural observations: the component is a classic **transaction-script architecture over a shared in-memory domain model**, with no repository, no unit of work, no domain events and no validation layer. Consistency relies on the ordering of statements inside each case and on the fact that the process is single-threaded.

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | Entry point `_MSG_Quest.cpp:24-28` | `npcIndex` is client-controlled and only range-checked; no distance, visibility or NPC-role verification | Any client can invoke any quest NPC on the map remotely; economy and progression services are reachable without physical interaction |
| High | Entry point `_MSG_Quest.cpp:22` | Payload cast to `MSG_STANDARDPARM2` without validating `m->Size` | Short packets cause reads past the received buffer; malformed input is not rejected |
| High | `GUARDA_REAL_EVT1` `:2676-2701` | Quest item 4055 is consumed and the "reward" is only announced, never inserted (no `PutItem`) | Guaranteed item loss for every player using the event NPC |
| High | `BLACKORACLE` `:2270` | `Carry[i + 1]` may read index `MaxCarry` when `i == MaxCarry - 1` | Out-of-bounds read of the inventory array; undefined behaviour |
| High | `KING` `:838-967` | Local state is mutated and items are consumed before/while asynchronous DB messages are sent, with no ack or rollback | Client and DB can diverge (mantle, sapphires, guild clan) if DBSrv rejects or drops the message |
| High | `EXPLOIT_LEADER` `:1667-1676` | `tvalue`/`bonustype` computed from `g_pBonusType`/`g_pBonusValue` but never assigned to the item | Reward sanctification value is always 0; the configured bonus table has no effect |
| High | `GOLD_DRAGON` `:1512` | `g_pTreasure[x].Rate[1]` uses the inventory slot index instead of the table index | Second-tier drop threshold read from an unrelated table entry; unpredictable reward rates |
| High | `CombineTreasureMap` `Server.cpp:8488-8506` | Any partial map set (items 788..793) is converted to 485 and reported as success | Players lose fragments; the caller treats it as a completed treasure quest (`:1470-1473`) |
| High | `MESTREHAB` `:1801-1818` | Sapphire consumption only takes 4131 (10-unit stack) when at least 10 units are still owed | Requirement can be partially unpaid after clamping; repeated exploitation yields free stat resets |
| High | `TREINADORNEWBIE1..3` `:1901-2039` | Quest items 451/452/453 are required but never consumed; only 524 is cleared | Quest items accumulate permanently; repeated chain replay is possible if the flags are ever reset |
| High | `KINGDOM` `:1141-1149` | `if (Clan != 7 && Clan != 8) { if (Clan == 7) ... else ... }` — inner test is unreachable | Clan 7 receives the clan-8 message; the branch's identity feedback is always wrong |
| Medium | `KING` `:740-756, 891-930` | Sapphire price inversion table only covers the global values 1/2/4/16/32/64 | Any other configured `Sapphire` (default is 8) falls through unchanged, producing an unintended price |
| Medium | `JEFFI` `:571-587` | Material loop trusts local counters rather than `Combine`'s result | Success can be reported with fewer rewards than the counters indicate (conservation defect in either direction) |
| Medium | `JEFFI` `:562-569` | `Ori`/`Lac` accumulate `cValue` when `cEffect == 61`, otherwise 1 | Stacked and unstacked items are counted inconsistently with `Combine`'s recount |
| Medium | `KIBITA` `:2520-2533` | `stones[pMob[conn].MOB.Class]` assumes class 0..3 | Class values outside 0..3 index past the array |
| Medium | `UXMAL` `:1312-1321` | Time window derived from `tm_min` with three blocked ranges | Registration is available only 12 of every 60 minutes; boundary minutes are implicitly excluded |
| Medium | `CAPAVERDE_TELEPORT` / `MOLARGARGULA` / `QUEST_CAPAREAL` | `rand()%5-3` produces -3..+1 | Asymmetric jitter; destination offsets biased by 1 unit |
| Medium | Whole file | 71 `sprintf` into the shared global `char temp[4096]` (`Server.cpp:277`) | Single global formatting buffer; not re-entrant; a long localization string plus long names risks truncation |
| Medium | Whole file | 42 `Log()` calls with inconsistent sink choice (`"-system"` vs account/IP) | Audit trail is inconsistent; some economy operations are not attributable |
| Medium | `LIDER_APRENDIZ` `:2633` | Uses `BaseScore.Level` while all other branches use `CurrentScore.Level` | Level band behaves differently under temporary modifiers |
| Medium | `SOBREVIVENTE` `:2599-2619` | No feedback when item 4127 is missing | Silent failure; poor diagnosability |
| Medium | `ZAKUM` `:237-246` | `GetUserInArea` overwrites the name buffer each match; message shows the last name and `count - 1` | Misleading report; `count - 1` is wrong when the requester is outside the rectangle |
| Medium | `PERZEN` `:2416-2422` | Only `sIndex` is copied from the NPC template item | Sanctification/effects configured on the template are silently discarded |
| Low | `KING` `:946-948, 1089-1092` | `int Group/Server/usGuild` computed but unused | Dead code |
| Low | `EXPLOIT_LEADER` `:1669-1674` | unused `bonustype`, `fvalue`, `tvalue` variables | Dead computation |
| Low | `KIBITA` `:2468-2515` | `#ifdef KIBITA_SOUL` never defined anywhere in the repository | Dead code path (feature not compiled) |
| Low | `default` branch `:2707-2710` | Unreachable because `npcMode == -1` returns before the switch | Diagnostic code that can never fire |
| Low | `_MSG_Quest.cpp:1319-1389` | Teleport and registration logic duplicated per quest with hard-coded coordinates | Two-place maintenance (also in `ProcessSecMinTimer.cpp`) |
| Low | `_MSG_Quest.cpp` | 35 cases with duplicated precondition/consumption sequences | Any rule change must be applied 5x for the ticket chain and 4x for the newbie chain |

---

## 11. Test Coverage Analysis

**Result: no test coverage exists for this component, and no test infrastructure exists in the repository.**

Evidence gathered during this analysis:

1. **No test project.** `W2PP Code Project.sln` declares exactly two projects: `DBSrv` (`Code/DBSrv/DBSrv.vcxproj`) and `TMSrv` (`Code/TMSrv/TMSrv.vcxproj`) — see `W2PP Code Project.sln:6-9`. There is no test, mock or benchmark project.
2. **No test files.** A recursive search for files matching `*test*` or `*spec*` outside the ignored folders (` .agents`, `.codegraph`, `.opencode`, `openspec`) returned no matches; the only directories whose names contain "spec" are `openspec/specs` (specification documents, not tests) and `.agents/skills/openspec-*` (agent skills, excluded by scope).
3. **No test framework.** No reference to GoogleTest, Catch2, CppUnit, MSTest, Boost.Test or any assertion harness appears in the solution, the project files, or the source tree.
4. **No fixtures or golden data.** No `.txt`, `.ini`, `.cfg` or `.dat` fixture files exist outside the build-output directories; item, message and treasure data are loaded at runtime from an external `Common/Settings` tree that is not part of the repository.
5. **No seams for testing.** `Exec_MSG_Quest` reads and writes the process globals `pMob`, `pUser`, `temp`, `Pista`, `g_pTreasure`, `GuildInfo`, `DBServerSocket` and the socket layer directly; there is no injection point, no interface and no stubbable boundary, so a unit test of any single quest rule would require constructing the whole server state.

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| Quest Handler (`_MSG_Quest.cpp`) | 0 | 0 | 0% (no executable tests exist) | Not assessable — no assertions, no fixtures, no harness |
| NPC mode resolution (`:51-160`) | 0 | 0 | 0% | 38 predicates, none exercised by an automated check; grades 6, 10-12, 17-21, 23-29, 31+ untested even manually |
| Ticket teleport chain (modes 20-24) | 0 | 0 | 0% | Would require coordinated checks against `ProcessSecMinTimer.cpp:1192-1209`; none present |
| Newbie chain (modes 6-9) | 0 | 0 | 0% | State machine with 4 stages and 3 different reward types, entirely untested |
| Economy branches (Jeffi, Sephirot, Arzan, Kibita, Master Hab) | 0 | 0 | 0% | Item/coin conservation not verified anywhere; the `MESTREHAB` partial-payment defect would be caught by a simple conservation test |
| Rune registration (UXMAL) | 0 | 0 | 0% | Shared-state interaction with `MobKilled.cpp` and `_MSG_Action.cpp` untested |
| Persistence hand-offs (King, Kingdom, Kibita) | 0 | 0 | 0% | Asynchronous DB messages have no test double and no assertion on resulting DB state |

Manual/observational verification that does exist: none in the repository. Behavioural expectations must be inferred from the code, from the localized message names (`_NN_*`, `_SN_*`, `_DN_*`, `_DS_*`, `_DD_*` in `Code/TMSrv/Language.h`) and from the audit log format strings, which act as the only recorded contract of intended behaviour.

**Assessment:** the absence of tests is a structural risk for this component rather than a coverage gap in an otherwise testable unit: the component cannot be exercised without a running server, a client, a DBSrv instance and a populated item/message dataset. The defects listed in section 10 (unrewarded `GUARDA_REAL_EVT1`, out-of-bounds `Carry[i+1]`, wrong treasure index, partial sapphire payment) are all of the class that a single table-driven unit test per branch would have detected.

---

*End of report.*
