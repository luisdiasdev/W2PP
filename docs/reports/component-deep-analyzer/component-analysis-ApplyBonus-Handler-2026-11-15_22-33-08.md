# Component Deep Analysis Report - ApplyBonus Handler

**Component:** `ApplyBonus Handler`
**Primary location:** `Code/TMSrv/_MSG_ApplyBonus.cpp` (219 lines of content, 7,629 bytes, final line without newline)
**Secondary locations in scope:** `Code/Basedef.h:1769-1777` (`MSG_ApplyBonus` wire structure), `Code/TMSrv/ProcessClientMessage.cpp:214-216` (routing), `Code/TMSrv/ProcessClientMessage.h:106` (declaration), `Code/Basedef.h:414-436` (`STRUCT_SCORE`), `Code/Basedef.h:438-481` (`STRUCT_MOB`), `Code/Basedef.h:483-543` (`STRUCT_MOBEXTRA`), `Code/Basedef.h:858-880` (`STRUCT_SPELL`), `Code/Basedef.h:882-906` (`STRUCT_ITEMLIST`), `Code/Basedef.cpp:810-856` (`BASE_GetBonusSkillPoint`), `Code/Basedef.cpp:858-1060` (`BASE_GetBonusScorePoint`), `Code/TMSrv/CMob.cpp:598-780` (`CMob::GetCurrentScore`), `Code/TMSrv/SendFunc.cpp:27-45, 1136-1231, 1622-1650` (send primitives), `Code/TMSrv/Server.cpp:684-707` (`AddCrackError`), `Code/TMSrv/Language.h:105-111, 126, 421-422` (UI strings)
**Project:** W2PP - decompiled Win32/MSVC MMORPG server emulator (W2)
**Service:** TMSrv (game/world server)
**Analysis date:** 2026-11-15 22:33:08
**Method:** static source analysis only. No code was executed, modified, built, or refactored.

---

## 1. Executive Summary

### 1.1 Purpose

The ApplyBonus Handler is the single server-side authority for converting a player's accumulated progression budgets into permanent character power. Three distinct budgets exist on the character record - `ScoreBonus` (free attribute points), `SpecialBonus` (special/mastery points) and `SkillBonus` (skill-purchase points) - and a single client message `_MSG_ApplyBonus` (wire Type `631` / `0x0277`) selects which budget to spend, on which target attribute or skill, and how much. The entire domain - point quantum, per-attribute side effects, level-derived ceilings, absolute ceilings, class restrictions, skill prerequisite chains, coin prices and the audit trail - is implemented inline in one 230-line function with no helper module, no service class and no state machine.

The component is best understood as a *terminal*, not a service: it does not create budget, it does not recompute budget, and it does not persist. It only consumes budget that other components (level-up, quest, item use, class change, login reconciliation) produced, applies the corresponding mutation to `pMob[conn].MOB`, and notifies the client. Every invariant that protects the economy therefore depends on the budgets arriving already consistent - an assumption the component neither verifies nor enforces before spending.

### 1.2 Role in the system

- **Architectural layer:** the client-message boundary of TMSrv, one arm of a 100+ case `switch` in `ProcessClientMessage`. It is the *only* writer of `ScoreBonus`, `SpecialBonus` and `SkillBonus` decreases in the whole server, and the only writer of `BaseScore.Str/Int/Dex/Con` increases outside GM commands (`Code/TMSrv/imple.cpp:118-260`), level-up (`Code/TMSrv/CMob.cpp:1080-1118`) and the score-reset NPC (`Code/TMSrv/_MSG_Quest.cpp:1868-1889`).
- **Caller:** exactly one, `ProcessClientMessage(conn, pMsg, FALSE)` at `Code/TMSrv/ProcessClientMessage.cpp:214-216`, reached from the WSA read pump at `Code/TMSrv/Server.cpp:4001`.
- **Callees:** the send primitives `SendEtc`, `SendScore`, `SendHpMode`, `SendClientMessage`; the derivation primitive `CMob::GetCurrentScore`; the anti-cheat counter `AddCrackError`; the text/audit primitives `Log` and the global `temp[4096]` buffer; and two read-only static data tables, `g_pSpell` (`SkillData.bin`) and `g_pItemList` (`ItemList.bin`).
- **Sibling components:** twenty-four other `_MSG_*.cpp` handlers sharing the same preamble idiom (`if (Hp <= 0 || Mode != USER_PLAY) { SendHpMode; AddCrackError; return; }`). ApplyBonus is the only one of them that is purely a *progression* terminal rather than an item/inventory terminal.
- **Architectural position:** relative to the budget producers the component is a leaf consumer; relative to character power (attack damage, shop prices, combine rates, HP/MP pools) it is an upstream source. Its blast radius is therefore game-wide and economically significant, even though the code is 8 KB.

### 1.3 Key findings

| # | Finding | Severity |
|---|---------|----------|
| F-01 | **Celestial skill budget is faked, then subtracted from the real field.** For `CELESTIAL`, `CELESTIALCS` and `SCELESTIAL` the affordability check uses a hardcoded `myskillbonus = 1500` (`:146-147`), but the charge at `:205` subtracts from the real `unsigned short MOB.SkillBonus`. A celestial character with 0 remaining points can learn a 100-point skill, wrapping the field to `65436`. The corruption is then made permanent by `BASE_GetBonusSkillPoint`, which assigns the negative remainder to the same unsigned field (`Code/Basedef.cpp:846-851`). | Critical |
| F-02 | **No attribute ceiling in the score branch.** `BaseScore.Str/Int/Dex/Con` are `short` (`Code/Basedef.h:430-433`) and are incremented by up to 100 per packet with no upper bound (`:45-63`). Crossing 32767 wraps to a negative attribute; the derived-score projector then propagates the negative value into damage and HP/MP math. | High |
| F-03 | **Budget wraparound is a reusable allocation multiplier.** `BASE_GetBonusScorePoint` writes `mob->ScoreBonus = leveluse - totaluse` into an `unsigned short` (`Code/Basedef.cpp:905, 938, 1002`). Whenever the claw-back heuristic fails (no single stat is large enough to absorb `dif`), the negative result becomes ~65500 points, i.e. 655 bulk allocations of 100. The ApplyBonus handler accepts that budget without question. | High |
| F-04 | **Bulk quantum bypasses the intent of the 300-point threshold.** `pontos = 100` when `ScoreBonus >= 300` (`:40-41`) and is applied with no per-attribute ceiling check, so a single packet can move 100 points of Str/Con/Int in one atomic step, exceeding the value the level-derived caps were calibrated for. | Medium |
| F-05 | **`Mastery`-style caps are inconsistent across the four special categories.** `Special[1..3]` can reach 255 when the corresponding "ultimate" skill bit (7/15/23) is set, but `Special[0]` is permanently capped at 200 (`:93-96`) and no gameplay reader of `Special[0]` exists anywhere in TMSrv (see A-01). | Medium |
| F-06 | **`TargetID` validation is dead weight.** The skill branch requires `TargetID` in `[MAX_USER, MAX_MOB)` - i.e. a mob slot (`:138-139`) - and then never uses the value. It cannot protect anything, and it forces every legitimate client to submit an unrelated NPC/mob index. | Medium |
| F-07 | **No packet-size or structure validation exists upstream.** `BASE_CheckPacket` is entirely commented out and unconditionally returns `FALSE` (`Code/Basedef.cpp:6475-6596`); `CPSock::ReadMessage` only enforces `12 <= Size <= 8192` (`Code/CPSock.cpp:397-411`). The handler casts and dereferences 18 bytes regardless of the declared size (`:23`). | Medium |
| F-08 | **No trade-state guard, unlike every sibling handler.** `_MSG_ReqBuy.cpp:32,38` and `_MSG_UseItem.cpp:51-58` abort when `TradeMode`/`Trade.OpponentID` is set; ApplyBonus has no such guard (`:21-30`), so budget can be spent while the character is inside a trade window. | Medium |
| F-09 | **No rate limit, cooldown or replay protection.** The WSA read pump drains the entire receive buffer in an unbounded `while(1)` (`Code/TMSrv/Server.cpp:3973-4002`), so an arbitrary number of ApplyBonus packets can be processed in a single event. The only throttle is the budget itself. | Medium |
| F-10 | **Audit lines are format-string sinks.** `Log` uses its assembled buffer as the `printf` format (`fprintf(fLogFile, LogTemp)` at `Code/TMSrv/Server.cpp:6723`) and `MobName` (16 bytes, `NAME_LENGTH`) is interpolated into that buffer at `:69`, `:112`, `:210`. A name containing `%` specifiers becomes part of the format string. | Medium |
| F-11 | **Dead ternary on the ultimate-skill price.** `ClassMaster != MORTAL && ClassMaster != ARCH ? 50000000 : 50000000` (`:175`, duplicated at `:199`) yields the same value on both branches; the apparent intent (a different price per class master) is not implemented. | Low |
| F-12 | **Dead stores on derived maxima.** `CurrentScore.MaxMp += 2*pontos` (`:52`) and `CurrentScore.MaxHp += 2*pontos` (`:62`) are immediately overwritten by `GetCurrentScore`, which re-projects `BaseScore` over `CurrentScore` (`Code/Basedef.cpp:2977-2982`). Only the `BaseScore` increments matter. | Low |
| F-13 | **Failure paths are inconsistent and partly silent.** Invalid stat/special `Detail` produce server-side log lines only, with no client reply (`:75`, `:123`); an out-of-range `BonusType` produces nothing at all (`:32, :85, :129, :250`); only the skill branch consistently answers with a UI message. | Low |
| F-14 | **`SendScore` broadcasts the full stat block to the whole 33x33 view grid** (`Code/TMSrv/SendFunc.cpp:1136-1192`, `GridMulticast(..., 0)` at `:1190`, `VIEWGRIDX/Y = 33` at `Code/Basedef.h:95-96`). Every allocation is O(1089) grid scans and exposes the player's current statistics to nearby clients. | Low |
| F-15 | **No persistence is triggered.** The handler never calls `SaveUser`; it relies on the minute-sweep saver (`Code/TMSrv/ProcessSecMinTimer.cpp:189`) and the logout saver (`Code/TMSrv/Server.cpp:7056-7083, 7117`). A crash between allocation and save loses the spent budget together with the gain, and a save rollback can replay an allocation. | Informational |
| F-16 | **The anti-cheat counter is not a control.** Malformed-state packets add 10 crack points (`:28`) against a disconnect threshold of 2,000,000,000 (`Code/TMSrv/Server.cpp:694`), i.e. 200,000,000 events. | Informational |
| F-17 | **Zero automated test coverage.** No test project, no test framework, no test files, no CI workflow anywhere in the repository (evidence in section 10). | High (process) |

---

## 2. Data Flow Analysis

### 2.1 Macro flow (network packet to world state)

```
1.  Client writes a HEADER+payload frame with Type = 631 (119 | FLAG_CLIENT2GAME)     Basedef.h:1769
      struct MSG_ApplyBonus { HEADER(12); short BonusType; short Detail; unsigned short TargetID; }  -> 18 bytes
2.  WSA_READ fires; CPSock::ReadMessage validates and decrypts in place               Server.cpp:3975 / CPSock.cpp:353-470
      - INITCODE == 0x1F11F311
      - 12 <= Size <= 8192  (sizeof(HEADER) .. MAX_MESSAGE_SIZE)                      CPSock.cpp:397
      - full frame present, CheckSum == computed
3.  Decode error (Error == 1 or 2) -> log "err,read" and stop; handler not called     Server.cpp:3990-3999
4.  ProcessClientMessage(conn, pMsg, FALSE)                                           ProcessClientMessage.cpp:38
      - std->ID in [0, MAX_USER) else log "err,packet"                                ProcessClientMessage.cpp:42-51
      - ServerDown >= 120 -> drop
      - pUser[conn].LastReceiveTime = SecCounter
      - Type == _MSG_Ping -> drop; ClientTick == SKIPCHECKTICK -> drop (client-originated only)
5.  switch(std->Type) -> case _MSG_ApplyBonus -> Exec_MSG_ApplyBonus(conn, pMsg)      ProcessClientMessage.cpp:214-216
6.  Cast pMsg to MSG_ApplyBonus*                                                        _MSG_ApplyBonus.cpp:23
7.  Liveness gate: MOB.CurrentScore.Hp > 0 AND pUser[conn].Mode == USER_PLAY           :25
      else SendHpMode(conn) + AddCrackError(conn, 10, 20) + return                    :27-29
8.  Branch on m->BonusType                                                             :32 / :85 / :129
      0 -> score (Str/Int/Dex/Con)      1 -> special (Special[0..3])      2 -> skill (LearnedSkill + SkillBonus)
9.  Branch-specific validation and budget checks (see 2.2-2.4)
10. In-place mutation of pMob[conn].MOB.{BaseScore, ScoreBonus, SpecialBonus, SkillBonus, LearnedSkill, Coin}
11. Re-derive: pMob[conn].GetCurrentScore(conn)   (BaseScore -> CurrentScore projector) :65 / :107 / :206
12. Notify: SendEtc(conn)  (budgets, coin, exp, LearnedSkill)                            SendFunc.cpp:1195-1231
            SendScore(conn) (CurrentScore + affects, grid-multicast)                     SendFunc.cpp:1136-1192
13. Audit: sprintf(temp, "etc,applybonus ...") -> Log(temp, AccountName, 0)             :69-70 / :112-113 / :210-211
14. Return to dispatcher; no DB write, no message back to the requester beyond 12        :71 / end :250
```

### 2.2 Branch A - score allocation (`BonusType == 0`, `:32-84`)

```
:34  ScoreBonus > 0 ?  --no--> SendScore(conn); return            (silent resync only)
:36  Detail in [0,3] ? --no--> Log("err,applybonus skill out of bound"); return   (no client reply)
:38  pontos = 1
:40  ScoreBonus >= 300 ? -> pontos = 100
:43  ScoreBonus -= pontos
:45  Detail 0 -> BaseScore.Str += pontos
:48  Detail 1 -> BaseScore.Int += pontos; BaseScore.MaxMp += 2p; CurrentScore.MaxMp += 2p
:55  Detail 2 -> BaseScore.Dex += pontos
:58  Detail 3 -> BaseScore.Con += pontos; BaseScore.MaxHp += 2p; CurrentScore.MaxHp += 2p
:65  GetCurrentScore(conn)   -> CurrentScore = BaseScore (+ equipment/affect abilities, Hp/Mp preserved)
:66  SendEtc(conn)
:67  SendScore(conn)
:69  Log("etc,applybonus name:%s bonustype:%d detail:%d pontos:%d")
:71  return
```

Note the ordering asymmetry: the budget is decremented before any stat is written, but because the whole sequence is unconditional after the two guards, there is no observably reachable state where the budget is spent and the stat is not granted.

### 2.3 Branch B - special allocation (`BonusType == 1`, `:85-127`)

```
:87  SpecialBonus > 0 ? --no--> SendEtc(conn); return
:89  Detail in [0,3] ? --no--> Log("err,applybonus Special, out of boundary"); return   (no client reply)
:91  max_special_level = 3 * (BaseScore.Level + 1)
:93  max_special = 200
:95  (bit7 & LearnedSkill) && Detail==1  ||  (bit15 & LearnedSkill) && Detail==2  ||  (bit23 & LearnedSkill) && Detail==3
       -> max_special = 255
:98  ClassMaster in {CELESTIAL, CELESTIALCS, SCELESTIAL} -> max_special_level += 1200
:101 Special[Detail] < (max_special_level >> 1) ? --no--> SendClientMessage(_NN_Maximum_Point_Now = 105); done
:103 Special[Detail] < max_special ? --no--> SendClientMessage(_NN_Maximum_Point_200_Now = 106); done
:105 SpecialBonus--
:106 Special[Detail]++
:107 GetCurrentScore(conn)
:109 SendScore(conn)
:110 SendEtc(conn)
:112 Log("etc,applybonus name:%s bonustype:%d special%d")
```

This branch is the most heavily gated part of the component: two independent ceilings must both pass, and it is the only branch with no bulk quantum (strictly one point per packet).

### 2.4 Branch C - skill allocation (`BonusType == 2`, `:129-249`)

```
:131 MobClass   = MOB.Class                       (unsigned char, 0..3)
:132 skillclass = (m->Detail - 5000) / 24
:133 skillpos   = (m->Detail - 5000) % 24
:134 MobTarget  = m->TargetID                     (computed, validated, then never used)
:136 Detail in [5000,5095] ? --no--> Log("err,MSG_ApplyBonus, request skill out of bound"); return
:138 TargetID in [MAX_USER, MAX_MOB) ? --no--> Log("err,MSG_ApplyBonus, request target id is out of bound"); return
:140 MobClass == skillclass ? --no--> SendClientMessage(_NN_Cant_Learn_Other_Class_Skill = 107); return
:142 Skillbonus   = g_pSpell[skillpos + 24*skillclass].SkillPoint
:144 myskillbonus = MOB.SkillBonus
:146 ClassMaster in celestial family ? -> myskillbonus = 1500
:149 Skillbonus <= myskillbonus ? --no--> SendClientMessage(_NN_Not_Enough_Skill_Point = 108); return
:153 skillpos in {7,15,23} ?
       :155 any of bits 7/15/23 already set -> _NN_Only_OneSkillLearn(402); return
       :163-167 count learned bits in the 8-bit block ending at skillpos
       :169 skillin != 7 -> _NN_Befor_LearnSkill(403); return
       :175 coin = 50000000 (degenerate ternary)
       :177 Coin < coin -> sprintf(temp, _DN_D_Cost(204), coin); SendClientMessage; return
:192 reqlv = (ClassMaster in {MORTAL,ARCH}) ? g_pItemList[Detail].ReqLvl : 0
:193 CurrentScore.Level >= reqlv ? --no--> _NN_Need_More_Level_To_Learn(110); return
:195 CurrentScore.Special[1] >= ReqInt && Special[2] >= ReqDex && Special[3] >= ReqCon
       --no--> _NN_Need_More_Mastery_To_Learn(111); return
:199 coin = 50000000 (degenerate ternary, recomputed)
:201 skillpos in {7,15,23} -> MOB.Coin -= coin
:204 LearnedSkill |= (1 << skillpos)
:205 SkillBonus  -= Skillbonus
:206 GetCurrentScore(conn)
:207 SendScore(conn)
:208 SendEtc(conn)
:210 Log("etc,applybonus name:%s bonustype:%d skillpos:%d")
```

### 2.5 Exit matrix

| Condition | Client sees | Server log | State mutated | Line |
|---|---|---|---|---|
| Dead or not `USER_PLAY` | `_MSG_SetHpMode` | `cra point: 10 type: 20` | none | `:25-30` |
| `BonusType` not 0/1/2 | nothing | nothing | none | `:250` |
| Score: `ScoreBonus == 0` | `_MSG_UpdateScore` | nothing | none | `:79-83` |
| Score: `Detail` out of 0..3 | nothing | `err,applybonus skill out of bound` | none | `:73-77` |
| Score: success | `_MSG_UpdateEtc` + `_MSG_UpdateScore` | `etc,applybonus ... pontos:n` | budget, attribute, MaxHP/MaxMP | `:43-71` |
| Special: `SpecialBonus == 0` | `_MSG_UpdateEtc` | nothing | none | `:125-126` |
| Special: `Detail` out of 0..3 | nothing | `err,applybonus Special, out of boundary` | none | `:122-123` |
| Special: half-level ceiling | `_NN_Maximum_Point_Now` (105) | nothing | none | `:118-119` |
| Special: 200/255 ceiling | `_NN_Maximum_Point_200_Now` (106) | nothing | none | `:115-116` |
| Special: success | `_MSG_UpdateScore` + `_MSG_UpdateEtc` | `etc,applybonus ... specialN` | `SpecialBonus`, `Special[n]` | `:105-113` |
| Skill: `Detail` out of range | nothing | `err,MSG_ApplyBonus, request skill out of bound` | none | `:244-247` |
| Skill: `TargetID` out of range | nothing | `err,MSG_ApplyBonus, request target id is out of bound` | none | `:238-241` |
| Skill: wrong class | `_NN_Cant_Learn_Other_Class_Skill` (107) | nothing | none | `:232-235` |
| Skill: unaffordable | `_NN_Not_Enough_Skill_Point` (108) | nothing | none | `:226-229` |
| Skill: second ultimate | `_NN_Only_OneSkillLearn` (402) | nothing | none | `:155-159` |
| Skill: prerequisites missing | `_NN_Befor_LearnSkill` (403) | nothing | none | `:169-173` |
| Skill: insufficient coin | `_DN_D_Cost` (204) rendered with 50,000,000 | nothing | none | `:177-182` |
| Skill: already learned | `_NN_Already_Learned_It` (109) | nothing | none | `:185-189` |
| Skill: level too low | `_NN_Need_More_Level_To_Learn` (110) | nothing | none | `:219-223` |
| Skill: mastery too low | `_NN_Need_More_Mastery_To_Learn` (111) | nothing | none | `:213-217` |
| Skill: success | `_MSG_UpdateScore` + `_MSG_UpdateEtc` | `etc,applybonus ... skillpos:n` | `Coin`, `LearnedSkill`, `SkillBonus` | `:199-211` |

### 2.6 Protocol surface

The component exposes **no HTTP, REST, GraphQL or gRPC endpoint**. Its sole external surface is the inbound TCP game-protocol message `_MSG_ApplyBonus`, which is handled in-process and never produces a dedicated reply message - state changes are propagated through the generic `_MSG_UpdateScore` (`SendScore`) and `_MSG_UpdateEtc` (`SendEtc`) broadcasts. There is no server-to-client ApplyBonus acknowledgement, which is why several failure paths (see F-13) leave the client without feedback.

| Surface | Direction | Type id | Payload | Size |
|---|---|---|---|---|
| `_MSG_ApplyBonus` | client -> game | `631` (`119 \| 0x0200`) | `MSG_ApplyBonus` | 18 bytes (`12 + 2 + 2 + 2`) |
| `_MSG_UpdateScore` | game -> client(s) | `_MSG_UpdateScore` | `MSG_UpdateScore` | via `SendScore` |
| `_MSG_UpdateEtc` | game -> client | `_MSG_UpdateEtc` | `MSG_UpdateEtc` | via `SendEtc` |
| `_MSG_SetHpMode` | game -> client | `_MSG_SetHpMode` | `MSG_SetHpMode` | via `SendHpMode` |
| `_MSG_MessagePanel` | game -> client | `_MSG_MessagePanel` | `MSG_MessagePanel` | via `SendClientMessage` |

### 2.7 Persistence path

The handler mutates only the in-memory `pMob[conn].MOB` and `pUser[conn]` records. Durability is external and deferred:

```
ApplyBonus  ->  pMob[conn].MOB (memory only)
                    |
                    +-- ProcessSecMinTimer (1 tick/min) -> SaveUser(SaveCount, 0)   ProcessSecMinTimer.cpp:189
                    +-- CharLogOut -> SaveUser(conn, 1)                              Server.cpp:7117
                    +-- other handlers that call SaveUser on notable events          Server.cpp:7056-7083
                    ->  _MSG_DBSaveMob (memcpy of STRUCT_MOB)  -> DBSrv              Server.cpp:7063
```

Because the budgets are *not* recomputed before saving, the persisted `ScoreBonus`/`SpecialBonus`/`SkillBonus` are exactly the values the handler left behind - including the wrapped values described in F-01 and F-03.

---

## 3. Business Rules & Logic

## Overview of the business rules:

| Rule Type | Rule Description | Location |
|-----------|------------------|----------|
| Precondition | Actor must be alive (`CurrentScore.Hp > 0`) and in `USER_PLAY` mode; otherwise HP/mode resync, 10 crack points, abort | `Code/TMSrv/_MSG_ApplyBonus.cpp:25-30` |
| Dispatch | `BonusType` selects the allocation domain (0 = score, 1 = special, 2 = skill); any other value is a silent no-op | `Code/TMSrv/_MSG_ApplyBonus.cpp:32, 85, 129, 250` |
| Validation | Score domain requires `ScoreBonus > 0`, else only a score resync is sent | `Code/TMSrv/_MSG_ApplyBonus.cpp:34, 79-83` |
| Validation | Score domain `Detail` must be 0..3 (Str/Int/Dex/Con); violation is logged, never answered | `Code/TMSrv/_MSG_ApplyBonus.cpp:36, 73-77` |
| Business Logic | Score point quantum is 1, or 100 when `ScoreBonus >= 300` | `Code/TMSrv/_MSG_ApplyBonus.cpp:38-41` |
| Accounting | `ScoreBonus` is debited by the quantum before the attribute is credited | `Code/TMSrv/_MSG_ApplyBonus.cpp:43` |
| Business Logic | `Detail` 0 -> Str, 1 -> Int (+2 MaxMp/point), 2 -> Dex, 3 -> Con (+2 MaxHp/point) | `Code/TMSrv/_MSG_ApplyBonus.cpp:45-63` |
| Invariant | Derived scores are re-projected from `BaseScore` after every mutation | `Code/TMSrv/_MSG_ApplyBonus.cpp:65, 107, 206` |
| Notification | Success is echoed with `SendEtc` (budgets) and `SendScore` (grid-multicast of `CurrentScore`) | `Code/TMSrv/_MSG_ApplyBonus.cpp:66-67, 109-110, 207-208` |
| Audit | Every success writes an `etc,applybonus` line keyed by account name | `Code/TMSrv/_MSG_ApplyBonus.cpp:69-70, 112-113, 210-211` |
| Validation | Special domain requires `SpecialBonus > 0`, else only `SendEtc` | `Code/TMSrv/_MSG_ApplyBonus.cpp:87, 125-126` |
| Validation | Special domain `Detail` must be 0..3 | `Code/TMSrv/_MSG_ApplyBonus.cpp:89, 122-123` |
| Constraint | Special ceiling from level: `3 * (BaseScore.Level + 1)`, plus 1200 for the celestial class masters | `Code/TMSrv/_MSG_ApplyBonus.cpp:91, 98-99` |
| Constraint | Hard gate: `Special[Detail]` must be below half the level ceiling | `Code/TMSrv/_MSG_ApplyBonus.cpp:101, 118-119` |
| Constraint | Absolute gate: `Special[Detail] < 200`, raised to 255 for `Detail` 1/2/3 when `LearnedSkill` bit 7/15/23 is set | `Code/TMSrv/_MSG_ApplyBonus.cpp:93-96, 103, 115-116` |
| Accounting | Exactly one `SpecialBonus` point buys exactly one `Special[Detail]` point | `Code/TMSrv/_MSG_ApplyBonus.cpp:105-106` |
| Encoding | Skill identity is `Detail = 5000 + 24*class + position`, valid only in `[5000, 5095]` | `Code/TMSrv/_MSG_ApplyBonus.cpp:132-133, 136, 244-247` |
| Validation | Skill request requires `TargetID` in `[MAX_USER, MAX_MOB)`, i.e. a mob slot; value is otherwise unused | `Code/TMSrv/_MSG_ApplyBonus.cpp:134, 138-139, 238-241` |
| Constraint | Only skills belonging to the actor's own class may be learned | `Code/TMSrv/_MSG_ApplyBonus.cpp:140, 232-235` |
| Accounting | Skill price is `g_pSpell[...].SkillPoint` and must not exceed the skill budget, which is forced to 1500 for celestial masters | `Code/TMSrv/_MSG_ApplyBonus.cpp:142-149, 226-229` |
| Constraint | At most one "ultimate" skill (position 7, 15 or 23) per character | `Code/TMSrv/_MSG_ApplyBonus.cpp:153-159` |
| Constraint | An ultimate skill requires the seven preceding skills of its 8-skill block | `Code/TMSrv/_MSG_ApplyBonus.cpp:161-173` |
| Constraint | An ultimate skill additionally costs 50,000,000 coins | `Code/TMSrv/_MSG_ApplyBonus.cpp:175-182, 199-202` |
| Constraint | A skill cannot be learned twice | `Code/TMSrv/_MSG_ApplyBonus.cpp:185-189` |
| Constraint | Level requirement comes from `g_pItemList[Detail].ReqLvl` for `MORTAL`/`ARCH`, and is 0 for celestial masters | `Code/TMSrv/_MSG_ApplyBonus.cpp:192-193, 219-223` |
| Constraint | Mastery requirement: `CurrentScore.Special[1..3]` must meet `ReqInt`/`ReqDex`/`ReqCon` from `g_pItemList[Detail]` | `Code/TMSrv/_MSG_ApplyBonus.cpp:195-197, 213-217` |
| Transaction | On success: optional coin charge, `LearnedSkill` bit set, `SkillBonus` debited, then recompute/notify/audit | `Code/TMSrv/_MSG_ApplyBonus.cpp:199-211` |
| Persistence | The handler never writes to the database; state survives via the minute sweep and logout save | `Code/TMSrv/ProcessSecMinTimer.cpp:189`, `Code/TMSrv/Server.cpp:7056-7083, 7117` |
| Reconciliation | Budgets are produced and repaired outside the component by `BASE_GetBonusScorePoint` / `BASE_GetBonusSkillPoint` | `Code/Basedef.cpp:810-856, 858-1060`, `Code/TMSrv/ProcessDBMessage.cpp:779-780` |
| Absence | No trade guard, no rate limit, no replay protection, no packet-size validation, no DB write, no atomic rollback | `Code/TMSrv/_MSG_ApplyBonus.cpp:21-250`, `Code/Basedef.cpp:6475-6596`, `Code/CPSock.cpp:397-411` |

## Detailed breakdown of the business rules:

---

### Business Rule: Liveness and play-mode precondition

**Overview**:
Every allocation request is rejected unless the actor is a living, fully logged-in character. The gate combines a hit-point test on the authoritative current score with the session state machine value `USER_PLAY`.

**Detailed description**:
The check at `:25` reads `pMob[conn].MOB.CurrentScore.Hp <= 0` OR `pUser[conn].Mode != USER_PLAY`. `CurrentScore.Hp` is the *derived* hit-point value (re-projected by `GetCurrentScore` from `BaseScore` plus equipment and affects), so a character killed by the last damage tick has `Hp <= 0` while still connected; such a session may not allocate points until it respawns. `USER_PLAY` is the terminal state of the `CUser` state machine (`Code/TMSrv/CUser.h:36`, value 22), reached only after the DB character-load reply has been fully processed; earlier states are `USER_SELCHAR` (11) and later ones `USER_SAVING4QUIT` (24). The comparison therefore also excludes characters in the character-selection screen and characters mid-logout.

On failure the handler calls `SendHpMode(conn)` (`Code/TMSrv/SendFunc.cpp:1622-1650`), which is a defensive resynchronisation of the client's hit-point and mode display, and then `AddCrackError(conn, 10, 20)`. `AddCrackError` (`Code/TMSrv/Server.cpp:684-707`) adds 10 to the per-session `NumError` counter, logs `cra point: 10 type: 20` because type 20 is not one of the suppressed types (3, 8, 15), and force-logs-out the character once `NumError >= 2000000000`. Reaching that threshold requires 200 million rejected packets, so the counter functions as an observability signal rather than a control.

Note the ordering: the mode test is a logical AND of two conditions evaluated in one expression with short-circuit `||`, so a dead-but-not-playing session never evaluates `pUser[conn].Mode`. Because `conn` is already range-checked by the dispatcher (`Code/TMSrv/ProcessClientMessage.cpp:42`), neither array access can be out of bounds.

**Rule workflow**:
```
receive _MSG_ApplyBonus
  -> CurrentScore.Hp <= 0 ?
       yes -> SendHpMode(conn); AddCrackError(conn, 10, 20); return
  -> pUser[conn].Mode != USER_PLAY ?
       yes -> SendHpMode(conn); AddCrackError(conn, 10, 20); return
  -> continue to BonusType dispatch
```

---

### Business Rule: BonusType domain dispatch

**Overview**:
`BonusType` is the discriminant of a three-valued union of allocation operations. Values outside `{0, 1, 2}` are accepted by the function and discarded without any effect, log entry or client response.

**Detailed description**:
The function is structured as `if (BonusType == 0) ... else if (BonusType == 1) ... else if (BonusType == 2) ...` with no final `else` and no explicit `return` at the end. `MSG_ApplyBonus::BonusType` is a signed `short` (`Code/Basedef.h:1773`), so the reachable domain is -32768..32767, of which only three values are meaningful. This is a message-type-in-message design: the outer protocol already selects the handler, and an inner discriminator selects the operation, which means the wire contract carries two independent type fields that must agree.

The practical consequence is that a malformed or exploratory client can probe `BonusType` values with no server-side footprint at all - no log line, no counter increment, no reply. Compare this with the sibling handlers, which log every out-of-domain branch. From an operator's perspective, silent acceptance removes the only signal that would reveal fuzzing of this message.

The observed comment on the field (`Code/Basedef.h:1773`) documents `0:ScoreBonus 1:Special 2:Skill`, and the `Detail` comment (`Code/Basedef.h:1774`) documents only the score meaning (`0:Str 1:Int 2:Dex 3:Con`); the two other `Detail` encodings (special index, and the packed `5000 + skill` value) are not documented on the wire structure at all, being discoverable only from the handler body. That documentation gap is the main source of ambiguity in the message contract (see A-03).

**Rule workflow**:
```
BonusType == 0 -> score branch  (:32-84)
BonusType == 1 -> special branch (:85-127)
BonusType == 2 -> skill branch  (:129-249)
otherwise      -> fall through the function, return silently
```

---

### Business Rule: Score budget must be positive

**Overview**:
The score branch refuses to act when the free-attribute budget is zero, and answers with a score resynchronisation instead of an error.

**Detailed description**:
`:34` guards the whole score branch on `pMob[conn].MOB.ScoreBonus > 0`. `ScoreBonus` is an `unsigned short` (`Code/Basedef.h:465`), so the comparison is unsigned and a wrapped value (~65000) passes; a genuinely exhausted budget (0) fails. Interestingly, the exhausted case *is* answered - `SendScore(conn)` at `:81` re-broadcasts the current score - while the invalid-`Detail` case at `:73-77` is not. That asymmetry suggests the zero-budget path was the one exercised by the normal client, which simply wants a refresh when it has nothing left to spend.

This is the only place in the component where a depleted budget produces a *resynchronisation* rather than a UI message. `SendScore` is a relatively expensive operation (a 33x33 grid scan, section F-14), so a client that repeatedly requests allocation at zero budget causes measurable work, though each request changes no state.

Because the check precedes every mutation, a character can never spend below zero through this branch: the quantum (1 or 100) is only applied when the balance is already strictly positive, and 100 is only chosen when the balance is at least 300 (`:40-41`), so the subtraction at `:43` cannot wrap. The score branch is, in that narrow sense, the only arithmetically safe branch in the component.

**Rule workflow**:
```
ScoreBonus > 0 ?
  no  -> SendScore(conn); return
  yes -> validate Detail, then debit quantum and credit attribute
```

---

### Business Rule: Score allocation target must be a primary attribute

**Overview**:
`Detail` must be one of the four primary attribute selectors 0..3, mapping to Str, Int, Dex, Con. Anything else is treated as a protocol violation and logged, with no reply to the client.

**Detailed description**:
`:36` tests `m->Detail >= 0 && m->Detail <= 3`. Because `Detail` is a signed `short` and this is the only test, negative values are explicitly excluded - which matters because `Detail` is later used as an index (`Special[m->Detail]` in the sibling branch); the sibling branch repeats the same guard, so index safety is local and duplicated rather than centralised.

The failure path (`:73-77`) logs the literal string `err,applybonus skill out of bound` - a copy-paste artefact from the skill branch, since no skill is involved at this point. The log carries the account name and the client IP, making it the only forensic trace of a malformed score request. There is no `AddCrackError` call on this path, so repeated malformed requests do not affect the crack counter at all.

The four mappings are not symmetrical. Str and Dex are pure single-field increments, whereas Int and Con carry a derived-pool side effect: `BaseScore.MaxMp += 2*pontos` / `BaseScore.MaxHp += 2*pontos`. That factor of 2 matches the attribute model in `BASE_GetHpMp` (`Code/Basedef.cpp:1183, 1195`), which computes `stathp = (Con - classBaseCon) * 2` and `statmp = (Int - classBaseInt) * 2`, so the handler is deliberately maintaining an invariant that the level-up/next-login recomputation will reproduce rather than contradict. The parallel `CurrentScore.MaxMp/MaxHp` increments on the next lines are redundant (F-12) because `GetCurrentScore` copies `BaseScore` over `CurrentScore`.

**Rule workflow**:
```
Detail in [0,3] ?
  no  -> Log("err,applybonus skill out of bound", account, ip); return
  yes -> apply the selected attribute mapping
```

---

### Business Rule: Score point quantum and bulk allocation threshold

**Overview**:
One request normally spends one budget point, except when the balance is at least 300, in which case a single request spends exactly 100 and credits exactly 100.

**Detailed description**:
`:38-41` initialises `pontos = 1` and promotes it to `100` when `ScoreBonus >= 300`. This is a client-convenience optimisation: with a 3-per-level budget accrual, a character that has banked many levels can commit them in bulk instead of clicking three hundred times. The threshold of 300 and the quantum of 100 are both hardcoded literals with no configuration path and no comment; they appear nowhere else in the codebase.

The rule is applied *before* any attribute ceiling is considered, which is the important architectural consequence: a single packet can move an attribute by 100, and 100 is large relative to the level-derived budget the game grants (5 per level for `MORTAL`, 6 per level for `ARCH`/celestial). There is no per-attribute cap in this branch at all - neither `MAX_HP`/`MAX_MP` (2,000,000,000, `Code/Basedef.h:203-204`) nor any attribute-specific limit is consulted - so the only ceiling on Str/Int/Dex/Con is the width of the `short` fields (F-02).

Note that the quantum is *not* adaptive: a balance of 299 yields 299 single-point allocations, a balance of 300 yields 3 allocations of 100. The number of packets required is therefore not monotonic in the balance, which matters for anything that counts allocations (audit review, or a rate limiter that keys on request frequency rather than point count).

**Rule workflow**:
```
pontos = 1
ScoreBonus >= 300 ? -> pontos = 100
ScoreBonus -= pontos
credit attribute by pontos
```

---

### Business Rule: Budget debit precedes attribute credit

**Overview**:
The score budget is decremented first (`:43`), then the attribute is incremented. The two writes are not bracketed by any transaction, rollback or verification, and there is no failure path between them.

**Detailed description**:
The sequence at `:43-63` is unconditional once the two guards have passed: `ScoreBonus -= pontos`, then exactly one of four `if` statements writes the attribute. Because `Detail` was already validated to be in 0..3, exactly one branch executes; the four `if`s are not `else if`, so the code relies on the validated domain rather than on mutual exclusion in the control flow. If the validation were ever loosened, multiple attributes would be credited by a single debit (a silent duplication).

Atomicity here is *temporal*, not transactional: the handler runs to completion on the single Win32 UI thread that owns the socket (`WSAAsyncSelect` completions are delivered to `MainWndProc`, `Code/CPSock.cpp:239, 323`), so no other packet, timer or DB reply can interleave between `:43` and `:65`. There is consequently no partially applied allocation observable by any other code path. The weakness is the opposite one: nothing verifies that the debited amount corresponds to a legitimate budget. If `ScoreBonus` were already inflated (F-03), the handler mechanically converts it into permanent attributes.

The debit itself cannot underflow in this branch, because the promotion to 100 requires a balance of at least 300 (see the previous rule). It also cannot be rejected: there is no "insufficient budget" check after the subtraction, only the pre-check `> 0`. A balance of 1 with `pontos = 1` leaves 0, which is the intended terminal state.

**Rule workflow**:
```
ScoreBonus -= pontos          (no verification of the resulting value)
if Detail == 0 -> Str  += pontos
if Detail == 1 -> Int  += pontos; MaxMp += 2*pontos
if Detail == 2 -> Dex  += pontos
if Detail == 3 -> Con  += pontos; MaxHp += 2*pontos
```

---

### Business Rule: Attribute-to-pool coupling (Int -> MaxMp, Con -> MaxHp)

**Overview**:
Allocating into Int also raises the maximum mana pool by two per point; allocating into Con also raises the maximum hit-point pool by two per point. Str and Dex have no pool side effect.

**Detailed description**:
`:48-53` and `:58-63` implement the coupling on `BaseScore` (authoritative) and mirror it on `CurrentScore` (redundant, F-12). The factor two is the game's attribute model, restated in `BASE_GetHpMp` (`Code/Basedef.cpp:1174-1208`): `MaxHp = BaseSIDCHM[cls][4] + (Con - baseCon) * 2 + level * incrementHp[cls]`, clamped to `MAX_HP`; `MaxMp` is the analogous expression over Int. Because the handler writes the same slope, a later `BASE_GetHpMp` recomputation (login, level up, class change, `_MSG_UseItem.cpp:2113, 2147, 3156`) preserves the granted pool rather than erasing it. This is an unusually careful invariant for this codebase and is the strongest evidence that the score branch was written with the recomputation pipeline in mind.

The important gap is the clamp. `BASE_GetHpMp` clamps to `MAX_HP`/`MAX_MP` (`:1188-1192, 1200-1205`); the ApplyBonus branch does not. It cannot exceed those limits through a legitimate budget (65,535 budget points * 2 = 131,070 extra pool, far below 2e9), but it also never checks that `BaseScore.MaxHp` stays consistent with `BaseScore.Con` - the invariant holds only because the increment is written in the same operation. A character whose `Con` is later reduced (the score-reset NPC at `Code/TMSrv/_MSG_Quest.cpp:1869-1876` subtracts stats and then calls `BASE_GetHpMp`) is re-derived correctly; a character whose `Con` becomes negative through a `short` overflow (F-02) would produce a negative `stathp` and a reduced pool on the next recomputation.

The pools are the mechanism by which a score allocation becomes combat power: `CurrentScore.MaxHp` bounds survivability, and `SendScore` transmits it to the client. Because the increment is applied to `BaseScore` before `GetCurrentScore`, the very same packet that spends the point also updates the client's bar maximum (the current HP value is preserved by the projector at `Code/Basedef.cpp:2977-2982`).

**Rule workflow**:
```
Detail 1: Int += pontos; BaseScore.MaxMp += 2*pontos; CurrentScore.MaxMp += 2*pontos
Detail 3: Con += pontos; BaseScore.MaxHp += 2*pontos; CurrentScore.MaxHp += 2*pontos
then GetCurrentScore -> CurrentScore = BaseScore (Hp/Mp preserved), abilities added
```

---

### Business Rule: Derived-score re-projection after mutation

**Overview**:
After every successful mutation the handler calls `pMob[conn].GetCurrentScore(conn)`, which rebuilds `CurrentScore` from `BaseScore` plus equipment/affect/soul modifiers and resets several dependent caches.

**Detailed description**:
`CMob::GetCurrentScore` (`Code/TMSrv/CMob.cpp:598-780`) is the projection function of the component's data model. For player indices it clears `MOB.Clan`, recomputes the four resistances from equipment (clamped to 100), zeroes the `Equip[0]` first effect and `MOB.Rsv`, recomputes `Parry`, forces `pUser[idx].Range = 23`, then invokes `BASE_GetCurrentScore` (`Code/Basedef.cpp:2973-...`), which saves the current Hp/Mp, copies `BaseScore` over `CurrentScore`, restores Hp/Mp, adds equipment and affect abilities (AC, damage, MaxHp, MaxMp, Str, Int, Dex, Con), applies soul multipliers, and finally clamps `Hp <= MaxHp` and `Mp <= MaxMp`.

The handler depends on this function for three distinct effects. First, correctness of what the client sees: without it, `SendScore` would transmit a stale `CurrentScore` and the new attribute would not appear until the next unrelated event. Second, second-order combat statistics: equipment-derived damage, AC and resistances are recalculated, so an Int allocation that unlocks an item requirement is reflected immediately. Third, the clamp: allocating Con raises `MaxHp`, and the clamp at `CMob.cpp:715-719` guarantees `Hp` never exceeds it.

`GetCurrentScore` has 20 call sites across TMSrv (`Server.cpp`, `SendFunc.cpp`, `imple.cpp`, `_MSG_CombineItemEhre.cpp`, `_MSG_TradingItem.cpp`, `ProjectDBMessage.cpp`, and this handler), so the component is one of its most frequent consumers. The call is unconditional in all three branches, including the special branch where no pool value changes - there it exists purely to propagate `Special[]` into the skill-damage computations that read `CurrentScore.Special[1..3]` at attack time (`Code/TMSrv/_MSG_Attack.cpp:262, 461, 850, 923, 979, 1454, 1468`).

**Rule workflow**:
```
mutate BaseScore / Special / budgets
  -> GetCurrentScore(conn)
       -> refresh resist/parry/range caches
       -> CurrentScore = BaseScore (+ abilities, + affects, + soul)
       -> clamp Hp, Mp
  -> SendScore / SendEtc
```

---

### Business Rule: Client notification on success

**Overview**:
A successful allocation is announced with two different messages: `SendEtc` carries the remaining budgets, coin and learned-skill mask to the requesting client only; `SendScore` carries the current score block to every player in the view grid.

**Detailed description**:
`SendEtc` (`Code/TMSrv/SendFunc.cpp:1195-1231`) builds `MSG_UpdateEtc` with `Exp`, `SpecialBonus`, `Learn` (the raw `LearnedSkill` mask), `ScoreBonus`, `SkillBonus`, `Coin`, `Hold` and `Magic`, then `AddMessage`s it. It validates `conn` range, `USER_PLAY` mode and socket validity, and calls `CloseUser(conn)` if the send buffer is full. This is the message that tells the client how many points remain, and it is the only feedback for the "no budget left" special path (`:126`).

`SendScore` (`Code/TMSrv/SendFunc.cpp:1136-1192`) builds `MSG_UpdateScore` with `CurrentScore`, critical, mana-save, guild fields, the affect array, the four resistances, three magic/unknown fields and `Magic`, then calls `GridMulticast(TargetX, TargetY, msg, 0)`. Passing `0` as `skip` means nothing is excluded, so the packet reaches every user in the 33x33 view grid including the sender; `GridMulticast` (`Code/TMSrv/SendFunc.cpp:843-972`) iterates the grid and `AddMessage`s per user, ignoring send failures for other users. The consequence is that each allocation performs up to 1,089 grid cell inspections and up to `MAX_USER` message inserts, and every nearby client receives the allocating player's full stat block - including `Special[]`, which `SendScore` otherwise masks with `0xCC` and then overwrites with `pMob[conn].MOB.Magic` in the same struct (`SendFunc.cpp:1166-1171`).

The two messages are ordered differently in the two branches: score and skill branches call `GetCurrentScore`, then `SendEtc`, then `SendScore`; the special branch calls `GetCurrentScore`, then `SendScore`, then `SendEtc` (`:107-110`). The order has no semantic effect because both messages are appended to the same output buffer and flushed together.

**Rule workflow**:
```
GetCurrentScore(conn)
  -> SendEtc(conn)     : budgets + coin + LearnedSkill to the actor
  -> SendScore(conn)   : CurrentScore to the 33x33 grid
```

---

### Business Rule: Audit trail for every allocation

**Overview**:
Every successful allocation emits a machine-parsable line prefixed `etc,applybonus` containing the character name, the branch, the detail and the amount, keyed by account name.

**Detailed description**:
The three log statements differ only in the trailing field: `pontos:%d` (`:69`), `special%d` (`:112`), `skillpos:%d` (`:210`). All three pass `pUser[conn].AccountName` as the second argument and `0` as the third, which suppresses the IP prefix in `Log` (`Code/TMSrv/Server.cpp:6715-6718`). They are the only audit records of budget consumption in the server; no dedicated transaction ledger exists.

`Log` (`Code/TMSrv/Server.cpp:6701-6727`) builds a 1,024-byte `LogTemp`, prefixing a timestamp, then `sprintf`s `"%s %s %s \n"` with the account and the message, writes it with `fprintf(fLogFile, LogTemp)` and mirrors it into the server window title. Two properties matter here. First, `LogTemp` is used as the *format* argument rather than as `"%s"`, so any `%` sequence inside the message - including one inside `pMob[conn].MOB.MobName` - is interpreted as a conversion specification (F-10). Second, the window-title mirroring means the most recent allocation is visible on the operator console, which is how administrators typically watch this handler in practice.

The message buffers are the shared global `temp[4096]` declared at `Code/TMSrv/Server.cpp:277`; the three `sprintf` calls write well within that bound because the only variable-length field is the 16-byte `MobName` (`Code/Basedef.h:132`). The audit line is written *after* the notification calls, so a `CloseUser` triggered inside `SendEtc` (`SendFunc.cpp:1230`) does not prevent the record from being produced - the `pUser` record is still populated at that point.

**Rule workflow**:
```
sprintf(temp, "etc,applybonus name:%s bonustype:%d <detail-field>", MobName, BonusType, ...)
Log(temp, AccountName, 0)
  -> timestamp + account + message -> fLogFile
  -> SetWindowText(hWndMain, LogTemp)
```

---

### Business Rule: Special budget must be positive

**Overview**:
The special branch refuses to act when `SpecialBonus` is zero, and answers with `SendEtc` only.

**Detailed description**:
`:87` guards on `pMob[conn].MOB.SpecialBonus > 0`. Unlike the score branch, exhaustion is answered with `SendEtc` (`:126`) - i.e. the client is told its current budgets rather than being shown a message. This is the same "refresh instead of error" idiom, applied to a different message.

`SpecialBonus` is an `unsigned short` (`Code/Basedef.h:466`) and is the only one of the three budgets that is *not* recomputed on login or level recalculation. It is incremented by exactly 2 per level for `MORTAL` and `ARCH` (`Code/TMSrv/CMob.cpp:1093, 1107`), by 0 per level for the celestial family (the celestial branch at `CMob.cpp:1112-1116` only recomputes score), set to 855 on celestial promotion (`Code/TMSrv/_MSG_UseItem.cpp:2953, 3029`), refunded by the circle-reset mechanic (`Code/TMSrv/_MSG_Quest.cpp:643-659`), transferable per celestial slot (`_MSG_Quest.cpp:1280-1297`, `_MSG_UseItem.cpp:3107, 3122`) and settable by the GM command `imple.cpp:241`. Because it is only ever incremented by legitimate flows and decremented here, it is the most trustworthy of the three counters - the only way to inflate it is through the celestial save/restore path or GM action.

The guard also means the special branch is the only one that cannot be driven into an arithmetic anomaly: the debit is exactly 1 (`:105`) after confirming the balance is at least 1, so no wraparound is reachable.

**Rule workflow**:
```
SpecialBonus > 0 ?
  no  -> SendEtc(conn); (no message, no log)
  yes -> validate Detail, then apply the two ceilings
```

---

### Business Rule: Special allocation target must be a special index

**Overview**:
`Detail` must be 0..3 and indexes the four-element `Special[]` array. Out-of-range values are logged as a boundary breach and are not answered.

**Detailed description**:
`:89` repeats the 0..3 domain test, this time guarding an actual array subscript at `:101, :103, :106`. `Special` is `short Special[4]` inside `STRUCT_SCORE` (`Code/Basedef.h:435`), and it exists in both `BaseScore` and `CurrentScore`; the handler reads and writes `BaseScore.Special[]` and lets the projection copy it into `CurrentScore`.

The failure path at `:122-123` logs `err,applybonus Special, out of boundary` with account and IP. Note the inconsistent casing and wording compared with the score branch's message (`:75`) and the skill branch's two messages (`:240, :246`): four different log strings cover the same class of violation, which complicates log-based detection of a fuzzing campaign.

The four categories have very different downstream meanings, which the wire comment does not document. `Special[1]` feeds skill damage tiers and the burn-mana chance (`_MSG_Attack.cpp:850, 923, 1454, 1468`); `Special[2]` feeds critical/master rates, summon counts, the alchemy and extraction success bonuses and - notably - the shop buy/sell price scaling (`_MSG_Buy.cpp:137-143`, `_MSG_Sell.cpp:156`, `_MSG_CombineItemAlquimia.cpp:82`, `_MSG_CombineItemExtracao.cpp:65`); `Special[3]` feeds skill damage and the reflect-damage formula (`_MSG_Attack.cpp:461, 979`, `CMob.cpp:744`); `Special[0]` has no reader in TMSrv outside this handler and the circle reset (see A-01). Allocating special points is therefore allocating *economy* as well as combat power, which is why the ceilings in the next two rules are the most important invariant in the component.

**Rule workflow**:
```
Detail in [0,3] ?
  no  -> Log("err,applybonus Special, out of boundary", account, ip); return
  yes -> compute ceilings for Special[Detail]
```

---

### Business Rule: Level-derived special ceiling (half budget)

**Overview**:
A special point may only be bought while `Special[Detail]` is below half of a level-derived allowance: `(3 * (BaseScore.Level + 1)) >> 1`, plus 1200 for the celestial class masters.

**Detailed description**:
`:91` computes `max_special_level = 3 * (pMob[conn].MOB.BaseScore.Level + 1)`, and `:98-99` adds `3 * 400 = 1200` when `extra.ClassMaster` is `CELESTIAL` (3), `CELESTIALCS` (4) or `SCELESTIAL` (5) - the three "celestial" progression states defined in `Code/Basedef.h:178-182`. The gate at `:101` then requires `Special[Detail] < (max_special_level >> 1)`, so the effective allowance is roughly 1.5 points per level (1,500 at level 999), or 751 points for a celestial character at level 100 where the 200/255 absolute cap becomes the binding constraint instead.

Two design points are worth recording. First, the ceiling is computed from `BaseScore.Level` while the skill branch's level gate uses `CurrentScore.Level` (`:193`); the two are equal today because the projection copies the level, but the inconsistency is a latent trap if any effect ever modifies the current level. Second, the `>> 1` is integer division of a positive value (levels are non-negative in practice), so there is no rounding hazard.

The failure message is `_NN_Maximum_Point_Now` (string id 105, `Code/TMSrv/Language.h:125`), which is a generic "maximum point reached" string - it does not tell the client *which* of the two ceilings was hit. Since this gate is checked first, a player at the level ceiling sees the generic message; a player at the absolute cap sees the distinct `_NN_Maximum_Point_200_Now` (id 106). The two messages are therefore distinguishable in the client only by which one appears, which is a usable but fragile signal.

**Rule workflow**:
```
max_special_level = 3 * (BaseScore.Level + 1)
celestial family ? -> max_special_level += 1200
Special[Detail] < (max_special_level >> 1) ?
  no  -> SendClientMessage(_NN_Maximum_Point_Now)
  yes -> evaluate the absolute cap
```

---

### Business Rule: Absolute special ceiling (200, or 255 with the ultimate skill)

**Overview**:
Independently of level, `Special[Detail]` may not reach 200 - except for `Detail` 1, 2 and 3, which may reach 255 when the character has learned the corresponding "ultimate" skill (bit 7, 15 or 23 of `LearnedSkill`).

**Detailed description**:
`:93` initialises `max_special = 200`; `:95-96` raises it to 255 when the class-relevant ultimate bit is set. The expression is a chain of `&&`/`||` over three bit tests and three `Detail` comparisons. Operator precedence makes it parse as `(((1<<7) & LearnedSkill) && Detail==1) || (((1<<15) & LearnedSkill) && Detail==2) || (((1<<23) & LearnedSkill) && Detail==3)`, which is the intended meaning: bit 7 gates category 1, bit 15 gates category 2, bit 23 gates category 3. `LearnedSkill` is a `long` (`Code/Basedef.h:461`) holding one 24-bit class skill block at a time (`MAX_SKILL = 24`, `Code/Basedef.h:111`), so bits 7, 15 and 23 are the last skill of each eight-skill tier - the "ultimate" of each branch, and exactly the skills the skill branch restricts to one per character.

The asymmetry is deliberate-looking but undocumented: category 0 (`Detail == 0`) has no ultimate skill and therefore remains at 200 forever. Given that no gameplay code reads `Special[0]` (A-01), the practical effect is that a player can burn budget into a stat that grants nothing, up to a lower ceiling. That is a design smell rather than a bug - but it is the kind of asymmetry that a test suite would have pinned down.

The second consequence is that the 255 allowance follows `LearnedSkill`, which is *class-scoped and mutable*: the celestial promotion sets `LearnedSkill = 1 << 30` (`_MSG_UseItem.cpp:3031`), which clears all 24 low bits, so a promoted character loses any 255 allowance and is capped at 200 for all categories until it re-learns an ultimate. Conversely, the celestial slot switch restores the mask (`_MSG_UseItem.cpp:3119`), restoring whatever allowance that slot had. The rule is therefore stateful in a way the local code does not reveal.

**Rule workflow**:
```
max_special = 200
(bit7 && Detail==1) || (bit15 && Detail==2) || (bit23 && Detail==3) ? -> max_special = 255
Special[Detail] < max_special ?
  no  -> SendClientMessage(_NN_Maximum_Point_200_Now)
  yes -> consume one point and grant one point
```

---

### Business Rule: One special budget point buys one special point

**Overview**:
The exchange rate in the special branch is exactly 1:1, with no bulk quantum and no discount.

**Detailed description**:
`:105-106` performs `SpecialBonus--` followed by `BaseScore.Special[m->Detail]++`. There is no quantum variable here, in contrast with the score branch, so a player with 855 special points must issue 855 separate packets to spend them - the celestial promotion grant (`_MSG_UseItem.cpp:2953, 3029`) is thus deliberately click-intensive, which is presumably an intentional pacing decision rather than an oversight.

Both operands are small integers and the guards guarantee `SpecialBonus >= 1` and `Special[Detail] <= 254` at this point, so neither write can overflow. The 1:1 rate combined with the two ceilings means the total special power a character can hold is bounded by the ceilings, not by the budget: excess `SpecialBonus` is simply unspendable once all four categories are maxed, and there is no conversion path from special points to anything else in this component (the circle-reset mechanic in `_MSG_Quest.cpp` is the only refund route, and it refunds *into* `SpecialBonus`).

This is the only rule in the component that is both fully idempotent in its arithmetic and fully bounded in its effect, which makes the special branch the natural reference implementation for the other two.

**Rule workflow**:
```
SpecialBonus--
BaseScore.Special[Detail]++
GetCurrentScore -> SendScore -> SendEtc -> Log
```

---

### Business Rule: Skill identity encoding (`5000 + 24*class + position`)

**Overview**:
A skill purchase encodes the skill as a single 16-bit value above 5000: `Detail = 5000 + skillclass * 24 + skillpos`, with `skillclass` 0..3 and `skillpos` 0..23, so the accepted range is `[5000, 5095]`.

**Detailed description**:
`:132-133` derives `skillclass = (m->Detail - 5000) / 24` and `skillpos = (m->Detail - 5000) % 24` *before* validating the range at `:136`. For `Detail < 5000` the division truncates toward zero, producing a negative or zero class and a negative remainder - values that are computed but never used, because the range check rejects the request before `MobClass == skillclass` at `:140`. For `Detail` in `[5000, 5095]` the decomposition is exact: 5000 -> class 0 position 0; 5023 -> class 0 position 23; 5024 -> class 1 position 0; 5095 -> class 3 position 23.

The offset 5000 is simultaneously the *item index* used later for the requirement lookups (`g_pItemList[m->Detail].ReqLvl/ReqInt/ReqDex/ReqCon` at `:192, :195-197`). That is the single most surprising aspect of the encoding: the value is a skill coordinate in this handler and an inventory item index in the data tables, i.e. the item list at indices 5000..5095 is expected to hold the skill-book item whose requirement fields describe the skill. `g_pItemList` is a 6,500-entry static array (`MAX_ITEMLIST = 6500`, `Code/Basedef.h:139`) fully populated from `ItemList.bin` (`Code/Basedef.cpp:2828-2859`), so the subscript is in bounds, but the *content* of entries 5000..5095 is a data concern that cannot be verified from the repository (A-04).

The encoding has no explicit class count check on the upper end beyond 5095: `5095 = 5000 + 3*24 + 23` is exactly the last valid skill of class 3, and `MAX_CLASS = 4` (`Code/Basedef.h:115`). The skill table `g_pSpell` has `MAX_SKILLINDEX = 103` entries (`Code/Basedef.h:140`), so `skillpos + 24*skillclass` peaks at 95, always in bounds. The encoding is thus arithmetically closed and safe, but only because the literals happen to line up; there is no symbolic constant tying 5000, 24, 4 and 103 together anywhere in the code.

**Rule workflow**:
```
skillclass = (Detail - 5000) / 24
skillpos   = (Detail - 5000) % 24
Detail in [5000, 5095] ? --no--> Log("err,MSG_ApplyBonus, request skill out of bound"); return
```

---

### Business Rule: Skill request must carry a mob-slot TargetID

**Overview**:
A skill purchase is rejected unless `TargetID` lies in `[MAX_USER, MAX_MOB)` - that is, unless it names an NPC/monster slot - and the value is then never used.

**Detailed description**:
`:134` copies `m->TargetID` into `MobTarget`; `:138-139` requires `MobTarget >= MAX_USER && MobTarget < MAX_MOB`, i.e. `1000 <= TargetID < 25000` (`Code/Basedef.h:56, 107`). The failure path logs `err,MSG_ApplyBonus, request target id is out of bound` with account and IP (`:240`) and returns. After the check, `MobTarget` is never read again in the function - the skill is applied to the *requesting* character, not to the target.

This is a vestigial validation, most plausibly inherited from a handler that applied effects to another mob (a buff, a summon, or a targeted skill). Its effect on the live protocol is purely negative: a legitimate client must invent an NPC/mob index to satisfy it, and the value carries no authority. Because the value is unused, it also cannot be a vector - but it does mean the handler accepts skill purchases whose target is, for example, a monster standing on the other side of the map, and that any future code that starts using `TargetID` would inherit an unvalidated, player-controlled index.

`TargetID` is an `unsigned short` (`Code/Basedef.h:1776`), so the domain is 0..65535 and the accepted window is a strict subset. Note that the check happens *after* the `Detail` range check but *before* the class check, so a request with both a bad skill and a bad target is reported as a skill error, not a target error - a small diagnosability detail.

**Rule workflow**:
```
MobTarget = m->TargetID
TargetID in [1000, 25000) ? --no--> Log("err,MSG_ApplyBonus, request target id is out of bound"); return
(validate class, cost, prerequisites, coin, level, mastery - MobTarget unused)
```

---

### Business Rule: Own-class skill restriction

**Overview**:
Only skills belonging to the acting character's own class may be purchased; the class is implied by the encoded skill coordinate.

**Detailed description**:
`:131` reads `MobClass = pMob[conn].MOB.Class` (an `unsigned char`, `Code/Basedef.h:444`), and `:140` compares it with the `skillclass` derived from the request. Because the request encodes the class explicitly (it is not inferred from the player), this rule is a genuine authorisation check: it prevents a warrior from buying a mage skill, which would otherwise corrupt the `LearnedSkill` mask (the mask is interpreted relative to the character's class by `BASE_GetBonusSkillPoint` at `Code/Basedef.cpp:838-844`, which indexes `g_pSpell[cls * MAX_SKILL + j]`).

The failure message is `_NN_Cant_Learn_Other_Class_Skill` (107, `Code/Language.h:127`), which is precise and correctly worded - in contrast with the mislabelled score-branch log, suggesting this branch (the most complex one) received the most attention.

The rule does *not* restrict `MobClass` itself to the valid range 0..3 at this point. That validation exists in the budget functions (`Code/Basedef.cpp:830-833, 862-863` both return `FALSE` for an out-of-range class), so a character with a corrupt class byte can still reach `g_pSpell[skillpos + 24*skillclass]` here, because `skillclass` comes from the request (validated to 0..3 by the `Detail` range) and is merely *compared* to the corrupt class - the equality test fails for any class outside 0..3, so the branch is unreachable in that case. The comparison operand order (`MobClass == skillclass`) makes this safe by accident: had it been `skillclass == MobClass` the result would be identical. There is no exploitable path, but the safety is again incidental rather than asserted.

**Rule workflow**:
```
MobClass == skillclass ? --no--> SendClientMessage(_NN_Cant_Learn_Other_Class_Skill); return
```

---

### Business Rule: Skill cost must fit the skill budget (celestial override)

**Overview**:
The price of a skill is the `SkillPoint` field of its `g_pSpell` entry and must not exceed the character's `SkillBonus` - except for the three celestial class masters, whose effective budget is hardcoded to 1500 while the charge is still applied to the real field.

**Detailed description**:
`:142` reads `Skillbonus = g_pSpell[skillpos + 24 * skillclass].SkillPoint` from `STRUCT_SPELL` (`Code/Basedef.h:858-880`, first field `int SkillPoint`). `:144` reads `myskillbonus = pMob[conn].MOB.SkillBonus`, and `:146-147` replaces it with the literal `1500` for `CELESTIAL`, `CELESTIALCS` and `SCELESTIAL`. `:149` then requires `Skillbonus <= myskillbonus`, failing with `_NN_Not_Enough_Skill_Point` (108).

The celestial override is the component's most consequential defect (F-01). `BASE_GetBonusSkillPoint` (`Code/Basedef.cpp:810-856`) already encodes the celestial advantage as `spellperlevel += 1500` for non-`ARCH`/non-`MORTAL` class masters, so the 1500 here duplicates that intent as a *floor on the check* rather than a *credit to the budget*. The two mechanisms are not equivalent: the budget function grants a one-off 1500-point allowance when the character is created/promoted, while this override ignores the actual balance at spend time. A celestial character whose `SkillBonus` has been partially spent (or was never granted, e.g. after a slot switch that restored a different `SkillBonus` at `_MSG_UseItem.cpp:3121`) passes the affordability test for any skill costing up to 1500, and `:205` then subtracts the real cost from the real `unsigned short` field. If the field is smaller than the cost, the subtraction wraps: 20 - 100 becomes 65,436.

The corruption is not transient. `SkillBonus` is persisted by `SaveUser` (`Code/TMSrv/Server.cpp:7063`) and next recomputed by `BASE_GetBonusSkillPoint`, which computes `rest = spellperlevel - spelluse` and assigns it to the same `unsigned short` (`Code/Basedef.cpp:846-851`). Once `spelluse` (the sum of all learned skills) exceeds `spellperlevel`, `rest` is negative and wraps to ~65,500, which is then treated as a legitimate budget by this very handler. The component therefore has a path by which one celestial character can learn every skill of its class without spending a single point, permanently.

**Rule workflow**:
```
Skillbonus   = g_pSpell[skillpos + 24*skillclass].SkillPoint
myskillbonus = MOB.SkillBonus
celestial family ? -> myskillbonus = 1500
Skillbonus <= myskillbonus ? --no--> SendClientMessage(_NN_Not_Enough_Skill_Point); return
```

---

### Business Rule: One ultimate skill per character

**Overview**:
Of the three "ultimate" skills (positions 7, 15 and 23), a character may own at most one.

**Detailed description**:
`:153` detects an ultimate request by position (`skillpos == 7 || skillpos == 15 || skillpos == 23`). `:155` then tests whether *any* of bits 7, 15 or 23 is already set in `LearnedSkill` and, if so, answers `_NN_Only_OneSkillLearn` (402) and returns. The rule spans all three skill branches of the class tree, so a character that has learned the tier-1 ultimate cannot take the tier-2 or tier-3 ultimate even if it has the points and the coin.

The check is a cross-tier exclusivity constraint, and it is the only rule in the component that inspects bits belonging to positions other than the requested one. It is also the only rule whose state is not visible from the requested coordinate alone - the client must know which ultimate it already has, which is why `SendEtc` transmits the raw `LearnedSkill` mask to the client (`SendFunc.cpp:1219`).

The exclusivity combines with the celestial promotion wipe (`LearnedSkill = 1 << 30`, `_MSG_UseItem.cpp:3031`) to produce a reset path: promotion clears bits 7/15/23, re-opening the choice, while the special-point allowance of 255 per category (previous rule) is lost until an ultimate is re-learned. The interaction is intentional-looking but is not documented anywhere; it is one of the strongest arguments for treating `LearnedSkill` as the authoritative progression state rather than deriving it.

**Rule workflow**:
```
skillpos in {7,15,23} ?
  yes -> (bit7 | bit15 | bit23) already set in LearnedSkill ?
           yes -> SendClientMessage(_NN_Only_OneSkillLearn); return
           no  -> continue to prerequisite, coin, duplicate, level, mastery gates
```

---

### Business Rule: Ultimate skill prerequisite chain (seven of eight)

**Overview**:
Before an ultimate skill can be bought, all seven preceding skills of its eight-skill block must already be learned.

**Detailed description**:
`:161-167` counts, for `i` in 0..7, the bits at positions `skillpos - i` that are set in `LearnedSkill`, accumulating into `skillin`. `:169` then requires exactly `skillin == 7`. The loop deliberately counts *eight* bits (including the target position) while the target bit is already known to be clear (the exclusivity check above guarantees bits 7/15/23 are clear, and the duplicate check at `:185` would catch any other case) - so the maximum attainable value is 7 and the test is equivalent to "all seven predecessors are learned". The `skillpos - i` indexing is safe for exactly the three ultimate positions: 7-7 = 0, 15-7 = 8, 23-7 = 16, so each block is `[0..7]`, `[8..15]`, `[16..23]` respectively.

The failure message `_NN_Befor_LearnSkill` (403, `Language.h:422`) is not a grammatical English sentence, which suggests it was authored by a non-native speaker and never revised; it nevertheless conveys "learn the earlier skill first". Note the ordering of checks: exclusivity first, then prerequisites, then the coin check, then duplicate, then level, then mastery. That means a character with insufficient coin but also missing prerequisites is told about the prerequisites, not the coin - the client learns requirements one at a time, in a fixed order, which is a deliberate (if undocumented) UX ordering.

The loop is O(8) and reads only `LearnedSkill`, so it is cheap; the rule is nevertheless the most restrictive in the component, since it forces an entire tier to be purchased before the ultimate becomes available, and the tier itself costs the sum of seven `SkillPoint` values.

**Rule workflow**:
```
for i in 0..7: if (1 << (skillpos - i)) & LearnedSkill -> skillin++
skillin == 7 ? --no--> SendClientMessage(_NN_Befor_LearnSkill); return
yes -> continue to coin check
```

---

### Business Rule: Ultimate skill coin price (50,000,000)

**Overview**:
An ultimate skill costs 50,000,000 coins in addition to its skill points; non-ultimate skills are free of coin cost.

**Detailed description**:
`:175` computes `coin` with a ternary whose both branches are the literal `50000000`; `:177-182` checks `pMob[conn].MOB.Coin < coin` and, on failure, formats `_DN_D_Cost` (204) with the price and sends it. `:199` recomputes the same degenerate ternary and `:201-202` deducts only when `skillpos` is 7, 15 or 23. The deduction is inside the success block, after every gate, so a rejected request never charges.

The degenerate ternary (F-11) is the clearest evidence of an unfinished intent: the expression distinguishes `ClassMaster != MORTAL && != ARCH` (i.e. the celestial family) from `MORTAL`/`ARCH`, but yields the same price. The natural reading is that a different price - most likely a higher one, or zero - was intended for celestial characters. Because both occurrences are identical, the code is internally consistent and behaves as a flat 50,000,000 price; the risk is that a future edit "fixes" one occurrence and not the other, desynchronising the affordability check from the charge.

`Coin` is an `int` (`Code/Basedef.h:448`), so 50,000,000 is comfortably representable and the comparison is a signed comparison with no overflow risk. The price is enormous relative to normal income flows but trivially reachable for a progressed character, and there is no alternative currency or item cost in this branch. Note also that the check at `:177` is duplicated in spirit by the level/mastery gates that follow: a character that fails the level gate *after* passing the coin check is not charged, but must re-satisfy the coin check (unchanged, since nothing consumed coin) on retry.

**Rule workflow**:
```
(ultimate only)
coin = 50000000
Coin < coin ? --no--> sprintf(temp, _DN_D_Cost, coin); SendClientMessage(temp); return
... all gates pass ...
(ultimate only) Coin -= coin
```

---

### Business Rule: No duplicate skill learning

**Overview**:
A skill may not be learned if its bit is already set in `LearnedSkill`.

**Detailed description**:
`:185-189` tests `(1 << skillpos) & LearnedSkill` and answers `_NN_Already_Learned_It` (109) on failure. This is the last of the "cheap" checks, coming after the ultimate-specific gates and before the level/mastery gates - which means an ultimate skill that is already learned is reported by *this* message only if it somehow passed the exclusivity check at `:155`. Since exclusivity rejects any set bit among 7/15/23, the duplicate path is unreachable for ultimate skills and reachable only for the twenty-one non-ultimate positions.

The redundancy is harmless but revealing: two independent mechanisms protect the same bit, one general (duplicate check) and one specific (exclusivity). The exclusivity check's wording (`_NN_Only_OneSkillLearn`) is also the message a player sees when attempting a *second* ultimate, which is correct but easy to confuse with "already learned".

`1 << skillpos` with `skillpos` in 0..23 is well-defined on a 32-bit `long`; `MAX_SKILL = 24` keeps the shift away from the sign bit, and the celestial marker at bit 30 (`_MSG_UseItem.cpp:3031`) never collides with a skill position.

**Rule workflow**:
```
(1 << skillpos) & LearnedSkill ? --yes--> SendClientMessage(_NN_Already_Learned_It); return
no -> level gate
```

---

### Business Rule: Skill level requirement from the item table

**Overview**:
For `MORTAL` and `ARCH` characters, the skill's level requirement is `g_pItemList[Detail].ReqLvl`; for the celestial family the requirement is zero.

**Detailed description**:
`:192` computes `reqlv = (ClassMaster == MORTAL || ClassMaster == ARCH) ? g_pItemList[m->Detail].ReqLvl : 0` and `:193` requires `pMob[conn].MOB.CurrentScore.Level >= reqlv`, failing with `_NN_Need_More_Level_To_Learn` (110). The lookup re-uses the skill coordinate as an item index (see the encoding rule), so the level curve of skill acquisition is not stored with the skills but with the corresponding item entry in `ItemList.bin` - a deployment-level coupling that this source tree cannot verify (A-04).

`ReqLvl` is a `short` (`Code/Basedef.h:888`). A zero requirement (missing or defaulted data) silently disables the gate - the comparison `Level >= 0` is always true - so a mis-provisioned item list silently removes a progression constraint without any log or error. This "fail-open on missing data" property is the counterpart of the fail-open ceiling discussed under the score branch, and it is shared by the mastery gate that follows.

The exemption for the celestial family is consistent with the design of that progression: celestial characters restart at a low level with a large budget (`BASE_GetBonusScorePoint` grants 1001 + cristals*100 + resets*200 for the celestial branch, `Code/Basedef.cpp:948-951`), so a level gate keyed on mortal levels would be meaningless. Note that the branch uses `CurrentScore.Level` here and `BaseScore.Level` in the special ceiling - the inconsistency noted earlier.

**Rule workflow**:
```
reqlv = (ClassMaster in {MORTAL,ARCH}) ? g_pItemList[Detail].ReqLvl : 0
CurrentScore.Level >= reqlv ? --no--> SendClientMessage(_NN_Need_More_Level_To_Learn); return
```

---

### Business Rule: Skill mastery requirements (Special[1..3])

**Overview**:
The skill additionally requires `CurrentScore.Special[1] >= ReqInt`, `Special[2] >= ReqDex` and `Special[3] >= ReqCon`, all read from the same item-table entry.

**Detailed description**:
`:195-197` chains three `&&` comparisons against `g_pItemList[m->Detail].ReqInt`, `.ReqDex`, `.ReqCon` (all `short`, `Code/Basedef.h:890-892`). failure yields `_NN_Need_More_Mastery_To_Learn` (111). This is where the special branch and the skill branch are coupled: the special points a character allocates through `BonusType == 1` are the currency that unlocks advanced skills, and the ceilings on those points (200/255, half-level) therefore bound skill progression.

The mapping of the three fields is counter-intuitive but internally consistent: `ReqInt` is checked against `Special[1]`, `ReqDex` against `Special[2]`, `ReqCon` against `Special[3]`. So the item field named after a primary attribute is actually satisfied by a *special* point category, not by `BaseScore.Int/Dex/Con`. `Special[0]` is not referenced at all - consistent with the observation that `Special[0]` has no downstream consumer (A-01). The reading uses `CurrentScore` rather than `BaseScore`, so equipment or affect bonuses to `Special[]` (if any exist) are honoured; `BASE_GetCurrentScore` copies `BaseScore.Special[]` and does not appear to add equipment bonuses to it, so the two are equal in practice.

Because all three requirements are checked in one `&&` chain, the client cannot tell which of the three failed - a UX limitation that pushes players to third-party calculators. The gate is also the last before the commit, so an error leaves the character one step away from success with all other resources intact.

**Rule workflow**:
```
Special[1] >= ReqInt && Special[2] >= ReqDex && Special[3] >= ReqCon ?
  no  -> SendClientMessage(_NN_Need_More_Mastery_To_Learn); return
  yes -> commit
```

---

### Business Rule: Skill purchase commit (coin, mask, budget)

**Overview**:
The successful skill purchase applies three mutations in a fixed order - coin deduction (ultimate only), `LearnedSkill` bit set, `SkillBonus` debit - and then re-derives and notifies.

**Detailed description**:
`:199-205` is the commit block. The coin deduction at `:201-202` repeats the degeneracy of `:175` and applies the same 50,000,000 only for positions 7/15/23; `:204` ORs the bit into `LearnedSkill`; `:205` subtracts the skill price (captured at `:142`, before any mutation) from `SkillBonus`. Ordering matters for exactly one reason: `Skillbonus` is a local `int` captured before the commit, so even if `SendClientMessage` or a log call in between had altered the budget (it cannot), the charge would be the value that was validated.

The fields mutated here are all persisted verbatim by `SaveUser` (`STRUCT_MOB` memcpy at `Code/TMSrv/Server.cpp:7063`) and all re-read by `BASE_GetBonusSkillPoint` on the next reconciliation, which is where the F-01 wraparound becomes permanent. There is no server-side record of the transaction other than the `etc,applybonus` log line at `:210-211`, which records neither the price, the coin balance nor the remaining budget - so reconstructing an anomalous purchase from logs alone is not possible.

The commit has no rollback and no post-condition assertion. In a single-threaded handler with no failing callees, that is acceptable; the only observable failure after `:204` would be a full send buffer inside `SendEtc`, which force-logs-out the character (`SendFunc.cpp:1230`) *after* the mutation and *before* the audit log, but the log is still written because it does not depend on the socket.

**Rule workflow**:
```
if ultimate -> Coin -= 50000000
LearnedSkill |= (1 << skillpos)
SkillBonus -= Skillbonus
GetCurrentScore(conn) -> SendScore(conn) -> SendEtc(conn)
Log("etc,applybonus name:%s bonustype:%d skillpos:%d")
```

---

### Business Rule: No database write from the handler

**Overview**:
The component never persists. All durability is delegated to the periodic and logout savers.

**Detailed description**:
The handler contains no call to `SaveUser`, no `DBServerSocket` traffic and no `MSG_DBSaveMob` construction. The only persistence path is external: `ProcessSecMinTimer` calls `SaveUser(SaveCount, 0)` once per minute (`Code/TMSrv/ProcessSecMinTimer.cpp:189`), `CharLogOut` calls `SaveUser(conn, 1)` (`Code/TMSrv/Server.cpp:7117`), and `SaveUser` copies the whole `STRUCT_MOB` into `MSG_DBSaveMob` (`Server.cpp:7063`) for DBSrv to write.

The consequence for this component is a durability window of up to one minute: an allocation made and then lost to a server crash is not persisted, so on restart the character has neither the spent budget nor the gained attribute - a consistent state. The risk is on the other side of the window: if the attribute is granted and the save occurs, but the client's view of the budget was based on the pre-save state... in practice the client resynchronises on login from the same record, so no inconsistency arises. The genuinely problematic case is the F-01/F-03 wraparound, which *is* persisted and therefore survives restarts.

This rule also explains why the handler is cheap: no synchronous DB round trip, no locking, no acknowledgement to wait for. Applying 100 points is a purely in-memory operation plus two socket appends.

**Rule workflow**:
```
mutate pMob[conn].MOB (memory)
(no persistence)
  -> minute sweep: SaveUser(SaveCount, 0)      ProcessSecMinTimer.cpp:189
  -> logout:      SaveUser(conn, 1)            Server.cpp:7117
```

---

### Business Rule: Budget reconciliation lives outside the component

**Overview**:
The three budgets are produced and repaired by other components; this handler only consumes them and never revalidates them against the character's level, class or spent statistics.

**Detailed description**:
`BASE_GetBonusScorePoint` (`Code/Basedef.cpp:858-1060`) recomputes `ScoreBonus` from the class base stats, the character's four attributes and a class-master-specific level formula: `leveluse = lvl*5` plus level-band adjustments for `MORTAL`, `lvl*6 + mortalLevel*6 + (lvl-354)*6` for `ARCH`, and `lvl*6 + cristal*100 + reset*200 + 1001 + archLevelBonus + level-band adjustments` for the celestial three. It then attempts a single-stat claw-back if `totaluse > leveluse` and finally writes `ScoreBonus = leveluse - totaluse`. `BASE_GetBonusSkillPoint` (`:810-856`) recomputes `SkillBonus = spellperlevel - sum(SkillPoint of learned)` with `spellperlevel = level*3 (+112 ARCH) (+1500 non-ARCH/non-MORTAL) (+(level-199) if >199) (+9 quest)`.

Both functions are invoked at login (`Code/TMSrv/ProcessDBMessage.cpp:779-780`), on level up (`Code/TMSrv/CMob.cpp:1095, 1109, 1114`), on celestial slot switch (`Code/TMSrv/_MSG_UseItem.cpp:3153-3154`), on the score-reset NPC (`Code/TMSrv/_MSG_Quest.cpp:1881`), on a quest reward (`_MSG_Quest.cpp:673`), on an Ehre combination (`_MSG_CombineItemEhre.cpp:167`) and on certain item uses (`_MSG_UseItem.cpp:3479`). The ApplyBonus handler is conspicuously absent from that list: it never calls either function before or after mutating the budgets.

That division of responsibility is the architectural core of the component and its main risk. When the reconciliation functions are correct, they clamp the budget to the legitimate value and every anomalous allocation is corrected on the next login. When they are not - specifically when `rest` is negative and wraps (F-03, F-01) - the handler converts the anomaly into permanent character power and the reconciliation *reproduces* it instead of repairing it. A defensive handler would recompute the budget before spending; this one trusts it, which is a deliberate performance trade-off with a known failure mode.

**Rule workflow**:
```
outside: BASE_GetBonusScorePoint / BASE_GetBonusSkillPoint -> write ScoreBonus / SkillBonus
this handler: read budget -> validate locally -> debit -> grant
outside: next reconciliation -> rest = allowance - consumed -> write back (may wrap)
```

---

### Business Rule: Absent cross-cutting guards (trade, rate, replay, size)

**Overview**:
The handler implements none of the cross-cutting protections that sibling handlers apply: no trade-state check, no rate limiting, no replay/tick validation, and no packet-size validation.

**Detailed description**:
Three of these are visible by comparison. Trade: `_MSG_ReqBuy.cpp:32-38` aborts when `pUser[conn].TradeMode` or `Trade.OpponentID` is set, and `_MSG_UseItem.cpp:51-58` does the same (and additionally tears down a dangling trade); `Exec_MSG_ApplyBonus` has no such check, so points can be spent with a trade window open - which matters because the trade handler snapshots and compares inventory, not stats, so the peer sees no change but the allocating player's combat power changes mid-negotiation. Rate: the read pump drains the whole buffer in an unbounded loop (`Server.cpp:3973-4002`) with no per-event cap, and the handler has no cooldown, so the only limit on request rate is the budget. Replay: `MSG_STANDARD::ClientTick` is only compared against the constant `SKIPCHECKTICK` at the dispatcher (`ProcessClientMessage.cpp:63`); nothing verifies monotonicity, so a captured packet can be replayed as long as the budget allows.

Size validation is structurally absent rather than locally forgotten. `BASE_CheckPacket` was written to compare `m->Size` against `sizeof()` for every message type - including `_MSG_ApplyBonus` at `Code/Basedef.cpp:6523` - but the entire body is inside a `/* ... */` block that ends at `:6594`, and the function unconditionally `return FALSE`. `CPSock::ReadMessage` only enforces `sizeof(HEADER) <= Size <= MAX_MESSAGE_SIZE` (`CPSock.cpp:397-411`), and `sizeof(HEADER)` is 12 while this packet is 18. The handler therefore reads 6 bytes past the declared length for a truncated packet; because the receive buffer is a persistent 128 KB array (`CPSock.h:35`), those bytes are previously-received attacker data rather than uninitialised memory, so the practical effect is that a client can choose `BonusType`/`Detail`/`TargetID` from a previous frame - it can already choose them directly, so the security impact is low, but the type/size contract is not enforced anywhere.

**Rule workflow**:
```
dispatcher: ID range, ServerDown, LastReceiveTime, Ping skip, ClientTick != SKIPCHECKTICK
CPSock:     INITCODE, 12 <= Size <= 8192, frame completeness, CheckSum
handler:    Hp/Mode only
(no trade guard, no rate limit, no replay check, no sizeof(MSG_ApplyBonus) check)
```

---

## 4. Component Structure

```
Code/TMSrv/
├── _MSG_ApplyBonus.cpp                  # the entire component (219 lines, 7,629 bytes)
│   └── Exec_MSG_ApplyBonus(int conn, char *pMsg)     :21-250
│       ├── preamble / MSG cast                       :23
│       ├── liveness + mode gate                      :25-30
│       ├── branch BonusType == 0  (score)            :32-84
│       ├── branch BonusType == 1  (special)          :85-127
│       └── branch BonusType == 2  (skill)            :129-249
├── ProcessClientMessage.h              # prototype: void Exec_MSG_ApplyBonus(int, char*)   :106
├── ProcessClientMessage.cpp            # routing case _MSG_ApplyBonus                     :214-216
├── TMSrv.vcxproj                       # <ClCompile Include="_MSG_ApplyBonus.cpp" />      :112
└── TMSrv.vcxproj.filters               # filter "MSG\USER_PLAY"                          :123-124

Code/
├── Basedef.h
│   ├── _MSG macro (12-byte header)     :925-930
│   ├── _MSG_ApplyBonus = 119|0x0200    :1769
│   ├── struct MSG_ApplyBonus           :1770-1777
│   ├── STRUCT_SCORE (Str..Special[4])  :414-436
│   ├── STRUCT_MOB (budgets, mask)      :438-481
│   ├── STRUCT_MOBEXTRA (ClassMaster)   :483-543
│   ├── STRUCT_SPELL (SkillPoint)       :858-880
│   ├── STRUCT_ITEMLIST (Req* fields)   :882-906
│   └── constants (MAX_*, MORTAL..)     :56, 107, 111, 115, 139-140, 178-182, 203-204
└── Basedef.cpp
    ├── BASE_GetBonusSkillPoint         :810-856
    ├── BASE_GetBonusScorePoint         :858-1060
    ├── BASE_GetHpMp                    :1174-1208
    ├── BASE_GetCurrentScore            :2973-...
    └── BASE_CheckPacket (disabled)     :6475-6596

Code/TMSrv/  (supporting, not owned)
├── CMob.cpp        CMob::GetCurrentScore                   :598-780
├── SendFunc.cpp    SendClientMessage / SendScore / SendEtc / SendHpMode / GridMulticast
├── Server.cpp      Log, AddCrackError, SaveUser, CharLogOut, WSA read pump
├── Language.h      _NN_*/_DN_* string ids
├── ProcessDBMessage.cpp  login-time budget reconciliation  :773-780
├── ProcessSecMinTimer.cpp minute sweep SaveUser            :189
├── _MSG_Quest.cpp  score reset NPC, circle reset, special transfer
└── _MSG_UseItem.cpp celestial promotion / slot switch       :2941-3164
```

| Function / file | Lines | Role |
|---|---|---|
| `Exec_MSG_ApplyBonus` | `_MSG_ApplyBonus.cpp:21-250` | Whole component; three-branch handler |
| `MSG_ApplyBonus` | `Basedef.h:1770-1777` | 18-byte wire contract |
| `BASE_GetBonusScorePoint` | `Basedef.cpp:858-1060` | Produces/repairs `ScoreBonus` |
| `BASE_GetBonusSkillPoint` | `Basedef.cpp:810-856` | Produces/repairs `SkillBonus` |
| `CMob::GetCurrentScore` | `CMob.cpp:598-780` | Projects `BaseScore` -> `CurrentScore` |
| `SendScore` | `SendFunc.cpp:1136-1193` | Grid-multicast `CurrentScore` |
| `SendEtc` | `SendFunc.cpp:1195-1231` | Directed budgets/coin/mask |
| `SendHpMode` | `SendFunc.cpp:1622-1650` | HP/mode resync on gate failure |
| `SendClientMessage` | `SendFunc.cpp:27-45` | Message-panel UI text |
| `AddCrackError` | `Server.cpp:684-707` | Crack counter (10 per gate failure) |
| `Log` | `Server.cpp:6701-6727` | Timestamped audit sink |

Line-count distribution inside the handler: preamble 10 lines (4.6%), score branch 53 (24%), special branch 43 (20%), skill branch 121 (55%). The skill branch dominates, and within it the validation chain is 46 lines (`:136-197`) against 13 lines of commit (`:199-211`) - a 3.5:1 validation-to-mutation ratio, which is the highest of the three branches (score 2:9, special 14:9).

Cyclomatic complexity by manual count of decision points: preamble 1, score branch 6 (BonusType, ScoreBonus, Detail bounds, bulk threshold, 4 independent `if`s counted as 4), special branch 9 (BonusType, SpecialBonus, Detail bounds, 3-way ultimate test as 3, celestial test as 3), skill branch 20 (BonusType, Detail range, target range, class, cost, celestial, ultimate position (3), exclusivity (3), prerequisite loop (2), skillin, coin threshold, duplicate, class-master test, level, 3 mastery terms). Estimated total ≈ 36 decision points in 230 lines - dense but tractable, all concentrated in one function.

---

## 5. Dependency Analysis

### 5.1 Internal dependencies (compile-time)

```
Exec_MSG_ApplyBonus
├── ProcessClientMessage.h
│   ├── Basedef.h .................. MSG_ApplyBonus, STRUCT_MOB, STRUCT_SCORE, STRUCT_SPELL,
│   │                                STRUCT_ITEMLIST, MORTAL/ARCH/CELESTIAL/CELESTIALCS/SCELESTIAL,
│   │                                MAX_USER, MAX_MOB, USER_PLAY (via CUser.h)
│   ├── CUser.h .................... USER_PLAY (22)
│   ├── CMob.h .................... pMob[], CMob::GetCurrentScore
│   ├── Server.h .................. pUser[], temp[4096], Log, AddCrackError, SaveUser
│   ├── SendFunc.h ................ SendScore, SendEtc, SendHpMode, SendClientMessage
│   ├── Language.h ................ _NN_Maximum_Point_Now(105), _NN_Maximum_Point_200_Now(106),
│   │                               _NN_Cant_Learn_Other_Class_Skill(107), _NN_Not_Enough_Skill_Point(108),
│   │                               _NN_Already_Learned_It(109), _NN_Need_More_Level_To_Learn(110),
│   │                               _NN_Need_More_Mastery_To_Learn(111), _NN_Only_OneSkillLearn(402),
│   │                               _NN_Befor_LearnSkill(403), _DN_D_Cost(204)
│   └── Basedef externs ........... g_pSpell[], g_pItemList[], g_pMessageStringTable[]
└── (MSVC precompiled headers / project-wide /D flags)
```

### 5.2 Runtime call graph

```
ProcessClientMessage            ProcessClientMessage.cpp:38
└── Exec_MSG_ApplyBonus          _MSG_ApplyBonus.cpp:21
    ├── SendHpMode               SendFunc.cpp:1622      [gate failure only]
    ├── AddCrackError            Server.cpp:684         [gate failure only]
    │   ├── Log
    │   └── CharLogOut           Server.cpp:7085        [only at 2e9 crack points]
    ├── SendClientMessage        SendFunc.cpp:27        [special/skill rejections]
    ├── CMob::GetCurrentScore    CMob.cpp:598           [every success]
    │   ├── BASE_GetMobAbility   (resist/parry/range/damage/str..con)
    │   └── BASE_GetCurrentScore Basedef.cpp:2973
    ├── SendScore                SendFunc.cpp:1136      [every success]
    │   └── GridMulticast        SendFunc.cpp:843 -> CPSock::AddMessage
    ├── SendEtc                  SendFunc.cpp:1195      [every success + some rejections]
    │   └── CPSock::AddMessage -> CloseUser on buffer full
    └── Log                      Server.cpp:6701        [every success]
```

### 5.3 Data dependencies (read)

| Symbol | Type | Origin | Use |
|---|---|---|---|
| `pMob[conn].MOB.CurrentScore.Hp` | `int` | runtime | liveness gate `:25` |
| `pUser[conn].Mode` | `int` | runtime | play gate `:25` |
| `pMob[conn].MOB.ScoreBonus` | `unsigned short` | persisted | budget `:34, :40, :43` |
| `pMob[conn].MOB.SpecialBonus` | `unsigned short` | persisted | budget `:87, :105` |
| `pMob[conn].MOB.SkillBonus` | `unsigned short` | persisted | budget `:144, :205` |
| `pMob[conn].MOB.BaseScore.Level` | `int` | persisted | special ceiling `:91` |
| `pMob[conn].MOB.CurrentScore.Level` | `int` | derived | skill level gate `:193` |
| `pMob[conn].MOB.BaseScore.Special[]` | `short[4]` | persisted | special ceiling/grant `:101-106` |
| `pMob[conn].MOB.CurrentScore.Special[1..3]` | `short[4]` | derived | mastery gate `:195-197` |
| `pMob[conn].MOB.Class` | `unsigned char` | persisted | class gate `:131, :140` |
| `pMob[conn].MOB.LearnedSkill` | `long` | persisted | ultimate/prereq/duplicate `:95, :155, :165, :185, :204` |
| `pMob[conn].MOB.Coin` | `int` | persisted | ultimate price `:177, :201` |
| `pMob[conn].extra.ClassMaster` | `short` | persisted | celestial branches `:98, :146, :175, :192, :199` |
| `g_pSpell[i].SkillPoint` | `int` | `SkillData.bin` | skill price `:142` |
| `g_pItemList[d].ReqLvl/ReqInt/ReqDex/ReqCon` | `short` | `ItemList.bin` | skill gates `:192-197` |

### 5.4 External dependencies

| Dependency | Type | Purpose | Failure behaviour |
|---|---|---|---|
| `SkillData.bin` (`g_pSpell`) | binary data file, XOR 0x5A obfuscated | skill prices | `BASE_ReadSkillBin` shows a message box and returns `FALSE` on missing file (`Basedef.cpp:2792-2826`); the array stays zeroed, so every skill costs 0 |
| `ItemList.bin` (`g_pItemList`) | binary data file, XOR 0x5A obfuscated | skill level/mastery requirements | message box + `FALSE` (`Basedef.cpp:2828-2847`); requirements default to 0, disarming the gates |
| `ExtraItem.bin` | optional binary overlay | extra item entries | silently skipped when absent (`Basedef.cpp:2861-2863`) |
| Message string table (`g_pMessageStringTable`, 2000 x 128) | loaded text table | all client UI text | a missing entry renders a blank/placeholder panel |
| Server log file (`fLogFile`) | file handle | audit trail | `Log` silently does nothing to the file if the handle is null (`Server.cpp:6722`) |
| Win32 message pump / `WSAAsyncSelect` | OS facility | serialisation of the handler | single-threaded; `WM_TIMER` and socket events cannot interleave |
| DBSrv (`SaveUser` -> `_MSG_DBSaveMob`) | internal service | persistence | asynchronous; handler does not observe the result |

---

## 6. Afferent and Efferent Coupling

Coupling is measured at the C++ function/struct level (the natural unit for this codebase; there are no classes with virtual dispatch in the analysed slice).

| Component | Type | Afferent | Efferent | Critical | Notes |
|---|---|---|---|---|---|
| `Exec_MSG_ApplyBonus` | function | **1** | **14** | High | Only caller `ProcessClientMessage`; 14 distinct outgoing symbols/data |
| `MSG_ApplyBonus` | struct | 2 (handler, latent `BASE_CheckPacket:6523`) | 1 (`_MSG` macro member) | Medium | Wire contract; latent validator use is inside commented-out code |
| `CMob::GetCurrentScore` | method | 20 | 3+ | High | Shared projector; this handler is 1 of 20 callers |
| `BASE_GetBonusScorePoint` | function | 5 | 2 | High | Producer of the budget this handler spends |
| `BASE_GetBonusSkillPoint` | function | 3 | 2 | High | Producer of the skill budget |
| `SendScore` | function | 25 | 3 | Medium | Cost amplifier (grid multicast) |
| `SendEtc` | function | 24 | 2 | Low | Directed budget refresh |
| `SendClientMessage` | function | many | 2 | Low | UI text |
| `SendHpMode` | function | 23 | 1 | Low | Gate-failure resync |
| `AddCrackError` | function | 21 | 3 | Low | Counter only |
| `Log` | function | many | 2 | Low | Shared audit sink |

### 6.1 The 14 efferent dependencies of `Exec_MSG_ApplyBonus`

| # | Efferent target | Kind | Direction |
|---|---|---|---|
| 1 | `pMob[]` (global `CMob` array) | data | read/write |
| 2 | `pUser[]` (global `CUser` array) | data | read |
| 3 | `CMob::GetCurrentScore` | function | call |
| 4 | `SendScore` | function | call |
| 5 | `SendEtc` | function | call |
| 6 | `SendHpMode` | function | call |
| 7 | `SendClientMessage` | function | call |
| 8 | `AddCrackError` | function | call |
| 9 | `Log` | function | call |
| 10 | `temp[4096]` | global buffer | write (shared, non-reentrant) |
| 11 | `g_pMessageStringTable` | data table | read |
| 12 | `g_pSpell` | data table | read |
| 13 | `g_pItemList` | data table | read |
| 14 | `MSG_ApplyBonus` + `MORTAL/ARCH/CELESTIAL*` + `MAX_USER/MAX_MOB/MAX_SKILL` constants | type/constant | compile-time |

### 6.2 Interpretation

**Afferent 1 / efferent 14 is an extremely asymmetric profile** - the highest efferent count of any handler in the TMSrv family per the architecture report (`docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md:142`). Instability `I = Ce/(Ca+Ce) = 14/15 = 0.93`: the component is almost purely a consumer of the rest of the server, with a single ingress. That is structurally healthy for a *terminal* (terminals should be unstable and depend on everything they touch), but it means the component has no seam at which its logic could be exercised in isolation - every one of the 14 dependencies is either a global array or a function that mutates globals.

**Cohesion** is functional rather than logical: all three branches serve one purpose (spend a progression budget) but the branches share almost no code. Only the preamble (`:25-30`), the notification pair (`SendScore`/`SendEtc`) and the audit idiom are common; the validation logic is triplicated with three different message vocabularies. The cohesion metric that best fits is LCOM-style "no shared instance variables, no shared helpers": cohesion is high at the *intent* level and low at the *code* level.

**Coupling to `Log` and `temp` is the most dangerous kind of efferent coupling** in this codebase: `temp[4096]` is a single global shared by unrelated handlers (`Server.cpp:277`) and `Log` consumes it as a format string, so the coupling is not merely structural - it is a shared mutable buffer with a format-string escape (F-10). Two allocations in flight (impossible in the current single-threaded model, but not asserted anywhere) would corrupt each other's audit lines.

**Coupling to `g_pItemList` is semantic, not syntactic**: the compiler cannot see that `m->Detail` is an item index. That is why the `Detail`-as-item-index coupling does not appear in any static coupling metric yet is the component's most fragile external contract (A-04).

---

## 7. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|---|---|---|---|---|---|
| Game client | External client | Inbound allocation request `_MSG_ApplyBonus` (Type 631) | TCP, custom framing (12-byte HEADER + payload, INITCODE handshake, XOR/offset byte obfuscation, byte-sum CheckSum) | Binary, packed `MSG_ApplyBonus` (18 bytes) | Size in `[12, 8192]` and CheckSum enforced by `CPSock::ReadMessage`; no per-type size check; handler returns without reply on most errors |
| Game client (broadcast) | External client(s) | Outbound score/budget/progress | same TCP framing | `MSG_UpdateScore`, `MSG_UpdateEtc`, `MSG_SetHpMode`, `MSG_MessagePanel` | `AddMessage` returns `FALSE` on buffer exhaustion; `SendEtc` then calls `CloseUser`; `GridMulticast` ignores failures for other users |
| `SkillData.bin` | Static data file (local FS) | Skill prices (`SkillPoint`) | `fopen`/`fread`, whole-file, XOR 0x5A | Binary array of `STRUCT_SPELL` (103 entries) | Message box + abort on missing file; checksum comparison compiled out (`#ifndef _DEBUG` block commented, `Basedef.cpp:2815-2818`) |
| `ItemList.bin` | Static data file (local FS) | Skill level/mastery requirement fields | `fopen`/`fread`, whole-file, XOR 0x5A | Binary array of `STRUCT_ITEMLIST` (6500 entries) | Message box + abort on missing file; checksum comparison compiled out (`Basedef.cpp:2851-2854`) |
| `ExtraItem.bin` | Optional static data file | Additional item entries appended to the table | `_open`/`_read`, `_O_BINARY` | Binary `STRUCT_ITEMLIST + 2` records | Silently skipped if the handle is invalid (`Basedef.cpp:2861-2863`) |
| DBSrv | Internal service | Persist `STRUCT_MOB` (indirect, asynchronous) | TCP, `_MSG_DBSaveMob` | Binary struct memcpy | No acknowledgement consumed by this handler; failure surfaces elsewhere |
| Log file / operator console | Local FS + Win32 window | Audit trail | `fprintf` + `SetWindowText` | Text, `etc,applybonus ...` | Silent no-op when `fLogFile` is null; format-string hazard (F-10) |
| Message string table | Static data (2000 x 128) | Client-facing failure text | in-memory indexing by `_NN_*` id | `char[128]` per entry | Missing/empty entries render an empty message panel |

---

## 8. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose |
|---|---|---|---|
| Command handler / message dispatcher | `switch(std->Type)` -> `Exec_MSG_ApplyBonus` | `ProcessClientMessage.cpp:214-216` | Route one wire type to one function; stateless, global-state-based |
| Guard-clause validation | Early `return` after each failed precondition | `:25-30, :34, :36, :73, :79, :87, :89, :122, :125, :136, :138, :140, :149, :155, :169, :177, :185, :193, :195, :232, :238, :244` | Flatten nesting; every failure path is a single-exit log/send+bail |
| Type-code multiplexing (discriminator union) | `BonusType` selects one of three semantically unrelated operations over the same struct | `:32, :85, :129` | Avoids three separate message types; collapses three use cases into one 18-byte contract |
| Transaction script | All validation, business rules, mutation, notification and audit in one procedural function | `:21-250` | No domain layer; the message handler *is* the use case |
| Materialised derived state (projector) | `BaseScore` authoritative, `CurrentScore` derived via `GetCurrentScore` | `:65, :107, :206` -> `CMob.cpp:598` | Single write path for derived stats; avoids incremental drift |
| Budget/ledger accounting | Monotonic counter decrementation (`ScoreBonus--`, `SpecialBonus--`, `SkillBonus -= cost`) | `:43, :105, :205` | Point economy without a transaction log |
| Sentinel / threshold promotion | `pontos = 1`, promoted to 100 at balance >= 300 | `:38-41` | Bulk-apply convenience |
| Bitmask capability set | `LearnedSkill` as a 24-bit (class-scoped) skill set; bits 7/15/23 as ultimate markers | `:95, :151, :155, :165, :185, :204` | O(1) membership and exclusivity tests |
| Uniform preamble convention | `if (Hp <= 0 \|\| Mode != USER_PLAY) { SendHpMode; AddCrackError(conn,10,20); return; }` | `:25-30` (identical in ~24 sibling handlers) | Consistent session-state defence across all client messages |
| Fail-open data-driven gating | Missing item/skill data yields zero requirements, silently disabling the gate | `:192-197` | Preserves server liveness when data is incomplete |
| Copy-paste family | Sibling `_MSG_*.cpp` handlers with the same shape; mislabelled log string at `:75` is the artefact | `Code/TMSrv/_MSG_*.cpp` | Development velocity; divergence risk |
| Repository-less persistence (deferred save) | In-memory mutation + periodic `SaveUser` | `ProcessSecMinTimer.cpp:189` | Avoids synchronous DB round trip per allocation |
| Audit-by-sprintf | `sprintf` into global `temp`, then `Log(temp, AccountName, 0)` | `:69, :112, :210` | Uniform log format across handlers |

### 8.1 Architectural decisions observed

1. **One message, three operations.** Rather than three message types, the protocol multiplexes via `BonusType`. The consequence is a contract whose `Detail` field has three unrelated encodings (attribute index, special index, packed skill coordinate) - documented only for the first (`Basedef.h:1774`).
2. **Budgets are derived, not stored as truth.** `ScoreBonus` and `SkillBonus` are recomputed from level and consumed stats by `BASE_GetBonusScorePoint`/`BASE_GetBonusSkillPoint`. This is a "recompute rather than record" decision that keeps the save file small and makes cheating by editing a counter self-correcting - except when the recomputation itself wraps (F-01, F-03).
3. **`SpecialBonus` is *not* derived.** It is purely accumulated (level-up +2, celestial 855, circle refunds) and only decremented here. The asymmetry with the other two budgets is a deliberate (or at least consistent) design choice: special points represent discrete progression events rather than a level formula.
4. **No domain layer.** All rules live in the handler. There is no `ApplyBonusService`, no policy object, no configuration for the thresholds (300, 100, 200, 255, 3, 1200, 1500, 50,000,000, 5000, 24). Every constant is a literal in the function body.
5. **Fail-open on data.** Both requirement gates and the ceilings treat missing/zero data as "no requirement". This keeps the server usable with partial data files at the cost of silently disabling constraints.
6. **Deferred persistence.** No save on allocation. The durability unit is the save sweep, not the transaction.

---

## 9. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|---|---|---|---|
| **Critical** | Skill branch, celestial override (`:146-147, :205`) | Affordability check uses a hardcoded 1500 while the charge hits the real `unsigned short SkillBonus`; wrap to ~65,000; made permanent by `Basedef.cpp:846-851` | Free skills within the session; permanent inflated skill budget after reconciliation; class-wide skill acquisition without progression |
| **High** | Score branch (`:45-63`) | No attribute ceiling; `Str/Int/Dex/Con` are `short`; up to +100 per packet | Signed overflow at 32767 -> negative attributes propagate into damage, HP/MP and item-requirement math |
| **High** | Budget integrity (`Basedef.cpp:905, 938, 1002, 850`) | Negative `leveluse - totaluse` / `spellperlevel - spelluse` assigned to `unsigned short` without clamping | A single anomalous reconciliation yields ~65,000 points, i.e. 655 bulk allocations; the handler treats it as legitimate |
| **High** | Testing | No test project, framework, files or CI anywhere | Every rule above is unprotected against regression; behaviour is only known from reading |
| **Medium** | Bulk quantum (`:40-41`) | 100-point step with no per-attribute cap | Single packet can exceed the granularity all level-derived ceilings assume |
| **Medium** | Special caps (`:91-103`) | `Special[0]` permanently capped at 200; `Special[1..3]` cap depends on a mutable, class-scoped `LearnedSkill` bit | Asymmetric, undocumented; celestial promotion silently removes the 255 allowance; `Special[0]` is spendable but has no gameplay reader |
| **Medium** | Dead target validation (`:134, :138-139`) | `TargetID` must be a mob slot and is then unused | Legitimate clients must submit a meaningless index; no protection gained |
| **Medium** | Upstream validation disabled (`Basedef.cpp:6475-6596`) | `BASE_CheckPacket` body commented out; only `12 <= Size <= 8192` enforced | No per-type `sizeof` contract; 18-byte reads on short frames |
| **Medium** | Trade interlock | No `TradeMode`/`Trade.OpponentID` guard, unlike siblings | Stats change mid-trade; no reconciliation with the trade protocol |
| **Medium** | Rate limiting | Unbounded per-event message drain (`Server.cpp:3973-4002`) | Allocation throughput limited only by budget; audit log and grid multicast amplification |
| **Medium** | Audit integrity (`Server.cpp:6720-6723`) | Assembled buffer used as `printf` format; `MobName` interpolated | Format-string read/leak through a crafted character name; potential crash on `%s`/`%n` |
| **Medium** | Persistence window | No save in handler; minute sweep | Crash loses both the spend and the gain (consistent) but an externally replayed save can duplicate an allocation |
| **Low** | Dead ternary (`:175, :199`) | Both branches yield 50,000,000 | Unimplemented class-master price differentiation; divergence risk on future edits |
| **Low** | Dead stores (`:52, :62`) | `CurrentScore.MaxHp/MaxMp` immediately overwritten | Misleads readers about which field is authoritative |
| **Low** | Failure feedback (`:75, :123, :250`) | Silent or log-only failures; three different log wordings; `:75` refers to "skill" in the score branch | Undiagnosable from the client; log-based detection fragmented |
| **Low** | Message-string coupling (`:116, :119, :157, :171, :180, :187, :215, :221, :228, :234`) | Ten distinct UI ids, some semantically adjacent (105 vs 106) | Client cannot distinguish which ceiling/Special category failed |
| **Low** | Constant literals | 300, 100, 200, 255, 3, 1200, 1500, 50000000, 5000, 24 inline | No tuning path; balance changes require a rebuild; magic-number coupling between handler and `BASE_GetBonusSkillPoint`'s 1500 |
| **Low** | `SendScore` fan-out (`SendFunc.cpp:1190`, `:843-972`) | Full 33x33 grid scan and up to `MAX_USER` message inserts per allocation | CPU and bandwidth amplification; exposes the allocator's stat block to nearby clients |
| **Low** | Inconsistent level source (`:91` vs `:193`) | `BaseScore.Level` vs `CurrentScore.Level` | Equal today; latent divergence if any effect mutates one |
| **Low** | `Skillbonus`/`myskillbonus` naming | Two similarly named locals with opposite meanings (price vs budget) | Misreading hazard in the most complex branch |
| **Informational** | Crack counter (`:28`, `Server.cpp:694`) | 10 points against a 2e9 threshold | 200,000,000 gate failures to disconnect; effectively unbounded |
| **Informational** | Documentation | `MSG_ApplyBonus` comments decode only `BonusType` and the score `Detail`; `LearnedSkill` comment claims four categories (`Basedef.h:461`) while only 24 bits are used and bit 30 is a marker | Contract must be reverse-engineered from the handler |

### 9.1 Note on the two "absence" risks

The absence of tests and the absence of a domain layer are related: because the rules exist only as inline literals, there is no seam at which a test could pin them, and because there are no tests, the literals can drift (the 1500 in `:146` versus the 1500 in `Basedef.cpp:817` is exactly such a drift: one is a budget, the other a floor on the check). This is recorded as an observation about the component's structure, not as a recommendation.

---

## 10. Test Coverage Analysis

### 10.1 Verdict

**There is no automated test coverage for this component, and no test infrastructure of any kind in the repository.**

### 10.2 Evidence

| Check | Command / source | Result |
|---|---|---|
| Test-named files anywhere | recursive enumeration for `*test*`, `*spec*`, `*_test*`, `*Tests*`, excluding `.git/.codegraph/.vs/Debug/Release` | One match only: `.agents/skills/.openspec-target` (an agent skill marker, not a test) |
| Test framework includes | grep for `gtest`, `doctest`, `catch2`, `TEST_CASE`, `BOOST_AUTO_TEST` across `Code/**/*.cpp` | No matches |
| Assertion usage | grep for `assert(` across `Code/**/*.cpp` | No matches |
| Test project in solution | `W2PP Code Project.sln` | Contains only `Code/DBSrv/DBSrv.vcxproj` and `Code/TMSrv/TMSrv.vcxproj`; both are `/SUBSYSTEM:WINDOWS` executables with `WinMain` |
| CI configuration | recursive enumeration for `*.yml` / `*.yaml` outside `.git` | Only `openspec/config.yaml` (spec-workflow config, not CI) |
| Dependency-injection / seam for testing | static reading of the handler | None: all 14 efferent dependencies are globals or global-mutating functions |
| Caller-side test hooks | `Code/TMSrv/Server.cpp:4001` | Direct call from the Win32 message pump; no test harness path |
| Codegraph corroboration | `codegraph_explore` blast-radius output for `Exec_MSG_ApplyBonus` | "1 caller in `ProcessClientMessage.cpp`; **no tests found within 3 caller hops**" |

### 10.3 Coverage table

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|---|---|---|---|---|
| `Exec_MSG_ApplyBonus` (all branches) | 0 | 0 | 0% | None - no test exists |
| Score allocation (quantum, mapping, pools) | 0 | 0 | 0% | None |
| Special allocation (two ceilings, 200/255) | 0 | 0 | 0% | None |
| Skill allocation (encoding, prerequisites, price) | 0 | 0 | 0% | None |
| Budget reconciliation interaction (`BASE_GetBonusScorePoint`/`SkillPoint`) | 0 | 0 | 0% | None |
| Persistence round trip (`SaveUser` -> `_MSG_DBSaveMob`) | 0 | 0 | 0% | None |
| Protocol framing (`CPSock::ReadMessage` for Type 631) | 0 | 0 | 0% | None |

### 10.4 Untested behaviour inventory

The following behaviours are reachable, economically significant, and have no executable verification of any kind. Each row is an analysis of what is unprotected, with the location that encodes the rule.

| # | Behaviour | Rule location | Why it matters |
|---|---|---|---|
| U-01 | Bulk quantum selection (1 vs 100) at the 300/299 boundary | `:40-41` | Off-by-one here changes the packet count and the audit record for every allocation |
| U-02 | Attribute mapping and the +2 MaxHp/MaxMp slope for Int/Con | `:45-63` | Must stay consistent with `BASE_GetHpMp` (`Basedef.cpp:1183, 1195`) or pools shrink on next login |
| U-03 | Score budget exhaustion path sends exactly `SendScore` (no message) | `:79-83` | Defines the client's idle-spam behaviour |
| U-04 | Half-level ceiling arithmetic `3*(Level+1)>>1` | `:91, :101` | The main pacing constraint of the special economy |
| U-05 | Celestial +1200 ceiling bonus | `:98-99` | Class-specific allowance; wrong value silently changes celestial power curve |
| U-06 | 200 vs 255 transition keyed on bits 7/15/23 | `:93-96` | The only difference between mortal and "ultimate" special power |
| U-07 | `Special[0]` staying at 200 | `:93` | Spendable but inert; a bug or a design decision, untested either way |
| U-08 | Skill coordinate arithmetic (`5000 + 24*class + pos`) at all four class boundaries and position 23 | `:132-133, :136` | Off-by-one admits the next class's skill or rejects the last one |
| U-09 | Ultimate exclusivity across the three tiers | `:155-159` | Character-defining choice; not verifiable by hand at scale |
| U-10 | Prerequisite loop counting semantics (`skillin == 7` over 8 bits) | `:163-169` | If the loop bounds drift, ultimates become unlearnable or free |
| U-11 | Coin charge applied only for positions 7/15/23, and only on success | `:177, :201-202` | Double-charge or free-ultimate paths |
| U-12 | Level gate exemption for the celestial family | `:192` | Wrong branch key disables progression gating entirely |
| U-13 | Mastery mapping `ReqInt->Special[1]`, `ReqDex->Special[2]`, `ReqCon->Special[3]` | `:195-197` | Cross-wired requirements would be invisible without a test matrix |
| U-14 | Celestial 1500 override and the `SkillBonus` underflow | `:146-147, :205` | The F-01 defect; a single test would have caught it |
| U-15 | Budget wraparound on negative reconciliation | `Basedef.cpp:846-851, 905` | The F-03 defect; needs a property test over level/stat combinations |
| U-16 | `sendScore` grid fan-out and `SendEtc` buffer-full logout ordering | `:66-67, :109-110, :207-208` | Notification is interleaved with mutation; failure semantics undefined |
| U-17 | Audit line content for all three branches | `:69, :112, :210` | The only fraud-investigation artefact, and it is unverified |

### 10.5 Manual/observational verification available

The only verification mechanism found in the repository is observational: the `etc,applybonus` lines in the server log (`:69, :112, :210`) mirrored to the server window title by `Log` (`Server.cpp:6725`), plus the GM command surface in `Code/TMSrv/imple.cpp:118-260` (`clearskill`, `level`, `hp`, `mp`) which can be used to drive the handler into specific states by hand. No scripted or reproducible verification exists.

---

## 11. Appendix - Analysis Ledger

### 11.1 Files examined

| File | Lines read | Purpose |
|---|---|---|
| `Code/TMSrv/_MSG_ApplyBonus.cpp` | 1-250 (full) | The component |
| `Code/Basedef.h` | 56, 75-76, 95-98, 107, 111, 115, 122, 129, 132, 139-140, 150, 172, 178-182, 203-204, 398-436, 438-481, 483-543, 826-906, 925-959, 1766-1777, 2461-2467 | Types, constants, wire contract |
| `Code/Basedef.cpp` | 810-856, 858-1045, 1174-1208, 2792-2872, 2973-3031, 6470-6596 | Budget producers, HP/MP model, data loaders, projector, disabled validator |
| `Code/TMSrv/ProcessClientMessage.cpp` | 35-250 | Dispatcher and routing |
| `Code/TMSrv/ProcessClientMessage.h` | 106 | Prototype |
| `Code/TMSrv/CMob.cpp` | 590-780, 1080-1124 | `GetCurrentScore` projector, level-up budget grants |
| `Code/TMSrv/SendFunc.cpp` | 27-61, 843-972, 1136-1231, 1622-1650 | Send primitives and grid multicast |
| `Code/TMSrv/Server.cpp` | 675-735, 3960-4014, 6701-6727, 7056-7085 | `AddCrackError`, read pump, `Log`, `SaveUser`/`CharLogOut` |
| `Code/TMSrv/Language.h` | 43, 105-149, 204, 222, 421-422 | UI string ids |
| `Code/TMSrv/CPSock.cpp` | 353-432, 505-584 | Frame validation, `AddMessage` |
| `Code/TMSrv/CPSock.h` | 35-50 | Buffer sizes, `HEADER` |
| `Code/TMSrv/CUser.h` | 27-37 | `USER_*` state constants |
| `Code/TMSrv/ProcessDBMessage.cpp` | 755-814 | Login-time reconciliation |
| `Code/TMSrv/ProcessSecMinTimer.cpp` | 189 | Minute-sweep persistence |
| `Code/TMSrv/_MSG_Quest.cpp` | 638-667, 1868-1889 | Circle reset, score-reset NPC |
| `Code/TMSrv/_MSG_UseItem.cpp` | 2941-2968, 3017-3047, 3095-3164 | Celestial promotion/slot switch (budget save/restore) |
| `Code/TMSrv/imple.cpp` | 100-149, 207, 241 | GM command surface |
| `Code/TMSrv/_MSG_Attack.cpp` | 262, 461, 819-858, 923, 979, 1454-1468 | Downstream consumers of `Special[]` |
| `Code/TMSrv/_MSG_Buy.cpp` | 130-149 | Downstream consumer of `Special[2]` (pricing) |
| `Code/TMSrv/_MSG_ReqBuy.cpp`, `_MSG_UseItem.cpp` | 32-38, 51-58 | Trade-guard comparison |
| `Code/TMSrv/TMSrv.vcxproj`, `.filters` | 112, 123-124 | Build inclusion |
| `docs/reports/architectural-analyzer/architectural-report-2026-11-15_21-58-11.md` | 142 | Provided coupling figures (Ca 1 / Ce 14) |
| `docs/reports/MANIFEST.md` | 53 | Component inventory entry #18 |

### 11.2 Verification commands used

- `codegraph_explore` for: handler + blast radius; `MSG_ApplyBonus`/budget fields; `GetCurrentScore`/send primitives; bonus-point functions; `g_pSpell`/`g_pItemList`; dispatcher routing; packet validation.
- Recursive file enumeration for test artifacts, CI files, build projects, data files (`*.bin`, `*.csv`, `*.txt`).
- Grep for test frameworks, `assert(`, `USER_PLAY`/`USER_*` definitions, `LearnedSkill` assignments, `Special[n]` readers, `SpecialBonus` writers, `BASE_GetBonus*Point` call sites, `SaveUser` call sites, `MAX_*` constants.
- `git log` / `git show` on the component: last functional change is the 2018 import; the newest commit touching the file (`06734d8`, 2018-05-22) is a copyright/EOF cosmetic edit only, so the analysed logic is the original decompiled/imported logic.
- Static line/byte measurements: 219 content lines, 7,629 bytes.

### 11.3 Ambiguity register (documented, not resolved)

| Id | Ambiguity | Evidence | Confidence in interpretation |
|---|---|---|---|
| A-01 | `Special[0]` (Detail 0) has no gameplay consumer; it can be allocated (capped at 200) but no TMSrv reader was found outside this handler and the circle-reset routine. Its intended meaning cannot be determined from the source. | `:93-106` writes it; no read of `Special[0]` in `Code/TMSrv/**` or `Code/Basedef.cpp`; the historical readers in `GetFunc.cpp:1312-1318, 1611-1612` are commented out in favour of `SkillBar` | High (absence verified); purpose unknown |
| A-02 | Intended ultimate-skill price for celestial class masters. The ternary at `:175`/`:199` distinguishes class masters but returns the same value; both branches are identical, so no intent can be inferred from code alone. | `:175, :199` | High (degeneracy verified); intent unknown |
| A-03 | What the client actually sends in `TargetID` for a skill purchase. No client source is present in this repository (`Code/ClientPatch_v7662` is a patch folder, not the client). Since the value is validated as a mob slot and then unused, the client-side behaviour cannot be established here. | `:134, :138-139` | Medium |
| A-04 | Whether `ItemList.bin` entries 5000..5095 actually carry the `ReqLvl/ReqInt/ReqDex/ReqCon` values the skill gates assume. The data file is not in the repository (`../../Common/ItemList.bin`, `Basedef.cpp:2835`), so the requirement gates cannot be verified. | `:192-197`, `Basedef.cpp:2828-2847` | High (file absent); data content unknown |
| A-05 | Whether the "exactly one ultimate" rule and the 255 special cap are intended to survive the celestial promotion wipe. `LearnedSkill = 1 << 30` clears bits 7/15/23 (`_MSG_UseItem.cpp:3031`), silently removing both the exclusivity memory and the 255 allowance. | `:95-96, :155-159` vs `_MSG_UseItem.cpp:3031` | Medium |
| A-06 | The `LearnedSkill` layout: the field comment claims four categories (`00_00_00_00`, `Basedef.h:461`), but only 24 bits are used per class and bit 30 is a celestial marker. Whether bits 24-31 have other meanings cannot be determined from the analysed slice. | `Basedef.h:461`, `_MSG_UseItem.cpp:3031`, `CMob.cpp:700` (`1 << 28` "Concentração"), `_MSG_Attack.cpp:203` | Medium |
| A-07 | Meaning of crack `Type` 20 passed to `AddCrackError` at `:28`. Types 3, 8 and 15 suppress logging; 20 does not, but no enumeration of types was found. | `:28`, `Server.cpp:686` | Low |
| A-08 | Why the score branch's out-of-range log line says "skill out of bound" (`:75`). Copy-paste artefact versus a shared error convention cannot be distinguished. | `:75` vs `:123`, `:240`, `:246` | Medium (artefact more likely) |
| A-09 | Whether the +2 `CurrentScore.MaxHp/MaxMp` writes (`:52, :62`) were ever load-bearing. They are dead under the current `GetCurrentScore` implementation but would matter if a future projector preserved `CurrentScore` maxima instead of overwriting them. | `:52, :62` vs `Basedef.cpp:2977-2982` | High (dead today); historical intent unknown |

### 11.4 Scope and exclusions

- **Analysed:** the handler, its wire contract, all directly called primitives, both budget producers, the projector, the persistence path, and the immediately adjacent producers/consumers needed to establish the meaning of each rule.
- **Excluded by parameter:** `.agents`, `.codegraph`, `.opencode`, `openspec`.
- **Examined only as context, not analysed:** the remaining ~23 `_MSG_*.cpp` handlers (compared only for the shared preamble and the trade guard), the client patch folder, and DBSrv internals beyond `SaveUser`'s message construction.
- **Not established:** client-side behaviour, `ItemList.bin` / `SkillData.bin` contents, and the semantics of `Special[0]` (A-01).
