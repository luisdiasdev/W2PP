# Component Deep Analysis Report

**Component:** CCastleZakum Event Module (`CCastleZakum`)

**Primary location:** `Code/TMSrv/CCastleZakum.cpp` (555 lines, 13,871 bytes) and `Code/TMSrv/CCastleZakum.h` (42 lines, 1,346 bytes)

**Secondary locations (callers, scheduler and callee bodies analysed for this report):**

- `Code/TMSrv/_MSG_UpdateItem.cpp:20-106` (`Exec_MSG_UpdateItem`, sole caller of `OpenCastleGate`)
- `Code/TMSrv/MobKilled.cpp:980` (`MobKilled` call site) and `Code/TMSrv/MobKilled.cpp:1904` (`KeyDrop` call site)
- `Code/TMSrv/_MSG_Action.cpp:238` (`CheckMove` call site)
- `Code/TMSrv/ProcessSecMinTimer.cpp:115-125` (`SecCounter`), `:1172` (`ProcessSecTimer` call), `:2019-2282` (`ProcessMinTimer`, `MinCounter`), `:2141-2209` (gate relock), `:2211-2282` (minute respawn loop)
- `Code/TMSrv/Server.cpp:3629` (boot load), `:3631-3632` (`SetTimer`), `:3718-3729` (`WM_TIMER` dispatch), `:6060-6106` (`SetCastleDoor`), `:7127` (`Reboot`), `:7166-7186` (gate initialisation), `:7472-7505` (`UpdateItem`)
- `Code/TMSrv/imple.cpp:250-262` (GM `reloadfile`), `:569-580` (GM `timer`)
- `Code/Basedef.h:155-156,1835-1847,658-670` (constants, wire contract, `STRUCT_CASTLEQUEST`)
- `Code/Basedef.cpp:1647-1842` (`BASE_GetItemAbility`)
- `Code/TMSrv/CMob.h:37-84` (`Leader`, `PartyList`, `GenerateIndex`), `Code/TMSrv/CItem.h:24-45` (`CItem::State/Delay/Height`)

**Analysis scope:** entire project root `E:\wyd\test-setup\w2pp`

**Ignored folders:** `.agents`, `.codegraph`, `.opencode`, `openspec`

**Report date:** 2026-11-15 22:46:10

---

## 1. Executive Summary

`CCastleZakum` is a 555-line, 8-method, all-static C++ class that implements the *Castle Zakum quest line* of TMSrv: a timed, party-scoped, key-gated boss event that runs inside a fixed rectangle of the world map (`x 2180..2296`, `y 1160..1269`). It owns the event's whole lifecycle — gate authorisation, arena purge, mob-table instantiation, countdown, boss-kill detection, reward settlement, timeout cleanup and the reset state machine — and it does so with seven file-scope mutable globals instead of an encapsulated instance.

**Role in the system**

- It is one of the 14 subsystems force-included by `Code/TMSrv/ProcessClientMessage.h:42`, so its class declaration is compiled into all 60 client-handler translation units and into `Server.cpp`.
- It is the **only** component that consumes `EF_KEYID` values `10..14`; the rest of the gate subsystem uses `EF_KEYID 1..9` (generic doors) and `15` (castle-war doors, `Code/TMSrv/Server.cpp:6092-6095`).
- It is the **only** component that mutates `mNPCGen.pList[].MinuteGenerate` from game logic (the other writers are the config reader `Code/TMSrv/CNPCGene.cpp:159-160`, the exhaustion rule in `Code/TMSrv/Server.cpp:3060-3064`, and the minute respawn loop itself).
- It is the single writer of the castle-quest reward path: item bundles, class-tiered experience and coin, all settled on the party leader and then fanned out to `PartyList`.

**Key findings**

1. **The whole event state is module-global and process-wide.** `CastleQuest[64]`, `CastleQuestPosition[9][4]`, `CastleQuestClear`, `CastleQuestTime`, `CastleQuestParty[13]`, `CastleQuestLevel` and `CastleLeader[16]` are defined at `Code/TMSrv/CCastleZakum.cpp:42-70` with external linkage and no accessor. `CastleQuestTime == -1` is a *sentinel* for "no run in progress"; there is no explicit state enum, so idle and uninitialised are indistinguishable.
2. **Entry-gate authorisation checks only the key id, not the quest tag.** Because `&&` binds tighter than `||`, the predicate `key != gatekey || Quest != CastleQuestLevel && gatekey != 10` (`Code/TMSrv/CCastleZakum.cpp:91,97`) means *for `gatekey == 10` only `EF_KEYID` must match*; the `EF_QUEST` stamp is read from the key and then used as the **quest selector** (`Quest` becomes `CastleQuestLevel` at line 155). Whoever holds any item whose `EF_KEYID` resolves to `10` starts a run at the level encoded in that item, with no level, guild, castle-ownership or item-consumption precondition beyond that.
3. **There is no proximity, line-of-sight or map check.** Any carrier can open any castle gate anywhere in the world, exactly like the legacy fallback path in `Code/TMSrv/_MSG_UpdateItem.cpp:54-105`.
4. **The requested state is ignored.** `OpenCastleGate` always applies `STATE_OPEN` (line 200) and multicasts the client's own packet (line 203), so a castle gate can never be *closed* through the protocol and spectators receive the client-supplied `State`, not the authoritative one.
5. **Wave generation mutates shared NPC-generator configuration and never restores it.** Line 150 sets `MinuteGenerate = 1` for every generator in `[MOB_INITIAL, MOB_END]`; the cleanup paths (lines 141, 335, 370) only set `-1` for generators that still have a live mob inside the arena rectangle. Generators whose mobs were killed during the run keep `MinuteGenerate == 1` and therefore keep respawning castle mobs every minute tick indefinitely after the event ends.
6. **Boss detection scans all 64 quest rows for every mob death in the world** (`Code/TMSrv/CCastleZakum.cpp:214-216`, called unconditionally from `Code/TMSrv/MobKilled.cpp:980`) and does **not** `break` on the first match, so a boss index declared in two rows pays the prize bundle twice.
7. **`CastleQuest.txt` is a runtime-only artefact and is absent from the repository.** Only the loader path is in-tree (`../../Common/Settings/CastleQuest.txt`, line 44). Missing file ⇒ modal `MessageBoxA` before the message pump starts (`Code/TMSrv/Server.cpp:3629` vs `:3650`) and a fully zeroed `CastleQuest[64]` table afterwards, which makes `BOSS[0] == 0` for all rows and therefore makes every kill of a `GenerateIndex == 0` mob announce "castle quest cleared".
8. **The parser is unsafe.** `sscanf` with unbounded `%s` into 128-byte buffers, `atoi` over uninitialised `item_str*` buffers when a prize line has fewer than seven numbers, writes to `CastleQuest[-1]` when data precedes the first `#`, and an off-by-one that writes `CastleQuest[64]` on the 65th `#` block.
9. **Gate re-closing is not owned by this module.** `Code/TMSrv/ProcessSecMinTimer.cpp:2141-2209` relocks any keyed init item (`EF_KEYID < 15`) that has been open for two minute ticks, using a `Delay` latch. `CCastleZakum` never resets gate state, never resets `CastleQuestLevel` on timeout, and never invalidates `CastleQuestParty`.
10. **Zero test coverage.** No test project, framework, harness, fixture or assertion exists anywhere in the repository (evidence in section 12), and this module is a prime candidate for silent regression because its only entry points are a protocol handler, three timer callbacks and a loot hook.

---

## 2. Data Flow Analysis

### 2.1 Flow A — Gate interaction (`_MSG_UpdateItem` → castle gate)

```
 1. Client sends Type=_MSG_UpdateItem (884), Size=20, ID=<conn>, payload {ItemID, State}
    (Code/Basedef.h:1839-1847; _MSG header = 12 bytes + int ItemID + int State)
 2. CPSock::ReadMessage decode -> Server.cpp:3975-4001 (WSA_READ) -> ProcessClientMessage(User, Msg, FALSE)
 3. ProcessClientMessage: std->ID in [0,MAX_USER), ping filter, ClientTick filter
    (Code/TMSrv/ProcessClientMessage.cpp:42-64)
 4. switch(std->Type) case _MSG_UpdateItem -> Exec_MSG_UpdateItem(conn, pMsg)
    (Code/TMSrv/ProcessClientMessage.cpp:180-182)
 5. Session gate: Hp == 0 || pUser[conn].Mode != USER_PLAY -> SendHpMode + AddCrackError(1,16) + return
    (Code/TMSrv/_MSG_UpdateItem.cpp:24-29)
 6. Domain gates: 0 <= m->State <= 5 ; 10000 <= m->ItemID < 15000
    (Code/TMSrv/_MSG_UpdateItem.cpp:30-49)
 7. gateid = m->ItemID - 10000
 8. Delegation: CCastleZakum::OpenCastleGate(conn, gateid, m)   [Code/TMSrv/_MSG_UpdateItem.cpp:51]
      gatekey = BASE_GetItemAbility(&pItem[gateid].ITEM, EF_KEYID=39)
      if (gatekey < 10 || gatekey > 14) -> return FALSE  (module disclaims, legacy path resumes)
      else                             -> module owns the request from here on (always returns TRUE)
 9. Key proof: linear scan of pMob[conn].MOB.Carry[0..MAX_CARRY=64)
      key   = BASE_GetItemAbility(&Carry[i], EF_KEYID)
      Quest = BASE_GetItemAbility(&Carry[i], EF_QUEST=58)
      match predicate: key == gatekey && (Quest == CastleQuestLevel || gatekey == 10)
10. Branch ENTRY_GATE (gatekey == 10)
      a. Quest out of [0, MAX_CASTLE_QUEST=64) -> silent return
      b. CastleQuestTime != -1 -> "quest by <leader> with <n>" message, return
      c. Purge arena: for x 2180..2296, y 1160..1269 -> pMobGrid mobs get
         mNPCGen.pList[gen].MinuteGenerate = -1 ; DeleteMob(tmob, 3)
      d. Spawn waves: for gx = MOB_INITIAL..MOB_END
            if gx not a boss  -> mNPCGen.pList[gx].MinuteGenerate = 1
            GenerateMob(gx,0,0) twice
      e. CastleQuestLevel = Quest ; CastleQuestTime = QuestTime - 1
      f. Leader = pMob[conn].Leader (0 -> conn) ; strncpy(CastleLeader, ...)
      g. SendClientSignalParm(Leader, ESCENE_FIELD, _MSG_StartTime, QuestTime)
      h. same signal to every live member in pMob[Leader].PartyList[0..12)
      i. CastleQuestParty[0..12] = PartyList[0..12], CastleQuestParty[12] = Leader
11. Branch INNER GATE (gatekey 11..14): Quest != CastleQuestLevel -> silent return
12. Key consumption: memset(Carry[i], 0) + SendItem(conn, ITEM_PLACE_CARRY=1, i, ...)
13. Apply: UpdateItem(gateid, STATE_OPEN=1, &height)
      -> BASE_UpdateItem(GridCharge, State, 1, PosX, PosY, pHeightGrid, Rotate, &height)
      -> pItem[gateid].State = 1 ; .Height ; .Delay = 0
14. Broadcast: if (isUpdate) GridMulticast(pItem.PosX, pItem.PosY, (MSG_STANDARD*)m, 0)
15. Audit: Log("etc,opengate gateid:%d X:%d Y:%d", AccountName, IP)
16. Return TRUE -> Exec_MSG_UpdateItem returns; legacy fallback never executes for keyid 10..14
```

**Data transformation summary — gate flow**

| Stage | Input | Output | Owner |
|-------|-------|--------|-------|
| Wire decode | framed bytes | `MSG_UpdateItem {ItemID, State}` | `CPSock::ReadMessage` |
| Normalisation | `ItemID` | `gateid ∈ [0,5000)` | `_MSG_UpdateItem.cpp:42` |
| Ownership test | `gateid` | `EF_KEYID` 10..14 owned, else disclaimed | `CCastleZakum.cpp:77-80` |
| Authorisation | carry[64], gatekey | key slot index `i` | `CCastleZakum.cpp:86-103` |
| Quest selection | key `EF_QUEST` | `CastleQuestLevel`, `CastleQuestTime` | `CCastleZakum.cpp:106-156` |
| World mutation | generator range | live mob set in arena | `GenerateMob` / `DeleteMob` |
| Client signalling | `QuestTime` | `_MSG_StartTime` to party | `SendClientSignalParm` |
| Item mutation | `gateid` | `CItem::State=1`, `Delay=0` | `UpdateItem` |
| Projection | client packet | 33x33 multicast | `GridMulticast` |
| Audit | gateid/X/Y | text log line | `Log` |

### 2.2 Flow B — Boss kill and reward settlement

```
 1. Server resolves lethal damage -> CCastleZakum::MobKilled(target, conn, PosX, PosY)
    [Code/TMSrv/MobKilled.cpp:980 - executed for EVERY mob death in the world]
 2. GenerateID = pMob[target].GenerateIndex                      (line 212)
 3. for k in 0..MAX_CASTLE_QUEST(64):                            (lines 214-216)
        if GenerateID == CastleQuest[k].BOSS[0] || == CastleQuest[k].BOSS[1]  -> reward block
        (no break: every matching row pays out)
 4. Notice: sprintf(_SN_CastleQuest_Killed, killer name) -> SendNoticeArea(2176,1160,2300,1276)
 5. CastleQuestClear = 1                                         (line 220)
 6. partyleader = pMob[conn].Leader ; <= 0 -> conn
 7. Leader item bundle: for j in 0..MAX_CARRY(64)
        if CastleQuest[k].Prize[j].sIndex) PutItem(partyleader, &Prize[j])
 8. Leader exp: ExpPrize[pMob[partyleader].extra.ClassMaster] -> MOB.Exp += v ; message _NS_GETEXP
 9. Leader coin: Coin = MOB.Coin + CoinPrize ; if <= 2,000,000,000 apply + SendEtc ; else
        message _NN_Cant_get_more_than_2G
10. Party propagation (only while CastleQuest[k].PartyPrize != 0):
        for i in 0..MAX_PARTY(12): partymember = pMob[partyleader].PartyList[i]
          if pUser[partymember].Mode != USER_PLAY -> continue
          repeat steps 7, 8, 9 for that member
11. No DB write, no event emission. Persistence relies on the regular user-save path.
```

### 2.3 Flow C — Key drop interception

```
 1. MobKilled drop loop rolls a drop for slot i of pMob[target].MOB.Carry[i]
    (Code/TMSrv/MobKilled.cpp:1882-1905)
 2. SetItemBonus(item, mob level, 0, dropBonus) mutates the item in place
 3. CCastleZakum::KeyDrop(target, conn, PosX, PosY, item)          [MobKilled.cpp:1904]
        ItemKey = BASE_GetItemAbility(item, EF_KEYID=39)
        if 11 <= ItemKey <= 14:
            item->stEffect[0].cEffect = EF_QUEST(58)
            item->stEffect[0].cValue  = CastleQuestLevel          <- cast to unsigned char
            PutItem(conn, item)                                   <- returned slot ignored
            return FALSE                                          <- caller skips its PutItem
        else return TRUE                                          <- caller performs PutItem
 4. Item never touches the ground: it appears directly in the killer's carry slot.
```

### 2.4 Flow D — Timer scheduler

```
 1. Server.cpp:3631-3632  SetTimer(hWndMain, TIMER_SEC,  500 ms, NULL)
                          SetTimer(hWndMain, TIMER_MIN, 12000 ms, NULL)   (overridable by GM)
 2. MainWndProc: WM_TIMER -> TIMER_SEC -> ProcessSecTimer()          [Server.cpp:3725-3728]
                            TIMER_MIN -> ProcessMinTimer()
 3. TMSrv ProcessSecTimer: SecCounter++  (ProcessSecMinTimer.cpp:123) -> ~1 tick / 500 ms
 4. ... :1172  CCastleZakum::ProcessSecTimer()
        if (SecCounter % 2 == 0)                                   -> effective 1 Hz
            if (CastleQuestTime == 0)  CastleQuestTime = -1 ; ClearArea(2180,1160,2296,1269) ;
                                       purge arena mobs + MinuteGenerate = -1
            if (CastleQuestTime >  0)  CastleQuestTime--
 5. TMSrv ProcessMinTimer: ... :2110  CCastleZakum::ProcessMinTimer()   (default 12 s cadence)
        CastleQuestClear == 1 -> 2, area notice _NN_CastleQuest_Initialize
        CastleQuestClear == 2 -> 0, ClearArea + purge arena mobs
 6. ... :2141-2209  generic Close Gates pass relocks open keyed init items (2-tick Delay latch)
 7. ... :2212-2226  respawn loop: for each generator with MinuteGenerate > 0,
        if MinCounter % MinuteGenerate == i % MinuteGenerate -> GenerateMob(i,0,0)
 8. ... :2282  MinCounter++
```

**Timer parameter reality check**

| Symbol | Declared meaning | Measured behaviour | Evidence |
|--------|------------------|--------------------|----------|
| `TIMER_SEC` | 1 second | 500 ms interval | `Code/TMSrv/Server.cpp:3631` |
| `SecCounter` | seconds | +1 per 500 ms → castle gate `%2` yields 1 Hz | `Code/TMSrv/ProcessSecMinTimer.cpp:123`, `CCastleZakum.cpp:316` |
| `TIMER_MIN` | 1 minute | 12,000 ms default, GM-settable `>= 5000` | `Code/TMSrv/Server.cpp:3632`, `imple.cpp:571-579` |
| `MinCounter` | minutes | +1 per `TIMER_MIN` callback → "minute" is 12 s in default config | `Code/TMSrv/ProcessSecMinTimer.cpp:2282` |
| `CastleQuestTime` | quest countdown | `QuestTime - 1` then 1 Hz decrement, `-1` = idle | `Code/TMSrv/CCastleZakum.cpp:156,318-341` |
| `CastleQuestClear` | clear state | `0 → 1 → 2 → 0`, two `TIMER_MIN` callbacks | `Code/TMSrv/CCastleZakum.cpp:347-374` |

### 2.5 Flow E — Movement guard

```
 1. Client sends _MSG_Action with TargetX/TargetY
 2. Exec_MSG_Action validates village/newbie/guild-zone rules (_MSG_Action.cpp:200-236)
 3. if (m->TargetX != pMob[conn].TargetX || m->TargetY != pMob[conn].TargetY)
        -> CCastleZakum::CheckMove(conn, m->TargetX, m->TargetY)          [_MSG_Action.cpp:238]
 4. CheckMove: find conn in CastleQuestParty[0..12]  -> index i (or 13 when absent)
 5. for k in 0..MAX_CASTLE_POS(9):
        if (i == 13 && target inside CastleQuestPosition[k]) -> DoTeleport(conn, current X, current Y)
 6. No other caller: recall, gate teleports, skill blinks and GM moves bypass this guard entirely.
```

### 2.6 Flow F — Configuration load

```
 1. WinMain -> CCastleZakum::ReadCastleQuest()                        [Server.cpp:3629]
 2. fopen("../../Common/Settings/CastleQuest.txt", "rt")
      NULL -> MessageBoxA(hWndMain, "CastleQuest.txt nao foi encontrado.", path, MB_OK) ; return
 3. line loop (fgets, 1024-byte buffer):
      '/'       -> comment, skip
      '#'       -> if (Num >= 64) break ; Num++ ; memset(&CastleQuest[Num], 0, sizeof)
      CR only   -> skip
      other     -> ParseCastleString(Num, line)
 4. ParseCastleString: sscanf("%s %s %s") then sscanf("%s %s %s %s %s %s %s %s"),
      atoi on item_value[0..6], _strupr(str1/str2), keyword dispatch into CastleQuest[Num]
 5. Reload at runtime: GM chat "reloadfile" -> imple.cpp:259 -> ReadCastleQuest() again
      (table is overwritten in place; no validation, no reset of CastleQuestLevel/Time)
```

**Wire / contract constants used by the module**

| Constant | Value | Meaning | Evidence |
|----------|-------|---------|----------|
| `_MSG_UpdateItem` | `116 \| 0x100 \| 0x200` = 884 | gate interaction packet | `Code/Basedef.h:1839` |
| `_MSG_StartTime` | `161 \| 0x100 \| 0x200` = 929 | countdown push to client | `Code/Basedef.h:2138` |
| `ESCENE_FIELD` | 30000 | server-as-sender id | `Code/Basedef.h:170` |
| `EF_KEYID` | 39 | gate/key identity effect | `Code/ItemEffect.h:79` |
| `EF_QUEST` | 58 | quest-level stamp effect | `Code/ItemEffect.h:98` |
| `STATE_OPEN` / `CLOSED` / `LOCKED` | 1 / 2 / 3 | gate state machine | `Code/Basedef.h:1835-1837` |
| `MAX_CASTLE_QUEST` | 64 | quest table size | `Code/Basedef.h:156` |
| `MAX_CASTLE_POS` | 9 | guarded rectangles | `Code/Basedef.h:155` |
| `MAX_CARRY` / `MAX_PARTY` | 64 / 12 | carry slots / party slots | `Code/Basedef.h:76,168` |
| `NAME_LENGTH` | 16 | `CastleLeader` capacity | `Code/Basedef.h:132` |

---

## 3. Business Rules & Logic

## Overview of the business rules

| # | Rule Type | Rule Description | Location |
|---|-----------|------------------|----------|
| BR-01 | Domain boundary | The module owns only gates whose `EF_KEYID` resolves to `10..14`; anything else returns `FALSE` so the legacy gate path resumes | `CCastleZakum.cpp:77-80`; `_MSG_UpdateItem.cpp:51-52` |
| BR-02 | Validation (possession) | A castle gate opens only if one of the 64 carry slots holds an item whose `EF_KEYID` equals the gate's `EF_KEYID` | `CCastleZakum.cpp:86-95` |
| BR-03 | Validation (quest binding) | Inner gates additionally require `EF_QUEST == CastleQuestLevel`; the entry gate (`gatekey == 10`) is exempt because `&&` binds before `\|\|` | `CCastleZakum.cpp:91,97` |
| BR-04 | Validation (range) | The entry key's `EF_QUEST` is interpreted as a quest row index and must satisfy `0 <= Quest < 64` | `CCastleZakum.cpp:108-109` |
| BR-05 | Concurrency | At most one castle run exists at a time: entry is refused while `CastleQuestTime != -1`, and the refusal names the current leader and member count | `CCastleZakum.cpp:111-127` |
| BR-06 | Lifecycle (purge) | Before spawning, every mob standing in the arena rectangle is deleted and its generator disabled | `CCastleZakum.cpp:130-144` |
| BR-07 | Lifecycle (spawn) | The run instantiates every generator in `[MOB_INITIAL, MOB_END]` twice; non-boss generators are additionally forced to respawn every minute tick | `CCastleZakum.cpp:147-154` |
| BR-08 | Lifecycle (timer) | Starting a run arms `CastleQuestTime = QuestTime - 1` and pushes `QuestTime` to the leader and every live party member via `_MSG_StartTime` | `CCastleZakum.cpp:155-173` |
| BR-09 | Lifecycle (membership) | The party is snapshotted into `CastleQuestParty[0..12]` with the leader forced into slot `MAX_PARTY` | `CCastleZakum.cpp:175-184` |
| BR-10 | Validation (inner gate) | Inner gates `11..14` reject a correctly-keyed item whose quest stamp differs from the running quest | `CCastleZakum.cpp:188-192` |
| BR-11 | Resource | A successful open consumes exactly one matching key item from its carry slot and resynchronises that slot | `CCastleZakum.cpp:195-196` |
| BR-12 | Feedback suppression | The `_NN_No_Key` message is suppressed when the gate item itself is `sIndex 773` | `CCastleZakum.cpp:99-100` |
| BR-13 | State application | Castle gates always transition to `STATE_OPEN`; `MSG_UpdateItem::State` is never applied, only echoed | `CCastleZakum.cpp:198-203` |
| BR-14 | Audit | Every castle gate open writes `etc,opengate gateid:X Y` with account and IP | `CCastleZakum.cpp:205-206` |
| BR-15 | Boss detection | A kill is a castle boss kill when `pMob[target].GenerateIndex` matches `BOSS[0]` or `BOSS[1]` of *any* of the 64 rows, and every matching row pays out (no `break`) | `CCastleZakum.cpp:212-216` |
| BR-16 | Announcement | Boss death is announced to every live player inside `x 2176..2300, y 1160..1276` with the killer's name | `CCastleZakum.cpp:218-219` |
| BR-17 | Lifecycle (clear) | Boss death arms `CastleQuestClear = 1`, which starts the two-phase reset | `CCastleZakum.cpp:220` |
| BR-18 | Reward (items) | Every non-zero `Prize[j].sIndex` (`j = 0..63`) is cloned into the party leader's inventory | `CCastleZakum.cpp:229-233` |
| BR-19 | Reward (exp) | Leader experience reward is `ExpPrize[ClassMaster]` when non-zero; class master `0` has no configured tier | `CCastleZakum.cpp:234-240`; `CCastleZakum.cpp:513-526` |
| BR-20 | Reward (coin) | Coin reward is applied only if `Coin + CoinPrize <= 2,000,000,000`; otherwise the cap message is sent and nothing changes | `CCastleZakum.cpp:242-256` |
| BR-21 | Reward (party) | Item, exp and coin rewards propagate to each `PartyList` member in `USER_PLAY` only when `PARTYPRIZE: ON` | `CCastleZakum.cpp:258-294` |
| BR-22 | Loot interception | Dropped keys `11..14` are stamped with the current quest level and inserted directly into the killer's inventory, bypassing the ground | `CCastleZakum.cpp:299-312` |
| BR-23 | Loot passthrough | Any other item (including an `EF_KEYID == 10` key) is handed back to the caller for the normal drop path | `CCastleZakum.cpp:311` |
| BR-24 | Timer (tick) | `CastleQuestTime` counts down at 1 Hz (`SecCounter % 2`) and never below `-1` | `CCastleZakum.cpp:316-342` |
| BR-25 | Timer (timeout) | Reaching `0` sets `CastleQuestTime = -1`, recalls players out of the arena and deletes/disables residual mobs | `CCastleZakum.cpp:318-339` |
| BR-26 | Reset state machine | Clear proceeds `1 → 2 → 0` on consecutive minute callbacks, with a notice on phase 1 and the actual eviction on phase 2 | `CCastleZakum.cpp:345-375` |
| BR-27 | External coupling | Open keyed gates are relocked by the generic minute pass after two `TIMER_MIN` ticks; the module never resets gate state itself | `ProcessSecMinTimer.cpp:2141-2209` |
| BR-28 | Access control | A player absent from the party snapshot is teleported back when the move target falls inside any of the 9 guarded rectangles | `CCastleZakum.cpp:377-394` |
| BR-29 | Configuration | `CastleQuest.txt` is parsed as `#`-delimited blocks of `KEY: value[, value...]` lines with `/` comments | `CCastleZakum.cpp:396-555` |
| BR-30 | Configuration fallback | A missing configuration file blocks boot with a modal message box and leaves all 64 rows zeroed | `CCastleZakum.cpp:398-405` |

## Detailed breakdown of the business rules

---

### Business Rule: BR-01 — Gate ownership domain filter

**Overview**:
`OpenCastleGate` is inserted into the `_MSG_UpdateItem` handler as a first refusal, and it must be able to state precisely which requests it did not handle. The discriminator is the gate item's `EF_KEYID`: values below 10 or above 14 mean "not a castle gate" and the function returns `FALSE`, which makes `Exec_MSG_UpdateItem` continue into its own key/state logic (`Code/TMSrv/_MSG_UpdateItem.cpp:54-105`).

**Detailed description**:
The value is obtained from the gate item, not from the packet: `BASE_GetItemAbility(&pItem[gateid].ITEM, EF_KEYID)` at line 77 dereferences the world item the client asked to change. `BASE_GetItemAbility` resolves static effects from `g_pItemList` and dynamic effects from the item's three `stEffect` slots (`Code/Basedef.cpp:1647-1842`), so the classification is a property of the configured world, not of player input. The range `10..14` therefore describes five distinct world objects: one entry gate and four inner doors.

The consequence of the filter being a `return FALSE` rather than an explicit dispatch is that the module is *permissive about the boundary but total inside it*: after line 80 every path returns `TRUE`, including every rejection. The caller cannot distinguish "opened", "refused, no key", "refused, wrong quest", "refused, run in progress"; the protocol simply produces a message (or silence) and no further server processing.

Because the discriminator is read from `pItem[gateid].ITEM`, it is also the mechanism that lets a GM create a castle gate at runtime (`imple.cpp:264-294`) or an operator place one through the init-item list (`g_pInitItem`, `Code/TMSrv/Server.cpp:7166-7186`). Any item assigned `EF_KEYID 10..14` immediately becomes a castle gate with this module's semantics; there is no register of sanctioned gate ids.

**Rule workflow**:
```
gatekey = BASE_GetItemAbility(&pItem[gateid].ITEM, EF_KEYID)
if (gatekey < 10) OR (gatekey > 14):
    return FALSE                     -> caller runs the legacy key path
else:
    return TRUE on every subsequent outcome (module owns the request)
```

---

### Business Rule: BR-02 — Key possession proof over the carry array

**Overview**:
Possession of the matching key is proved by a linear scan of the player's 64 carry slots, comparing `EF_KEYID`. The first slot whose `EF_KEYID` equals the gate's key id and whose quest tag satisfies BR-03 is the key that will be consumed.

**Detailed description**:
The scan at lines 86-95 reads two effects per slot, `EF_KEYID` and `EF_QUEST`, and stores them into loop-external variables `key` and `Quest`. This means the values observed after the loop are those of the last examined slot, which is why line 97 repeats the whole predicate instead of testing a "found" flag. When the loop breaks on a match, `i` identifies the slot consumed later at lines 195-196; when no slot matches, `i == MAX_CARRY` and the function has already returned at line 102, so the out-of-range slot index is never used.

Only carry slots are examined. Equipped items, the trade window, the shop, storage and the "extra" bags are invisible to the rule, so a player wearing or trading a valid gate key cannot open the gate. Conversely, the rule does not verify that the matched key is in a legal state (bound, sealed, cursed, donated): any item carrying the effect counts.

The scan is `O(MAX_CARRY)` = 64 iterations with two calls into `BASE_GetItemAbility`, each of which performs static-effect and dynamic-effect resolution. That is the dominant cost of a gate interaction, and it is repeated for both the entry gate and the four inner gates with no caching.

**Rule workflow**:
```
for i in 0 .. MAX_CARRY-1:
    key   = BASE_GetItemAbility(&Carry[i], EF_KEYID)
    Quest = BASE_GetItemAbility(&Carry[i], EF_QUEST)
    if (key == gatekey) AND ((Quest == CastleQuestLevel) OR (gatekey == 10)):
        break                                   -> slot i is the consumed key
if no slot matched:
    apply BR-12 feedback, return TRUE
```

---

### Business Rule: BR-03 — Quest-tag conjunction and operator precedence

**Overview**:
Inner doors are bound to the *running* quest: a key stamped with a different quest level must not open them. The entry gate is deliberately exempt, because the quest level is carried *by* the entry key and becomes the run's level. The code expresses both behaviours in one predicate whose precedence makes the intent implicit.

**Detailed description**:
`key != gatekey || Quest != CastleQuestLevel && gatekey != 10` parses as `(key != gatekey) || ((Quest != CastleQuestLevel) && (gatekey != 10))`. For `gatekey == 10` the second disjunct is false, so only the key id matters: any item with `EF_KEYID == 10` passes the possession test regardless of what quest level it was stamped with. For `gatekey` in `11..14` both conditions must hold, and a key carrying the right id but a stale or absent quest stamp is rejected exactly like a missing key.

The rejected case shares the failure path with a genuine absence of key (lines 97-103): the player receives `_NN_No_Key` (message id 79, `Code/TMSrv/Language.h:99`) unless the gate is item `773`. This means a player holding a *correct but stale* inner-gate key is told "no key", which is indistinguishable from not carrying one — intentional or not, the observable behaviour is the same.

`CastleQuestLevel` is the module-global initialised to `-1` at line 69 and updated only on entry-gate use (line 155). Because `EF_QUEST` is stored as an unsigned byte (`cValue`), a key stamped while `CastleQuestLevel == -1` holds `255` and can therefore never satisfy `Quest == CastleQuestLevel` for the `-1` default; the entry gate, being exempt, still works, and the first run sets the level for everything that follows.

**Rule workflow**:
```
if (key != gatekey):
    reject (BR-12)
if (Quest != CastleQuestLevel) AND (gatekey != 10):
    reject (BR-12)
if (gatekey == 10):
    continue to BR-04 / BR-05     (Quest becomes the run selector)
else:  # 11..14
    continue to BR-10
```

---

### Business Rule: BR-04 — Entry-key quest level range validation

**Overview**:
The quest level carried by the entry key indexes the `CastleQuest[64]` table directly, so the module bounds it to `[0, 64)` before any dereference. Values outside the range are refused silently.

**Detailed description**:
Line 108 tests `Quest < 0 || Quest >= MAX_CASTLE_QUEST` and returns `TRUE` without a message. `Quest` comes from `BASE_GetItemAbility(carry, EF_QUEST)`, whose dynamic branch accumulates `stEffect[i].cValue` (an `unsigned char`), so the raw range is `0..255` and only the upper bound can realistically trigger. The silent refusal means a rogue or legacy key with a stamp above 63 produces a "gate does nothing" experience for the player.

The bound protects the global table against an out-of-range read at line 147 (`CastleQuest[Quest].MOB_INITIAL`) and at line 156 (`CastleQuest[Quest].QuestTime`). It does not validate that the row *contains* a usable definition: a row with `MOB_END == 0` and `QuestTime == 0` passes the test and starts a degenerate run (see BR-07 and BR-08).

The check also sits *after* the key match, which is significant: an out-of-range key id would already have failed BR-02, so BR-04 can only ever fire for an item whose `EF_KEYID` is exactly 10 and whose `EF_QUEST` is 64 or more. This is reachable in practice only by GM-created or corrupted items, not by normal gameplay.

**Rule workflow**:
```
if (gatekey == 10):
    if (Quest < 0) OR (Quest >= 64):
        return TRUE          (silent, no message, no state change)
    else:
        continue to BR-05
```

---

### Business Rule: BR-05 — Single concurrent run lock

**Overview**:
The event is globally exclusive: while `CastleQuestTime != -1` no new run can start. The refusal is informative — it names the current leader and the number of registered party members — and it is the only place the module reveals live event state to a player.

**Detailed description**:
Lines 111-127 count entries in `CastleQuestParty[0..MAX_PARTY)` that are non-zero and below `MAX_USER`, then format `_SD_Zakum_Quest_by_S_N` (message id 222) with `CastleLeader` and the count. The count excludes the leader, who lives in slot `MAX_PARTY` by construction (BR-09), so the number shown is "other party members", not "participants". A solo run therefore reports `0`.

The lock is released in exactly two places, both of which write `-1`: the timeout branch of the second timer (line 320) and nothing else — a boss kill does **not** release it (`CastleQuestClear` is a different variable). Consequently, after a successful clear, the arena is evacuated within two minute ticks but the entry gate remains blocked until the original countdown expires naturally. Players standing outside during that window get the "quest by <leader>" message even though the quest is already won.

`CastleLeader` is copied with `strncpy(..., NAME_LENGTH)` = 16 bytes at line 163 from `pMob[Leader].MOB.MobName`, which is itself a 16-byte field; the destination is not re-terminated, so a name filling all 16 bytes leaves no terminator inside the buffer. It is then used in a `sprintf` with `%s` at line 123.

**Rule workflow**:
```
if (gatekey == 10) AND (CastleQuestTime != -1):
    count = #(CastleQuestParty[i] != 0 && < MAX_USER)   for i in 0..MAX_PARTY-1
    send "_SD_Zakum_Quest_by_S_N(CastleLeader, count)"
    return TRUE                                          (no world change)
else:
    continue to BR-06
```

---

### Business Rule: BR-06 — Pre-spawn arena purge

**Overview**:
Before a run spawns its mob table, the arena rectangle `x 2180..2296, y 1160..1269` is swept: every mob found on the mob grid is deleted and its generator is switched off. This makes each run idempotent with respect to leftovers from a crashed or abandoned previous run.

**Detailed description**:
The sweep is a double loop over the rectangle (117 x 110 = 12,870 grid cells) reading `pMobGrid[y][x]` (`unsigned short`, `4096 x 4096`, `Code/TMSrv/Server.h:54`). Entries below `MAX_USER` (1000) are skipped, which excludes the "empty" sentinel `0` and every player slot; anything else is treated as a mob index. For each such mob the code reads `pMob[tmob].GenerateIndex`, writes `-1` into `mNPCGen.pList[generate].MinuteGenerate` and calls `DeleteMob(tmob, 3)`.

Two unvalidated indirections occur here and are repeated verbatim in the two cleanup paths (lines 330-337, 365-372): `generate` is used as an index into `mNPCGen.pList` (declared with `MAX_NPCGENERATOR = 8192` slots, `Code/Basedef.h:109`) with no range test, and `tmob` is used to index `pMob` (`MAX_MOB = 25000`) with only a lower bound. A grid cell holding a stale value above `MAX_MOB` would be dereferenced.

`DeleteMob(tmob, 3)` (`Code/TMSrv/Server.cpp:7020-7054`) multicasts `_MSG_RemoveMob` with `RemoveType = 3`, decrements `mNPCGen.pList[gen].CurrentNumMob` (clamped at zero), zeroes HP, clears `Mode` and frees the grid cell. Note the ordering: `MinuteGenerate` is disabled *before* deletion, so the deletion cannot immediately trigger a respawn.

**Rule workflow**:
```
for x in 2180..2296:
  for y in 1160..1269:
    tmob = pMobGrid[y][x]
    if (tmob < MAX_USER): continue
    gen = pMob[tmob].GenerateIndex
    mNPCGen.pList[gen].MinuteGenerate = -1        (no bounds check)
    DeleteMob(tmob, 3)
```

---

### Business Rule: BR-07 — Spawn wave composition and per-minute respawn forcing

**Overview**:
The run populates the arena by iterating the generator index range declared in the quest row and calling `GenerateMob` twice per index, while forcing non-boss generators to a one-tick respawn interval. The rule gives the event a "double wave" shape and makes the arena self-replenishing for the duration of the countdown.

**Detailed description**:
`for (int x = MOB_INITIAL; x <= MOB_END; x++)` (line 147) assumes `MOB_INITIAL <= MOB_END`; there is no validation, and an inverted range simply produces no spawns while still setting `CastleQuestLevel` and `CastleQuestTime`. `GenerateMob(index, 0, 0)` (`Code/TMSrv/Server.cpp:2995-...`) does *not* spawn one mob: it resolves a randomized group of `MinGroup..MaxGroup` (capped by `MaxNumMob`), relocates the index if `MinuteGenerate >= 500`, copies the generator's leader template into a free mob slot and distributes segment positions from the generator's own `SegmentListX/Y`. Calling it twice therefore means "two independently randomized groups per index", not exactly two mobs.

For bosses the code deliberately does not touch `MinuteGenerate` (`if (x != BOSS[0] && x != BOSS[1])`), so boss respawn cadence remains whatever `NPCGener.txt` declares; the two unconditional `GenerateMob` calls still spawn two boss groups per entry. Bosses with a `MinuteGenerate >= 500` are additionally subject to the relocation branch inside `GenerateMob`, meaning a "boss" index used as a random pool would spawn normal mobs instead.

The mutation at line 150 is the most consequential side effect in the module: `MinuteGenerate = 1` rewrites the shared generator configuration that `ProcessMinTimer`'s respawn loop consumes (`Code/TMSrv/ProcessSecMinTimer.cpp:2212-2226`). With `MinuteGenerate == 1`, `MinCounter % 1 == i % 1` is always true, so every such generator runs `GenerateMob` on every minute tick. Nothing in the module restores the previous value, and the cleanup paths only disable generators that still have a live mob at clean-up time (BR-25/BR-26), so generators emptied by combat keep respawning after the event.

**Rule workflow**:
```
for gx = CastleQuest[Quest].MOB_INITIAL .. MOB_END:
    if (gx != BOSS[0] && gx != BOSS[1]):
        mNPCGen.pList[gx].MinuteGenerate = 1      (permanent mutation)
    GenerateMob(gx, 0, 0)
    GenerateMob(gx, 0, 0)
```

---

### Business Rule: BR-08 — Countdown arming and client notification

**Overview**:
A run's authoritative timer is `CastleQuestTime`, armed to `QuestTime - 1`, and the configured `QuestTime` is pushed to the client via `_MSG_StartTime` so the client can render a countdown. Everyone in the leader's party at that instant is notified.

**Detailed description**:
Line 155-156 assigns `CastleQuestLevel = Quest` and `CastleQuestTime = CastleQuest[Quest].QuestTime - 1`. The off-by-one is meaningful: the second timer branch `if (CastleQuestTime > 0) CastleQuestTime--` would otherwise consume one extra second before the client countdown reached zero. `QuestTime == 0` therefore produces `CastleQuestTime == -1` immediately, which is the *idle* sentinel: the run never arms, and the arena is not evacuated by BR-25 until the entry gate is used again (each entry attempt re-purges the arena at BR-06, so the practical effect is "instant recycle").

The notification path is asymmetric. The leader is identified as `pMob[conn].Leader`, falling back to `conn` when zero (lines 158-161); the signal is sent with `ESCENE_FIELD` as the sender id and `CastleQuest[Quest].QuestTime` as the payload. Party members are then iterated over `pMob[Leader].PartyList[0..MAX_PARTY)` with the guards `partyconn > 0 && partyconn < MAX_USER && partyconn != Leader && pUser[partyconn].Mode == USER_PLAY`, so offline, summon and leader duplicates are skipped. Members who are online but in a transitional mode (not `USER_PLAY`) never learn that a run started.

Nothing is persisted: `CastleQuestTime` lives only in process memory, so a server restart during a run leaves gates locked and generators mutated but the run forgotten. On restart, `Reboot()` re-locks keyed init items (`Code/TMSrv/Server.cpp:7178-7186`) but does not restore `MinuteGenerate` for castle generators, which are re-read from `NPCGener.txt` by `CNPCGenerator::ReadNPCGenerator` (`Code/TMSrv/CNPCGene.cpp:45-122`).

**Rule workflow**:
```
CastleQuestLevel = Quest
CastleQuestTime  = CastleQuest[Quest].QuestTime - 1
Leader = (pMob[conn].Leader != 0) ? pMob[conn].Leader : conn
strncpy(CastleLeader, pMob[Leader].MOB.MobName, NAME_LENGTH)
SendClientSignalParm(Leader, ESCENE_FIELD, _MSG_StartTime, QuestTime)
for each m in pMob[Leader].PartyList:
    if (m valid) AND (m != Leader) AND (pUser[m].Mode == USER_PLAY):
        SendClientSignalParm(m, ESCENE_FIELD, _MSG_StartTime, QuestTime)
```

---

### Business Rule: BR-09 — Party snapshot registration

**Overview**:
The run's legitimate participants are frozen into `CastleQuestParty[13]` at start time: 12 party slots copied from the leader's `PartyList` plus the leader itself in the last slot. This array is the sole authority used by the movement guard (BR-28) and by the "busy" message's member count (BR-05).

**Detailed description**:
The copy loop (lines 175-184) iterates `x` from 0 to `MAX_PARTY` inclusive; at `x == MAX_PARTY` it writes `CastleQuestParty[MAX_PARTY] = Leader` and `continue`s, otherwise it copies `pMob[Leader].PartyList[x]` — a `short` — into an `int` array. `PartyList` holds party member indices, but nothing filters out entries that are zero, out of range, summons (`>= MAX_USER`) or stale.

The snapshot is never invalidated. Neither the timeout path (BR-25), nor the clear state machine (BR-26), nor a new run started at a different level resets the array before overwriting it; only a subsequent entry-gate use rewrites it. Between runs, therefore, a stale snapshot keeps granting passage through the guarded rectangles to players who may have left the party, logged out or changed characters, and keeps denying passage to the current party of the last leader.

Because `CheckMove` requires an exact `CastleQuestParty[i] == conn` match on the mover's *connection index*, a player who reconnects after a disconnect obtains a new index in some builds and a recycled one in others; the snapshot is index-based, not name- or account-based, so identity drift silently changes who is admitted.

**Rule workflow**:
```
for x in 0..MAX_PARTY:
    if (x == MAX_PARTY):
        CastleQuestParty[MAX_PARTY] = Leader
    else:
        CastleQuestParty[x] = pMob[Leader].PartyList[x]
(no invalidation on timeout, clear, logout or new leader)
```

---

### Business Rule: BR-10 — Inner-gate quest binding

**Overview**:
Doors `11..14` inside the castle only respond to keys stamped with the level of the run currently in progress. This prevents keys carried over from a previous run from opening the inner wing of a new one.

**Detailed description**:
The check at lines 188-192 is the only content of the inner-gate branch: `if (Quest != CastleQuestLevel) return TRUE;` — silent, with no client message. Note the contrast with BR-03: the same mismatch on an inner gate during the *possession* scan is reported as "no key", while a mismatch that survives the scan (impossible in the current code, since the scan uses the identical predicate) is silent. In practice lines 188-192 are unreachable dead code, because any key reaching them already satisfied `Quest == CastleQuestLevel` at line 91.

What the rule therefore actually enforces is the conjunction at the scan. The practical consequence is that a player who looted an inner key during run *N* cannot use it during run *N+1*: `KeyDrop` stamped it with the level current at drop time (`CastleQuestLevel`), and the next run rewrites `CastleQuestLevel` at line 155. The key remains in the inventory as a permanently inert item with a stale quest stamp, and the only observable feedback is `_NN_No_Key`.

Because `CastleQuestLevel` is not reset to `-1` when a run ends, keys from the last run stay valid for the inner doors during the whole idle period. Any player still holding one can walk into the inner wing even though no event is running, provided the doors are open — and doors relock on their own schedule (BR-27), not on the event's.

**Rule workflow**:
```
if (gatekey in 11..14):
    if (Quest != CastleQuestLevel):
        return TRUE     (dead branch in practice: scan already enforced this)
    else:
        continue to BR-11
```

---

### Business Rule: BR-11 — Single key consumption

**Overview**:
Opening a castle gate destroys exactly one key: the carry slot that satisfied the possession scan is zeroed and resynchronised to the client. This is the only consumable cost of the event.

**Detailed description**:
Lines 195-196 operate on the loop index `i` that survived the scan. Because the function returned at line 102 whenever the scan failed, `i` here always identifies a matching slot. `memset(&pMob[conn].MOB.Carry[i], 0, sizeof(STRUCT_ITEM))` clears the 10-byte item, then `SendItem(conn, ITEM_PLACE_CARRY=1, i, ...)` pushes the now-empty slot so the client cannot retain a ghost copy.

The slot index is a local variable of the loop declared at line 85 and re-used by two inner loops that shadow it (`for (int i = 0; ...)` at lines 115 and 167 create `int i` inside their own scope). C++ scoping keeps the outer `i` intact, so consumption is correct — but the shadowing means a future change to the inner loops (for example removing the `int`) would silently consume the wrong slot.

No check precedes the destruction. A player whose only entry key opens the gate loses the item even if the resulting run is degenerate (BR-08 with `QuestTime == 0`), and no "key consumed but nothing happened" feedback exists. The key is not returned if `UpdateItem` subsequently fails to change state: line 200's return value only controls the multicast, not the consumption.

**Rule workflow**:
```
memset(&pMob[conn].MOB.Carry[i], 0, sizeof(STRUCT_ITEM))
SendItem(conn, ITEM_PLACE_CARRY, i, &pMob[conn].MOB.Carry[i])
isUpdate = UpdateItem(gateid, STATE_OPEN, &height)
if (isUpdate): GridMulticast(...)
Log("etc,opengate ...")
```

---

### Business Rule: BR-12 — Missing-key feedback with item 773 suppression

**Overview**:
A failed possession test normally reports `_NN_No_Key` to the client. When the gate item's own `sIndex` is `773` the message is suppressed, producing a silently inert gate.

**Detailed description**:
Lines 99-101 read `pItem[gateid].ITEM.sIndex` and compare against the literal `773`. No symbolic constant exists in the codebase for this value and the item list is not in the repository, so the semantic identity of item 773 cannot be established from source; it is a hard-coded magic number used identically in the legacy fallback at `Code/TMSrv/_MSG_UpdateItem.cpp:83`.

The suppression is consistent across both failure shapes — genuinely missing key and stale quest stamp — because both reach the same return. It is *not* applied to BR-04 (out-of-range quest) or BR-10, which are silent for all gates. The observable matrix is therefore: no key → message (unless 773); wrong level → message (unless 773); level out of range → silence; inner-door mismatch reaching line 190 → silence.

`SendClientMessage` (`Code/TMSrv/SendFunc.cpp:27-45`) is range-checked and copies into a `MSG_MessagePanel` of `MESSAGE_LENGTH` 96 bytes, forcing the last two bytes to zero. The message text is read from `g_pMessageStringTable[79]`, populated from `Language.txt` at boot (`Code/TMSrv/Server.cpp:7160`), so the suppression is a message-table-independent decision made before lookup.

**Rule workflow**:
```
if (key/quest predicate failed):
    if (pItem[gateid].ITEM.sIndex != 773):
        SendClientMessage(conn, g_pMessageStringTable[_NN_No_Key])
    return TRUE       (no consumption, no state change, no audit line)
```

---

### Business Rule: BR-13 — Server-forced `STATE_OPEN` with client packet echo

**Overview**:
A castle gate always opens. The `State` value supplied by the client is validated upstream (`0..5`) and then ignored for state application, but the *whole client packet* is rebroadcast to nearby players.

**Detailed description**:
Line 200 calls `UpdateItem(gateid, STATE_OPEN, &heigth)` unconditionally. `UpdateItem` (`Code/TMSrv/Server.cpp:7472-7505`) delegates the transition to `BASE_UpdateItem`, which rejects a no-op transition (`delta == 0`) and returns `FALSE`; on success it writes `pItem[Gate].State = state`, `.Height`, and resets `.Delay = 0`. `UpdateItem` then returns `TRUE` unconditionally after the state write.

Line 203 multicasts `(MSG_STANDARD*)m` — the client's own buffer — so spectators receive `ID = conn` (the opener), the client's `State`, `Size`, `KeyWord`, `ClientTick` and checksum. The authoritative server-side pattern used elsewhere (for example `Code/TMSrv/Server.cpp:6078-6088` for castle-war doors) builds a fresh `MSG_UpdateItem` with `ID = ESCENE_FIELD` and `State = pItem[].State`. Consequently a castle gate can look closed to other clients while being open on the server, and the "opener" identity is leaked into the view window.

There is no path in this module that closes or locks a castle gate. Closing is entirely delegated to the generic minute pass (BR-27) and to `Reboot()` (`Code/TMSrv/Server.cpp:7178-7186`). The unused locals `state` and `gatestate2` at lines 74-75 are a leftover of the original logic that used the requested state to decide whether a key was required.

**Rule workflow**:
```
UpdateItem(gateid, STATE_OPEN, &height)      # ignores m->State
if (result): GridMulticast(x, y, (MSG_STANDARD*)m, 0)
             # spectators see client-supplied State/ID, not server state
```

---

### Business Rule: BR-14 — Audit trail on gate opening

**Overview**:
Every castle gate interaction that reaches line 205 writes an `etc,opengate` record containing the gate id, its coordinates, the account and the IP. Because every path inside the module returns `TRUE` after line 205, this includes failed opens in the legacy sense — but not the early returns of BR-03/BR-04/BR-10.

**Detailed description**:
`sprintf(temp, "etc,opengate gateid:%d X:%d Y:%d", gateid, pItem[gateid].PosX, pItem[gateid].PosY)` uses the process-wide 4096-byte `temp` buffer (`Code/TMSrv/Server.cpp:277`) and `Log(temp, pUser[conn].AccountName, pUser[conn].IP)` appends to the daily log file. The same literal prefix is used by the legacy path (`_MSG_UpdateItem.cpp:103-104`), so castle opens and generic gate opens are indistinguishable in the log by prefix alone; only the gate id range tells them apart.

The log is written after the state change and the multicast, so a successful entry is always recorded, but there is no corresponding record for a *rejected* castle attempt: the early returns at 102, 109, 126 and 191 produce no log line and no crack-error accumulation. Repeated "no key" spam therefore leaves no forensic trace in this module, unlike the tamper paths in the handler.

`Log` runs synchronously on the Win32 message thread and does file I/O per call; a gate-heavy event adds one write per gate interaction to the same serialised file that all other subsystems use.

**Rule workflow**:
```
sprintf(temp, "etc,opengate gateid:%d X:%d Y:%d", gateid, PosX, PosY)
Log(temp, pUser[conn].AccountName, pUser[conn].IP)
return TRUE
```

---

### Business Rule: BR-15 — Boss-kill detection across all quest rows

**Overview**:
A kill counts as a castle boss kill if the dead mob's generator index equals `BOSS[0]` or `BOSS[1]` of *any* of the 64 configured quest rows. Every matching row pays out, because the loop does not break.

**Detailed description**:
`MobKilled` is invoked for every mob death in the world (`Code/TMSrv/MobKilled.cpp:980`, before several unrelated event branches), and begins by reading `pMob[target].GenerateIndex` (line 212). The outer loop runs over all 64 rows regardless of whether a run is in progress, so the predicate is independent of `CastleQuestLevel`, `CastleQuestTime` and of the mob being inside the arena rectangle.

Two consequences follow. First, a boss killed while no run is active still triggers the announcement, still sets `CastleQuestClear = 1`, and still pays whatever rewards the matching row declares — the event does not require its own lifecycle to be running. Second, duplicate declarations pay duplicate rewards: because there is no `break`, two rows sharing a boss index produce two full settlements (items, exp, coin) inside one call.

The absence of `CastleQuestLevel` from the predicate combined with the all-zero fallback table (BR-30) creates the worst case: with `CastleQuest.txt` missing, every row has `BOSS[0] == BOSS[1] == 0`, and every kill of a mob with `GenerateIndex == 0` announces a cleared castle quest and arms the reset state machine. Mobs with generate index 0 are ordinary content (`Code/TMSrv/MobKilled.cpp:1001` handles `GenerateID == 0 || 1 || 2` explicitly).

**Rule workflow**:
```
GenerateID = pMob[target].GenerateIndex
for k in 0..63:
    if (GenerateID == CastleQuest[k].BOSS[0]) OR (GenerateID == CastleQuest[k].BOSS[1]):
        reward(k)        # BR-16..BR-21, executed for every matching row
    # no break
```

---

### Business Rule: BR-16 — Area-wide kill announcement

**Overview**:
Boss death is announced to every live player inside the arena announcement rectangle, using the killer's character name as the subject.

**Detailed description**:
Line 218 formats `_SN_CastleQuest_Killed` (message id 163) with `pMob[conn].MOB.MobName`, and `SendNoticeArea(temp, 2176, 1160, 2300, 1276)` (`Code/TMSrv/SendFunc.cpp:155-165`) delivers it to every slot with `Mode == USER_PLAY` whose `TargetX/TargetY` fall inside the inclusive rectangle. The announcement rectangle is deliberately wider than the arena used for spawns and cleanups (`2180..2296`, `1160..1269`), so players standing just outside the playfield still see the result.

`conn` is the killer slot as supplied by the combat code, which may legitimately be a summon or a pet (`>= MAX_USER`). In that case the "name" is the summon's mob name and the reward path below targets a mob slot, not a player; `PutItem`/`SendClientMessage`/`SendEtc` all reject non-user indices, so the announcement is the only effect.

The message is broadcast only; there is no whisper to the leader, no guild notice, no persistence. A leader who is out of the area at the moment of the kill sees nothing of the announcement but still receives the reward items (BR-18) because settlement targets the leader by index, not by location.

**Rule workflow**:
```
sprintf(temp, _SN_CastleQuest_Killed, pMob[conn].MOB.MobName)
SendNoticeArea(temp, 2176, 1160, 2300, 1276)
```

---

### Business Rule: BR-17 — Clear-state arming

**Overview**:
A boss death arms the two-phase reset by setting `CastleQuestClear = 1`. The flag is the only hand-off between the kill path (executed from combat) and the reset path (executed from the minute timer).

**Detailed description**:
Line 220 writes the module-global `CastleQuestClear`, whose lifecycle is `0 (idle) → 1 (armed) → 2 (announcing) → 0 (cleaned)` as implemented in `ProcessMinTimer` (BR-26). Because the kill path runs on the same single-threaded message loop as the timer, no locking is needed and the flag is set exactly once per matching boss row — but a duplicate row sets it twice with no additional effect, since it becomes `1` either way.

Arming does **not** stop the countdown: `CastleQuestTime` continues to decrement (BR-24) independently of `CastleQuestClear`. A boss killed with 90 % of the countdown remaining therefore produces two separate cleanups — the clear state machine (within two minute ticks) and, later, the timeout branch when the countdown reaches zero. During the interval the entry gate stays blocked (BR-05) and the arena is already empty.

Arming also does not clear `CastleQuestLevel` or `CastleQuestParty`, so the still-armed state is observable through BR-03 (inner doors stay level-bound) and BR-28 (stale party still admitted) until a new entry overwrites them.

**Rule workflow**:
```
on boss kill:
    SendNoticeArea(...)     # BR-16
    CastleQuestClear = 1    # consumed by ProcessMinTimer
    (CastleQuestTime keeps counting down independently)
```

---

### Business Rule: BR-18 — Item prize bundle to the leader

**Overview**:
The leader receives a clone of every non-empty prize slot declared in the matching quest row. The bundle is a fixed array of up to 64 `STRUCT_ITEM` values and is delivered slot-by-slot with no capacity pre-check.

**Detailed description**:
Lines 229-233 iterate `j` over `MAX_CARRY` (64) and call `PutItem(partyleader, &CastleQuest[k].Prize[j])` whenever `sIndex != 0`. `PutItem` (`Code/TMSrv/Server.cpp:709-737`) looks for the first empty slot below `pMob[conn].MaxCarry`, copies the 10-byte item, sends `_MSG_SendItem` for that slot, and returns `FALSE` when no slot is free — but the return value here is discarded.

The failure mode is silent item loss: a leader with a full bag receives nothing for the overflowing slots, gets no message, and the module continues with exp and coin. The same applies to the party loop (BR-21). Because prizes are copied from the configuration table by value and never re-randomised, every run at the same level yields the identical bundle; `SetItemBonus` is not applied, so prize items carry exactly the effects written in `CastleQuest.txt`.

The bundle is paid to `partyleader`, computed as `pMob[conn].Leader` with `<= 0` falling back to the killer. When the killer is a summon (`conn >= MAX_USER`) and it has no leader relation, `partyleader` becomes a mob index and every `PutItem`/`SendClientMessage`/`SendEtc` call is rejected by its range guard, so the entire settlement is a no-op.

**Rule workflow**:
```
partyleader = pMob[conn].Leader ; if (partyleader <= 0) partyleader = conn
for j in 0..63:
    if (CastleQuest[k].Prize[j].sIndex != 0):
        PutItem(partyleader, &CastleQuest[k].Prize[j])    # failure ignored
```

---

### Business Rule: BR-19 — Class-tiered experience prize

**Overview**:
The experience reward is selected by the recipient's `extra.ClassMaster` index into `ExpPrize[6]`. A zero entry means "no reward", which is why class master `0` — and mortal, whose configured tier is overwritten by `SUBCELESTIAL` — depends entirely on the configuration file.

**Detailed description**:
`STRUCT_CASTLEQUEST::ExpPrize[6]` (`Code/Basedef.h:666`) is populated by three mutually independent parser branches: `EXPPRIZE_ARCH:` writes index 1, `EXPPRIZE_MORTAL:` writes index 2, `EXPPRIZE_CELESTIAL:` writes both 3 and 4, and `EXPPRIZE_SUBCELESTIAL:` writes index 2 as well — so a file containing both `EXPPRIZE_MORTAL:` and `EXPPRIZE_SUBCELESTIAL:` has the second one win, silently. Index 0 and index 5 are never written by the parser; class master `0` therefore always resolves to zero (no reward) and `SCELESTIAL` (5) also resolves to zero.

The reward is applied by direct mutation: `pMob[partyleader].MOB.Exp += ExpPrize[...]` followed by a client message `_NS_GETEXP` (id 473). There is no overflow guard (the field is `long long`, `Code/Basedef.h:450`), no per-run cap, no verification that the recipient is inside the arena, and no level-up processing in this path — the module mutates raw experience and relies on whatever consumer normally derives level from it, unlike the ordinary kill path which at least computes via `GetExpApply` first.

The same block is duplicated for party members (lines 271-277) with the same zero-guard, so the per-member award is identical in size to the leader's rather than split. A 12-member party therefore multiplies the configuration value by 13.

**Rule workflow**:
```
v = CastleQuest[k].ExpPrize[pMob[recipient].extra.ClassMaster]
if (v != 0):
    pMob[recipient].MOB.Exp += v
    SendClientMessage(recipient, _NS_GETEXP with v)
```

---

### Business Rule: BR-20 — Coin prize with 2,000,000,000 cap

**Overview**:
The coin reward is applied only if the resulting balance stays at or below two billion; otherwise the player is told they cannot hold more and the balance is untouched. This is the only reward rule with an explicit arithmetic guard.

**Detailed description**:
The computation is `unsigned int Coin = pMob[recipient].MOB.Coin + CastleQuest[k].CoinPrize` (lines 244, 281). `MOB.Coin` is a signed `int` and `CoinPrize` a signed `int` from configuration, but the sum is evaluated in `unsigned int`, so a negative configured prize or a negative balance wraps to a very large unsigned value and fails the `<= 2000000000` test — the player then receives the cap message rather than losing money, which is a safe-but-confusing outcome.

On success the code sends `_DN_receive_D_gold` (id 294) *before* assigning and then calls `SendEtc(recipient)` to resynchronise the displayed coin/exp/skill block. The message-before-write ordering means a client that reacts instantly could display the new amount before the server field changes; since both happen inside one message-loop iteration and `SendEtc` follows, the client converges on the correct value.

On failure the only effect is `_NN_Cant_get_more_than_2G` (id 273). The prize is dropped, not banked, not converted, not retried. A leader at the cap therefore receives the item and exp components but permanently loses the coin component of every castle run.

**Rule workflow**:
```
if (CoinPrize != 0):
    Coin = (unsigned)(MOB.Coin + CoinPrize)
    if (Coin <= 2000000000):
        SendClientMessage(recipient, _DN_receive_D_gold)
        MOB.Coin = Coin
        SendEtc(recipient)
    else:
        SendClientMessage(recipient, _NN_Cant_get_more_than_2G)
```

---

### Business Rule: BR-21 — Party reward propagation

**Overview**:
When the quest row enables `PARTYPRIZE: ON`, the identical item, exp and coin settlement is repeated for every party member currently in `USER_PLAY` mode. When disabled, only the leader is paid.

**Detailed description**:
The loop header is `for (int i = 0; i < MAX_PARTY && CastleQuest[k].PartyPrize; i++)` — the flag is used as the loop *condition*, so a zero flag skips the whole block while still evaluating `i < MAX_PARTY` first. Members are read from `pMob[partyleader].PartyList[i]`; the only filter is `pUser[partymember].Mode != USER_PLAY → continue`, which means zero indices and out-of-range indices resolve to `pUser[0]`/`pUser[x]` and are skipped by the mode test rather than by a range test.

The leader is not excluded from this loop. In the common convention the leader does not appear in its own `PartyList`, but if it does (or if the leader is listed twice) it is paid twice: once by BR-18..BR-20 and once here. Every member is also paid the full bundle rather than a share, so with `PARTYPRIZE: ON` the total item volume is `(1 + members) × prize slots` per boss, and the reward path can fill 13 inventories in one call.

The rule does not verify that members are inside the arena, that they participated in the kill, that they are still in the same party as when the run started, or that they are alive. A member idling in town at the moment of the kill is paid in full, while a member who disconnected is silently skipped.

**Rule workflow**:
```
if (CastleQuest[k].PartyPrize != 0):
    for i in 0..MAX_PARTY-1:
        m = pMob[partyleader].PartyList[i]
        if (pUser[m].Mode != USER_PLAY): continue
        pay items (BR-18 logic)
        pay exp   (BR-19 logic)
        pay coin  (BR-20 logic)
```

---

### Business Rule: BR-22 — Key drop stamping and direct inventory insertion

**Overview**:
When loot generation produces an item whose `EF_KEYID` is `11..14`, the module stamps it with the level of the currently running quest and inserts it straight into the killer's inventory, returning `FALSE` so the caller does not drop it on the ground.

**Detailed description**:
`KeyDrop(target, conn, PosX, PosY, Key)` is called from the generic drop routine (`Code/TMSrv/MobKilled.cpp:1904`) after `SetItemBonus` has been applied to the mob's carry slot, and before the item is either put into the inventory or written to the ground. Only the `EF_KEYID` matters for the decision; `target`, `PosX` and `PosY` are accepted but never used, which is why keys cannot be made to drop on the floor even by changing the caller.

Stamping writes `Key->stEffect[0].cEffect = EF_QUEST` and `Key->stEffect[0].cValue = CastleQuestLevel`. Two side effects follow: an existing effect in slot 0 of the item is destroyed (the item template's first bonus is overwritten), and `CastleQuestLevel == -1` truncates to `255` because `cValue` is an `unsigned char` (`Code/Basedef.h:407-410`). A key dropped while no run is active is therefore stamped with a level that can never match a real run, permanently inert but still consumable at the entry gate (which ignores the stamp per BR-03).

`PutItem(conn, Key)` is called and its result discarded, so a killer with a full inventory silently loses the key: the mob is being deleted and the item exists only in that transient buffer. `BASE_GetItemAbility` additionally scales `EF_KEYID` by the item's sanctification (`value * (sanc + 10) / 10`, `Code/Basedef.cpp:1813-1816`) because `EF_KEYID` is not in the exclusion list — a sanctified key item with id 11 and one sanc point reads as `12`, so "the key id" is not stable across item enhancement.

**Rule workflow**:
```
ItemKey = BASE_GetItemAbility(Key, EF_KEYID)
if (11 <= ItemKey <= 14):
    Key->stEffect[0].cEffect = EF_QUEST
    Key->stEffect[0].cValue  = CastleQuestLevel      # -1 -> 255
    PutItem(conn, Key)                               # result ignored
    return FALSE                                     # caller skips ground drop
return TRUE                                          # caller drops normally
```

---

### Business Rule: BR-23 — Non-quest item passthrough

**Overview**:
Everything that is not an inner-gate key is returned to the caller unchanged, which then performs the normal inventory-or-ground decision. This includes the entry key (`EF_KEYID == 10`) and every non-key item.

**Detailed description**:
The single `return TRUE` at line 311 ends the function without mutation. The caller (`Code/TMSrv/MobKilled.cpp:1904-1905`) interprets `TRUE` as "you may now place this item yourself" and calls `PutItem(conn, item)`, so in the current implementation even pass-through items end up in the inventory rather than on the ground — the distinction between the two branches is therefore *only* the quest stamp, not the delivery mechanism.

The asymmetry is meaningful for the entry key: because `10` is not stamped, an entry key obtained from a mob always carries whatever `EF_QUEST` its template declares, and that value becomes the quest level for the run it starts (BR-04). If the template has no `EF_QUEST`, the entry key selects quest row 0.

Because the branch decision is based on the *current* effect read (`BASE_GetItemAbility`), and that read is sanc-scaled (BR-22), an inner key item enhanced to a sanctification level that shifts its id out of `11..14` falls into this branch and enters the world unstamped — and can then never open its door.

**Rule workflow**:
```
if (ItemKey not in 11..14):
    return TRUE          # no mutation; caller performs PutItem
```

---

### Business Rule: BR-24 — One-second countdown tick

**Overview**:
`CastleQuestTime` decrements once per second and floors at `-1`, which is the module's "no run" sentinel. The tick is derived from the global second counter's parity, not from a dedicated timer.

**Detailed description**:
`ProcessSecTimer` is called from `TMSrv::ProcessSecTimer` after `SecCounter++` (`Code/TMSrv/ProcessSecMinTimer.cpp:123,1172`). A `TIMER_SEC` interval of 500 ms (`Code/TMSrv/Server.cpp:3631`) makes `SecCounter` advance twice per wall-clock second, so `if (SecCounter % 2 == 0)` gates the body to a 1 Hz cadence. During the shutdown sequence the interval is changed to 200 ms (`Code/TMSrv/ProcessSecMinTimer.cpp:115`), which would make castle countdowns run ~2.5x faster; that state is only reachable while `ServerDown` is counting.

The body has two mutually exclusive branches: `CastleQuestTime == 0` performs the timeout (BR-25) and immediately writes `-1`; `CastleQuestTime > 0` decrements. Negative values are left untouched, so the sentinel is stable and a second timeout cannot fire until a new run arms the counter. Because `CastleQuestTime` is written by the entry path at line 156 and read here without any atomicity concern (single-threaded loop), there is no race, but there is also no persistence: a restart forgets the countdown while leaving the arena populated.

The effective duration is `QuestTime` seconds from the moment of entry (`QuestTime - 1` then decrements until zero). A value of `1` collapses to a single-tick window because the counter already holds `0` when the entry returns and the next even tick performs the timeout.

**Rule workflow**:
```
if (SecCounter % 2 == 0):
    if (CastleQuestTime == 0):
        CastleQuestTime = -1
        ClearArea(2180,1160,2296,1269)
        purge arena mobs, MinuteGenerate = -1
    if (CastleQuestTime > 0):
        CastleQuestTime--
```

---

### Business Rule: BR-25 — Timeout cleanup

**Overview**:
When the countdown reaches zero the module evacuates the arena and disables the generators of every mob still standing in it. This is the only automatic end-of-run path.

**Detailed description**:
`ClearArea(x1,y1,x2,y2)` (`Code/TMSrv/Server.cpp:5754-5775`) iterates the user table, selects `USER_PLAY` users whose `TargetX/TargetY` fall in the rectangle, revives any with `Hp <= 0` to `Hp = 2` with a score resend, and calls `DoTeleport(i, ...)` → `DoRecall`. Because the whole 0..999 user table is scanned, the cost is bounded by player count, not by the rectangle.

The mob purge repeats BR-06 exactly: grid sweep, `tmob < MAX_USER` skip, `MinuteGenerate = -1`, `DeleteMob(tmob, 3)`. It runs immediately after `ClearArea`, so players are already out when the mobs are removed. Since only generators with a *live* mob in the rectangle are disabled, generators whose mobs were killed earlier in the run keep `MinuteGenerate == 1` from BR-07 and continue producing castle mobs on subsequent minute ticks — the arena can therefore partially repopulate indefinitely after a timeout, with no event running and no further cleanup pass.

Nothing here touches `CastleQuestLevel`, `CastleQuestParty`, `CastleQuestClear` or the gate states. A timeout is therefore a *soft* end: the run flag and countdown are reset, but membership, level binding and generator configuration survive.

**Rule workflow**:
```
CastleQuestTime = -1
ClearArea(2180,1160,2296,1269)        # recall + revive
for x in 2180..2296, y in 1160..1269:
    tmob = pMobGrid[y][x]
    if (tmob < MAX_USER): continue
    mNPCGen.pList[pMob[tmob].GenerateIndex].MinuteGenerate = -1
    DeleteMob(tmob, 3)
```

---

### Business Rule: BR-26 — Two-phase reset state machine

**Overview**:
The post-boss reset is a three-state machine driven by the minute callback: `0` idle, `1` armed by the kill, `2` announced, back to `0` after evacuation. Each phase costs one `TIMER_MIN` callback.

**Detailed description**:
Phase `1 → 2` (lines 347-351) sends `_NN_CastleQuest_Initialize` (id 164) to the announcement rectangle `2176..2300 × 1160..1276` and does nothing else, giving players one tick of warning. Phase `2 → 0` (lines 352-374) performs `ClearArea` plus the same grid purge as BR-25, then clears the flag. With the default `TIMER_MIN` of 12,000 ms the warning-to-eviction gap is 12 seconds; with the GM-set minimum of 5,000 ms it is 5 seconds.

The machine is a plain `int` with `if/else if`, not an enum, and the phases are interdependent through the timer cadence rather than through explicit transition events. If the minute timer is stopped or its interval changed while `CastleQuestClear == 1`, the reset stalls in `1` and the arena stays populated; if the interval is very long, players get a long warning period during which the entry gate stays blocked (BR-05) because the countdown has not finished.

The state machine is also re-entrant with the timeout path: a timeout (BR-25) can evacuate the arena while `CastleQuestClear == 1` is still pending, producing two evacuations for one run and a duplicate `_NN_CastleQuest_Initialize` notice if the timeout fires between phase 1 and phase 2.

**Rule workflow**:
```
on each TIMER_MIN callback:
    if (CastleQuestClear == 1):
        CastleQuestClear = 2
        SendNoticeArea(_NN_CastleQuest_Initialize, 2176,1160,2300,1276)
    else if (CastleQuestClear == 2):
        CastleQuestClear = 0
        ClearArea(2180,1160,2296,1269)
        purge arena mobs (MinuteGenerate = -1, DeleteMob type 3)
```

---

### Business Rule: BR-27 — External gate relock coupling

**Overview**:
The module opens doors but never closes them. Relocking is performed by the generic minute pass, which locks any open keyed init item after a one-tick `Delay` latch, including all `EF_KEYID 10..14` castle gates.

**Detailed description**:
`Code/TMSrv/ProcessSecMinTimer.cpp:2142-2209` iterates `g_pInitItem[i]` from index 17 upwards, resolves the item at the configured grid position, reads `BASE_GetItemAbility(&pItem[ipg].ITEM, 39)`, and for any open item with a non-zero id below 15 arms `pItem[ipg].Delay = 1` on the first pass and calls `UpdateItem(ipg, 3, ...)` on the second. `UpdateItem` resets `Delay` to 0, so the latch re-arms each cycle.

Three consequences shape the castle event. First, a gate opened by `OpenCastleGate` stays open for one to two `TIMER_MIN` ticks (12–24 s by default) and then returns to `STATE_LOCKED` — much shorter than the quest countdown, so players must re-key doors during a run. Second, only gates listed in `g_pInitItem[17..g_dwInitItem)` participate; a castle gate created at runtime by the GM command (`Code/TMSrv/imple.cpp:264-294`) never relocks. Third, the pass runs *after* `CCastleZakum::ProcessMinTimer()` in the same callback (`:2110` before `:2142`), so a gate can be relocked in the same tick that the event announces its evacuation.

The module is unaware of this: it holds no reference to gate state, does not know whether its doors are open, and cannot keep a door open for the length of a run.

**Rule workflow**:
```
(external, each TIMER_MIN)
for i in 17..g_dwInitItem-1:
    ipg = pItemGrid[g_pInitItem[i].PosY][g_pInitItem[i].PosX]
    key = BASE_GetItemAbility(&pItem[ipg].ITEM, 39)
    if (key != 0) AND (pItem[ipg].State == STATE_OPEN) AND (key < 15):
        if (pItem[ipg].Delay == 0): pItem[ipg].Delay = 1 ; continue
        UpdateItem(ipg, STATE_LOCKED) ; pItem[ipg].State = STATE_LOCKED
```

---

### Business Rule: BR-28 — Movement-based zone access control

**Overview**:
Players who are not in the run's party snapshot are bounced back if the destination of a movement step falls inside any of the nine guarded rectangles. Party members are unrestricted.

**Detailed description**:
`CheckMove` is called from the action handler only when the move target actually differs from the current position (`Code/TMSrv/_MSG_Action.cpp:211,238`), so it is a *step* filter, not a position filter. The membership test builds `i` by scanning `CastleQuestParty[0..MAX_PARTY]` for `conn` and leaves `i == MAX_PARTY + 1` when absent; the guard loop then runs over the nine rectangles and reruns `DoTeleport(conn, pMob[conn].TargetX, pMob[conn].TargetY)` — the pre-move coordinates — as soon as one contains the destination.

The geometry is declared as `[x1, y1, x2, y2]` and compared with `targetx >= x1 && targety >= y1 && targetx <= x2 && targety <= y2`. Eight of the nine entries are stored in ascending order; the first entry, `{ 2221, 1270, 2259, 1246 }` (line 49), has `y1 > y2`, so its predicate requires `y >= 1270 && y <= 1246`, which can never be true. The comment marks it "Primeiro bloco" (first block); as written, that block is unguarded.

The rule is bypassable by any non-movement relocation: recall, `DoTeleport` from other systems, a GM move, a resurrect-and-move sequence, or simply logging in inside the rectangle (no check runs at login). It also never evicts a player who is already inside: only an outgoing step is evaluated, and a blocked step leaves the player where it was, so a legitimate party member who entered and then dropped party keeps access.

**Rule workflow**:
```
i = first index with CastleQuestParty[i] == conn, else MAX_PARTY+1
for k in 0..MAX_CASTLE_POS-1:
    if (i == MAX_PARTY+1) AND (tx within rect k):
        DoTeleport(conn, pMob[conn].TargetX, pMob[conn].TargetY)
        return
# party members: no restriction; rect[0] unreachable due to inverted y bounds
```

---

### Business Rule: BR-29 — Configuration grammar

**Overview**:
`CastleQuest.txt` is an ASCII table of up to 64 quest rows, each introduced by `#`, containing `KEYWORD: value` lines, optional `/` comments and blank CR lines. The parser is a hand-written `fgets`/`sscanf` dispatcher with no schema validation.

**Detailed description**:
`ReadCastleQuest` (lines 396-443) opens the file in text mode at `../../Common/Settings/CastleQuest.txt` (line 44, relative to the working directory), then processes line by line. A leading `/` (ASCII 47) is a comment and is skipped; a leading `#` starts a new row by incrementing `Num` and `memset`-ing the row; a line whose first byte is `\r` (13) is skipped as blank; everything else is handed to `ParseCastleString`. `ParseCastleString` first runs `sscanf("%s %s %s")` for the keyword and two values, then a second `sscanf("%s %s %s %s %s %s %s %s")` for the prize form, converts seven tokens with `atoi`, uppercases `str1`/`str2` and dispatches on a string table of eighteen keywords.

The recognised keywords are `MOB_INITIAL:`, `MOB_END:`, `BOSS1:`, `BOSS2:`, `COINPRIZE:`, `EXPPRIZE_ARCH:`, `EXPPRIZE_MORTAL:`, `EXPPRIZE_CELESTIAL:`, `EXPPRIZE_SUBCELESTIAL:`, `PARTYPRIZE:` (`ON`/anything else), `QUESTTIME:` and `PRIZE_<n>:` for `n` in `0..63`, where the seven numbers map to `sIndex, effect0, value0, effect1, value1, effect2, value2`. Unknown keywords fall through every `strcmp` in the prize loop and are ignored without a diagnostic; there is no counter, no line number and no error reporting at all.

Three structural defects are visible in the parser. (a) `if (Num >= MAX_CASTLE_QUEST) break;` is checked *before* `Num++`, so a file with 65 blocks writes `CastleQuest[64]` — one element past the array. (b) If the first non-comment line is data rather than `#`, `Num` is still `-1` and every assignment writes `CastleQuest[-1]`, i.e. eight bytes before the array. (c) `item_str1..7` are declared without initialisation (lines 470-476) and `atoi` is applied unconditionally, so a `PRIZE_n:` line with fewer than seven numbers reads uninitialised stack memory into the prize fields; likewise `sscanf` with unbounded `%s` into `str1/str2/str3` (128 bytes) and `item_str1..7` (256 bytes) overflows on long tokens, while the only length test (`strlen(str2) > 80`, line 492) runs after the copy. A reload through the GM command (`imple.cpp:259`) re-runs all of this on a live server with no rollback if it fails midway.

**Rule workflow**:
```
open ../../Common/Settings/CastleQuest.txt
while (fgets(line, 1024)):
    '/'    -> skip
    '#'    -> if (Num >= 64) break ; Num++ ; zero row
    '\r'   -> skip
    other  -> ParseCastleString(Num, line)         # Num may be -1
              sscanf x2, atoi x7 (uninitialised when short), _strupr
              dispatch on keyword -> write CastleQuest[Num]
```

---

### Business Rule: BR-30 — Missing configuration fallback and boot blocking

**Overview**:
If `CastleQuest.txt` cannot be opened, the server shows a modal message box and continues with an all-zero quest table. No exception path, no retry, no in-memory default.

**Detailed description**:
Lines 398-405 call `fopen`; on `NULL` they invoke `MessageBoxA(hWndMain, "CastleQuest.txt nao foi encontrado.", CASTLE_QUEST_PATH, MB_OK)`. `ReadCastleQuest` is invoked from `WinMain` at `Code/TMSrv/Server.cpp:3629`, twenty-one lines before the `GetMessage` loop starts at `:3650`, so the dialog is modal before the server is able to process sockets: the process waits for human acknowledgement while the main window exists but no messages are pumped by the application.

After dismissal the function returns and `CastleQuest[64]` remains as declared — zero-initialised global storage. Every row therefore has `MOB_INITIAL = MOB_END = 0`, `BOSS = {0,0}`, `QuestTime = 0`, `CoinPrize = 0`, `PartyPrize = 0` and empty prizes. The observable consequences are: any `EF_KEYID == 10` key with `EF_QUEST == 0` starts a run that spawns generator 0 twice (BR-07) and arms `CastleQuestTime = -1` (BR-08), so the entry gate never blocks; and every death of a `GenerateIndex == 0` mob matches `BOSS[0]` of all 64 rows (BR-15), announcing `_SN_CastleQuest_Killed`, arming `CastleQuestClear` (BR-17) and driving the reset machine on every such kill.

There is no check that a loaded row is coherent (`MOB_INITIAL <= MOB_END`, `MOB_END < MAX_NPCGENERATOR`, bosses inside `[MOB_INITIAL, MOB_END]`, `QuestTime > 0`). A row with `MOB_END` above 8191 makes line 150 index `mNPCGen.pList` out of bounds, and `GenerateMob` would then read the same out-of-range slot; a row with `MOB_END` below `MAX_USER` would make the module mark and delete *players*' generator entries. Every one of these is a configuration error that the module cannot detect because nothing is validated after parsing.

**Rule workflow**:
```
if (fopen fails):
    MessageBoxA(hWndMain, "CastleQuest.txt nao foi encontrado.", path, MB_OK)
    return                          # table stays all-zero
(no validation of loaded rows; no fallback defaults; no reload retry)
```

---

### Implicit rules and undocumented constraints

These behaviours are not written as rules but are enforced by the code and must be considered part of the contract.

| # | Constraint | Mechanism | Evidence | Confidence |
|---|------------|-----------|----------|------------|
| I-01 | `EF_KEYID` and `EF_QUEST` are scaled by sanctification | `BASE_GetItemAbility` applies `value * (sanc+10) / 10` because neither effect is in the exclusion list | `Code/Basedef.cpp:1809-1816` | High |
| I-02 | Quest stamps are truncated to 8 bits | `stEffect[].cValue` is `unsigned char`; `CastleQuestLevel == -1` becomes 255 | `Code/Basedef.h:403-410`; `CCastleZakum.cpp:306` | High |
| I-03 | `CastleQuestLevel` survives the run | No assignment of `-1` on timeout, clear or reload | `CCastleZakum.cpp:155,314-375` | High |
| I-04 | Party snapshot survives the run | No reset of `CastleQuestParty` outside entry | `CCastleZakum.cpp:175-184,345-375` | High |
| I-05 | Generator respawn forcing survives the run | `MinuteGenerate = 1` never restored; cleanup only disables generators with live mobs | `CCastleZakum.cpp:150,141,335,370` | High |
| I-06 | The event is not persisted | All state is process memory; restart forgets the run, keeps gate and generator mutations | `CCastleZakum.cpp:42-70` | High |
| I-07 | One event per server, not per castle | No server/instance key in any of the state | `CCastleZakum.cpp:65-70` | High |
| I-08 | Rewards do not require participation | Settlement targets indices from `PartyList`, not positions in the arena | `CCastleZakum.cpp:224-294` | High |
| I-09 | Exp rewards bypass level-up processing | Direct `MOB.Exp +=` with only a client message | `CCastleZakum.cpp:236,273` | Medium |
| I-10 | Message-box on missing config blocks startup | Called from `WinMain` before `GetMessage` | `Server.cpp:3629,3650` | High |
| I-11 | `SECTION 773` gate is silent | Hard-coded magic item index shared with the legacy path | `CCastleZakum.cpp:99`; `_MSG_UpdateItem.cpp:83` | High |
| I-12 | A dead-reckoned 12 s "minute" | `MinCounter++` per `TIMER_MIN` callback, default 12,000 ms | `ProcessSecMinTimer.cpp:2282`; `Server.cpp:3632` | High |

---

## 4. Component Structure

```
Code/TMSrv/
├── CCastleZakum.h                       # 42 lines - class declaration only
│                                        #   8 public static methods, 1 private static const char* const
│                                        #   no members, no constructor, no state (all state is file-scope)
├── CCastleZakum.cpp                     # 555 lines - entire implementation + 7 exported globals
│   │
│   ├── (module state, lines 42-70)
│   │   ├── STRUCT_CASTLEQUEST CastleQuest[64]        # parsed quest definition table
│   │   ├── const char* const CASTLE_QUEST_PATH       # "../../Common/Settings/CastleQuest.txt"
│   │   ├── int CastleQuestPosition[9][4]             # guarded rectangles [x1,y1,x2,y2]
│   │   ├── int CastleQuestClear                      # 0 idle / 1 armed / 2 announcing
│   │   ├── int CastleQuestTime                       # countdown, -1 = idle
│   │   ├── int CastleQuestParty[13]                  # 12 members + leader at [12]
│   │   ├── int CastleQuestLevel                       # active (or last) quest row, -1 = none
│   │   └── char CastleLeader[16]                     # leader name snapshot
│   │
│   ├── (authorisation, lines 72-208)
│   │   └── OpenCastleGate(conn, gateid, MSG_UpdateItem*)   # 137 lines, ~36 decision points
│   │       ├── keyid filter 10..14
│   │       ├── carry scan + consume
│   │       ├── entry-gate orchestration (purge, spawn, timer, party)
│   │       └── inner-gate level check
│   │
│   ├── (combat integration, lines 210-312)
│   │   ├── MobKilled(target, conn, PosX, PosY)        # 88 lines, ~18 decision points, 4-deep nesting
│   │   │   ├── boss match over 64 rows
│   │   │   ├── announcement + clear arming
│   │   │   └── leader/party reward settlement
│   │   └── KeyDrop(target, conn, PosX, PosY, item)    # 14 lines, loot hook
│   │
│   ├── (scheduler integration, lines 314-375)
│   │   ├── ProcessSecTimer()                          # 30 lines, 1 Hz countdown + timeout
│   │   └── ProcessMinTimer()                          # 31 lines, 2-phase clear machine
│   │
│   ├── (movement guard, lines 377-394)
│   │   └── CheckMove(conn, targetx, targety)          # 18 lines, membership + 9 rectangles
│   │
│   └── (configuration, lines 396-555)
│       ├── ReadCastleQuest()                          # 48 lines, file loop
│       └── ParseCastleString(Num, str)                # 111 lines, 18 keyword dispatch
│
├── _MSG_UpdateItem.cpp                  # caller: line 51 (sole caller of OpenCastleGate)
├── MobKilled.cpp                        # callers: line 980 (MobKilled), line 1904 (KeyDrop)
├── _MSG_Action.cpp                      # caller: line 238 (CheckMove)
├── ProcessSecMinTimer.cpp               # callers: line 1172, line 2110; scheduler owner; gate relock
├── Server.cpp                           # boot load 3629; gate init 7166-7186; UpdateItem 7472
├── imple.cpp                            # GM reloadfile 259; GM timer 571-579
├── ProcessClientMessage.h               # line 42 force-include (compile-time coupling to 60 TUs)
└── TMSrv.vcxproj                        # build: line 93 <ClCompile>, line 171 <ClInclude>
```

**Method inventory and size**

| Method | Lines | Decision points | Responsibility | Returns |
|--------|-------|-----------------|----------------|---------|
| `OpenCastleGate` | 137 | 35 | Gate authorisation and entry orchestration | `FALSE` = not a castle gate, `TRUE` = owned |
| `MobKilled` | 88 | 17 | Boss detection and reward settlement | `void` |
| `KeyDrop` | 14 | 2 | Loot stamping and inventory insertion | `FALSE` = already placed, `TRUE` = caller places |
| `ProcessSecTimer` | 30 | 6 | Countdown and timeout cleanup | `void` |
| `ProcessMinTimer` | 31 | 5 | Two-phase clear machine | `void` |
| `CheckMove` | 18 | 8 | Party membership zone guard | `void` |
| `ReadCastleQuest` | 48 | 8 | Configuration file loop | `void` |
| `ParseCastleString` | 111 | 17 | Keyword dispatch into the quest table | `TRUE`/`FALSE` (both ignored) |

Decision points counted by inspection as occurrences of `if`, `else if`, `for`, `while`, `case`, `?:`, `&&`, `||` inside each body; approximate cyclomatic complexity equals the count plus one.

**Module-level data contract**

| Symbol | Type | Scope | Written by | Read by (outside module) |
|--------|------|-------|------------|--------------------------|
| `CastleQuest` | `STRUCT_CASTLEQUEST[64]` | external | `ParseCastleString` | none |
| `CastleQuestPosition` | `int[9][4]` | external | static initialiser | none |
| `CastleQuestClear` | `int` | external | `MobKilled`, `ProcessMinTimer` | none |
| `CastleQuestTime` | `int` | external | `OpenCastleGate`, `ProcessSecTimer` | none |
| `CastleQuestParty` | `int[13]` | external | `OpenCastleGate` | none |
| `CastleQuestLevel` | `int` | external | `OpenCastleGate` | none |
| `CastleLeader` | `char[16]` | external | `OpenCastleGate` | none |
| `CASTLE_QUEST_PATH` | `const char* const` | class-private | initialiser | none |

No external translation unit references any of the seven globals (verified across `Code/**`), so the external linkage is unnecessary exposure rather than an actual shared contract.

---

## 5. Dependency Analysis

### 5.1 Internal dependency chains

```
[Protocol path]
ProcessClientMessage (dispatch, Code/TMSrv/ProcessClientMessage.cpp:180-182)
  -> Exec_MSG_UpdateItem (Code/TMSrv/_MSG_UpdateItem.cpp:20)
     -> CCastleZakum::OpenCastleGate (CCastleZakum.cpp:72)
        -> BASE_GetItemAbility (Code/Basedef.cpp:1647)          [EF_KEYID, EF_QUEST]
        -> UpdateItem (Code/TMSrv/Server.cpp:7472)
           -> BASE_UpdateItem (Code/Basedef.cpp:5292)
              -> pHeightGrid / g_pGroundMask (all-zero in this tree)
        -> GenerateMob (Code/TMSrv/Server.cpp:2995)
           -> mNPCGen.pList[] (CNPCGenerator)
           -> GetEmptyNPCMob, ReadMob-derived templates
        -> DeleteMob (Code/TMSrv/Server.cpp:7020)
           -> GridMulticast (_MSG_RemoveMob)
        -> SendClientSignalParm / SendClientMessage / SendItem (Code/TMSrv/SendFunc.cpp)
        -> GridMulticast (broadcast of client packet)
        -> Log

[Combat path]
MobKilled (Code/TMSrv/MobKilled.cpp:980)
  -> CCastleZakum::MobKilled (CCastleZakum.cpp:210)
     -> SendNoticeArea -> SendClientMessage (Code/TMSrv/SendFunc.cpp)
     -> PutItem (Code/TMSrv/Server.cpp:709) -> SendItem
     -> SendEtc (Code/TMSrv/SendFunc.cpp:1195) -> _MSG_UpdateEtc
     -> g_pMessageStringTable (_SN_CastleQuest_Killed, _NS_GETEXP, _DN_receive_D_gold, _NN_Cant_get_more_than_2G)
MobKilled drop loop (Code/TMSrv/MobKilled.cpp:1904)
  -> CCastleZakum::KeyDrop (CCastleZakum.cpp:299)
     -> BASE_GetItemAbility, PutItem

[Movement path]
Exec_MSG_Action (Code/TMSrv/_MSG_Action.cpp:238)
  -> CCastleZakum::CheckMove (CCastleZakum.cpp:377)
     -> DoTeleport (Code/TMSrv/Server.cpp:8212) -> GetEmptyMobGrid -> GetAction -> GridMulticast

[Timer path]
WM_TIMER (Code/TMSrv/Server.cpp:3723-3729)
  -> TMSrv ProcessSecTimer (ProcessSecMinTimer.cpp:1172)
       -> CCastleZakum::ProcessSecTimer (CCastleZakum.cpp:314)
          -> SecCounter, ClearArea, DeleteMob, mNPCGen
  -> TMSrv ProcessMinTimer (ProcessSecMinTimer.cpp:2110)
       -> CCastleZakum::ProcessMinTimer (CCastleZakum.cpp:345)
          -> SendNoticeArea, ClearArea, DeleteMob, mNPCGen
       -> generic Close Gates pass (ProcessSecMinTimer.cpp:2141-2209) [relocks castle gates]
       -> generic respawn loop (ProcessSecMinTimer.cpp:2212-2226)     [consumes MinuteGenerate=1]

[Boot / admin path]
WinMain -> CCastleZakum::ReadCastleQuest (Server.cpp:3629)
  -> ParseCastleString -> CastleQuest[]
GM chat "reloadfile" -> imple.cpp:259 -> ReadCastleQuest (second, live-load caller)
Reboot() (Server.cpp:7166-7186) -> forces EF_KEYID 1..14 init items to STATE_LOCKED
```

### 5.2 External dependencies

| Dependency | Kind | Version / identity | Purpose in this component | Error handling |
|------------|------|--------------------|---------------------------|----------------|
| Win32 `MessageBoxA` | OS API | `user32` via `Windows.h` | Report missing configuration file | None (modal, blocks `WinMain`) |
| CRT `fopen/fgets/fclose` | Runtime | MSVC CRT | Read `CastleQuest.txt` | Only `NULL` from `fopen` is handled |
| CRT `sscanf/atoi/_strupr/strcmp/strlen/strncpy/sprintf/memset` | Runtime | MSVC CRT | Parse and format | None (no bounds checks) |
| `mNPCGen` (`CNPCGenerator`) | In-process subsystem | `CNPCGenerator::ReadNPCGenerator` from `NPCGener.txt` (`Code/TMSrv/CNPCGene.cpp:45-122`) | Mob templates, spawn caps, respawn cadence | None (index assumed valid) |
| `pMob[]` / `pMobGrid[][]` | In-process subsystem | `CMob[25000]`, `unsigned short[4096][4096]` (`Code/TMSrv/Server.h:54,230`) | Entity and spatial state | Partial (`tmob < MAX_USER` only) |
| `pItem[]` | In-process subsystem | `CItem[5000]` (`Code/TMSrv/CItem.h:45`) | Gate objects and their `EF_KEYID` | `gateid` validated by caller only |
| `g_pItemList[]` | In-process subsystem | item list, `MAX_ITEMLIST = 6500` | `EF_KEYID`/`EF_QUEST` resolution | Range-checked inside `BASE_GetItemAbility` |
| `g_pMessageStringTable[]` | In-process subsystem | `Language.txt` (`Server.cpp:7160`) | 6 message ids | None |
| `g_pInitItem[]` | In-process subsystem | `BASE_ReadInitItem`/`BASE_InitializeInitItem` (`Code/Basedef.cpp:5880,5947`) | Gate placement and relock enumeration | Not used by this module |
| Text log file | File system | `Log` (`Code/TMSrv/Server.cpp`/`DBSrv`) + `StartLog` | `etc,opengate` audit | None |
| `CastleQuest.txt` | Configuration file | `../../Common/Settings/CastleQuest.txt` | Quest table (64 rows) | `MessageBoxA` + abort load |

No network protocol is spoken by the module itself: it emits only in-process messages (`MSG_Standard`, `MSG_StandardParm`, `MSG_SendItem`, `MSG_RemoveMob`, `MSG_UpdateEtc`, `MSG_MessagePanel`) handed to `CPSock`.

### 5.3 Runtime order-sensitive dependencies

| Ordering | Coupled items | Consequence |
|----------|---------------|-------------|
| `ReadCastleQuest` before the message loop | `Server.cpp:3629` < `:3650` | Missing file blocks startup with a modal dialog |
| `SecCounter++` before `CCastleZakum::ProcessSecTimer` | `ProcessSecMinTimer.cpp:123` < `:1172` | First castle tick occurs on the first even counter value |
| `CCastleZakum::ProcessMinTimer` before Close Gates | `:2110` < `:2141` | A gate may relock in the same tick the arena is cleared |
| `MinCounter++` after the respawn loop | `:2212-2226` < `:2282` | `MinuteGenerate = 1` takes effect on the next minute tick, not the current one |
| `Reboot()` init-item locking after `ReadInitItem` | `Server.cpp:7166-7186` | Gates start LOCKED; the module never re-locks them |
| `MobKilled` called before other `GenerateID` branches | `MobKilled.cpp:980` < `:983-999` | Castle reward runs even for mobs also handled by other events |

---

## 6. Afferent and Efferent Coupling

The component's unit of coupling is the class `CCastleZakum` (static-only) plus its seven exported globals. `Ca` counts distinct dependent call sites that bind at runtime; `Ce` counts distinct external symbols (functions, globals, constants) the module binds to.

| Component (method) | Ca | Efferent coupling (Ce) | Critical |
|--------------------|----|------------------------|----------|
| `CCastleZakum::OpenCastleGate` | 1 (`_MSG_UpdateItem.cpp:51`) | 15 — `BASE_GetItemAbility`, `UpdateItem`, `GenerateMob`, `DeleteMob`, `GridMulticast`, `SendClientMessage`, `SendItem`, `SendClientSignalParm`, `Log`, `pItem`, `pMob`, `pUser`, `pMobGrid`, `mNPCGen`, `g_pMessageStringTable` | High |
| `CCastleZakum::MobKilled` | 1 (`MobKilled.cpp:980`) | 8 — `SendNoticeArea`, `SendClientMessage`, `PutItem`, `SendEtc`, `pMob`, `pUser`, `g_pMessageStringTable`, `CastleQuest` | High |
| `CCastleZakum::KeyDrop` | 1 (`MobKilled.cpp:1904`) | 3 — `BASE_GetItemAbility`, `PutItem`, `CastleQuestLevel` | Medium |
| `CCastleZakum::ProcessSecTimer` | 1 (`ProcessSecMinTimer.cpp:1172`) | 6 — `SecCounter`, `ClearArea`, `DeleteMob`, `pMobGrid`, `mNPCGen`, `pMob` | Medium |
| `CCastleZakum::ProcessMinTimer` | 1 (`ProcessSecMinTimer.cpp:2110`) | 6 — `SendNoticeArea`, `ClearArea`, `DeleteMob`, `pMobGrid`, `mNPCGen`, `g_pMessageStringTable` | Medium |
| `CCastleZakum::CheckMove` | 1 (`_MSG_Action.cpp:238`) | 3 — `DoTeleport`, `pMob`, `CastleQuestParty` | Medium |
| `CCastleZakum::ReadCastleQuest` | 2 (`Server.cpp:3629`, `imple.cpp:259`) | 7 — `fopen`, `fgets`, `fclose`, `MessageBoxA`, `hWndMain`, `ParseCastleString`, `CastleQuest` | High (boot) |
| `CCastleZakum::ParseCastleString` | 1 (`ReadCastleQuest`) | 7 — `sscanf`, `atoi`, `strcmp`, `strlen`, `_strupr`, `sprintf`, `temp` | Medium |
| **Class aggregate (runtime)** | **8 call sites / 6 dependent TUs** | **25 distinct external callables + 9 globals + ~16 constants** | **High** |
| **Header aggregate (compile time)** | **60 client-handler TUs** via `ProcessClientMessage.h:42`, plus `Server.cpp`, `imple.cpp`, `MobKilled.cpp`, `_MSG_UpdateItem.cpp`, `_MSG_Action.cpp` | 1 include (`Basedef.h`) | Medium (recompilation fan-out) |

**Afferent detail (who depends on this module and how)**

| Dependent | Site | Nature of dependency |
|-----------|------|----------------------|
| `Exec_MSG_UpdateItem` | `_MSG_UpdateItem.cpp:51` | Hard runtime delegation; relies on the `TRUE`/`FALSE` ownership contract to decide whether to run the legacy key path |
| Mob-kill pipeline | `MobKilled.cpp:980` | Unconditional hook on every mob death; the module must tolerate any `GenerateID` |
| Loot pipeline | `MobKilled.cpp:1904` | Interception hook inside the drop loop; depends on the inverted `BOOL` convention |
| `Exec_MSG_Action` | `_MSG_Action.cpp:238` | Post-validation movement hook; runs after all map-attribute rules |
| Sec scheduler | `ProcessSecMinTimer.cpp:1172` | Runs inside the global second callback after `SecCounter++` |
| Minute scheduler | `ProcessSecMinTimer.cpp:2110` | Runs inside the global minute callback before gate relock and respawn |
| Boot sequence | `Server.cpp:3629` | Mandatory initialisation before the message loop |
| GM command handler | `imple.cpp:259` | Live reload of the quest table |
| All handler TUs | `ProcessClientMessage.h:42` | Compile-time only; no symbol is used outside the six sites above |
| Build system | `TMSrv.vcxproj:93,171`; `TMSrv.vcxproj.filters:61,288-289,341-342` | Compilation unit and filter folder |

**Coupling assessment**

- **Efferent is dominated by world-mutation primitives.** 8 of 15 dependencies of `OpenCastleGate` write shared world state (`pItem`, `pMob`, `pMobGrid`, `mNPCGen`, multicast, inventory). The method is therefore not testable without a fully initialised world.
- **Afferent is thin but load-bearing.** Six call sites, three of which are inside *global* callbacks (timer, kill, movement) that execute on every tick/kill/step, so the module sits on hot paths despite its narrow interface.
- **The `BOOL` return convention is inverted between two methods.** `OpenCastleGate` returns `TRUE` for "handled" while `KeyDrop` returns `FALSE` for "already placed by me"; both are used as inline conditions at their call sites, making the contract easy to invert by mistake.
- **Header-level coupling is far larger than symbol-level coupling** — 60 TUs recompile on any change to the 42-line header, and the header exposes seven `static` methods and no state, so nothing is encapsulation-protected.
- **Data coupling to `mNPCGen` is the most dangerous edge** because it violates the module's apparent boundary: `CCastleZakum` permanently rewrites the global NPC respawn configuration that every other region of the world depends on.

---

## 7. Endpoints (Invocation Surface)

The module **does not expose an endpoint of its own**. It is reachable through four external surfaces that belong to other components; they are listed here because they are the only ways to reach its business rules.

| Surface | Protocol / trigger | Entry symbol | Payload | Handler binding |
|---------|--------------------|--------------|---------|-----------------|
| Gate interaction | Game protocol `_MSG_UpdateItem` (type 884, size 20) | `Exec_MSG_UpdateItem` → `OpenCastleGate` | `{int ItemID, int State}` | `Code/TMSrv/ProcessClientMessage.cpp:180-182` |
| Mob death | Server-internal combat pipeline | `MobKilled` → (`MobKilled`, `KeyDrop`) | `{target, conn, PosX, PosY}` | `Code/TMSrv/MobKilled.cpp:980,1904` |
| Movement step | Game protocol `_MSG_Action` (internal call) | `Exec_MSG_Action` → `CheckMove` | `{TargetX, TargetY}` | `Code/TMSrv/_MSG_Action.cpp:238` |
| Timer tick | Win32 `WM_TIMER` (`TIMER_SEC`, `TIMER_MIN`) | `ProcessSecTimer` / `ProcessMinTimer` | none | `Code/TMSrv/Server.cpp:3723-3729` |
| Quest table reload | GM chat command `reloadfile` | `ProcessImple` → `ReadCastleQuest` | chat text | `Code/TMSrv/imple.cpp:250-262` |

**Client-observable effects (outbound messages emitted by this module)**

| Message | Id | Sent to | Trigger |
|---------|----|---------|---------|
| `_MSG_MessagePanel` (`_NN_No_Key`) | 79 | opener | key/quest predicate failed, gate item != 773 |
| `_MSG_MessagePanel` (`_SD_Zakum_Quest_by_S_N`) | 222 | opener | entry attempted while `CastleQuestTime != -1` |
| `_MSG_MessagePanel` (`_SN_CastleQuest_Killed`) | 163 | area `2176..2300 × 1160..1276` | boss death |
| `_MSG_MessagePanel` (`_NN_CastleQuest_Initialize`) | 164 | area `2176..2300 × 1160..1276` | clear phase `1 → 2` |
| `_MSG_MessagePanel` (`_NS_GETEXP`) | 473 | reward recipient | exp prize applied |
| `_MSG_MessagePanel` (`_DN_receive_D_gold`) | 294 | reward recipient | coin prize applied |
| `_MSG_MessagePanel` (`_NN_Cant_get_more_than_2G`) | 273 | reward recipient | coin cap hit |
| `_MSG_StartTime` | 929 | leader + live party | run start |
| `_MSG_UpdateItem` (echo) | 884 | area around gate | gate opened |
| `_MSG_SendItem` | — | opener / recipients | key consumed, prize granted |
| `_MSG_UpdateEtc` | — | reward recipient | coin resync |

---

## 8. Integration Points

| Integration | Type | Purpose | Protocol | Data Format | Error Handling |
|-------------|------|---------|----------|-------------|----------------|
| `_MSG_UpdateItem` handler | Internal caller | Gate open request | In-process `MSG_UpdateItem` | 20-byte binary struct | `BOOL` ownership flag (`TRUE` = handled) |
| Mob-kill pipeline | Internal caller | Boss reward trigger | Direct call | `int` (target, conn, X, Y) | None (rewards tolerate invalid indices via callee guards) |
| Loot pipeline | Internal caller | Key stamping | Direct call | `STRUCT_ITEM*` | None (`PutItem` result discarded) |
| Action handler | Internal caller | Zone membership guard | Direct call | `int` (conn, x, y) | None (`DoTeleport` self-validates) |
| Sec timer | Scheduler | Countdown, timeout | `WM_TIMER` 500 ms | none | None |
| Min timer | Scheduler | Clear state machine | `WM_TIMER` 12,000 ms (GM-settable ≥ 5,000) | none | None |
| `CNPCGenerator` (`mNPCGen`) | Shared subsystem | Mob templates, spawn caps, respawn cadence | Direct struct access | `CNPCGen::pList[8192]` | None (index unchecked) |
| `CItem` table (`pItem`) | Shared subsystem | Gate objects | Direct struct access | `CItem[5000]` | `gateid` validated by caller |
| `CMob` table (`pMob`) | Shared subsystem | Players, mobs, party links, carry | Direct struct access | `CMob[25000]` | Partial range checks in callees |
| `g_pMessageStringTable` | Shared subsystem | Localised text | Indexed array | `char*` | None |
| `CastleQuest.txt` | File / configuration | Quest table | Text, `#`-delimited blocks | ASCII | `MessageBoxA` + abort load; no row validation |
| `Log` file | File / audit | `etc,opengate` records | Appended text | ISO-ish timestamp + line | None |
| `Reboot()` gate init | Upstream writer | Force `EF_KEYID 1..14` items to LOCKED | Direct call | — | Skips items with invalid `sIndex` |
| Generic Close Gates pass | Downstream writer | Relock open castle gates | Direct call to `UpdateItem` | — | `Delay` latch, 2 ticks |
| Generic respawn loop | Downstream reader | Consume `MinuteGenerate == 1` | Direct struct read | — | Skips `MinuteGenerate <= 0` |
| GM `reloadfile` | Operator command | Re-parse quest table | Chat command | text | None (mid-flight failure leaves a partially filled table) |
| GM `timer` | Operator command | Change `TIMER_MIN` | Chat command | `int >= 5000` | Rejects `< 5000` with a message |

---

## 9. Design Patterns & Architecture

| Pattern | Implementation | Location | Purpose | Assessment |
|---------|----------------|----------|---------|------------|
| Static class as namespace | `class CCastleZakum` with only `static` methods, no members, no constructor | `Code/TMSrv/CCastleZakum.h:25-42` | Group eight procedures under one name | Shape only; the actual state lives in seven file-scope globals, so the class provides no encapsulation |
| Table-driven configuration | `STRUCT_CASTLEQUEST CastleQuest[64]` populated by a text parser | `CCastleZakum.cpp:42,396-555` | Data-driven quest rows instead of hard-coded levels | Effective for content authoring; zero schema validation |
| Hand-written recursive-free parser / keyword dispatcher | `else if (!strcmp(str1, "KEY:"))` chain over 18 keywords | `CCastleZakum.cpp:498-553` | Map text keys to struct fields | Works; unsafe buffers and no diagnostics |
| Ownership hand-off (chain of responsibility) | `BOOL` return distinguishing "handled" from "not mine" | `CCastleZakum.cpp:79-80`, `_MSG_UpdateItem.cpp:51-52` | Allow two gate subsystems to share one packet | Contract is inverted between the two module methods |
| Sentinel-based state machine (anti-pattern) | `CastleQuestTime == -1` means idle | `CCastleZakum.cpp:67,111,156,320` | Avoid an explicit run-state enum | Idle and "never initialised" are indistinguishable; `0` is a legal transient |
| Explicit multi-phase flag machine | `CastleQuestClear` `0 → 1 → 2 → 0` | `CCastleZakum.cpp:65,220,347-374` | Deferred, announced cleanup | Clear; but phase timing is tied to an operator-tunable timer |
| Party snapshot (capability list) | `CastleQuestParty[13]` frozen at run start | `CCastleZakum.cpp:68,175-184` | Authorise participants without re-checking party state | Never invalidated, index-based, no revalidation |
| Value-object reward bundle | `Prize[MAX_CARRY]` + `ExpPrize[6]` + `CoinPrize` in one row | `CCastleZakum.cpp:229-294` | Declarative reward settlement | Settlement duplicated three times (leader, member, key) by copy-paste |
| Broadcast projection | `SendNoticeArea` / `GridMulticast` over hard-coded rectangles | `CCastleZakum.cpp:203,219,350` | Area-of-interest messaging | Rectangles are magic numbers, three different ones in one file |
| Hook / interception callback | `KeyDrop` returning "handled" to suppress the ground drop | `CCastleZakum.cpp:299-312` | Turn a world drop into a direct grant | Inverted `BOOL` relative to `OpenCastleGate` |
| Polling scheduler integration | `SecCounter % 2`, `MinCounter % N` | `CCastleZakum.cpp:316`; `ProcessSecMinTimer.cpp:2224` | Timer without a dedicated thread | Cadence depends on a global mutable interval |
| Global mutable singleton state | Seven file-scope variables with external linkage | `CCastleZakum.cpp:42-70` | Share event state across callbacks | Makes the module untestable in isolation and impossible to instance per server/instance |

**Architectural decisions observed**

1. **Event state is process-global, not per-party or per-instance.** There is exactly one castle quest in the world; `CastleQuestTime`, `CastleQuestLevel` and `CastleQuestParty` are single values. The architecture therefore encodes "one castle, one run, one server" — consistent with `CastleServer`/`CastleState` in `Code/TMSrv/Server.cpp:6223-6250`, but incompatible with sharding or instancing.
2. **Authorisation is item-based, not location-based.** Every gate rule reduces to an inventory effect check; the world position of the opener is never consulted. This makes the castle reachable from anywhere and puts the whole access-control burden on item possession.
3. **Time is derived from counters, not from wall clock.** `CastleQuestTime` counts callback ticks. There is no `time()`-based expiry, so a stalled server freezes the countdown instead of expiring the run.
4. **The module owns content setup but not content teardown.** Spawn forcing and gate opening are performed here; spawn restoration and gate relocking belong to the generic timer pass and `Reboot()`. The asymmetry is the root cause of the state leakage described in BR-07, BR-25 and BR-27.
5. **Configuration is data, delivered as text, reloadable at runtime.** The quest table is re-read on demand with no versioning, no validation and no rollback, and the reload mutates live state (`CastleQuestLevel` is not recomputed).

---

## 10. Technical Debt & Risks

| Risk Level | Component Area | Issue | Impact |
|------------|----------------|-------|--------|
| High | `ParseCastleString` | `sscanf` with unbounded `%s` into 128-byte (`str1..3`) and 256-byte (`item_str1..7`) stack buffers; the only length test runs after the copy | Stack corruption from an over-long line in `CastleQuest.txt` (`CCastleZakum.cpp:459,470-487,492`) |
| High | `ParseCastleString` | `item_str1..7` are uninitialised and always passed to `atoi`; a prize line with fewer than seven numbers yields garbage prize effects/values | Non-deterministic item generation from mis-authored config (`CCastleZakum.cpp:470-487`) |
| High | `ReadCastleQuest` | `Num` starts at `-1`; a data line before the first `#` writes `CastleQuest[-1]` (out-of-bounds, 8 bytes before the array) | Memory corruption from a misplaced header (`CCastleZakum.cpp:407,436,498-553`) |
| High | `ReadCastleQuest` | `if (Num >= MAX_CASTLE_QUEST) break;` is checked before `Num++`, so the 65th `#` writes `CastleQuest[64]` | Off-by-one out-of-bounds write (`CCastleZakum.cpp:426-431`) |
| High | Spawn configuration | `mNPCGen.pList[x].MinuteGenerate = 1` is never restored; cleanup only disables generators with live mobs | Castle area keeps respawning event mobs after the event (BR-07, BR-25) |
| High | Reward settlement | `PutItem` return value discarded in all four prize loops | Silent item loss on a full inventory; no retry, no message (`CCastleZakum.cpp:232,268`) |
| High | Reward settlement | `MobKilled` matches `GenerateIndex` against all 64 rows with no `break`, and does not check `CastleQuestLevel` or the arena | Duplicate rewards when rows share boss ids; rewards while no run is active (`CCastleZakum.cpp:214-216`) |
| High | Configuration fallback | Missing `CastleQuest.txt` leaves an all-zero table with `BOSS[0] == 0`, so every `GenerateIndex == 0` kill announces a clear | Spurious event state transitions in a misconfigured deployment (`CCastleZakum.cpp:398-405`) |
| High | Configuration validation | No check on `MOB_INITIAL <= MOB_END`, `MOB_END < 8192`, boss indices inside the range, `QuestTime > 0` | Out-of-bounds `mNPCGen.pList` access, or marking player-range generators (`CCastleZakum.cpp:147-156`) |
| Medium | Gate application | `UpdateItem(gateid, STATE_OPEN)` result only gates the multicast; a failed transition still consumes the key and writes the audit line | Key consumed with no state change (`CCastleZakum.cpp:195-206`) |
| Medium | Gate broadcast | The client's own packet is multicast, exposing `ID = conn`, client `State`, `KeyWord`, `ClientTick` | Visual desync between clients and server; opener identity leak (`CCastleZakum.cpp:203`) |
| Medium | Authorisation model | No proximity, map or ownership check; any key carrier can open any castle gate from anywhere | Unauthorised entry enabled by item transfer (`CCastleZakum.cpp:72-103`) |
| Medium | Entry gate | `gatekey == 10` bypasses the quest-tag check by operator precedence; any `EF_KEYID 10` item starts a run at the level it carries | Unintended quest-level selection; permissive entry (`CCastleZakum.cpp:91,97`) |
| Medium | Zone guard | `CastleQuestPosition[0] = { 2221, 1270, 2259, 1246 }` has inverted Y bounds, so the first block is never guarded | One of nine guarded areas is effectively open (`CCastleZakum.cpp:49,388`) |
| Medium | Zone guard | Only movement steps are checked; recall, teleport, login and GM moves bypass `CheckMove`; existing occupants are never evicted | Access control is advisory (`CCastleZakum.cpp:377-394`; `_MSG_Action.cpp:238`) |
| Medium | Membership state | `CastleQuestParty` is never invalidated on timeout, clear, logout or reload | Stale participants keep access across runs (`CCastleZakum.cpp:68,175-184`) |
| Medium | Quest level state | `CastleQuestLevel` is never reset to `-1`, so inner-gate keys stay valid while idle | Keys remain usable between runs (`CCastleZakum.cpp:155`) |
| Medium | Loot hook | `stEffect[0]` is overwritten when stamping, destroying an existing item bonus; `PutItem` result discarded | Item value mutation and key loss on a full bag (`CCastleZakum.cpp:305-308`) |
| Medium | Effect resolution | `EF_KEYID`/`EF_QUEST` are sanctification-scaled by `BASE_GetItemAbility` (not on the exclusion list) | Enhanced key items read a different id/level than authored (`Code/Basedef.cpp:1809-1816`) |
| Medium | Exp reward | Direct `MOB.Exp +=` with no overflow guard, no level-up processing, no participation check | Unbounded experience mutation on any matching kill (`CCastleZakum.cpp:236,273`) |
| Medium | Party reward | `PartyPrize` used as loop condition; leader not excluded; every member gets the full bundle | Reward multiplication up to 13x (`CCastleZakum.cpp:258-294`) |
| Medium | Timer semantics | `CastleQuestTime` derives from a callback-parity counter; `TIMER_SEC`/`TIMER_MIN` are mutable (`imple.cpp:571-579`) | Countdown duration is not wall-clock stable (`ProcessSecMinTimer.cpp:123,2282`) |
| Medium | Event exclusivity | Boss kill does not release `CastleQuestTime`; entry stays blocked until natural expiry | Party cannot restart after a win (`CCastleZakum.cpp:220,111-127`) |
| Medium | Configuration reload | `reloadfile` re-parses live with no rollback and no state reconciliation | Half-loaded table on a mid-file parse failure (`imple.cpp:259`) |
| Low | Audit | No record for rejected castle attempts; prefix identical to the generic gate log | Missing forensic trail (`CCastleZakum.cpp:102,126,191` vs `:205`) |
| Low | `CastleLeader` | `strncpy` of 16 bytes into a 16-byte buffer without forced terminator; used with `%s` | Potential unterminated-string read (`CCastleZakum.cpp:163,123`) |
| Low | Code cleanliness | Unused locals `state`, `gatestate2`; unused parameters `target`, `PosX`, `PosY` in `KeyDrop`; unused `secondvalue`; dead branch at 188-192 | Misleading code, dead logic (`CCastleZakum.cpp:74-75,188-192,299,457`) |
| Low | Duplication | Key-scan and gate-open logic duplicated between the module and the legacy path; the copy lacks the quest check | Divergent gate semantics (`CCastleZakum.cpp:86-103` vs `_MSG_UpdateItem.cpp:65-90`) |
| Low | Geometry | Three different hard-coded rectangles (`2180..2296 × 1160..1269` for spawn/cleanup, `2176..2300 × 1160..1276` for messages, nine rects for the guard) | Off-by-a-few inconsistencies between "arena" and "announcement area" (`CCastleZakum.cpp:130,219,321,350`) |
| Low | Performance | 12,870-cell grid sweep per purge; 64-row boss scan per mob death; 64-slot carry scan per gate | CPU spent on global hot paths (`CCastleZakum.cpp:130-144,214-216,86-95`) |
| Low | Encapsulation | Seven globals with external linkage and no accessor; empty class façade | No isolation, no test seams (`CCastleZakum.cpp:42-70`) |
| Low | Header coupling | Included by `ProcessClientMessage.h:42`, so 60 handler TUs recompile on any header change | Build fan-out (`ProcessClientMessage.h:42`) |

---

## 11. Test Coverage Analysis

**Result: no tests exist for this component — or for any other part of the project.**

Evidence gathered across the entire project root (excluding `.agents`, `.codegraph`, `.opencode`, `openspec`):

| Evidence | Finding |
|----------|---------|
| Test project files | Only three build files exist: `W2PP Code Project.sln`, `Code/TMSrv/TMSrv.vcxproj`, `Code/DBSrv/DBSrv.vcxproj`. No test project, no test target, no test configuration |
| Test source files | No `*test*`, `*Test*`, `*spec*` file exists anywhere under `Code/` or the repository root |
| Test frameworks | No occurrence of `gtest`, `CppUnit`, `catch2`, `doctest`, `BOOST_TEST` or `assert.h` in any `.c`, `.cpp`, `.h`, `.hpp` file |
| Test harness / fixtures / mocks | None; no `tests/`, `test/`, `spec/`, `__tests__`, `mocks/`, `fixtures/` directory |
| CI configuration | No workflow, pipeline or script configuration in the repository |
| Assertions in production code | No `assert` usage; error signalling relies on `Log`, `MessageBoxA` and the `AddCrackError` accumulator |
| Agent-produced evidence | `codegraph_explore` reports "no tests found within 3 caller hops" for every symbol of this module (`OpenCastleGate`, `MobKilled`, `KeyDrop`, `ProcessSecTimer`, `ProcessMinTimer`, `CheckMove`, `ReadCastleQuest`, `ParseCastleString`) |

**Coverage matrix**

| Component | Unit Tests | Integration Tests | Coverage | Test Quality |
|-----------|------------|-------------------|----------|--------------|
| `CCastleZakum::OpenCastleGate` | 0 | 0 | 0 % | None; 137 lines, ~36 decision points, five gate-id branches, entirely untested |
| `CCastleZakum::MobKilled` | 0 | 0 | 0 % | None; reward arithmetic (coin cap, class-tier exp) verified only by production use |
| `CCastleZakum::KeyDrop` | 0 | 0 | 0 % | None; stamping and truncation behaviour unverified |
| `CCastleZakum::ProcessSecTimer` | 0 | 0 | 0 % | None; countdown boundary (`QuestTime` 0/1) unverified |
| `CCastleZakum::ProcessMinTimer` | 0 | 0 | 0 % | None; phase machine (`1 → 2 → 0`) unverified |
| `CCastleZakum::CheckMove` | 0 | 0 | 0 % | None; rectangle geometry including the inverted Y bounds unverified |
| `CCastleZakum::ReadCastleQuest` | 0 | 0 | 0 % | None; missing-file, empty-file, 65-block and pre-header cases unverified |
| `CCastleZakum::ParseCastleString` | 0 | 0 | 0 % | None; 18 keyword branches, short prize lines and long tokens unverified |
| Module globals (`CastleQuest[64]`, timers, party snapshot) | 0 | 0 | 0 % | No fixture can construct them; state is process-global |

**Structural reasons the component is untestable as written** (facts, not recommendations)

1. `CCastleZakum` has no instance state and no injected dependencies; every method reaches directly into `pMob`, `pUser`, `pItem`, `pMobGrid` and `mNPCGen`, so any test would need the full server world initialised.
2. `ReadCastleQuest` obtains its input from a fixed relative path (`../../Common/Settings/CastleQuest.txt`, line 44) with no parameter or override.
3. `ProcessSecTimer` and `ProcessMinTimer` read the global `SecCounter`/`MinCounter` directly (`CCastleZakum.cpp:316`), so their cadence cannot be driven deterministically.
4. Time and randomness enter through `rand()` inside `GenerateMob` and through the mutable `TIMER_*` intervals, both process-global.
5. Winsock/Win32 initialisation (`hWndMain`, `SendClient*`, `GridMulticast`) is required for any code path that produces output.

**Manual verification surfaces available today** (the only executable evidence)

| Surface | How the behaviour is currently validated |
|---------|------------------------------------------|
| Gate opening | Production play + `etc,opengate` log lines |
| Reward settlement | Production play + item/exp/coin messages; item drops are additionally logged by `MobKilled.cpp:1907-1910` |
| Configuration load | Server boot (missing file shows a modal dialog) and GM `reloadfile` |
| Timer phases | Production observation of `_NN_CastleQuest_Initialize` and area eviction |

---

## 12. Ambiguities, Assumptions and Analysis Confidence

| # | Ambiguity | Evidence | Confidence in interpretation |
|---|-----------|----------|------------------------------|
| A-01 | The semantic identity of gate item `sIndex 773` (why the "no key" message is suppressed for it) | `CCastleZakum.cpp:99`; `_MSG_UpdateItem.cpp:83`; no `g_pItemList` data or item list file in the repository | Low — the behaviour is certain, the purpose is not |
| A-02 | Whether inner-gate keys `11..14` correspond to specific doors (one key per door) or are interchangeable within a level | Keys are matched by `EF_KEYID` equality at `CCastleZakum.cpp:91`; mapping depends on the gate items' configured effects | Medium — the code enforces exact id equality, so keys are door-specific by construction |
| A-03 | Whether the event is intended to be startable without a running countdown after a boss kill | Boss kill only sets `CastleQuestClear` (`:220`); `CastleQuestTime` untouched | High for the code behaviour; intent unknown |
| A-04 | Whether `CastleQuestTime` is intended as seconds | Decrement once per 1 Hz tick (`:316-341`); `_MSG_StartTime` payload is `QuestTime` | High for mechanics; unit semantics inferred from the 1 Hz cadence |
| A-05 | Whether experience awarded here is expected to trigger level-up elsewhere | No `LevelUp`/`CheckLevel` symbol exists in TMSrv; ordinary kill path also writes `MOB.Exp +=` directly | Medium — consistent with the rest of the codebase, so the omission is likely systemic rather than local |
| A-06 | Whether `MOB_INITIAL == MOB_END == 0` rows are intended as "disabled" | `ReadCastleQuest` zeroes every row on `#` and only writes what the file provides | Medium — a zeroed row is indistinguishable from a deliberately empty one |
| A-07 | Whether the `#` block count limit is meant to be 64 or 65 valid rows | `#define MAX_CASTLE_QUEST 64` (`Code/Basedef.h:156`) vs off-by-one check at `:426` | High that the code is off by one; intent is 64 |
| A-08 | Whether `PARTYPRIZE` semantics are "reward the party in full" or "share the reward" | Loop pays the identical bundle to each member (`:258-294`) | High for behaviour; intent unknown |
| A-09 | Whether the announcement rectangle (`2176..2300 × 1160..1276`) is intentionally wider than the arena (`2180..2296 × 1160..1269`) | Three literal rectangles at `:130,219,321,350`; no named constants | Medium — the difference is small and consistent, suggesting a deliberate "include bystanders" margin |
| A-10 | Whether `CastleQuestPosition[0]`'s inverted Y pair is a transcription error or an intentionally disabled zone | Entry `{ 2221, 1270, 2259, 1246 }` at `:49` vs ascending order in the other eight entries | High that it is a defect; intent unknown |
| A-11 | Whether castle gates are meant to be re-openable during a run given the 2-tick relock | Relock at `ProcessSecMinTimer.cpp:2141-2209` vs `CastleQuestTime` of many ticks | Medium — the interplay is real but no documentation exists |
| A-12 | Whether `KeyDrop`'s `target`/`PosX`/`PosY` parameters were once used for a position-based drop | Present in both the header (`CCastleZakum.h:33`) and the call (`MobKilled.cpp:1904`), unused in the body | Medium — dead API surface, purpose lost |
| A-13 | Actual production values for `MOB_INITIAL/MOB_END/BOSS/QuestTime/Prize` | `CastleQuest.txt` is not in the repository (searched the whole tree) | Not determinable — all numeric behaviour depends on a file absent from the code tree |
| A-14 | Whether `NPCGener.txt`/`InitItem.txt` place the castle gates and generators at the hard-coded coordinates | Both files are runtime artefacts, absent from the repository | Not determinable |

**Analysis method and confidence**

- All eight methods were read in full from the working tree (`CCastleZakum.cpp:1-555`, `CCastleZakum.h:1-42`).
- Every call site was located by full-project symbol search: eight call sites in six translation units, plus build-system entries and the forced header include.
- All callee behaviour referenced in this report (`UpdateItem`, `BASE_UpdateItem`, `BASE_GetItemAbility`, `GenerateMob`, `DeleteMob`, `ClearArea`, `DoTeleport`, `PutItem`, `Send*`, `Log`, respawn loop, relock pass) was verified in source, not inferred from names.
- Numeric behaviour of the event cannot be stated as fact because the configuration file is absent; the report describes the mechanism and the dependency, not the deployed values.
